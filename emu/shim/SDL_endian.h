/*
 * Copyright (C) 2026 Neil Rackett
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * File: SDL_endian.h
 * Description: What rp/src/doom/i_swap.h takes from SDL when not built
 *              for the RP2040: the byte order of the WAD data, which is
 *              the host's on a little-endian machine, as on the RP2040.
 */

#ifndef MDDOOM_EMU_SDL_ENDIAN_H
#define MDDOOM_EMU_SDL_ENDIAN_H

#if __BYTE_ORDER__ != __ORDER_LITTLE_ENDIAN__
#error "EmuMD builds of MD/DOOM need a little-endian host"
#endif

#define SDL_LIL_ENDIAN 1234
#define SDL_BIG_ENDIAN 4321
#define SDL_BYTEORDER SDL_LIL_ENDIAN
#define SDL_SwapLE16(x) (x)
#define SDL_SwapLE32(x) (x)
#define SDL_SwapBE16(x) __builtin_bswap16(x)
#define SDL_SwapBE32(x) __builtin_bswap32(x)

#endif
