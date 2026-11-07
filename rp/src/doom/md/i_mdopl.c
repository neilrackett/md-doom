/*
 * Copyright (C) 2026 Neil Rackett
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Based on rp2040-doom's i_oplmusic.c (Chocolate Doom's):
 * Copyright(C) 1993-1996 Id Software, Inc.
 * Copyright(C) 2005-2014 Simon Howard
 * Copyright(C) 2021-2022 Graham Sanderson
 * GPL-2.0-or-later.
 *
 * File: i_mdopl.c
 * Description: Doom's music as DMX played it on an OPL2: GENMIDI
 *              instruments on nine two-operator voices, with Doom 1.9's
 *              voice stealing, volume curve and pitch table, all carried
 *              over from upstream's i_oplmusic.c. Upstream plays MIDI
 *              (MUS converted); this is driven straight from MUS events,
 *              as DMX itself was, so a channel is a MUS channel and 15 is
 *              percussion (upstream swaps MIDI's 9 back to 15 for the
 *              same reason). Only the instruments the song lists are in
 *              RAM, loaded by i_mdmusic.c at song start. The chip is
 *              emu8950 (doom/opl), at its own 49,716 Hz.
 */
#include "config.h"

#include <string.h>

#include "emu8950.h"
#include "i_mdopl.h"
#if PICO_ON_DEVICE
#include "hardware/interp.h"
#endif

#define OPL_NUM_VOICES 9
#define MUS_CHANNELS 16
#define GENMIDI_MAIN 128u
#define GENMIDI_FLAG_FIXED 0x0001
#define GENMIDI_FLAG_2VOICE 0x0004
#define INSTR_SLOTS 24 /* the shareware songs use 21 at most */

#define OPL_NUM_OPERATORS 21
#define OPL_REG_WAVEFORM_ENABLE 0x01
#define OPL_REG_TIMER_CTRL 0x04
#define OPL_REGS_TREMOLO 0x20
#define OPL_REGS_LEVEL 0x40
#define OPL_REGS_ATTACK 0x60
#define OPL_REGS_SUSTAIN 0x80
#define OPL_REGS_FREQ_1 0xA0
#define OPL_REGS_FREQ_2 0xB0
#define OPL_REGS_FEEDBACK 0xC0
#define OPL_REGS_WAVEFORM 0xE0

typedef struct __attribute__((packed)) {
  uint8_t tremolo, attack, sustain, waveform, scale, level;
} genmidi_op_t;

typedef struct __attribute__((packed)) {
  genmidi_op_t modulator;
  uint8_t feedback;
  genmidi_op_t carrier;
  uint8_t unused;
  int16_t base_note_offset;
} genmidi_voice_t;

typedef struct __attribute__((packed)) {
  uint16_t flags;
  uint8_t fine_tuning;
  uint8_t fixed_note;
  genmidi_voice_t voices[2];
} genmidi_instr_t;

_Static_assert(sizeof(genmidi_instr_t) == GENMIDI_INSTR_BYTES, "a GENMIDI record");

typedef struct {
  const genmidi_instr_t *instrument;
  uint8_t volume;
  uint8_t volume_base;
  int8_t bend;
} channel_t;

typedef struct {
  uint8_t index, op1, op2;
  uint8_t instr_voice; /* 1 for the second voice of a double-voice instrument */
  uint8_t key;         /* the MUS key that started it */
  uint8_t note;        /* the key, or a fixed-pitch instrument's own note */
  uint8_t note_volume;
  uint8_t car_volume, mod_volume; /* as last written */
  uint16_t freq;
  const genmidi_instr_t *instr;
  channel_t *channel;
} voice_t;

static const uint8_t voice_operators[2][OPL_NUM_VOICES] = {
    {0x00, 0x01, 0x02, 0x08, 0x09, 0x0a, 0x10, 0x11, 0x12},
    {0x03, 0x04, 0x05, 0x0b, 0x0c, 0x0d, 0x13, 0x14, 0x15},
};

/* OPL frequency register value for each 1/32 of a semitone: the first
 * 284 entries, then one octave that the rest wrap round. */
static const uint16_t frequency_curve[668] = {
    0x133, 0x133, 0x134, 0x134, 0x135, 0x136, 0x136, 0x137, 0x137, 0x138, 0x138, 0x139,
    0x139, 0x13a, 0x13b, 0x13b, 0x13c, 0x13c, 0x13d, 0x13d, 0x13e, 0x13f, 0x13f, 0x140,
    0x140, 0x141, 0x142, 0x142, 0x143, 0x143, 0x144, 0x144, 0x145, 0x146, 0x146, 0x147,
    0x147, 0x148, 0x149, 0x149, 0x14a, 0x14a, 0x14b, 0x14c, 0x14c, 0x14d, 0x14d, 0x14e,
    0x14f, 0x14f, 0x150, 0x150, 0x151, 0x152, 0x152, 0x153, 0x153, 0x154, 0x155, 0x155,
    0x156, 0x157, 0x157, 0x158, 0x158, 0x159, 0x15a, 0x15a, 0x15b, 0x15b, 0x15c, 0x15d,
    0x15d, 0x15e, 0x15f, 0x15f, 0x160, 0x161, 0x161, 0x162, 0x162, 0x163, 0x164, 0x164,
    0x165, 0x166, 0x166, 0x167, 0x168, 0x168, 0x169, 0x16a, 0x16a, 0x16b, 0x16c, 0x16c,
    0x16d, 0x16e, 0x16e, 0x16f, 0x170, 0x170, 0x171, 0x172, 0x172, 0x173, 0x174, 0x174,
    0x175, 0x176, 0x176, 0x177, 0x178, 0x178, 0x179, 0x17a, 0x17a, 0x17b, 0x17c, 0x17c,
    0x17d, 0x17e, 0x17e, 0x17f, 0x180, 0x181, 0x181, 0x182, 0x183, 0x183, 0x184, 0x185,
    0x185, 0x186, 0x187, 0x188, 0x188, 0x189, 0x18a, 0x18a, 0x18b, 0x18c, 0x18d, 0x18d,
    0x18e, 0x18f, 0x18f, 0x190, 0x191, 0x192, 0x192, 0x193, 0x194, 0x194, 0x195, 0x196,
    0x197, 0x197, 0x198, 0x199, 0x19a, 0x19a, 0x19b, 0x19c, 0x19d, 0x19d, 0x19e, 0x19f,
    0x1a0, 0x1a0, 0x1a1, 0x1a2, 0x1a3, 0x1a3, 0x1a4, 0x1a5, 0x1a6, 0x1a6, 0x1a7, 0x1a8,
    0x1a9, 0x1a9, 0x1aa, 0x1ab, 0x1ac, 0x1ad, 0x1ad, 0x1ae, 0x1af, 0x1b0, 0x1b0, 0x1b1,
    0x1b2, 0x1b3, 0x1b4, 0x1b4, 0x1b5, 0x1b6, 0x1b7, 0x1b8, 0x1b8, 0x1b9, 0x1ba, 0x1bb,
    0x1bc, 0x1bc, 0x1bd, 0x1be, 0x1bf, 0x1c0, 0x1c0, 0x1c1, 0x1c2, 0x1c3, 0x1c4, 0x1c4,
    0x1c5, 0x1c6, 0x1c7, 0x1c8, 0x1c9, 0x1c9, 0x1ca, 0x1cb, 0x1cc, 0x1cd, 0x1ce, 0x1ce,
    0x1cf, 0x1d0, 0x1d1, 0x1d2, 0x1d3, 0x1d3, 0x1d4, 0x1d5, 0x1d6, 0x1d7, 0x1d8, 0x1d8,
    0x1d9, 0x1da, 0x1db, 0x1dc, 0x1dd, 0x1de, 0x1de, 0x1df, 0x1e0, 0x1e1, 0x1e2, 0x1e3,
    0x1e4, 0x1e5, 0x1e5, 0x1e6, 0x1e7, 0x1e8, 0x1e9, 0x1ea, 0x1eb, 0x1ec, 0x1ed, 0x1ed,
    0x1ee, 0x1ef, 0x1f0, 0x1f1, 0x1f2, 0x1f3, 0x1f4, 0x1f5, 0x1f6, 0x1f6, 0x1f7, 0x1f8,
    0x1f9, 0x1fa, 0x1fb, 0x1fc, 0x1fd, 0x1fe, 0x1ff, 0x200, 0x201, 0x201, 0x202, 0x203,
    0x204, 0x205, 0x206, 0x207, 0x208, 0x209, 0x20a, 0x20b, 0x20c, 0x20d, 0x20e, 0x20f,
    0x210, 0x210, 0x211, 0x212, 0x213, 0x214, 0x215, 0x216, 0x217, 0x218, 0x219, 0x21a,
    0x21b, 0x21c, 0x21d, 0x21e, 0x21f, 0x220, 0x221, 0x222, 0x223, 0x224, 0x225, 0x226,
    0x227, 0x228, 0x229, 0x22a, 0x22b, 0x22c, 0x22d, 0x22e, 0x22f, 0x230, 0x231, 0x232,
    0x233, 0x234, 0x235, 0x236, 0x237, 0x238, 0x239, 0x23a, 0x23b, 0x23c, 0x23d, 0x23e,
    0x23f, 0x240, 0x241, 0x242, 0x244, 0x245, 0x246, 0x247, 0x248, 0x249, 0x24a, 0x24b,
    0x24c, 0x24d, 0x24e, 0x24f, 0x250, 0x251, 0x252, 0x253, 0x254, 0x256, 0x257, 0x258,
    0x259, 0x25a, 0x25b, 0x25c, 0x25d, 0x25e, 0x25f, 0x260, 0x262, 0x263, 0x264, 0x265,
    0x266, 0x267, 0x268, 0x269, 0x26a, 0x26c, 0x26d, 0x26e, 0x26f, 0x270, 0x271, 0x272,
    0x273, 0x275, 0x276, 0x277, 0x278, 0x279, 0x27a, 0x27b, 0x27d, 0x27e, 0x27f, 0x280,
    0x281, 0x282, 0x284, 0x285, 0x286, 0x287, 0x288, 0x289, 0x28b, 0x28c, 0x28d, 0x28e,
    0x28f, 0x290, 0x292, 0x293, 0x294, 0x295, 0x296, 0x298, 0x299, 0x29a, 0x29b, 0x29c,
    0x29e, 0x29f, 0x2a0, 0x2a1, 0x2a2, 0x2a4, 0x2a5, 0x2a6, 0x2a7, 0x2a9, 0x2aa, 0x2ab,
    0x2ac, 0x2ae, 0x2af, 0x2b0, 0x2b1, 0x2b2, 0x2b4, 0x2b5, 0x2b6, 0x2b7, 0x2b9, 0x2ba,
    0x2bb, 0x2bd, 0x2be, 0x2bf, 0x2c0, 0x2c2, 0x2c3, 0x2c4, 0x2c5, 0x2c7, 0x2c8, 0x2c9,
    0x2cb, 0x2cc, 0x2cd, 0x2ce, 0x2d0, 0x2d1, 0x2d2, 0x2d4, 0x2d5, 0x2d6, 0x2d8, 0x2d9,
    0x2da, 0x2dc, 0x2dd, 0x2de, 0x2e0, 0x2e1, 0x2e2, 0x2e4, 0x2e5, 0x2e6, 0x2e8, 0x2e9,
    0x2ea, 0x2ec, 0x2ed, 0x2ee, 0x2f0, 0x2f1, 0x2f2, 0x2f4, 0x2f5, 0x2f6, 0x2f8, 0x2f9,
    0x2fb, 0x2fc, 0x2fd, 0x2ff, 0x300, 0x302, 0x303, 0x304, 0x306, 0x307, 0x309, 0x30a,
    0x30b, 0x30d, 0x30e, 0x310, 0x311, 0x312, 0x314, 0x315, 0x317, 0x318, 0x31a, 0x31b,
    0x31c, 0x31e, 0x31f, 0x321, 0x322, 0x324, 0x325, 0x327, 0x328, 0x329, 0x32b, 0x32c,
    0x32e, 0x32f, 0x331, 0x332, 0x334, 0x335, 0x337, 0x338, 0x33a, 0x33b, 0x33d, 0x33e,
    0x340, 0x341, 0x343, 0x344, 0x346, 0x347, 0x349, 0x34a, 0x34c, 0x34d, 0x34f, 0x350,
    0x352, 0x353, 0x355, 0x357, 0x358, 0x35a, 0x35b, 0x35d, 0x35e, 0x360, 0x361, 0x363,
    0x365, 0x366, 0x368, 0x369, 0x36b, 0x36c, 0x36e, 0x370, 0x371, 0x373, 0x374, 0x376,
    0x378, 0x379, 0x37b, 0x37c, 0x37e, 0x380, 0x381, 0x383, 0x384, 0x386, 0x388, 0x389,
    0x38b, 0x38d, 0x38e, 0x390, 0x392, 0x393, 0x395, 0x397, 0x398, 0x39a, 0x39c, 0x39d,
    0x39f, 0x3a1, 0x3a2, 0x3a4, 0x3a6, 0x3a7, 0x3a9, 0x3ab, 0x3ac, 0x3ae, 0x3b0, 0x3b1,
    0x3b3, 0x3b5, 0x3b7, 0x3b8, 0x3ba, 0x3bc, 0x3bd, 0x3bf, 0x3c1, 0x3c3, 0x3c4, 0x3c6,
    0x3c8, 0x3ca, 0x3cb, 0x3cd, 0x3cf, 0x3d1, 0x3d2, 0x3d4, 0x3d6, 0x3d8, 0x3da, 0x3db,
    0x3dd, 0x3df, 0x3e1, 0x3e3, 0x3e4, 0x3e6, 0x3e8, 0x3ea, 0x3ec, 0x3ed, 0x3ef, 0x3f1,
    0x3f3, 0x3f5, 0x3f6, 0x3f8, 0x3fa, 0x3fc, 0x3fe, 0x36c,
};

/* MIDI volume to OPL level. */
static const uint8_t volume_mapping_table[128] = {
    0,   1,   3,   5,   6,   8,   10,  11,  13,  14,  16,  17,  19,  20,  22,  23,
    25,  26,  27,  29,  30,  32,  33,  34,  36,  37,  39,  41,  43,  45,  47,  49,
    50,  52,  54,  55,  57,  59,  60,  61,  63,  64,  66,  67,  68,  69,  71,  72,
    73,  74,  75,  76,  77,  79,  80,  81,  82,  83,  84,  84,  85,  86,  87,  88,
    89,  90,  91,  92,  92,  93,  94,  95,  96,  96,  97,  98,  99,  99,  100, 101,
    101, 102, 103, 103, 104, 105, 105, 106, 107, 107, 108, 109, 109, 110, 110, 111,
    112, 112, 113, 113, 114, 114, 115, 115, 116, 117, 117, 118, 118, 119, 119, 120,
    120, 121, 121, 122, 122, 123, 123, 123, 124, 124, 125, 125, 126, 126, 127, 127,
};

static OPL *s_opl;

static genmidi_instr_t s_instr[INSTR_SLOTS];
static uint8_t s_instr_slot[GENMIDI_INSTRS]; /* slot + 1, 0 if not loaded */
static unsigned s_instr_count;

static channel_t s_channels[MUS_CHANNELS];
static voice_t s_voices[OPL_NUM_VOICES];
static voice_t *s_free[OPL_NUM_VOICES];
static voice_t *s_alloced[OPL_NUM_VOICES];
static int s_nfree, s_nalloced;

static unsigned s_music_volume = 127;
static unsigned s_start_volume = 127;

static inline void write_reg(unsigned reg, unsigned value) {
  OPL_writeReg(s_opl, reg, (uint8_t)value);
}

static const genmidi_instr_t *instrument(unsigned index) {
  const unsigned slot = index < GENMIDI_INSTRS ? s_instr_slot[index] : 0u;
  return slot ? &s_instr[slot - 1u] : NULL;
}

void opl_md_clear_instruments(void) {
  memset(s_instr_slot, 0, sizeof(s_instr_slot));
  s_instr_count = 0;
}

bool opl_md_store_instrument(unsigned index, const uint8_t rec[GENMIDI_INSTR_BYTES]) {
  if (index >= GENMIDI_INSTRS || s_instr_count >= INSTR_SLOTS) return false;
  if (!s_instr_slot[index]) {
    memcpy(&s_instr[s_instr_count], rec, GENMIDI_INSTR_BYTES);
    s_instr_slot[index] = (uint8_t)++s_instr_count;
  }
  return true;
}

static void voice_key_off(voice_t *voice) {
  write_reg(OPL_REGS_FREQ_2 + voice->index, voice->freq >> 8);
}

static voice_t *get_free_voice(void) {
  if (s_nfree == 0) return NULL;
  voice_t *result = s_free[0];
  s_nfree--;
  for (int i = 0; i < s_nfree; i++) s_free[i] = s_free[i + 1];
  s_alloced[s_nalloced++] = result;
  return result;
}

/* Back to the free list, at its end (as Doom does). */
static void release_voice(int index) {
  if (index >= s_nalloced) return;
  voice_t *voice = s_alloced[index];
  voice_key_off(voice);
  voice->channel = NULL;
  voice->note = 0;
  s_nalloced--;
  for (int i = index; i < s_nalloced; i++) s_alloced[i] = s_alloced[i + 1];
  s_free[s_nfree++] = voice;
}

/* The scale and level fields share the level register; a carrier
 * starts at the maximum and is set by set_voice_volume. */
static void load_operator(unsigned op, const genmidi_op_t *data, bool max_level, uint8_t *volume) {
  const unsigned level = data->scale | (max_level ? 0x3fu : data->level);
  *volume = (uint8_t)level;
  write_reg(OPL_REGS_LEVEL + op, level);
  write_reg(OPL_REGS_TREMOLO + op, data->tremolo);
  write_reg(OPL_REGS_ATTACK + op, data->attack);
  write_reg(OPL_REGS_SUSTAIN + op, data->sustain);
  write_reg(OPL_REGS_WAVEFORM + op, data->waveform);
}

static void set_voice_instrument(voice_t *voice, const genmidi_instr_t *instr, unsigned instr_voice) {
  if (voice->instr == instr && voice->instr_voice == instr_voice) return;
  voice->instr = instr;
  voice->instr_voice = (uint8_t)instr_voice;
  const genmidi_voice_t *data = &instr->voices[instr_voice];
  const bool modulating = (data->feedback & 0x01) == 0;
  /* Doom loads the second operator first. */
  load_operator(voice->op2, &data->carrier, true, &voice->car_volume);
  load_operator(voice->op1, &data->modulator, !modulating, &voice->mod_volume);
  write_reg(OPL_REGS_FEEDBACK + voice->index, data->feedback | 0x30u);
}

static void set_voice_volume(voice_t *voice, unsigned volume) {
  voice->note_volume = (uint8_t)volume;
  const genmidi_voice_t *opl_voice = &voice->instr->voices[voice->instr_voice];
  const unsigned midi_volume = 2u * (volume_mapping_table[voice->channel->volume] + 1u);
  const unsigned full_volume = (volume_mapping_table[voice->note_volume] * midi_volume) >> 9;
  const unsigned car_volume = 0x3fu - full_volume;
  if (car_volume == (voice->car_volume & 0x3fu)) return;
  voice->car_volume = (uint8_t)(car_volume | (voice->car_volume & 0xc0u));
  write_reg(OPL_REGS_LEVEL + voice->op2, voice->car_volume);
  /* Non-modulated feedback: both operators are heard, so both follow. */
  if ((opl_voice->feedback & 0x01) != 0 && opl_voice->modulator.level != 0x3f) {
    unsigned mod_volume = opl_voice->modulator.level;
    if (mod_volume < car_volume) mod_volume = car_volume;
    mod_volume |= voice->mod_volume & 0xc0u;
    if (mod_volume != voice->mod_volume) {
      voice->mod_volume = (uint8_t)mod_volume;
      write_reg(OPL_REGS_LEVEL + voice->op1, mod_volume | (opl_voice->modulator.scale & 0xc0u));
    }
  }
}

static void init_voices(void) {
  s_nfree = OPL_NUM_VOICES;
  s_nalloced = 0;
  for (int i = 0; i < OPL_NUM_VOICES; i++) {
    s_voices[i].index = (uint8_t)i;
    s_voices[i].op1 = voice_operators[0][i];
    s_voices[i].op2 = voice_operators[1][i];
    s_voices[i].instr = NULL;
    s_voices[i].channel = NULL;
    s_free[i] = &s_voices[i];
  }
}

static void set_channel_volume(channel_t *channel, unsigned volume, bool clip_start) {
  channel->volume_base = (uint8_t)volume;
  if (volume > s_music_volume) volume = s_music_volume;
  if (clip_start && volume > s_start_volume) volume = s_start_volume;
  channel->volume = (uint8_t)volume;
  for (int i = 0; i < OPL_NUM_VOICES; i++) {
    if (s_voices[i].channel == channel) set_voice_volume(&s_voices[i], s_voices[i].note_volume);
  }
}

/* The music volume caps every channel; percussion is set to it outright
 * (as Doom does). */
void opl_md_music_volume(unsigned volume) {
  if (volume > 127u) volume = 127u;
  if (s_music_volume == volume) return;
  s_music_volume = volume;
  for (unsigned i = 0; i < MUS_CHANNELS; i++) {
    set_channel_volume(&s_channels[i], i == OPL_MD_PERCUSSION ? volume : s_channels[i].volume_base,
                       false);
  }
}

void opl_md_key_off(unsigned ch, unsigned key) {
  channel_t *channel = &s_channels[ch & 15u];
  for (int i = 0; i < s_nalloced; i++) {
    if (s_alloced[i]->channel == channel && s_alloced[i]->key == key) {
      release_voice(i);
      i--;
    }
  }
}

/* Every voice busy: drop one. A second voice goes first; otherwise the
 * highest-numbered channel's, as lower channels take precedence. */
static void replace_existing_voice(void) {
  int result = 0;
  for (int i = 0; i < s_nalloced; i++) {
    if (s_alloced[i]->instr_voice != 0 || s_alloced[i]->channel >= s_alloced[result]->channel) {
      result = i;
    }
  }
  release_voice(result);
}

static unsigned frequency_for_voice(const voice_t *voice) {
  const genmidi_voice_t *gm_voice = &voice->instr->voices[voice->instr_voice];
  int note = voice->note;
  if ((voice->instr->flags & GENMIDI_FLAG_FIXED) == 0) note += gm_voice->base_note_offset;
  while (note < 0) note += 12;
  while (note > 95) note -= 12;
  int freq_index = 64 + 32 * note + voice->channel->bend;
  if (voice->instr_voice != 0) freq_index += (voice->instr->fine_tuning / 2) - 64;
  if (freq_index < 0) freq_index = 0;
  /* The first notes use the start of the table; the rest go round its
   * last octave, with the octave in the register's block bits (seven at
   * most; beyond it the original breaks down in its own way). */
  if (freq_index < 284) return frequency_curve[freq_index];
  const unsigned sub_index = (unsigned)(freq_index - 284) % (12u * 32u);
  unsigned octave = (unsigned)(freq_index - 284) / (12u * 32u);
  if (octave >= 7u) octave = 7u;
  return frequency_curve[sub_index + 284u] | (octave << 10);
}

static void update_voice_frequency(voice_t *voice) {
  const unsigned freq = frequency_for_voice(voice);
  if (voice->freq != freq) {
    write_reg(OPL_REGS_FREQ_1 + voice->index, freq & 0xffu);
    write_reg(OPL_REGS_FREQ_2 + voice->index, (freq >> 8) | 0x20u);
    voice->freq = (uint16_t)freq;
  }
}

static void voice_key_on(channel_t *channel, const genmidi_instr_t *instr, unsigned instr_voice,
                         unsigned note, unsigned key, unsigned volume) {
  voice_t *voice = get_free_voice();
  if (!voice) return;
  voice->channel = channel;
  voice->key = (uint8_t)key;
  voice->note = (uint8_t)((instr->flags & GENMIDI_FLAG_FIXED) ? instr->fixed_note : note);
  set_voice_instrument(voice, instr, instr_voice);
  set_voice_volume(voice, volume);
  voice->freq = 0;
  update_voice_frequency(voice); /* key on */
}

void opl_md_key_on(unsigned ch, unsigned key, unsigned volume) {
  if (volume == 0) {
    opl_md_key_off(ch, key);
    return;
  }
  ch &= 15u;
  channel_t *channel = &s_channels[ch];
  const genmidi_instr_t *instr;
  unsigned note = key;
  if (ch == OPL_MD_PERCUSSION) {
    if (key < 35 || key > 81) return;
    instr = instrument(GENMIDI_MAIN + key - 35u);
    note = 60;
  } else {
    instr = channel->instrument;
  }
  if (!instr) return; /* not in the song's list */
  if (s_nfree == 0) replace_existing_voice();
  voice_key_on(channel, instr, 0, note, key, volume);
  if (instr->flags & GENMIDI_FLAG_2VOICE) voice_key_on(channel, instr, 1, note, key, volume);
}

void opl_md_program(unsigned ch, unsigned program) {
  s_channels[ch & 15u].instrument = instrument(program & 127u);
}

void opl_md_volume(unsigned ch, unsigned volume) {
  set_channel_volume(&s_channels[ch & 15u], volume > 127u ? 127u : volume, true);
}

void opl_md_channel_off(unsigned ch) {
  channel_t *channel = &s_channels[ch & 15u];
  for (int i = 0; i < s_nalloced; i++) {
    if (s_alloced[i]->channel == channel) {
      release_voice(i);
      i--;
    }
  }
}

/* Only the most significant 7 bits count, as in Doom. The voices bent
 * move to the end of the list, so they are the last to be stolen. */
void opl_md_bend(unsigned ch, unsigned mus_bend) {
  channel_t *channel = &s_channels[ch & 15u];
  channel->bend = (int8_t)((int)(mus_bend >> 1) - 64);
  voice_t *updated[OPL_NUM_VOICES], *not_updated[OPL_NUM_VOICES];
  int nu = 0, nn = 0;
  for (int i = 0; i < s_nalloced; i++) {
    if (s_alloced[i]->channel == channel) {
      update_voice_frequency(s_alloced[i]);
      updated[nu++] = s_alloced[i];
    } else {
      not_updated[nn++] = s_alloced[i];
    }
  }
  for (int i = 0; i < nn; i++) s_alloced[i] = not_updated[i];
  for (int i = 0; i < nu; i++) s_alloced[nn + i] = updated[i];
}

static void init_channel(channel_t *channel) {
  channel->instrument = instrument(0);
  channel->volume_base = 100;
  channel->volume = (uint8_t)(s_music_volume < 100u ? s_music_volume : 100u);
  channel->bend = 0;
}

void opl_md_restart(void) {
  for (unsigned i = 0; i < MUS_CHANNELS; i++) init_channel(&s_channels[i]);
}

void opl_md_all_off(void) {
  while (s_nalloced) release_voice(0);
}

void opl_md_start(void) {
  opl_md_all_off();
  /* The instruments were reloaded: make every voice load its own again. */
  for (int i = 0; i < OPL_NUM_VOICES; i++) s_voices[i].instr = NULL;
  s_start_volume = s_music_volume;
  opl_md_restart();
}

/* As DMX: melodic voices off, percussion left to ring. */
void opl_md_pause(void) {
  for (int i = 0; i < OPL_NUM_VOICES; i++) {
    if (s_voices[i].channel && s_voices[i].channel != &s_channels[OPL_MD_PERCUSSION]) {
      voice_key_off(&s_voices[i]);
    }
  }
}

bool opl_md_voices_busy(void) { return s_nalloced != 0; }

/* As Doom: every operator's level at the minimum, and (including some
 * registers that do not exist) the rest cleared. */
static void init_registers(void) {
  for (unsigned r = OPL_REGS_LEVEL; r <= OPL_REGS_LEVEL + OPL_NUM_OPERATORS; ++r) write_reg(r, 0x3f);
  for (unsigned r = OPL_REGS_ATTACK; r <= OPL_REGS_WAVEFORM + OPL_NUM_OPERATORS; ++r) write_reg(r, 0);
  for (unsigned r = 1; r < OPL_REGS_LEVEL; ++r) write_reg(r, 0);
  write_reg(OPL_REG_TIMER_CTRL, 0x60);
  write_reg(OPL_REG_TIMER_CTRL, 0x80);
  write_reg(OPL_REG_WAVEFORM_ENABLE, 0x20);
}

bool opl_md_init(void) {
  if (!s_opl) s_opl = OPL_new(3579545, OPL_MD_RATE);
  if (!s_opl) return false;
  init_registers();
  init_voices();
  opl_md_clear_instruments();
  opl_md_restart();
  return true;
}

/* The chip's mono output, n samples. On the RP2040 it runs the
 * interpolators of the core it is on, and on Core 1 this interrupts the
 * renderer, which uses them too: theirs are put back afterwards. */
void opl_md_render(int32_t *buf, uint32_t n) {
#if PICO_ON_DEVICE
  interp_hw_save_t save0, save1;
  interp_save(interp0, &save0);
  interp_save(interp1, &save1);
#endif
  OPL_calc_buffer_linear(s_opl, buf, n);
#if PICO_ON_DEVICE
  interp_restore(interp0, &save0);
  interp_restore(interp1, &save1);
#endif
}
