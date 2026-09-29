/*
 * Copyright (C) 2026 Neil Rackett
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * File: constants.h
 * Description: rp/src/include/constants.h for EmuMD: what the firmware's
 *              logic uses of it, with memmap_rp.ld's regions in EmuMD's
 *              flash (mdfw_flash, at the same offsets) and the shared
 *              cartridge region (ROM_IN_RAM) as the ROM4 window the ST
 *              reads. The GPIO and clock constants belong to the
 *              hardware set-up, which is not built.
 */

#ifndef CONSTANTS_H
#define CONSTANTS_H

#include <stdint.h>

#include "hardware/regs/addressmap.h"
#include "pico.h"

#define HEX_BASE 16
#define DEC_BASE 10
#define SEC_TO_MS 1000

#define ROM_BANKS 1
#define ROM_SIZE_BYTES 0x10000
#define ROM_SIZE_WORDS (ROM_SIZE_BYTES / 2)
#define ROM_SIZE_LONGWORDS (ROM_SIZE_BYTES / 4)

#ifndef CURRENT_APP_UUID_KEY
#define CURRENT_APP_UUID_KEY "PLACEHOLDER"
#endif

#define GET_CURRENT_TIME() \
  (((uint64_t)timer_hw->timerawh) << 32u | timer_hw->timerawl)

/* memmap_rp.ld, as offsets into the 2 MB flash. */
#define MDDOOM_PACK_FLASH_OFFSET 0x5D000u
#define MDDOOM_PACK_FLASH_BYTES (780u * 1024u)
#define MDDOOM_BOOSTER_FLASH_OFFSET 0x120000u
#define MDDOOM_CONFIG_FLASH_OFFSET 0x1E0000u
#define MDDOOM_GLOBAL_LOOKUP_FLASH_OFFSET 0x1FE000u
#define MDDOOM_GLOBAL_CONFIG_FLASH_OFFSET 0x1FF000u

// NOLINTBEGIN(readability-identifier-naming)
#define __flash_binary_start (*(unsigned int *)&mdfw_flash[0])
#define _pack_flash_start (*(unsigned int *)&mdfw_flash[MDDOOM_PACK_FLASH_OFFSET])
#define _booster_app_flash_start \
  (*(unsigned int *)&mdfw_flash[MDDOOM_BOOSTER_FLASH_OFFSET])
#define _config_flash_start (*(unsigned int *)&mdfw_flash[MDDOOM_CONFIG_FLASH_OFFSET])
#define _global_lookup_flash_start \
  (*(unsigned int *)&mdfw_flash[MDDOOM_GLOBAL_LOOKUP_FLASH_OFFSET])
#define _global_config_flash_start \
  (*(unsigned int *)&mdfw_flash[MDDOOM_GLOBAL_CONFIG_FLASH_OFFSET])
#define __rom_in_ram_start__ (*(unsigned int *)mdfw_rom4())
// NOLINTEND(readability-identifier-naming)

static inline uint32_t pack_flash_size(void) { return MDDOOM_PACK_FLASH_BYTES; }

/* reset.h's jump to the Booster (never called here) names these. */
#define PPB_BASE 0xe0000000u
#define M0PLUS_VTOR_OFFSET 0x0000ed08u

#endif  // CONSTANTS_H
