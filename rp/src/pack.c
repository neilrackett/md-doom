/*
 * Copyright (C) 2026 Neil Rackett
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * File: pack.c
 * Description: SD card -> PACK_FLASH programmer. See pack.h.
 */

#include "pack.h"

#include <string.h>

#include "constants.h"
#include "debug.h"
#include "fb_chunked.h"
#include "ff.h"
#include "hardware/flash.h"
#include "hardware/sync.h"
#include "pico/stdlib.h"

/* Staging chunk: read this much from SD, then program it. The chunked
 * framebuffer is borrowed as the staging buffer, so this is bounded by
 * its size. */
#define PACK_CHUNK 32768u
_Static_assert(PACK_CHUNK <= FB_CHUNKED_SIZE, "staging chunk must fit the framebuffer");

static uint32_t s_loaded;

const uint8_t *pack_base(void) { return (const uint8_t *)&_pack_flash_start; }

uint32_t pack_capacity(void) { return pack_flash_size(); }

uint32_t pack_loaded_size(void) { return s_loaded; }

bool pack_present(void) {
  return memcmp(pack_base(), "IWHX", 4) == 0;
}

bool pack_load(const char *path, pack_progress_cb_t progress) {
  FIL f;
  if (f_open(&f, path, FA_READ) != FR_OK) {
    DPRINTF("pack_load: cannot open '%s'\n", path);
    return false;
  }
  const uint32_t size = (uint32_t)f_size(&f);
  if (size == 0 || size > pack_capacity()) {
    DPRINTF("pack_load: '%s' is %lu bytes, window is %lu\n", path,
            (unsigned long)size, (unsigned long)pack_capacity());
    f_close(&f);
    return false;
  }

  const uint32_t base = (uint32_t)(uintptr_t)&_pack_flash_start - XIP_BASE;
  const uint32_t erase_len =
      (size + FLASH_SECTOR_SIZE - 1u) & ~(uint32_t)(FLASH_SECTOR_SIZE - 1u);

  s_loaded = 0;
  if (progress) progress(0, size);

  /* One erase for the whole image: the SDK promotes a 64 KB-aligned
   * range to block erases, several times faster than sector by sector. */
  fb_core1_park();
  uint32_t ints = save_and_disable_interrupts();
  flash_range_erase(base, erase_len);
  restore_interrupts(ints);
  fb_core1_unpark();

  uint8_t *staging = fb_chunked_buffer;
  uint32_t done = 0;
  bool ok = true;
  while (done < size) {
    uint32_t want = size - done;
    if (want > PACK_CHUNK) want = PACK_CHUNK;
    UINT br = 0;
    if (f_read(&f, staging, want, &br) != FR_OK || br == 0) {
      ok = false;
      break;
    }
    /* Pad the final short chunk out to a whole page; erased flash is
     * 0xFF, which is also what an absent byte should read as. */
    uint32_t prog = (br + FLASH_PAGE_SIZE - 1u) & ~(uint32_t)(FLASH_PAGE_SIZE - 1u);
    if (prog > br) memset(staging + br, 0xFF, prog - br);

    fb_core1_park();
    ints = save_and_disable_interrupts();
    flash_range_program(base + done, staging, prog);
    restore_interrupts(ints);
    fb_core1_unpark();

    done += br;
    if (progress) progress(done, size);
    if (br < want) break; /* short read: end of file */
  }
  f_close(&f);

  if (ok && done >= size) {
    s_loaded = size;
    DPRINTF("pack_load: '%s' %lu bytes programmed\n", path, (unsigned long)size);
    return true;
  }
  DPRINTF("pack_load: '%s' failed after %lu bytes\n", path, (unsigned long)done);
  return false;
}
