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
 * The 16 colours are chosen for each frame from the colours on screen
 * (the adaptive palette), out of whichever PLAYPAL page Doom asks for,
 * so the damage, bonus and radiation-suit tints still work; the m68k
 * shows them with the frame they were chosen for. With the status bar
 * on screen it gets 16 of its own.
 *
 * In ST high res (fb_st_mono()) the same calls draw a 640x400 one-bit
 * picture instead: each colour's brightness, dithered at the full
 * resolution with the same dither modes.
 *
 * On a TT or a Falcon with a colour monitor (fb_st_eight()) there is no
 * reduction at all: the frame goes out as 8 bitplanes with PLAYPAL as
 * its 256 colours (fb_eight_publish), and the dither does not apply
 * (doom_video_eight()). The melt is the ST's there: see below.
 */

#ifndef DOOM_VIDEO_H_INCLUDED
#define DOOM_VIDEO_H_INCLUDED

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* How a colour that falls between two of the 16 is drawn. Saved in the
 * app config by number (DITHER): renumbering them means raising
 * ACONFIG_VERSION_NUMBER (aconfig.h). */
typedef enum {
  DOOM_VIDEO_DITHER_NEAREST = 0, /* closest colour, no dither          */
  DOOM_VIDEO_DITHER_BAYER2,      /* 2x2 ordered                        */
  DOOM_VIDEO_DITHER_BAYER4,      /* 4x4 ordered (default)              */
  DOOM_VIDEO_DITHER_BLUENOISE,   /* 32x32 void-and-cluster tile        */
  DOOM_VIDEO_DITHER_COUNT
} doom_video_dither_t;

/* Reset to the default dither (4x4 Bayer) with no PLAYPAL installed;
 * nothing publishes until doom_video_set_playpal(). */
void doom_video_init(void);

/* Install a 256-entry palette (768 bytes of RGB, one PLAYPAL page). Doom
 * calls this via I_SetPalette at level start and on every tint change.
 * The bytes are copied; the caller's buffer need not stay live. The 16
 * colours and the dither LUT are built from it at the next publish,
 * from the frame. */
void doom_video_set_playpal(const uint8_t *rgb768);

/* True when the ST shows Doom's own 256 colours (fb_st_eight): the
 * dither modes, which work on the 16-colour picture, do not apply. */
bool doom_video_eight(void);

/* Change the dither mode, from the next frame. */
void doom_video_set_dither(doom_video_dither_t mode);
doom_video_dither_t doom_video_get_dither(void);
const char *doom_video_dither_name(doom_video_dither_t mode);

/* Whether this frame has the status bar in rows 168..199. The bar then
 * gets 16 colours of its own, switched in by the m68k at line 168. */
void doom_video_set_status_bar(bool shown);

/* Doom's screen melt. doom_video_wipe_begin() takes the last published
 * frame as the picture that melts away and fb_chunked_buffer as the one
 * beneath it (nothing may draw into the buffer until the wipe is done);
 * each doom_video_wipe_step(tics) advances the columns by Doom's rules
 * for `tics` game tics and publishes, and returns true once every
 * column has fallen off the bottom. The melt runs in place on the cart
 * FB, so it needs no second buffer. In 256 colours the cart FB holds
 * only half a picture, so the ST melts its own: the new frame goes to it
 * first and each step sends only the columns' heights
 * (fb_eight_melt_begin / fb_eight_melt_step). */
void doom_video_wipe_begin(void);
bool doom_video_wipe_step(int tics);

/* Convert fb_chunked_buffer (320x200 PLAYPAL indices) to ST planar via
 * the LUT and hand it to the m68k, VBL-synced and tear-free. Blocks on
 * the ST's VBL like fb_publish(), so one call per frame paces the app
 * to 50 Hz. The 16 colour words go out in the same window as the
 * pixels; the m68k latches them as it copies the frame and shows them
 * with it. */
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
