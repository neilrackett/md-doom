/*
 * Copyright (C) 2026 Neil Rackett
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * File: doom_sound.c
 * Description: Sound-effect mixer. See doom_sound.h.
 */

#include "doom_sound.h"

#include <string.h>

#include "audio.h"

typedef struct {
  const uint8_t *pcm;
  uint32_t len;
  uint32_t pos;      /* 16.16 sample position                      */
  uint32_t step;     /* 16.16 input samples per output sample      */
  uint32_t rate_hz;  /* so the step can be recomputed on a mode change */
  int gain;          /* 0..127, volume folded with separation      */
  uint32_t started;  /* serial number, for stealing the oldest     */
} channel_t;

static channel_t s_ch[DOOM_SOUND_CHANNELS];
static uint32_t s_serial;
static int s_master = 8; /* 0..15 */
static audio_mode_t s_mode_for_steps = AUDIO_MODE_SILENT;

/* Hand-tuned (chA, chB) YM volume pairs from the 1988 Ghostbusters
 * demo's SAMPLE1 table, indexed by the top 6 bits of an unsigned 8-bit
 * sample. Two channels summing on the YM's logarithmic DAC give about
 * six effective bits where one gives four. */
const uint8_t doom_sound_ghost_lut[64][2] = {
    {0, 0},  {0, 2},  {1, 2},  {2, 2},  {2, 3},  {1, 4},  {2, 4},  {2, 5},
    {0, 6},  {2, 6},  {3, 6},  {4, 6},  {2, 7},  {4, 7},  {5, 7},  {2, 8},
    {3, 8},  {4, 8},  {5, 8},  {2, 9},  {3, 9},  {4, 9},  {5, 9},  {6, 9},
    {7, 9},  {3, 10}, {4, 10}, {5, 10}, {6, 10}, {7, 10}, {0, 11}, {1, 11},
    {2, 11}, {4, 11}, {5, 11}, {6, 11}, {7, 11}, {8, 11}, {8, 11}, {9, 11},
    {9, 11}, {0, 12}, {1, 12}, {2, 12}, {3, 12}, {4, 12}, {5, 12}, {6, 12},
    {8, 12}, {8, 12}, {9, 12}, {9, 12}, {9, 12}, {10, 12}, {0, 13}, {2, 13},
    {3, 13}, {4, 13}, {5, 13}, {6, 13}, {7, 13}, {8, 13}, {8, 13}, {9, 13},
};

#if MDDOOM_TEST_CARD
static uint32_t output_rate(audio_mode_t mode) {
  return (mode == AUDIO_MODE_YM) ? DOOM_SOUND_RATE_YM : DOOM_SOUND_RATE_DMA;
}

static uint32_t step_for(uint32_t rate_hz, audio_mode_t mode) {
  return (uint32_t)(((uint64_t)rate_hz << 16) / output_rate(mode));
}

static int fold_gain(int vol, int sep) {
  /* Mono: the louder of the two stereo sides, as the listener would
   * hear from a sound to one side. */
  if (vol < 0) vol = 0;
  if (vol > 127) vol = 127;
  if (sep < 0) sep = 0;
  if (sep > 255) sep = 255;
  int side = (sep >= 128) ? sep : (255 - sep); /* 128..255 */
  return (vol * side) / 255;
}

void doom_sound_init(void) {
  memset(s_ch, 0, sizeof(s_ch));
  s_serial = 0;
  s_mode_for_steps = AUDIO_MODE_SILENT;
}

int doom_sound_start(const uint8_t *pcm, uint32_t len, uint32_t rate_hz,
                     int vol, int sep) {
  if (pcm == NULL || len == 0 || rate_hz == 0) return -1;

  int slot = -1;
  uint32_t oldest = 0xFFFFFFFFu;
  for (int i = 0; i < DOOM_SOUND_CHANNELS; i++) {
    if (s_ch[i].pcm == NULL) {
      slot = i;
      break;
    }
    if (s_ch[i].started < oldest) {
      oldest = s_ch[i].started;
      slot = i;
    }
  }

  channel_t *c = &s_ch[slot];
  c->pcm = pcm;
  c->len = len;
  c->pos = 0;
  c->rate_hz = rate_hz;
  c->step = step_for(rate_hz, audio_get_mode());
  c->gain = fold_gain(vol, sep);
  c->started = ++s_serial;
  return slot;
}

void doom_sound_stop(int channel) {
  if (channel < 0 || channel >= DOOM_SOUND_CHANNELS) return;
  s_ch[channel].pcm = NULL;
}

bool doom_sound_is_playing(int channel) {
  if (channel < 0 || channel >= DOOM_SOUND_CHANNELS) return false;
  return s_ch[channel].pcm != NULL;
}

void doom_sound_update_params(int channel, int vol, int sep) {
  if (channel < 0 || channel >= DOOM_SOUND_CHANNELS) return;
  s_ch[channel].gain = fold_gain(vol, sep);
}

void doom_sound_set_volume(int vol) {
  if (vol < 0) vol = 0;
  if (vol > 15) vol = 15;
  s_master = vol;
}

int doom_sound_get_volume(void) { return s_master; }

void doom_sound_fill(uint8_t *buf, uint32_t bytes) {
  const audio_mode_t mode = audio_get_mode();
  if (mode == AUDIO_MODE_SILENT) {
    memset(buf, 0, bytes);
    return;
  }
  const bool ym = (mode == AUDIO_MODE_YM);
  const uint32_t nsamp = ym ? bytes / 2u : bytes;

  /* The back-end is only known once the m68k has reported it, which is
   * after boot, so channels started before then carry the wrong step.
   * Re-derive them on the first fill after a change. */
  if (mode != s_mode_for_steps) {
    for (int i = 0; i < DOOM_SOUND_CHANNELS; i++) {
      if (s_ch[i].pcm) s_ch[i].step = step_for(s_ch[i].rate_hz, mode);
    }
    s_mode_for_steps = mode;
  }

  for (uint32_t i = 0; i < nsamp; i++) {
    int32_t acc = 0;
    for (int ch = 0; ch < DOOM_SOUND_CHANNELS; ch++) {
      channel_t *c = &s_ch[ch];
      if (c->pcm == NULL) continue;
      const uint32_t idx = c->pos >> 16;
      if (idx >= c->len) {
        c->pcm = NULL;
        continue;
      }
      /* Unsigned 8-bit -> signed, scaled by the channel's gain. */
      acc += ((int32_t)c->pcm[idx] - 128) * c->gain;
      c->pos += c->step;
    }
    /* Sum of up to 8 channels at gain 127 -> clamp to signed 8-bit. */
    int32_t v = (acc * s_master) / (127 * 15);
    if (v > 127) v = 127;
    if (v < -127) v = -127;

    if (ym) {
      const uint8_t k = (uint8_t)((v + 128) >> 2);
      buf[2u * i] = doom_sound_ghost_lut[k][0];
      buf[2u * i + 1u] = doom_sound_ghost_lut[k][1];
    } else {
      buf[i] = (uint8_t)(int8_t)v;
    }
  }
}

#endif /* MDDOOM_TEST_CARD */
