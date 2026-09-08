/*
 * Copyright (C) 2026 Neil Rackett
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * File: doom_video.c
 * Description: 256 -> 16 colour reduction with ordered dither, fused
 *              with the chunky-to-planar conversion. See doom_video.h.
 *
 * Two halves:
 *
 *  1. Palette work, done whenever the PLAYPAL page or a mode changes:
 *     pick 16 reference colours, snap them to what the ST can display,
 *     and build s_lut[16][256] -- for each of the 16 dither cells, the
 *     pen every PLAYPAL index maps to. A dithered index is drawn as its
 *     nearest reference on some cells and its second-nearest on the
 *     rest, the split decided by where the colour falls on the line
 *     between the two (t in 0..16) against the cell's threshold. The
 *     fixed retro palettes use a different pairing (see "vivid" below):
 *     a pen chosen by hue alone, shaded against black by brightness.
 *
 *  2. Per frame: doom_video_publish() walks fb_chunked_buffer 16 pixels
 *     at a time, maps each pixel through the LUT for its cell, and packs
 *     the four plane words with the same multiply transpose the
 *     framework's asm worker uses. The bottom half of the screen runs on
 *     Core 1 through fb_core1_dispatch, straight into the cart FB in the
 *     m68k's post-blit slack (there is no planar scratch buffer).
 */

#include "doom_video.h"

#include <stdlib.h>
#include <string.h>

#include "audio.h"
#include "bluenoise.h" /* 32x32 threshold tile, 16 levels, in flash */
#include "cart_shared.h"
#include "debug.h"
#include "fb.h"
#include "fb_chunked.h"
#include "palette.h"
#include "pico/stdlib.h"
#include "pico/time.h"

/* Per-file -O3: the per-frame LUT + c2p pass is the one hot loop here. */
#pragma GCC optimize("O3")

/* ------------------------------------------------------------------ */
/* State                                                               */

static uint8_t s_playpal[768];   /* last installed page, RGB           */
static bool s_have_playpal;
static doom_video_palette_t s_pal_mode = DOOM_VIDEO_PAL_SUBSET;
static doom_video_dither_t s_dither = DOOM_VIDEO_DITHER_BAYER4;

static uint8_t s_ref_rgb[16][3];  /* the 16 colours as the ST shows them */
static int s_ref_count = 16;      /* fewer for the 4-colour CGA palette   */
static bool s_vivid;              /* fixed palette: hue-first matching   */
static uint16_t s_st_colors[16];  /* the same, as ST palette words       */
static uint8_t s_nearest[256];    /* PLAYPAL index -> nearest pen        */

/* [cell][index] -> pen, cell = ((y & 3) << 2) | (x & 3). 4 KB. */
static uint8_t s_lut[16][256] __attribute__((aligned(4)));

/* For the blue-noise mode, which cannot be a per-cell LUT (1024 cells):
 * per index, nearest pen (bits 0..3), second pen (bits 4..7) and the
 * 0..16 mix level (bits 8..12); a pixel takes the second pen where the
 * level beats the tile's threshold. */
static uint16_t s_pair[256];

static uint32_t s_convert_us;

/* STDOOM's hand-picked 16 PLAYPAL indices (Jonas Eschenburg): black, two
 * blues, two greens, dark red, greys, flesh, yellow, browns, bright red,
 * olive. Refined against the shareware WAD over a long time; it holds
 * up well on flesh tones, the sky and the status bar. */
static const uint8_t s_subset[16] = {0,   201, 205, 117, 123, 185, 87,  100,
                                     60,  95,  227, 104, 69,  79,  179, 159};

/* Fixed palettes, RGB 0..255, snapped to the ST hardware palette. The 16
 * ST colours become the famous palette and every PLAYPAL colour maps to
 * its nearest two. From the DOOM Accelerator, plus CGA. */
static const uint8_t s_pal_ega[16][3] = {
    {0, 0, 0},     {0, 0, 170},     {0, 170, 0},     {0, 170, 170},
    {170, 0, 0},   {170, 0, 170},   {170, 85, 0},    {170, 170, 170},
    {85, 85, 85},  {85, 85, 255},   {85, 255, 85},   {85, 255, 255},
    {255, 85, 85}, {255, 85, 255},  {255, 255, 85},  {255, 255, 255}};
static const uint8_t s_pal_cga[4][3] = { /* palette 1, high intensity */
    {0, 0, 0}, {85, 255, 255}, {255, 85, 255}, {255, 255, 255}};
static const uint8_t s_pal_c64[16][3] = { /* Pepto reconstruction */
    {0, 0, 0},       {255, 255, 255}, {104, 55, 43},   {112, 164, 178},
    {111, 61, 134},  {88, 141, 67},   {53, 40, 121},   {184, 199, 111},
    {111, 79, 37},   {67, 57, 0},     {154, 103, 89},  {68, 68, 68},
    {108, 108, 108}, {154, 210, 132}, {108, 94, 181},  {149, 149, 149}};
static const uint8_t s_pal_zx[16][3] = { /* non-bright 0xD7, bright 0xFF */
    {0, 0, 0},       {0, 0, 215},     {215, 0, 0},     {215, 0, 215},
    {0, 215, 0},     {0, 215, 215},   {215, 215, 0},   {215, 215, 215},
    {0, 0, 0},       {0, 0, 255},     {255, 0, 0},     {255, 0, 255},
    {0, 255, 0},     {0, 255, 255},   {255, 255, 0},   {255, 255, 255}};
static const uint8_t s_pal_pico8[16][3] = {
    {0, 0, 0},       {29, 43, 83},    {126, 37, 83},   {0, 135, 81},
    {171, 82, 54},   {95, 87, 79},    {194, 195, 199}, {255, 241, 232},
    {255, 0, 77},    {255, 163, 0},   {255, 236, 39},  {0, 228, 54},
    {41, 173, 255},  {131, 118, 156}, {255, 119, 168}, {255, 204, 170}};

/* ------------------------------------------------------------------ */
/* ST colour helpers                                                   */

/* 8-bit channel -> ST/STE palette nibble. The ST's own 3 bits are the
 * top three of the value; the STE's fourth, finer bit sits in bit 3 of
 * the nibble, which a plain ST ignores. */
static uint16_t channel_nibble(uint8_t v) {
  uint16_t r = (uint16_t)((v & 0xE0u) >> 5);
  r |= (uint16_t)((v & 0x10u) >> 1);
  return r;
}

static uint16_t st_color_word(uint8_t r, uint8_t g, uint8_t b) {
  return (uint16_t)((channel_nibble(r) << 8) | (channel_nibble(g) << 4) |
                    channel_nibble(b));
}

/* What the STE actually displays for a channel value: the top nibble,
 * spread back to 0..255. (Not channel_nibble(v) * 17 -- the STE's
 * nibble bit order is not the value's.) */
static uint8_t displayed_channel(uint8_t v) {
  return (uint8_t)((v >> 4) * 17u);
}

/* Perceptual distance, the "redmean" approximation: green-weighted and
 * red-adjusted so skies and browns pick sensible neighbours. */
static long color_dist(int r1, int g1, int b1, int r2, int g2, int b2) {
  long rmean = ((long)r1 + (long)r2) / 2;
  long dr = r1 - r2, dg = g1 - g2, db = b1 - b2;
  return (((512 + rmean) * dr * dr) >> 8) + 4 * dg * dg +
         (((767 - rmean) * db * db) >> 8);
}

static void two_nearest_ref(const uint8_t *c, uint8_t *out_a,
                            uint8_t *out_b) {
  long best = 0x7FFFFFFFL, best2 = 0x7FFFFFFFL;
  uint8_t a = 0, b = 0;
  for (uint8_t k = 0; k < (uint8_t)s_ref_count; k++) {
    long d = color_dist(c[0], c[1], c[2], s_ref_rgb[k][0], s_ref_rgb[k][1],
                        s_ref_rgb[k][2]);
    if (d < best) {
      best2 = best;
      b = a;
      best = d;
      a = k;
    } else if (d < best2) {
      best2 = d;
      b = k;
    }
  }
  *out_a = a;
  *out_b = b;
}

/* ------------------------------------------------------------------ */
/* "Vivid" matching for the fixed palettes                             */

/* Doom's PLAYPAL is mostly dark, desaturated browns and greys, and the
 * light-diminishing colormaps push the visible pixels darker still.
 * Matched by plain distance against a set of saturated primaries,
 * nearly everything lands on black with a sprinkle of colour, which is
 * not what anyone picking the ZX Spectrum palette wanted. So the fixed
 * palettes match differently:
 *
 *  - the pen is chosen by hue alone (the colour and each pen scaled to
 *    full brightness before comparing), so a dark brown wall is still a
 *    brown/yellow wall;
 *  - the colour's brightness is stretched (gamma 0.75; 0.5 turned the
 *    walls into white dither) so Doom's mid-tones reach further up a
 *    pen instead of sitting at a quarter of one;
 *  - the dither then runs along a brightness ladder: black, plus every
 *    pen of the chosen hue (EGA's dark/bright pairs, C64's greys), and
 *    the colour dithers between the two rungs bracketing its brightness.
 *
 * All of it happens in the LUT rebuild; the per-pixel path is unchanged. */

static int luma(const uint8_t *c) {
  return (c[0] * 77 + c[1] * 151 + c[2] * 28) >> 8;
}

/* Scale a colour so its brightest channel is 255. Black has no hue;
 * treat it as neutral so it pairs with the greys. */
static void chroma_of(const uint8_t *c, int *out) {
  int m = c[0];
  if (c[1] > m) m = c[1];
  if (c[2] > m) m = c[2];
  for (int k = 0; k < 3; k++) out[k] = m ? (c[k] * 255) / m : 255;
}

static int chroma_dist(const int *p, const int *q) {
  int dr = p[0] - q[0], dg = p[1] - q[1], db = p[2] - q[2];
  return dr * dr + dg * dg + db * db;
}

static int isqrt16(int n) {
  int r = 0;
  for (int bit = 1 << 8; bit; bit >>= 1) {
    if ((r + bit) * (r + bit) <= n) r += bit;
  }
  return r;
}

#define VIVID_BLACK_LUMA 24     /* pens darker than this are "black"    */
#define VIVID_LADDER_TOL 16000  /* chroma distance to share a ladder    */

/* Pick the two pens and the mix level for colour `c`. */
static void vivid_pair(const uint8_t *c, uint8_t *out_a, uint8_t *out_b,
                       int *out_t16) {
  int ref_y[16], ref_ch[16][3], ch[3];
  for (int k = 0; k < s_ref_count; k++) {
    ref_y[k] = luma(s_ref_rgb[k]);
    chroma_of(s_ref_rgb[k], ref_ch[k]);
  }
  chroma_of(c, ch);

  /* The hue pen: nearest chroma among the pens that are not black. */
  int h = -1, best = 0x7FFFFFFF;
  for (int k = 0; k < s_ref_count; k++) {
    if (ref_y[k] < VIVID_BLACK_LUMA) continue;
    int d = chroma_dist(ch, ref_ch[k]);
    if (d < best) {
      best = d;
      h = k;
    }
  }
  if (h < 0) h = 0;

  /* Stretched brightness, y = 255 * (Y/255)^0.75 as two square roots,
   * then the ladder rungs bracketing it. */
  int y = isqrt16(luma(c) * 255);
  y = (y * isqrt16(y * 255)) / 255;
  int a = -1, b = -1;
  for (int k = 0; k < s_ref_count; k++) {
    if (ref_y[k] >= VIVID_BLACK_LUMA &&
        chroma_dist(ref_ch[k], ref_ch[h]) > VIVID_LADDER_TOL)
      continue;
    if (ref_y[k] <= y) {
      if (a < 0 || ref_y[k] > ref_y[a]) a = k;
    } else {
      if (b < 0 || ref_y[k] < ref_y[b]) b = k;
    }
  }
  if (a < 0) a = b;
  if (b < 0) b = a;
  *out_a = (uint8_t)a;
  *out_b = (uint8_t)b;
  *out_t16 = (a == b) ? 0 : ((y - ref_y[a]) * 16) / (ref_y[b] - ref_y[a]);
}

/* ------------------------------------------------------------------ */
/* Generated palette: median cut + k-means over the 256 colours        */

static uint8_t s_palgen_idx[256];
static int s_palgen_axis;

static int palgen_cmp(const void *pa, const void *pb) {
  uint8_t a = *(const uint8_t *)pa;
  uint8_t b = *(const uint8_t *)pb;
  int va = s_playpal[(uint32_t)a * 3u + (uint32_t)s_palgen_axis];
  int vb = s_playpal[(uint32_t)b * 3u + (uint32_t)s_palgen_axis];
  return va - vb;
}

static void median_cut(uint8_t out_rgb[16][3]) {
  static const int axis_w[3] = {2, 4, 3}; /* favour the axes the eye resolves */
  uint16_t box_start[16], box_count[16];
  uint8_t nboxes = 1;

  for (int i = 0; i < 256; i++) s_palgen_idx[i] = (uint8_t)i;
  box_start[0] = 0;
  box_count[0] = 256;

  while (nboxes < 16u) {
    int best = -1, best_axis = 0;
    long best_score = -1;
    for (int b = 0; b < nboxes; b++) {
      int mn[3] = {255, 255, 255}, mx[3] = {0, 0, 0};
      if (box_count[b] < 2u) continue;
      for (uint16_t k = 0; k < box_count[b]; k++) {
        const uint8_t *c = &s_playpal[(uint32_t)s_palgen_idx[box_start[b] + k] * 3u];
        for (int ch = 0; ch < 3; ch++) {
          if (c[ch] < mn[ch]) mn[ch] = c[ch];
          if (c[ch] > mx[ch]) mx[ch] = c[ch];
        }
      }
      long arange = -1;
      int axis = 0;
      for (int ch = 0; ch < 3; ch++) {
        long r = (long)(mx[ch] - mn[ch]) * axis_w[ch];
        if (r > arange) {
          arange = r;
          axis = ch;
        }
      }
      if (arange > best_score) {
        best_score = arange;
        best = b;
        best_axis = axis;
      }
    }
    if (best < 0) break;
    s_palgen_axis = best_axis;
    qsort(&s_palgen_idx[box_start[best]], box_count[best], 1, palgen_cmp);
    uint16_t half = (uint16_t)(box_count[best] / 2u);
    box_start[nboxes] = (uint16_t)(box_start[best] + half);
    box_count[nboxes] = (uint16_t)(box_count[best] - half);
    box_count[best] = half;
    nboxes++;
  }

  for (int b = 0; b < 16; b++) {
    out_rgb[b][0] = out_rgb[b][1] = out_rgb[b][2] = 0;
    if (b < nboxes && box_count[b] > 0u) {
      uint32_t s[3] = {0, 0, 0};
      for (uint16_t k = 0; k < box_count[b]; k++) {
        const uint8_t *c = &s_playpal[(uint32_t)s_palgen_idx[box_start[b] + k] * 3u];
        s[0] += c[0];
        s[1] += c[1];
        s[2] += c[2];
      }
      for (int ch = 0; ch < 3; ch++) out_rgb[b][ch] = (uint8_t)(s[ch] / box_count[b]);
    }
  }
}

#define KMEANS_ITERS 8
static void kmeans(uint8_t cen[16][3]) {
  for (int it = 0; it < KMEANS_ITERS; it++) {
    uint32_t sum[16][3] = {{0}};
    uint32_t count[16] = {0};
    for (int i = 0; i < 256; i++) {
      const uint8_t *c = &s_playpal[i * 3];
      long best = 0x7FFFFFFFL;
      int bk = 0;
      for (int k = 0; k < 16; k++) {
        long d = color_dist(c[0], c[1], c[2], cen[k][0], cen[k][1], cen[k][2]);
        if (d < best) {
          best = d;
          bk = k;
        }
      }
      sum[bk][0] += c[0];
      sum[bk][1] += c[1];
      sum[bk][2] += c[2];
      count[bk]++;
    }
    for (int k = 0; k < 16; k++) {
      if (count[k]) {
        for (int ch = 0; ch < 3; ch++) cen[k][ch] = (uint8_t)(sum[k][ch] / count[k]);
      }
    }
  }
}

/* ------------------------------------------------------------------ */
/* Dither thresholds                                                   */

static const uint8_t s_bayer4[16] = {0, 8,  2,  10, 12, 4,  14, 6,
                                     3, 11, 1,  9,  15, 7,  13, 5};
static const uint8_t s_bayer2[4] = {2, 10, 14, 6}; /* scaled to 0..15 */
static const uint8_t s_halftone[16] = {6,  7,  8,  9,  5,  0,  1,  10,
                                       4,  3,  2,  11, 15, 14, 13, 12};

static uint8_t dither_threshold(doom_video_dither_t mode, uint8_t cell) {
  switch (mode) {
    case DOOM_VIDEO_DITHER_BAYER2:
      return s_bayer2[(((cell >> 2) & 1u) << 1) | (cell & 1u)];
    case DOOM_VIDEO_DITHER_HALFTONE:
      return s_halftone[cell & 15u];
    default:
      return s_bayer4[cell & 15u];
  }
}

/* ------------------------------------------------------------------ */
/* Palette + LUT build                                                 */

static void set_refs_from_rgb(const uint8_t (*src)[3], int count) {
  s_ref_count = count;
  for (uint8_t k = 0; k < 16u; k++) {
    /* Pens past `count` are never chosen by the LUT; park them on the
     * first colour so the ST palette slot holds nothing surprising. */
    const uint8_t *c = src[k < count ? k : 0];
    s_st_colors[k] = st_color_word(c[0], c[1], c[2]);
    s_ref_rgb[k][0] = displayed_channel(c[0]);
    s_ref_rgb[k][1] = displayed_channel(c[1]);
    s_ref_rgb[k][2] = displayed_channel(c[2]);
  }
}

static void build_refs(void) {
  uint8_t pal[16][3];
  s_vivid = false;
  switch (s_pal_mode) {
    case DOOM_VIDEO_PAL_GENERATED:
      median_cut(pal);
      kmeans(pal);
      break;
    case DOOM_VIDEO_PAL_GREY:
      for (int k = 0; k < 16; k++) pal[k][0] = pal[k][1] = pal[k][2] = (uint8_t)(k * 17);
      break;
    case DOOM_VIDEO_PAL_EGA: set_refs_from_rgb(s_pal_ega, 16); s_vivid = true; return;
    case DOOM_VIDEO_PAL_CGA: set_refs_from_rgb(s_pal_cga, 4); s_vivid = true; return;
    case DOOM_VIDEO_PAL_C64: set_refs_from_rgb(s_pal_c64, 16); s_vivid = true; return;
    case DOOM_VIDEO_PAL_ZX: set_refs_from_rgb(s_pal_zx, 16); s_vivid = true; return;
    case DOOM_VIDEO_PAL_PICO8: set_refs_from_rgb(s_pal_pico8, 16); s_vivid = true; return;
    default:
      for (int k = 0; k < 16; k++) {
        const uint8_t *c = &s_playpal[(uint32_t)s_subset[k] * 3u];
        pal[k][0] = c[0];
        pal[k][1] = c[1];
        pal[k][2] = c[2];
      }
      break;
  }
  set_refs_from_rgb(pal, 16);
}

static void rebuild(void) {
  if (!s_have_playpal) return;
  build_refs();

  for (uint32_t i = 0; i < 256u; i++) {
    const uint8_t *c = &s_playpal[i * 3u];
    uint8_t a, b, nearest;
    int t16;
    if (s_vivid) {
      vivid_pair(c, &a, &b, &t16);
      nearest = (t16 >= 8) ? b : a;
    } else {
      two_nearest_ref(c, &a, &b);
      nearest = a;

      /* Project the colour onto the a->b line: t in 0..16. */
      int dr = (int)s_ref_rgb[b][0] - (int)s_ref_rgb[a][0];
      int dg = (int)s_ref_rgb[b][1] - (int)s_ref_rgb[a][1];
      int db = (int)s_ref_rgb[b][2] - (int)s_ref_rgb[a][2];
      int denom = dr * dr + dg * dg + db * db;
      int dot = ((int)c[0] - (int)s_ref_rgb[a][0]) * dr +
                ((int)c[1] - (int)s_ref_rgb[a][1]) * dg +
                ((int)c[2] - (int)s_ref_rgb[a][2]) * db;
      if (denom <= 0 || dot <= 0) {
        t16 = 0;
      } else if (dot >= denom) {
        t16 = 16;
      } else {
        t16 = (dot * 16) / denom;
      }
    }
    s_nearest[i] = nearest;

    if (s_dither == DOOM_VIDEO_DITHER_NEAREST) {
      for (uint8_t cell = 0; cell < 16u; cell++) s_lut[cell][i] = nearest;
      s_pair[i] = (uint16_t)(nearest | (nearest << 4));
      continue;
    }

    for (uint8_t cell = 0; cell < 16u; cell++) {
      s_lut[cell][i] = (t16 > (int)dither_threshold(s_dither, cell)) ? b : a;
    }
    s_pair[i] = (uint16_t)(a | (b << 4) | (t16 << 8));
  }

  palette_set(s_st_colors);
}

/* ------------------------------------------------------------------ */
/* Public palette API                                                  */

void doom_video_init(void) {
  s_have_playpal = false;
  s_pal_mode = DOOM_VIDEO_PAL_SUBSET;
  s_dither = DOOM_VIDEO_DITHER_BAYER4;
  s_convert_us = 0;
  memset(s_lut, 0, sizeof(s_lut));
  memset(s_nearest, 0, sizeof(s_nearest));
}

void doom_video_set_playpal(const uint8_t *rgb768) {
  memcpy(s_playpal, rgb768, sizeof(s_playpal));
  s_have_playpal = true;
  rebuild();
}

void doom_video_set_palette_mode(doom_video_palette_t mode) {
  if (mode >= DOOM_VIDEO_PAL_COUNT) return;
  s_pal_mode = mode;
  rebuild();
}

void doom_video_set_dither(doom_video_dither_t mode) {
  if (mode >= DOOM_VIDEO_DITHER_COUNT) return;
  s_dither = mode;
  rebuild();
}

doom_video_palette_t doom_video_get_palette_mode(void) { return s_pal_mode; }
doom_video_dither_t doom_video_get_dither(void) { return s_dither; }

const char *doom_video_palette_name(doom_video_palette_t mode) {
  switch (mode) {
    case DOOM_VIDEO_PAL_SUBSET: return "STDOOM 16";
    case DOOM_VIDEO_PAL_GENERATED: return "GENERATED";
    case DOOM_VIDEO_PAL_GREY: return "GREYSCALE";
    case DOOM_VIDEO_PAL_EGA: return "EGA";
    case DOOM_VIDEO_PAL_CGA: return "CGA";
    case DOOM_VIDEO_PAL_C64: return "C64";
    case DOOM_VIDEO_PAL_ZX: return "ZX SPECTRUM";
    case DOOM_VIDEO_PAL_PICO8: return "PICO-8";
    default: return "?";
  }
}

const char *doom_video_dither_name(doom_video_dither_t mode) {
  switch (mode) {
    case DOOM_VIDEO_DITHER_NEAREST: return "NEAREST";
    case DOOM_VIDEO_DITHER_BAYER2: return "BAYER 2X2";
    case DOOM_VIDEO_DITHER_BAYER4: return "BAYER 4X4";
    case DOOM_VIDEO_DITHER_HALFTONE: return "HALFTONE";
    case DOOM_VIDEO_DITHER_BLUENOISE: return "BLUE NOISE";
    default: return "?";
  }
}

const uint16_t *doom_video_st_palette(void) { return s_st_colors; }

uint8_t doom_video_nearest_pen(uint8_t idx) { return s_nearest[idx]; }

/* ------------------------------------------------------------------ */
/* Fused LUT + chunky-to-planar                                        */

/* Convert 96-pixel chunks [k0, k1) of fb_chunked_buffer straight into the
 * cart FB at their chunk-reversed positions (see fb_cart_offset), mapping
 * every byte through the LUT for its dither cell on the way.
 *
 * For each 16-pixel block: four groups of four pixels, each mapped
 * through the LUT for its column's cell into a uint32 q (pixel 0 in the
 * low byte), then plane K's nibble for the group is
 *     ((q >> K) & 0x01010101) * 0x80402010 >> 28
 * with pixel 0 landing in the nibble's MSB, the ST's leftmost-is-MSB
 * convention. Group g's nibble goes to bits 12 - 4g of plane word K.
 * The four plane words are stored as two little-endian uint32s, plane
 * 0 in the low half of the first, so the cart-bus word swap presents
 * them to the m68k as four big-endian words in plane order -- the same
 * layout fb_c2p_half emits. */
static uint8_t *s_cart_fb;

static inline void __not_in_flash_func(doom_c2p_block)(uint32_t *dst, const uint8_t *src,
                                                       unsigned y) {
  const uint8_t *l0 = s_lut[((y & 3u) << 2) | 0u];
  const uint8_t *l1 = s_lut[((y & 3u) << 2) | 1u];
  const uint8_t *l2 = s_lut[((y & 3u) << 2) | 2u];
  const uint8_t *l3 = s_lut[((y & 3u) << 2) | 3u];
  uint32_t p01 = 0, p23 = 0; /* (plane1 << 16) | plane0, (plane3 << 16) | plane2 */
  for (unsigned g = 0; g < 4u; g++) {
    uint32_t q = (uint32_t)l0[src[0]] | ((uint32_t)l1[src[1]] << 8) |
                 ((uint32_t)l2[src[2]] << 16) | ((uint32_t)l3[src[3]] << 24);
    src += 4;
    const unsigned sh = 12u - 4u * g;
    uint32_t n0 = (((q >> 0) & 0x01010101u) * 0x80402010u) >> 28;
    uint32_t n1 = (((q >> 1) & 0x01010101u) * 0x80402010u) >> 28;
    uint32_t n2 = (((q >> 2) & 0x01010101u) * 0x80402010u) >> 28;
    uint32_t n3 = (((q >> 3) & 0x01010101u) * 0x80402010u) >> 28;
    p01 |= (n0 << sh) | (n1 << (sh + 16u));
    p23 |= (n2 << sh) | (n3 << (sh + 16u));
  }
  dst[0] = p01;
  dst[1] = p23;
}

/* Blue-noise variant: pens chosen per pixel against the 32x32 tile.
 * `x` is the block's column (a multiple of 16), so the tile row slice
 * for these 16 pixels is contiguous: (x & 31) is 0 or 16. */
static inline void __not_in_flash_func(doom_c2p_block_bn)(uint32_t *dst, const uint8_t *src,
                                                          unsigned x, unsigned y) {
  const uint8_t *nz = &bluenoise[(y & (BLUENOISE_N - 1u)) * BLUENOISE_N + (x & (BLUENOISE_N - 1u))];
  uint32_t p01 = 0, p23 = 0;
  for (unsigned g = 0; g < 4u; g++) {
    uint32_t q = 0;
    for (unsigned i = 0; i < 4u; i++) {
      const uint16_t v = s_pair[src[i]];
      const uint32_t pen = ((v >> 8) > nz[i]) ? ((v >> 4) & 15u) : (v & 15u);
      q |= pen << (8u * i);
    }
    src += 4;
    nz += 4;
    const unsigned sh = 12u - 4u * g;
    uint32_t n0 = (((q >> 0) & 0x01010101u) * 0x80402010u) >> 28;
    uint32_t n1 = (((q >> 1) & 0x01010101u) * 0x80402010u) >> 28;
    uint32_t n2 = (((q >> 2) & 0x01010101u) * 0x80402010u) >> 28;
    uint32_t n3 = (((q >> 3) & 0x01010101u) * 0x80402010u) >> 28;
    p01 |= (n0 << sh) | (n1 << (sh + 16u));
    p23 |= (n2 << sh) | (n3 << (sh + 16u));
  }
  dst[0] = p01;
  dst[1] = p23;
}

static bool s_use_bn; /* latched per frame from s_dither */

static inline void __not_in_flash_func(doom_c2p_block_any)(uint32_t *dst, const uint8_t *src,
                                                           unsigned pix) {
  if (s_use_bn) {
    doom_c2p_block_bn(dst, src, pix % FB_CHUNKED_W, pix / FB_CHUNKED_W);
  } else {
    doom_c2p_block(dst, src, pix / FB_CHUNKED_W);
  }
}

/* Chunk k: 96 pixels = 6 blocks; block b of chunk k starts at pixel
 * k*96 + b*16, on row (that / 320). Its 8 planar bytes land at
 * fb_cart_offset(k*48 + b*8). */
static void __not_in_flash_func(doom_c2p_chunks)(unsigned k0, unsigned k1) {
  for (unsigned k = k0; k < k1; k++) {
    const unsigned pix0 = k * 96u;
    const uint32_t cart0 = fb_cart_offset(k * (uint32_t)CART_FB_CHUNK_BYTES);
    for (unsigned b = 0; b < 6u; b++) {
      const unsigned pix = pix0 + b * 16u;
      doom_c2p_block_any((uint32_t *)(s_cart_fb + cart0 + b * 8u),
                         fb_chunked_buffer + pix, pix);
    }
  }
}

static void __not_in_flash_func(doom_c2p_bottom_job)(void *arg) {
  (void)arg;
  doom_c2p_chunks(CART_FB_CHUNK_COUNT / 2u, CART_FB_CHUNK_COUNT);
}

void doom_video_publish(void) {
  s_cart_fb = (uint8_t *)fb_screen.framebuffer;
  s_use_bn = (s_dither == DOOM_VIDEO_DITHER_BLUENOISE);

  /* The cart FB is written in place, so only in the m68k's post-blit
   * slack: wait for its ack, convert on both cores, mark ready. */
  fb_wait_blit_ack();
  const uint32_t t0 = time_us_32();

  fb_core1_dispatch(doom_c2p_bottom_job, NULL);
  doom_c2p_chunks(0u, CART_FB_CHUNK_COUNT / 2u);
  /* Tail: the last 64 pixels, natural order at the end of the FB. */
  for (unsigned b = 0; b < CART_FB_CHUNK_TAIL / 8u; b++) {
    const unsigned pix = 2u * CART_FB_CHUNK_COVERED + b * 16u;
    doom_c2p_block_any((uint32_t *)(s_cart_fb + CART_FB_CHUNK_COVERED + b * 8u),
                       fb_chunked_buffer + pix, pix);
  }
  fb_core1_wait();

  s_convert_us = time_us_32() - t0;
  fb_frame_done();

#if defined(_DEBUG) && (_DEBUG != 0)
  /* Every 64 frames: the conversion time (must stay inside the m68k's
   * ~3 ms post-blit slack), the longest frame-to-frame interval, the
   * longest wait for the m68k's ack, and the audio interrupt's worst
   * cases -- enough to see where a stall comes from. */
  static uint32_t s_frames, s_last_us, s_frame_max_us;
  const uint32_t now = time_us_32();
  if (s_last_us && now - s_last_us > s_frame_max_us) s_frame_max_us = now - s_last_us;
  s_last_us = now;
  if ((++s_frames & 63u) == 0) {
    uint32_t fill_max, fills, cb_max, cbs;
    audio_debug_stats(&fill_max, &fills, &cb_max, &cbs);
    extern void md_prof_report(char *buf, int len);
    extern volatile uint32_t md_sound_dbg_starts, md_sound_dbg_max_playing;
    char prof[96];
    md_prof_report(prof, sizeof(prof));
    DPRINTF("c2p %s %lu us | frame max %lu ms | ack wait max %lu us | audio core %d mode %d len %lu: cbs %lu, fills %lu, fill max %lu us, cb max %lu us, starts %lu, max playing %lu | max ms: %s\n",
            doom_video_dither_name(s_dither), (unsigned long)s_convert_us,
            (unsigned long)(s_frame_max_us / 1000u), (unsigned long)fb_debug_wait_max_us(),
            audio_vbl_timer_core(), (int)audio_get_mode(), (unsigned long)audio_get_fill_bytes(), (unsigned long)cbs,
            (unsigned long)fills, (unsigned long)fill_max, (unsigned long)cb_max,
            (unsigned long)md_sound_dbg_starts, (unsigned long)md_sound_dbg_max_playing, prof);
    md_sound_dbg_starts = md_sound_dbg_max_playing = 0;
    s_frame_max_us = 0;
  }
#endif
}

uint32_t doom_video_last_convert_us(void) { return s_convert_us; }

/* ------------------------------------------------------------------ */
/* Melt wipe                                                           */

/* Size, not speed: at the file's -O3 the compose below unrolled to 8 KB
 * of RAM-resident code and the debug build fell out of flash. A step
 * only touches the band of rows that are moving, so -Os is plenty. */
#pragma GCC push_options
#pragma GCC optimize("Os")

/* Doom's screen melt, done in place on the cart FB. There is no second
 * frame buffer to hold the picture being melted away, but the m68k only
 * ever reads the cart FB, so the last published frame is still sitting
 * there in planar form: that is the old picture. The new one is the
 * frame in fb_chunked_buffer, which nothing touches while the melt runs.
 *
 * Each step advances the 160 two-pixel columns by Doom's rules
 * (f_wipe.c's wipe_doMelt) and rewrites every plane word bottom-up. A
 * column that has scrolled past a row shows the new frame there, through
 * the usual LUT + c2p; otherwise its bits come from the same column
 * `delta` rows up in the cart FB, which bottom-up order guarantees still
 * hold the previous step's picture. Columns that moved by the same
 * amount are shifted together in one tight pass, so a step is a few
 * passes of two loads and two stores per row rather than a per-row sort. The old pixels keep the dither they
 * were given at their original rows, as a moving picture should. The two
 * cores take the left and right halves of the screen, so neither reads
 * a word the other writes. Rows nothing has moved in are left alone:
 * the untouched old picture early in the melt, and the fully revealed
 * new one late in it, so a step's work is the band in between (~1 ms;
 * rewriting everything measured 3.9 ms, past the m68k's slack). */

#define WIPE_COLS 160 /* two-pixel columns, as in the original */

static int16_t s_wipe_y[WIPE_COLS] __cart_app_free("wipe");   /* Doom's y[]: < 0 waiting, 200 done */
static int16_t s_wipe_off[WIPE_COLS] __cart_app_free("wipe"); /* offset drawn by the last step  */
static uint32_t s_wipe_rand;
static uint32_t s_wipe_max_us;

static uint32_t wipe_random(void) {
  s_wipe_rand ^= s_wipe_rand << 13;
  s_wipe_rand ^= s_wipe_rand >> 17;
  s_wipe_rand ^= s_wipe_rand << 5;
  return s_wipe_rand;
}

/* fb_cart_offset(y * 160 + g * 8) for row y, 16-pixel group g, without
 * the divide: (y*160)/48 is (y*10)/3, and the group's 8 bytes never
 * straddle a chunk. */
static inline uint32_t wipe_cart_off(unsigned y, unsigned g) {
  const uint32_t o = y * 160u + g * 8u;
  if (o >= (uint32_t)CART_FB_CHUNK_COVERED) return o;
  uint32_t k = (y * 10u * 0xAAABu) >> 17;
  uint32_t r = y * 160u - k * 48u + g * 8u; /* 0..184 */
  const uint32_t add = (r >= 48u) + (r >= 96u) + (r >= 144u);
  k += add;
  r -= add * 48u;
  return (CART_FB_CHUNK_COUNT - 1u - k) * CART_FB_CHUNK_BYTES + r;
}

/* A cart-FB cursor on one group that walks up a row at a time: a row is
 * 160 bytes = 3 chunks + 16, and the chunks run backwards in the cart FB,
 * so the row above is 3 chunks later minus 16 bytes, with a chunk
 * borrow. Rows 0..198 only: the last 32 bytes of row 199 are the m68k's
 * unreversed tail, which wipe_cart_off handles. */
typedef struct {
  uint32_t co; /* cart offset      */
  int r;       /* offset in chunk  */
} wipe_ptr_t;

static inline void wipe_ptr_init(wipe_ptr_t *p, unsigned y, unsigned g) {
  uint32_t k = (y * 10u * 0xAAABu) >> 17;
  uint32_t r = y * 160u - k * 48u + g * 8u;
  const uint32_t add = (r >= 48u) + (r >= 96u) + (r >= 144u);
  k += add;
  r -= add * 48u;
  p->co = (CART_FB_CHUNK_COUNT - 1u - k) * CART_FB_CHUNK_BYTES + r;
  p->r = (int)r;
}

static inline void wipe_ptr_up(wipe_ptr_t *p) {
  p->co += 3u * CART_FB_CHUNK_BYTES - 16u;
  p->r -= 16;
  if (p->r < 0) {
    p->r += CART_FB_CHUNK_BYTES;
    p->co += 2u * CART_FB_CHUNK_BYTES;
  }
}

static inline void wipe_rmw(uint32_t *dst, const uint32_t *src, uint32_t m) {
  dst[0] = (dst[0] & ~m) | (src[0] & m);
  dst[1] = (dst[1] & ~m) | (src[1] & m);
}

static void __not_in_flash_func(wipe_compose_groups)(unsigned g0, unsigned g1) {
  uint8_t *const fb = s_cart_fb;
  for (unsigned g = g0; g < g1; g++) {
    /* Per column: offset now, and how far it moved this step. Columns
     * that moved by the same amount form a class: one source row, one
     * mask, one pass. */
    int16_t on[8];
    int ymin_static = FB_CHUNKED_H; /* rows above were all-new last step too */
    int ymax = 0;
    unsigned nd = 0;
    int16_t dd[8], dlo[8];
    uint32_t dm[8];
    for (unsigned c = 0; c < 8u; c++) {
      int v = s_wipe_y[g * 8u + c];
      if (v < 0) v = 0;
      if (v > FB_CHUNKED_H) v = FB_CHUNKED_H;
      on[c] = (int16_t)v;
      if (v > ymax) ymax = v;
      const int prev = s_wipe_off[g * 8u + c];
      if (prev < ymin_static) ymin_static = prev;
      const int16_t d = (int16_t)(v - prev);
      if (!d) continue; /* still, or already gone: nothing to move */
      const uint32_t m = 3u << (14u - 2u * c);
      unsigned j;
      for (j = 0; j < nd; j++) {
        if (dd[j] == d) {
          dm[j] |= m;
          if (v < dlo[j]) dlo[j] = (int16_t)v;
          break;
        }
      }
      if (j == nd) {
        dd[nd] = d;
        dm[nd] = m;
        dlo[nd] = (int16_t)v;
        nd++;
      }
    }
    if (!nd) continue; /* nothing in this group changed */

    /* The old picture: each class shifts down by its delta, bottom-up,
     * from row 199 to the top of its highest column. Rows above a
     * column's own offset get stale bits here; the new pass below
     * overwrites them. */
    for (unsigned j = 0; j < nd; j++) {
      const uint32_t m = dm[j] * 0x10001u;
      const int d = dd[j];
      int y = FB_CHUNKED_H - 1;
      if (dlo[j] > y) continue;
      wipe_rmw((uint32_t *)(fb + wipe_cart_off((unsigned)y, g)),
               (const uint32_t *)(fb + wipe_cart_off((unsigned)(y - d), g)), m);
      if (--y < dlo[j]) continue;
      wipe_ptr_t pd, ps;
      wipe_ptr_init(&pd, (unsigned)y, g);
      wipe_ptr_init(&ps, (unsigned)(y - d), g);
      for (;;) {
        wipe_rmw((uint32_t *)(fb + pd.co), (const uint32_t *)(fb + ps.co), m);
        if (--y < dlo[j]) break;
        wipe_ptr_up(&pd);
        wipe_ptr_up(&ps);
      }
    }

    /* The new picture: rows some column has revealed, down to the ones
     * that were fully revealed last step and need no repaint. */
    for (int y = ymax - 1; y >= ymin_static; y--) {
      uint32_t mnew = 0;
      for (unsigned c = 0; c < 8u; c++) {
        if (on[c] > y) mnew |= (3u << (14u - 2u * c)) * 0x10001u;
      }
      uint32_t t[2];
      const unsigned pix = (unsigned)y * FB_CHUNKED_W + g * 16u;
      doom_c2p_block_any(t, fb_chunked_buffer + pix, pix);
      uint32_t *dst = (uint32_t *)(fb + wipe_cart_off((unsigned)y, g));
      if (mnew == 0xFFFFFFFFu) {
        dst[0] = t[0];
        dst[1] = t[1];
      } else {
        wipe_rmw(dst, t, mnew);
      }
    }
    for (unsigned c = 0; c < 8u; c++) s_wipe_off[g * 8u + c] = on[c];
  }
}

static void __not_in_flash_func(wipe_right_job)(void *arg) {
  (void)arg;
  wipe_compose_groups(10u, 20u);
}

void doom_video_wipe_begin(void) {
  s_wipe_rand = time_us_32() | 1u;
  s_wipe_y[0] = -(int16_t)(wipe_random() % 16u);
  for (unsigned i = 1; i < WIPE_COLS; i++) {
    int v = s_wipe_y[i - 1] + (int)(wipe_random() % 3u) - 1;
    if (v > 0) v = 0;
    else if (v == -16) v = -15;
    s_wipe_y[i] = (int16_t)v;
  }
  memset(s_wipe_off, 0, sizeof(s_wipe_off));
  s_wipe_max_us = 0;
}

bool doom_video_wipe_step(int tics) {
  bool done = true;
  while (tics-- > 0) {
    for (unsigned i = 0; i < WIPE_COLS; i++) {
      int y = s_wipe_y[i];
      if (y < 0) {
        y++;
        done = false;
      } else if (y < FB_CHUNKED_H) {
        int dy = (y < 16) ? y + 1 : 8;
        if (y + dy >= FB_CHUNKED_H) dy = FB_CHUNKED_H - y;
        y += dy;
        done = false;
      }
      s_wipe_y[i] = (int16_t)y;
    }
  }

  s_cart_fb = (uint8_t *)fb_screen.framebuffer;
  s_use_bn = (s_dither == DOOM_VIDEO_DITHER_BLUENOISE);
  fb_wait_blit_ack();
  const uint32_t t0 = time_us_32();
  fb_core1_dispatch(wipe_right_job, NULL);
  wipe_compose_groups(0u, 10u);
  fb_core1_wait();
  const uint32_t us = time_us_32() - t0;
  if (us > s_wipe_max_us) s_wipe_max_us = us;
  fb_frame_done();

  if (done) DPRINTF("melt done, step max %lu us\n", (unsigned long)s_wipe_max_us);
  return done;
}

#pragma GCC pop_options
