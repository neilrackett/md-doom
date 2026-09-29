/*
 * Copyright (C) 2026 Neil Rackett
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * File: sd_card.h
 * Description: The FatFs SD driver's header, which rp/src/include/sdcard.h
 *              includes, for EmuMD: the SD card is a host folder, so only
 *              FatFs itself is needed.
 */

#ifndef SD_CARD_H
#define SD_CARD_H

#include "ff.h"

typedef struct sd_card_t sd_card_t;
typedef struct spi_t spi_t;

#endif
