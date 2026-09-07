/*
 * Copyright (C) 2026 Neil Rackett
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * File: doom_sound.h
 * Description: Sound-effect mixer feeding the cart audio buffer.
 *
 * Doom's sound effects are 8-bit unsigned PCM, normally at 11,025 Hz.
 * Up to DOOM_SOUND_CHANNELS of them play at once; each frame the m68k
 * asks for one VBL's worth of output and the mixer resamples every
 * active channel onto it. The output format follows the back-end the
 * ST reported at boot (audio_get_mode()):
 *
 *   - STE DMA: signed 8-bit mono at 25,033 Hz, ~500 samples a VBL. The
 *     count varies by a sample or two as the m68k steers the buffer
 *     length; the mixer just produces however many it is asked for.
 *   - YM2149: 5,585 Hz, each sample a (vA, vB) pair of channel volumes
 *     from the Ghostbusters LUT, 224 bytes a VBL.
 *
 * Music is not handled here (there is none in the initial build).
 */

#ifndef DOOM_SOUND_H_INCLUDED
#define DOOM_SOUND_H_INCLUDED

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DOOM_SOUND_CHANNELS 8

/* Output rates of the two back-ends, for anything that pre-computes
 * against them. */
#define DOOM_SOUND_RATE_DMA 25033u
#define DOOM_SOUND_RATE_YM  5585u

void doom_sound_init(void);

/* Start `len` samples of 8-bit unsigned PCM at `rate_hz` on a free
 * channel (the oldest one is stolen if none is free). `vol` is 0..127
 * and `sep` is Doom's stereo separation, 0..255 with 128 centred; the
 * output is mono so only its effect on loudness survives. Returns the
 * channel, or -1 if `pcm` is NULL or `len` is 0. The sample data must
 * stay live while the channel plays. */
int doom_sound_start(const uint8_t *pcm, uint32_t len, uint32_t rate_hz,
                     int vol, int sep);

void doom_sound_stop(int channel);
bool doom_sound_is_playing(int channel);
void doom_sound_update_params(int channel, int vol, int sep);

/* Master volume, 0..15 (Doom's sfx volume slider). */
void doom_sound_set_volume(int vol);
int doom_sound_get_volume(void);

/* Ghostbusters (vA, vB) YM volume pairs, indexed by the top 6 bits of an
 * unsigned 8-bit sample. Shared with the engine's sound module. */
extern const uint8_t doom_sound_ghost_lut[64][2];

/* The per-VBL fill callback to install with audio_set_fill_callback(). */
void doom_sound_fill(uint8_t *buf, uint32_t bytes);

#ifdef __cplusplus
}
#endif

#endif /* DOOM_SOUND_H_INCLUDED */
