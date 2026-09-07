/*
 * Copyright (C) 2026 Neil Rackett
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Based on rp2040-doom's pico/i_video.c:
 * Copyright(C) 1993-1996 Id Software, Inc.
 * Copyright(C) 2005-2014 Simon Howard
 * Copyright(C) 2021-2022 Graham Sanderson
 * GPL-2.0-or-later.
 *
 * File: i_video.c
 * Description: Doom's video interface on the MD framework.
 *
 * Upstream renders into two 320x168 buffers and composites the status
 * bar, HUD and menus from "vpatch" display lists at VGA scan-out on the
 * other core. Here there is one 320x200 buffer -- the framework's
 * chunked framebuffer -- rendered and shown synchronously: after the
 * renderer finishes a frame (the end of pd_end_frame) the overlay list
 * is drawn into rows 168..199 (and wherever else it lands) with the same
 * V_DrawPatchList the engine uses for full-screen pages, the palette
 * page Doom asked for is turned into 16 ST colours + a dither LUT, and
 * doom_video_publish() converts and hands the frame to the m68k.
 *
 * The two-buffer bookkeeping (render_frame_index, next_frame_index,
 * the render_frame_ready / display_frame_freed semaphores) is kept so
 * the renderer is touched as little as possible; both indices simply
 * resolve to the one buffer.
 */

#include <string.h>

#include "pico/sem.h"
#include "pico/stdlib.h"

#include "config.h"
#include "d_event.h"
#include "doom/doomstat.h"
#include "doom/r_data.h"
#include "doomtype.h"
#include "i_input.h"
#include "i_system.h"
#include "i_video.h"
#include "picodoom.h"
#include "v_video.h"
#include "w_wad.h"
#include "whddata.h"
#include "z_zone.h"

#include "doom_video.h"
#include "fb_chunked.h"

/* Set while Core 1 has the SIO interpolators in use; nothing else uses
 * them here, so it is only ever read. */
volatile uint8_t interp_in_use;

boolean screenvisible = true;
boolean screensaver_mode = false;
isb_int8_t usegamma = 0;
unsigned int joywait = 0;

pixel_t *I_VideoBuffer;

/* Both "frames" are the framework's chunked buffer: 320x200 bytes, the
 * top 168 rows the view, the bottom 32 the status bar. */
uint8_t *const frame_buffer[2] = {fb_chunked_buffer, fb_chunked_buffer};

static int8_t next_pal = -1;
static int8_t shown_pal = -1;
static const uint8_t *s_playpal; /* page 0 in the current pack */

semaphore_t render_frame_ready, display_frame_freed;

uint8_t next_video_type;
uint8_t next_frame_index;
uint8_t next_overlay_index;
uint8_t display_frame_index;
uint8_t display_overlay_index;
uint8_t display_video_type;

/* The melt wipe never runs here (pd_end_frame is called with wipe off),
 * but the renderer still references its state. */
uint8_t *wipe_yoffsets;
int16_t *wipe_yoffsets_raw;
uint32_t *wipe_linelookup;
volatile uint8_t wipe_min;

static boolean initialized;

void I_ShutdownGraphics(void) {}
void I_StartFrame(void) {}
void I_SetWindowTitle(const char *title) { (void)title; }
void I_FinishUpdate(void) {}
void I_UpdateNoBlit(void) {}
int I_GetPaletteIndex(int r, int g, int b) { (void)r; (void)g; (void)b; return 0; }
void I_BindVideoVariables(void) {}
void I_GraphicsCheckCommandLine(void) {}
void I_CheckIsScreensaver(void) {}
void I_DisplayFPSDots(boolean dots_on) { (void)dots_on; }

void I_SetPaletteNum(int doompalette) { next_pal = (int8_t)doompalette; }

/* A level pack was swapped: the PLAYPAL pointer belongs to the old one.
 * Called by the pack hook; the next frame re-resolves and re-applies. */
void I_MD_PackChanged(void) {
  s_playpal = NULL;
  if (shown_pal >= 0) next_pal = shown_pal;
  shown_pal = -1;
}

/* Derive the requested PLAYPAL page from page 0. The WHX only carries
 * page 0 (whd_gen drops the other 13 when they are the standard tints),
 * so the pain, bonus and radiation-suit palettes are computed the way
 * upstream's scan-out does it. */
static void apply_palette(int pal) {
  static uint8_t page[768];
  if (!s_playpal) {
    lumpindex_t l = W_GetNumForName("PLAYPAL");
    s_playpal = W_CacheLumpNum(l, PU_STATIC);
  }
  const uint8_t *src = s_playpal;
  if (pal == 0) {
    memcpy(page, src, sizeof(page));
  } else {
    int mul, r0, g0, b0;
    if (pal < 9) {
      mul = pal * 65536 / 9;
      r0 = 255; g0 = b0 = 0;
    } else if (pal < 13) {
      mul = (pal - 8) * 65536 / 8;
      r0 = 215; g0 = 186; b0 = 69;
    } else {
      mul = 65536 / 8;
      r0 = b0 = 0; g0 = 256;
    }
    uint8_t *dst = page;
    for (int i = 0; i < 256; i++) {
      int r = *src++, g = *src++, b = *src++;
      r += ((r0 - r) * mul) >> 16;
      g += ((g0 - g) * mul) >> 16;
      b += ((b0 - b) * mul) >> 16;
      *dst++ = (uint8_t)r;
      *dst++ = (uint8_t)g;
      *dst++ = (uint8_t)b;
    }
  }
  doom_video_set_playpal(page);
}

/* Called at the end of pd_end_frame, once the renderer has released the
 * frame: composite the overlays, apply any palette change, publish. */
void I_MD_PresentFrame(void) {
  if (!sem_available(&render_frame_ready)) return;
  sem_acquire_blocking(&render_frame_ready);
  display_video_type = next_video_type;
  display_frame_index = next_frame_index;
  display_overlay_index = next_overlay_index;

  if (display_video_type >= FIRST_VIDEO_TYPE_WITH_OVERLAYS) {
    /* The status bar, HUD widgets, menus and intermission text. Drawn
     * straight into the frame; rows 168..199 hold nothing else in a
     * level, and full-screen pages are redrawn every frame. */
    pixel_t *saved = I_VideoBuffer;
    I_VideoBuffer = fb_chunked_buffer;
    vpatch_clip_top = 0;
    vpatch_clip_bottom = SCREENHEIGHT;
    V_DrawPatchList(vpatchlists->overlays[display_overlay_index]);
    I_VideoBuffer = saved;
  }

  if (next_pal != -1) {
    if (next_pal != shown_pal) {
      apply_palette(next_pal);
      shown_pal = next_pal;
    }
    next_pal = -1;
  }

  doom_video_publish();
  sem_release(&display_frame_freed);

  /* Core 1 is idle from here until the next pd_begin_frame, so this is
   * where anything that needs to park it (a flash write) can run. */
  extern void I_MD_FlushVideoSettings(void);
  I_MD_FlushVideoSettings();
}

void I_InitGraphics(void) {
  sem_init(&render_frame_ready, 0, 2);
  sem_init(&display_frame_freed, 1, 2);
  pd_init();
  I_VideoBuffer = fb_chunked_buffer;
  initialized = true;
}

void I_StartTic(void) {
  if (!initialized) return;
  I_GetEvent();
}
