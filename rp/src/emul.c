/*
 * Copyright (C) 2026 Neil Rackett
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * File: emul.c
 * Description: Boot path for MD/DOOM. Brings up the cartridge bus
 *              emulator, the 320x200 framebuffer, the ROM3 cart-bus
 *              capture ring, the audio refill interrupt and the SD
 *              card, then hands off to the game (md/md_main.c), or
 *              to the test card's own loop with MDDOOM_TEST_CARD=1.
 */

#include "emul.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "aconfig.h"
#include "audio.h"
#include "cart_shared.h"
#include "commemul.h"
#include "constants.h"
#include "debug.h"
#include "doomapp.h"
#include "fb.h"
#include "ff.h"
#include "ikbd.h"
#include "memfunc.h"
#include "palette.h"
#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "reset.h"
#include "romemul.h"
#include "sdcard.h"
#include "settings/settings.h"
#include "target_firmware.h"
#include "xpadin.h"

/* Cart-corruption canary: the first 16 bytes of the served cart region
 * (the m68k cartridge header + entry) must stay byte-identical to the
 * embedded image after COPY_FIRMWARE_TO_RAM. Any change means something
 * on the RP wrote into the region the m68k is executing from -- which
 * makes the ST bomb or freeze on the boot frame, with no clue on the RP
 * side that anything is wrong. Logged (debug builds only) with a stage
 * tag so the culprit can be localised over UART. */
/* How long to wait at boot for the m68k's relocation heartbeat. Long
 * enough for a cold ST's memory test, which is seconds on a 1 MB
 * machine, and irrelevant to the user either way: the ST cannot display
 * anything until the m68k is blitting, which is after the signal. */
#define ST_RELOC_TIMEOUT_MS 15000u
/* How long to give the ST to come back after a reset before rebooting
 * anyway. Its cold boot has to get as far as reading the cartridge. */
#define ST_REJOIN_TIMEOUT_MS 5000u
/* Reboots caused by a lost ST, counted across reboots in a watchdog
 * scratch register (0 belongs to the Booster request). Three in a row
 * means something is wrong that rebooting will not fix, so stop. */
#define ST_LOSS_REBOOT_LIMIT 3

/* The ST was reset. Put the cartridge image back before TOS looks for
 * it -- by now the zone may be living in that address space -- and then
 * restart the RP so the two come up together. Everything the engine was
 * doing is gone either way: the restore overwrites whatever the zone
 * had there, which is why Core 1 and the audio refill are stopped
 * first. Does not return. */
static void __attribute__((noreturn)) st_lost_recover(void) {
  DPRINTF("ST lost: restoring the cartridge image\n");
  audio_stop_vbl_timer();
  multicore_reset_core1();
  ERASE_FIRMWARE_IN_RAM();
  COPY_FIRMWARE_TO_RAM((uint16_t *)target_firmware, target_firmware_length);

  const uint32_t strikes = watchdog_hw->scratch[1] + 1u;
  watchdog_hw->scratch[1] = strikes;

  /* Wait for the ST to find the restored cartridge and start
   * heartbeating again, so the reboot lands while it is busy rather
   * than leaving it reading an unserved bus. */
  if (fb_wait_st_reloc(ST_REJOIN_TIMEOUT_MS)) {
    DPRINTF("ST is back; rebooting to rejoin it\n");
  } else {
    DPRINTF("ST did not return in time; rebooting anyway\n");
  }

  if (strikes >= ST_LOSS_REBOOT_LIMIT) {
    /* Rebooting is not helping. Leave the cartridge restored so the ST
     * can at least boot, and stop. */
    DPRINTF("%u ST losses in a row; not rebooting again\n",
            (unsigned)strikes);
    for (;;) {
      fb_pump_rom3();
    }
  }

  reset_device();
  for (;;) {
    tight_loop_contents();
  }
}

static bool cart_check(const char *stage) {
  static bool ok = true;
  if (ok &&
      memcmp((const void *)&__rom_in_ram_start__, target_firmware, 16) != 0) {
    ok = false;
    const volatile uint16_t *c =
        (const volatile uint16_t *)&__rom_in_ram_start__;
    DPRINTF("CART REGION CORRUPTED (%s): %04X %04X %04X %04X\n", stage, c[0],
            c[1], c[2], c[3]);
  }
  return ok;
}

void emul_start() {
  // Level packs live in the app folder (per-app config
  // ACONFIG_PARAM_FOLDER, default "/doom" from aconfig.c).
  SettingsConfigEntry *folder =
      settings_find_entry(aconfig_getContext(), ACONFIG_PARAM_FOLDER);
  const char *folderName = folder ? folder->value : "/doom";

  // The .cart_app_free buffers live inside the shared region's unused
  // hole. The linker script hard-codes that window (ld cannot read C
  // headers), so verify it here against cart_shared.h's authoritative
  // offsets -- a drifted script would let the ST-visible layout and the
  // parked buffers collide.
  {
    extern char __cart_app_free_start__[], __cart_app_free_end__[];
    const char *base = (const char *)&__rom_in_ram_start__;
    if (__cart_app_free_start__ < base + CART_APP_FREE_OFFSET ||
        __cart_app_free_end__ > base + CART_FRAMEBUFFER_OFFSET) {
      panic(".cart_app_free outside the shared-region hole");
    }
  }

  // RP2040 RAM is undefined at power-on and firmware.py only emits the
  // bytes up to the last non-zero in BOOT.BIN, so zero the whole 64 KB
  // shared region first -- every byte the m68k can see must be
  // deterministic -- then copy the cartridge image into it.
  ERASE_FIRMWARE_IN_RAM();
  COPY_FIRMWARE_TO_RAM((uint16_t *)target_firmware, target_firmware_length);
  cart_check("post-copy");

  // Controller-input receivers before commemul: both are fed by the ROM3
  // dispatcher from fb_init onward, and their producers run on the main
  // loop rather than from an IRQ.
  ikbd_init();
  xpadin_init();

  // Cartridge ROM4 read engine (served by chained DMA -> PIO, no CPU).
  if (init_romemul(false) < 0) {
    panic("init_romemul failed");
  }

  // ROM3 cart-bus capture ring (PIO + DMA) BEFORE fb_init, whose first
  // fb_publish() drains it while waiting for the m68k VBL ack.
  if (commemul_init() < 0) {
    panic("commemul_init failed");
  }
  cart_check("post-commemul");

  // Wait for the m68k to say it has relocated itself into ST RAM. This
  // has to come before anything that could write into the cartridge
  // code area, which is why it sits here rather than later: from this
  // point the ST is provably alive and provably not executing from the
  // region we want to reclaim. A boot without it is not an error -- no
  // ST attached, an older firmware image, or the user held Shift for
  // the desktop -- it simply means the reclaim stays off, so say which
  // of the two happened on every boot.
  if (fb_wait_st_reloc(ST_RELOC_TIMEOUT_MS)) {
    DPRINTF("ST relocated to RAM; cartridge code area is reclaimable\n");
    fb_set_st_lost_handler(st_lost_recover);
  } else {
    DPRINTF("no ST relocation in %u ms; running without the reclaim\n",
            (unsigned)ST_RELOC_TIMEOUT_MS);
  }

  // 320x200 4bpp framebuffer + fb_screen for the draw primitives.
  // (This launches Core 1 for the chunky->planar worker.)
  if (fb_init(&fb_mode_320x200) < 0) {
    panic("fb_init failed");
  }
  DPRINTF("fb_init OK\n");
  cart_check("post-fb");

  // Default palette for the boot splash; doom_video replaces it with the
  // 16 colours it derives from PLAYPAL.
  palette_init();

  // Cart audio buffer producer. The back-end (STE DMA or YM2149) is
  // reported by the m68k over the cart bus; audio.c decodes that report
  // from the ROM3 ring and picks the matching sample format.
  audio_init();
  // Refill the cart audio buffer from a timer interrupt, in step with the
  // m68k's VBL, so sound never depends on how long a frame takes.
  audio_start_vbl_timer(1);

  // SD card -- best effort. Level packs live in `folderName`.
  FATFS fsys;
  if (sdcard_initFilesystem(&fsys, folderName) != SDCARD_INIT_OK) {
    DPRINTF("SD card unavailable. Continuing without SD.\n");
  }
  cart_check("post-sd");

#if !MDDOOM_TEST_CARD
  // Hand over to Doom. D_DoomMain never returns; the engine's I_GetEvent
  // and I_MD_PresentFrame do what the loop below does for the test card.
  extern void doomgame_start(const char *folder);
  doomgame_start(folderName);
#else
  // The pipeline test card (MDDOOM_TEST_CARD=1). ESC keeps its default
  // meaning of "exit to GEM" (ikbd's ESC auto-exit).
  DPRINTF("doomapp_init...\n");
  doomapp_init();
  cart_check("post-doomapp-init");
  audio_set_fill_callback(doomapp_audio_fill);

  // Main loop:
  //   0. Re-arm the command sentinel to NOP (makes an exit's BOOT_GEM a
  //      one-shot so it doesn't re-trigger on the next ST reset).
  //   1. Drain the ROM3 ring -> IKBD demux + VBL frame-sync + the
  //      m68k's sound-capability report.
  //   2. Run the IKBD demux.
  //   3. Forward decoded key events to the app.
  //   4. Render one frame into the framebuffer (blocks on the VBL).
  //   (The cart audio buffer is refilled by Core 1 while it is idle.)
  DPRINTF("Entering main loop\n");
  while (true) {
    ikbd_clear_command();
    fb_pump_rom3();
    ikbd_pump();

    ikbd_key_event_t k;
    while (ikbd_pop_key(&k)) {
      doomapp_handle_key(&k);
    }

    doomapp_render_frame();
    cart_check("main-loop");
  }
#endif
}
