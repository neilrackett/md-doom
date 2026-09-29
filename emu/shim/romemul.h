/*
 * Copyright (C) 2026 Neil Rackett
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * File: romemul.h
 * Description: rp/src/include/romemul.h for EmuMD, where ROM4 is served
 *              by the emulator (emu/mdfw_app.c has init_romemul).
 */

#ifndef ROMEMUL_H
#define ROMEMUL_H

#include <stdbool.h>

int init_romemul(bool copyFlashToRAM);

#endif
