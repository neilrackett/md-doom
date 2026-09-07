/*
 * Copyright (C) 2026 Neil Rackett
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * File: i_picosound.h
 * Description: Stand-in for rp2040-doom's pico/i_picosound.h. The engine
 *              includes it for NUM_SOUND_CHANNELS; the sound module itself
 *              is i_mdsound.c.
 */
#ifndef __I_PICO_SOUND__
#define __I_PICO_SOUND__

#include "pico.h"

#ifndef NUM_SOUND_CHANNELS
#define NUM_SOUND_CHANNELS 8
#endif

#endif
