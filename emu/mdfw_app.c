/*
 * Copyright (C) 2026 Neil Rackett
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * File: mdfw_app.c
 * Description: MD/DOOM for EmuMD (see ../mdfw.ini). What main() does
 *              after its hardware set-up, on the firmware's own thread
 *              because the game never returns to a main loop; a stand-in
 *              for the Booster's first-run set-up; and the functions of
 *              the left-out hardware sources that the rest calls.
 */

#include <stdlib.h>
#include <string.h>

#include "aconfig.h"
#include "commemul.h"
#include "emul.h"
#include "gconfig.h"
#include "hardware/flash.h"
#include "mdfw.h"
#include "reset.h"
#include "romemul.h"
#include "sdcard.h"
#include "settings.h"

extern const uint16_t target_firmware[];
extern uint16_t target_firmware_length;

/* ---- The Booster's first-run set-up --------------------------------- */

/* A real Multi-device has been through the Booster long before MD/DOOM
 * runs: it made this app the one to boot and gave it a config sector in
 * the lookup table. The emulated flash starts empty, so do the same. */
static void booster_stand_in(void) {
  mdfw_log("first run: setting up the global config for this app");
  SettingsContext *global = gconfig_getContext();
  settings_put_string(global, PARAM_BOOT_FEATURE, CURRENT_APP_UUID_KEY);
  settings_save(global, true);

  uint8_t page[FLASH_PAGE_SIZE];
  memset(page, 0, sizeof(page)); /* sector 0, then the end of the table */
  memcpy(page, CURRENT_APP_UUID_KEY, UUID_SIZE);
  flash_range_erase(MDDOOM_GLOBAL_LOOKUP_FLASH_OFFSET, FLASH_SECTOR_SIZE);
  flash_range_program(MDDOOM_GLOBAL_LOOKUP_FLASH_OFFSET, page, sizeof(page));
}

/* ---- Power-on --------------------------------------------------------- */

static int app_init(void) {
  /* The RP has the cartridge in place long before the ST first looks;
   * emul_start() copies it again, as it does on the RP2040. */
  mdfw_rom4_load(target_firmware, target_firmware_length);
  return 0;
}

/* main.c from the configuration on. */
static void app_main(void) {
  if (gconfig_init(CURRENT_APP_UUID_KEY) != GCONFIG_SUCCESS) {
    booster_stand_in();
    if (gconfig_init(CURRENT_APP_UUID_KEY) != GCONFIG_SUCCESS) {
      mdfw_log("the global config did not take");
      return;
    }
  }
  switch (aconfig_init(CURRENT_APP_UUID_KEY)) {
    case ACONFIG_SUCCESS:
      break;
    case ACONFIG_INIT_ERROR:
      settings_save(aconfig_getContext(), true);
      break;
    default:
      mdfw_log("no config sector for this app");
      return;
  }
  emul_start();
}

const mdfw_app_t mdfw_app = {
    .name = MDFW_NAME,
    .version = MDFW_VERSION,
    .init = app_init,
    .main = app_main,
};

/* ---- What the left-out sources provided ------------------------------- */

/* romemul.c: ROM4 is served by EmuMD. */
int init_romemul(bool copyFlashToRAM) {
  (void)copyFlashToRAM;
  return 0;
}

/* commemul.c's MD/DOOM addition: has a read of `window_hi` arrived since
 * *cursor, without taking anything from the ring? */
bool commemul_scan(uint32_t *cursor, uint16_t window_hi) {
  uint16_t sample;
  bool seen = false;
  while (mdfw_rom3_peek(cursor, &sample)) {
    if ((sample & 0xFF00u) == window_hi) seen = true;
  }
  return seen;
}

/* sdcard.c: the card is EmuMD's folder. */
sdcard_status_t sdcard_initFilesystem(FATFS *fsPtr, const char *folderName) {
  if (f_mount(fsPtr, "0:", 1) != FR_OK) return SDCARD_MOUNT_ERROR;
  FILINFO info;
  if (f_stat(folderName, &info) != FR_OK && f_mkdir(folderName) != FR_OK) {
    return SDCARD_CREATE_FOLDER_ERROR;
  }
  return SDCARD_INIT_OK;
}

/* reset.c: reboot, which EmuMD does between two turns of the emulator. */
void reset_device(void) {
  watchdog_reboot(0, 0, RESET_WATCHDOG_TIMEOUT);
  for (;;) tight_loop_contents();
}

/* i_system.c routes malloc into the zone with the linker's --wrap. A host
 * build does without (malloc is the host's), but its __wrap_ functions
 * still name these. */
void *__real_malloc(size_t size) { return malloc(size); }
void *__real_calloc(size_t count, size_t size) { return calloc(count, size); }
void *__real_realloc(void *ptr, size_t size) { return realloc(ptr, size); }
void __real_free(void *ptr) { free(ptr); }
