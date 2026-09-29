/*
 * Copyright (C) 2026 Neil Rackett
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * File: debug.h
 * Description: rp/src/include/debug.h for EmuMD: EmuMD's DPRINTF (to
 *              Hatari's log with mdfw run -V), plus what the firmware's
 *              own debug.h also brings in.
 */

#ifndef MDDOOM_EMU_DEBUG_H
#define MDDOOM_EMU_DEBUG_H
#define DEBUG_H /* the original's guard: keep it out */

#include "constants.h"
#include "pico/stdlib.h"

#include_next "debug.h"

#define DPRINT_HEAP()

#endif
