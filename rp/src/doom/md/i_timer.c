/*
 * Copyright (C) 2026 Neil Rackett
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * File: i_timer.c
 * Description: Doom's timer interface on the RP2040. Upstream's
 *              I_GetTime returned TICRATE x milliseconds; this one
 *              returns tics.
 */

#include "pico/time.h"

#include "doomtype.h"
#include "i_timer.h"

int I_GetTime(void) {
  return (int)((time_us_64() * TICRATE) / 1000000u);
}

int I_GetTimeMS(void) { return (int)(time_us_64() / 1000); }

void I_Sleep(int ms) { sleep_ms(ms); }

void I_WaitVBL(int count) { I_Sleep((count * 1000) / 70); }

void I_InitTimer(void) {}
