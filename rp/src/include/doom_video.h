/*
 * Copyright (C) 2026 Neil Rackett
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * File: doom_video.h
 * Description: Doom's 256-colour frame -> the ST's 16 colours.
 *
 * Doom renders 320x200 bytes of PLAYPAL indices into fb_chunked_buffer.
 * The ST can show 16 colours, so each frame goes through a 256 -> 16
 * lookup with a 4x4 ordered dither and is converted to ST planar in the
 * same pass: doom_video_publish() reads the 8 bpp buffer, maps every
 * pixel through the LUT for its dither cell, packs the result straight
 * into the cart framebuffer, and hands the frame to the m68k. The 16 hardware
 * colours travel with it through the cart palette slot.
 *
 * The 16 colours come from one of a few sources (doom_video_palette_t)
 * and are re-derived from whichever PLAYPAL page Doom asks for, so the
 * damage, bonus and radiation-suit tints still work: the same 16 slots
 * are picked out of the tinted page and the LUT is rebuilt against them.
 * The fixed retro palettes match by hue and shade by brightness instead
 * of by plain distance, or Doom's dark colours would all come out black.
 *
 * This is the same reduction the DOOM Accelerator microfirmware for
 * STDOOM does, carried over here; the default 16-colour subset is the
 * one hand-picked for STDOOM by Jonas Eschenburg.
 */

#ifndef DOOM_VIDEO_H_INCLUDED
#define DOOM_VIDEO_H_INCLUDED

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* How the 16 on-screen colours are chosen from the current 256. */
typedef enum {
  DOOM_VIDEO_PAL_SUBSET = 0, /* STDOOM's hand-picked PLAYPAL subset  */
  DOOM_VIDEO_PAL_GENERATED,  /* median cut + k-means over the page   */
  DOOM_VIDEO_PAL_GREY,       /* 16-step grey ramp                    */
  DOOM_VIDEO_PAL_EGA,        /* the famous fixed palettes, for fun:  */
  DOOM_VIDEO_PAL_CGA,        /*   CGA palette 1 (4 colours)          */
  DOOM_VIDEO_PAL_C64,
  DOOM_VIDEO_PAL_ZX,
  DOOM_VIDEO_PAL_PICO8,
  DOOM_VIDEO_PAL_COUNT
} doom_video_palette_t;

/* How a colour that falls between two of the 16 is drawn. */
typedef enum {
  DOOM_VIDEO_DITHER_NEAREST = 0, /* closest colour, no dither          */
  DOOM_VIDEO_DITHER_BAYER2,      /* 2x2 ordered                        */
  DOOM_VIDEO_DITHER_BAYER4,      /* 4x4 ordered (default)              */
  DOOM_VIDEO_DITHER_HALFTONE,    /* 4x4 clustered dot                  */
  DOOM_VIDEO_DITHER_BLUENOISE,   /* 32x32 void-and-cluster tile        */
  DOOM_VIDEO_DITHER_COUNT
} doom_video_dither_t;

/* Reset to the defaults (subset palette, 4x4 Bayer) with no PLAYPAL
 * installed; nothing publishes until doom_video_set_playpal(). */
void doom_video_init(void);

/* Install a 256-entry palette (768 bytes of RGB, one PLAYPAL page) and
 * rebuild the 16 ST colours + the dither LUT from it. Doom calls this
 * via I_SetPalette at level start and on every tint change; it costs
 * well under a millisecond, so no caching is done. The bytes are copied;
 * the caller's buffer need not stay live. */
void doom_video_set_playpal(const uint8_t *rgb768);

/* Change the palette source or dither mode. Both rebuild the LUT from
 * the last installed PLAYPAL. */
void doom_video_set_palette_mode(doom_video_palette_t mode);
void doom_video_set_dither(doom_video_dither_t mode);
doom_video_palette_t doom_video_get_palette_mode(void);
doom_video_dither_t doom_video_get_dither(void);
const char *doom_video_palette_name(doom_video_palette_t mode);
const char *doom_video_dither_name(doom_video_dither_t mode);

/* Doom's screen melt. doom_video_wipe_begin() takes the last published
 * frame as the picture that melts away and fb_chunked_buffer as the one
 * beneath it (nothing may draw into the buffer until the wipe is done);
 * each doom_video_wipe_step(tics) advances the columns by Doom's rules
 * for `tics` game tics and publishes, and returns true once every
 * column has fallen off the bottom. The melt runs in place on the cart
 * FB, so it needs no second buffer. */
void doom_video_wipe_begin(void);
bool doom_video_wipe_step(int tics);

/* Convert fb_chunked_buffer (320x200 PLAYPAL indices) to ST planar via
 * the LUT and hand it to the m68k, VBL-synced and tear-free. Blocks on
 * the ST's VBL like fb_publish(), so one call per frame paces the app
 * to 50 Hz. The 16 colour words go out in the same frame. */
void doom_video_publish(void);

/* Microseconds the last doom_video_publish() spent in the LUT + c2p
 * pass (not counting the VBL wait). For the timings overlay. */
uint32_t doom_video_last_convert_us(void);

/* The 16 ST colour words currently published (0000.0RRR.0GGG.0BBB with
 * the STE low bit in bit 3 of each nibble), for anything that wants to
 * draw in a known pen: entry `i` of this table is the hardware colour a
 * PLAYPAL index that maps to pen `i` will show as. */
const uint16_t *doom_video_st_palette(void);

#ifdef __cplusplus
}
#endif

#endif /* DOOM_VIDEO_H_INCLUDED */
