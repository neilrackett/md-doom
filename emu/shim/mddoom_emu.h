/*
 * Copyright (C) 2026 Neil Rackett
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * File: mddoom_emu.h
 * Description: Included ahead of every source (mdfw.ini's cflags). The
 *              headers in rp/src/include include their neighbours in
 *              quotes, which finds them in the same folder before any
 *              stand-in, so the stand-ins go first and their include
 *              guards keep the originals out. And what newlib's headers
 *              bring in on the RP2040 and the host's do not: uint and
 *              friends.
 */

#ifndef MDDOOM_EMU_H
#define MDDOOM_EMU_H

#include <stdint.h>
#include <sys/types.h>

#include "cart_shared.h"
#include "constants.h"
#include "debug.h"
#include "memfunc.h"

#endif
