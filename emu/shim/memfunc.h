/*
 * Copyright (C) 2026 Neil Rackett
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * File: memfunc.h
 * Description: rp/src/include/memfunc.h for EmuMD: the shared cartridge
 *              region is filled with memset and memcpy rather than the
 *              XIP stream and DMA.
 */

#ifndef MEMFUNC_H
#define MEMFUNC_H

#include <string.h>

#include "constants.h"
#include "debug.h"

#define ERASE_FIRMWARE_IN_RAM()                                \
  do {                                                         \
    memset((void *)&__rom_in_ram_start__, 0,                   \
           ROM_SIZE_LONGWORDS * ROM_BANKS * sizeof(uint32_t)); \
    DPRINTF("RAM for the firmware zeroed.\n");                 \
  } while (0)

/* emulROM_length is in 16-bit words, as the DMA version counts it. */
#define COPY_FIRMWARE_TO_RAM(emulROM, emulROM_length)                   \
  do {                                                                  \
    memcpy((void *)&__rom_in_ram_start__, (emulROM),                    \
           (size_t)(emulROM_length) * sizeof(uint16_t));                \
    DPRINTF("Emulation firmware copied to RAM.\n");                     \
  } while (0)

#endif  // MEMFUNC_H
