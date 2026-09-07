/*
 * Copyright (C) 2026 Neil Rackett
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * File: md_main.c
 * Description: Where the framework hands over to Doom, and the level
 *              pack swap the engine calls at the start of every map.
 *
 * The game data is not one WHX but one per level (see pack.h and
 * tools/levelpack.py). At boot the E1M1 pack is programmed into flash
 * unless it is already there, then D_DoomMain runs against it. When
 * P_SetupLevel is entered for a different map, I_MD_LoadLevelPack
 * programs that map's pack and re-points everything the engine had
 * resolved against the old one: the WAD directory, the texture / flat
 * / sprite / colormap tables, the sound lump numbers, the shared vpatch
 * palettes and the PLAYPAL page.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "config.h"
#include "doom/doomstat.h"
#include "doom/r_data.h"
#include "doom/sounds.h"
#include "doomtype.h"
#include "i_system.h"
#include "v_video.h"
#include "w_wad.h"
#include "whddata.h"

#include "aconfig.h"
#include "bg_mono.h" /* the 1-bit backdrop, defined here and only here */
#include "debug.h"
#include "doom_video.h"
#include "fb.h"
#include "fb_chunked.h"
#include "fb_font.h"
#include "pack.h"
#include "settings/settings.h"

extern void D_DoomMain(void);
extern void I_Init(void);
extern void I_MD_PackChanged(void);
extern void I_MD_SoundStopAll(void);
extern void V_ResetSharedPalettes(void);
extern const struct FB_FONT font8x8;

/* The map the pack in flash was built for, e.g. "E1M1"; empty if unknown. */
static char s_pack_map[8];
static char s_folder[64] = "/doom";

/* whd_gen names the WHX after its source WAD, so a pack built by
 * tools/levelpack.py announces its map in the header. */
static void read_pack_map(void) {
  s_pack_map[0] = '\0';
  if (!pack_present()) return;
  const whdheader_t *h = (const whdheader_t *)(pack_base() + 12);
  if (h->name[0] == 'E' && h->name[2] == 'M') {
    memcpy(s_pack_map, h->name, 4);
    s_pack_map[4] = '\0';
  }
}

/* The firmware's own screens: the logo and credit from desc/bg-mono.png
 * with a message in the clear band between them (rows 81..194). PLAYPAL
 * index 4 is white and 0 black in Doom's palette (and in the boot palette
 * below), and the LUT maps them to the nearest pens. */
#define PEN_BLACK 0
#define PEN_WHITE 4
#define BAND_TOP 100

static void draw_backdrop(void) {
  const uint8_t *src = bg_mono;
  uint8_t *dst = fb_chunked_buffer;
  for (int i = 0; i < FB_CHUNKED_SIZE / 8; i++) {
    const uint8_t b = *src++;
    for (int k = 7; k >= 0; k--) {
      *dst++ = (b >> k) & 1u ? PEN_WHITE : PEN_BLACK;
    }
  }
  font_set_font(&font8x8);
  font_set_color(PEN_WHITE);
  font_set_border(0, 0);
  font_align(FONT_ALIGN_CENTER);
}

static void loading_screen(const char *map, uint32_t done, uint32_t total) {
  char line[24];
  draw_backdrop();
  /* One string per line: centred alignment centres each font_print call. */
  snprintf(line, sizeof(line), "LOADING %s", map);
  font_move(160, BAND_TOP);
  font_print(line);
  if (total) {
    const int w = (int)((uint64_t)done * (FB_CHUNKED_W - 40) / total);
    for (int y = BAND_TOP + 16; y < BAND_TOP + 22; y++) {
      memset(fb_chunked_buffer + y * FB_CHUNKED_W + 20, PEN_WHITE, (size_t)w);
    }
  }
  doom_video_publish();
}

/* A level pack is not on the card: say where to get them and stay put.
 * The ST keeps the screen; the cartridge idles. */
static void __attribute__((noreturn)) missing_pack_screen(const char *map) {
  char line[32];
  draw_backdrop();
  snprintf(line, sizeof(line), "%s.WHX NOT FOUND", map);
  font_move(160, BAND_TOP);
  font_print(line);
  font_move(160, BAND_TOP + 24);
  font_print("Download the level packs from");
  font_move(160, BAND_TOP + 36);
  font_print("neilrackett.com/atarist");
  font_move(160, BAND_TOP + 48);
  font_print("and copy them to the DOOM folder");
  font_move(160, BAND_TOP + 60);
  font_print("on your SD card");
  doom_video_publish();
  for (;;) {
    fb_pump_rom3();
    sleep_ms(20);
  }
}

static const char *s_loading_map;
static void progress(uint32_t done, uint32_t total) {
  loading_screen(s_loading_map, done, total);
}

static void program_pack(const char *map) {
  char path[96];
  snprintf(path, sizeof(path), "%s/%s.whx", s_folder, map);
  s_loading_map = map;
  loading_screen(map, 0, 0);
  if (!pack_load(path, progress)) {
    DPRINTF("cannot load %s\n", path);
    missing_pack_screen(map);
  }
  read_pack_map();
}

void I_MD_LoadLevelPack(int ep, int mp) {
  char map[8];
  snprintf(map, sizeof(map), "E%dM%d", ep, mp);
  if (strcmp(map, s_pack_map) == 0) return;

  DPRINTF("level pack: %s -> %s\n", s_pack_map, map);
  I_MD_SoundStopAll();
  program_pack(map);

  /* Re-point everything resolved against the previous pack. */
  W_AddFile("");
  R_InitData();
  for (int i = 1; i < NUMSFX; i++) {
    sfx_mut(&S_sfx[i])->lumpnum = -1;
  }
  V_ResetSharedPalettes();
  I_MD_PackChanged();
}

void doomgame_start(void) {
  SettingsConfigEntry *fe =
      settings_find_entry(aconfig_getContext(), ACONFIG_PARAM_FOLDER);
  if (fe && fe->value[0]) {
    strncpy(s_folder, fe->value, sizeof(s_folder) - 1);
    s_folder[sizeof(s_folder) - 1] = '\0';
  }

  /* Until the engine installs PLAYPAL, give the reducer a palette that
   * makes the loading screen legible: 0 black, 1..15 white, then greys. */
  {
    static uint8_t boot_pal[768];
    for (int i = 0; i < 256; i++) {
      const uint8_t v = (i == 0) ? 0 : (i < 16) ? 255 : (uint8_t)i;
      boot_pal[3 * i] = boot_pal[3 * i + 1] = boot_pal[3 * i + 2] = v;
    }
    doom_video_init();
    doom_video_set_playpal(boot_pal);
  }

  /* Boot into E1M1. If flash still holds it from last time, skip the
   * (several seconds of) programming. */
  read_pack_map();
  if (strcmp(s_pack_map, "E1M1") != 0) {
    program_pack("E1M1");
  }

  /* The dither and palette chosen last time (keypad * and /). */
  {
    SettingsConfigEntry *e = settings_find_entry(aconfig_getContext(), ACONFIG_PARAM_DITHER);
    int v = e ? atoi(e->value) : (int)DOOM_VIDEO_DITHER_BAYER4;
    if (v >= 0 && v < (int)DOOM_VIDEO_DITHER_COUNT) doom_video_set_dither((doom_video_dither_t)v);
    e = settings_find_entry(aconfig_getContext(), ACONFIG_PARAM_PALETTE);
    v = e ? atoi(e->value) : (int)DOOM_VIDEO_PAL_SUBSET;
    if (v >= 0 && v < (int)DOOM_VIDEO_PAL_COUNT) doom_video_set_palette_mode((doom_video_palette_t)v);
  }

  DPRINTF("D_DoomMain\n");
  I_Init();
  D_DoomMain(); /* never returns */
}

/* Remember the dither and palette across sessions. Writes the app's
 * settings sector, so Core 1 is held in RAM for the flash access. */
void I_MD_SaveVideoSettings(void) {
  SettingsContext *ctx = aconfig_getContext();
  settings_put_integer(ctx, ACONFIG_PARAM_DITHER, (int)doom_video_get_dither());
  settings_put_integer(ctx, ACONFIG_PARAM_PALETTE, (int)doom_video_get_palette_mode());
  fb_core1_park();
  settings_save(ctx, true);
  fb_core1_unpark();
}

/* Save games: none yet (the upstream slots live at the top of flash,
 * which here belongs to the Booster). The menu sees no saved games and a
 * save quietly fails. The SD card is the place for them later. */
#include "doom/p_saveg.h"
void P_SaveGameGetExistingFlashSlotAddresses(flash_slot_info_t *slots, int count) {
  for (int i = 0; i < count; i++) {
    slots[i].data = NULL;
    slots[i].size = 0;
  }
}
boolean P_SaveGameWriteFlashSlot(int slot, const uint8_t *buffer, uint size, uint8_t *buffer4k) {
  (void)slot; (void)buffer; (void)size; (void)buffer4k;
  return false;
}
