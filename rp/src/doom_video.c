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
 *     between the two (t in 0..16) against the cell's threshold.
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

#include "cart_shared.h"
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
static uint16_t s_st_colors[16];  /* the same, as ST palette words       */
static uint8_t s_nearest[256];    /* PLAYPAL index -> nearest pen        */

/* [cell][index] -> pen, cell = ((y & 3) << 2) | (x & 3). 4 KB. */
static uint8_t s_lut[16][256] __attribute__((aligned(4)));

static uint32_t s_convert_us;

/* STDOOM's hand-picked 16 PLAYPAL indices (Jonas Eschenburg): black, two
 * blues, two greens, dark red, greys, flesh, yellow, browns, bright red,
 * olive. Refined against the shareware WAD over a long time; it holds
 * up well on flesh tones, the sky and the status bar. */
static const uint8_t s_subset[16] = {0,   201, 205, 117, 123, 185, 87,  100,
                                     60,  95,  227, 104, 69,  79,  179, 159};

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
  for (uint8_t k = 0; k < 16u; k++) {
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

static void set_refs_from_rgb(const uint8_t src[16][3]) {
  for (uint8_t k = 0; k < 16u; k++) {
    s_st_colors[k] = st_color_word(src[k][0], src[k][1], src[k][2]);
    s_ref_rgb[k][0] = displayed_channel(src[k][0]);
    s_ref_rgb[k][1] = displayed_channel(src[k][1]);
    s_ref_rgb[k][2] = displayed_channel(src[k][2]);
  }
}

static void build_refs(void) {
  uint8_t pal[16][3];
  switch (s_pal_mode) {
    case DOOM_VIDEO_PAL_GENERATED:
      median_cut(pal);
      kmeans(pal);
      break;
    case DOOM_VIDEO_PAL_GREY:
      for (int k = 0; k < 16; k++) pal[k][0] = pal[k][1] = pal[k][2] = (uint8_t)(k * 17);
      break;
    default:
      for (int k = 0; k < 16; k++) {
        const uint8_t *c = &s_playpal[(uint32_t)s_subset[k] * 3u];
        pal[k][0] = c[0];
        pal[k][1] = c[1];
        pal[k][2] = c[2];
      }
      break;
  }
  set_refs_from_rgb(pal);
}

static void rebuild(void) {
  if (!s_have_playpal) return;
  build_refs();

  for (uint32_t i = 0; i < 256u; i++) {
    const uint8_t *c = &s_playpal[i * 3u];
    uint8_t a, b;
    two_nearest_ref(c, &a, &b);
    s_nearest[i] = a;

    if (s_dither == DOOM_VIDEO_DITHER_NEAREST) {
      for (uint8_t cell = 0; cell < 16u; cell++) s_lut[cell][i] = a;
      continue;
    }

    /* Project the colour onto the a->b line: t in 0..16. */
    int dr = (int)s_ref_rgb[b][0] - (int)s_ref_rgb[a][0];
    int dg = (int)s_ref_rgb[b][1] - (int)s_ref_rgb[a][1];
    int db = (int)s_ref_rgb[b][2] - (int)s_ref_rgb[a][2];
    int denom = dr * dr + dg * dg + db * db;
    int dot = ((int)c[0] - (int)s_ref_rgb[a][0]) * dr +
              ((int)c[1] - (int)s_ref_rgb[a][1]) * dg +
              ((int)c[2] - (int)s_ref_rgb[a][2]) * db;
    int t16;
    if (denom <= 0 || dot <= 0) {
      t16 = 0;
    } else if (dot >= denom) {
      t16 = 16;
    } else {
      t16 = (dot * 16) / denom;
    }
    for (uint8_t cell = 0; cell < 16u; cell++) {
      s_lut[cell][i] = (t16 > (int)dither_threshold(s_dither, cell)) ? b : a;
    }
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
    default: return "?";
  }
}

const char *doom_video_dither_name(doom_video_dither_t mode) {
  switch (mode) {
    case DOOM_VIDEO_DITHER_NEAREST: return "NEAREST";
    case DOOM_VIDEO_DITHER_BAYER2: return "BAYER 2X2";
    case DOOM_VIDEO_DITHER_BAYER4: return "BAYER 4X4";
    case DOOM_VIDEO_DITHER_HALFTONE: return "HALFTONE";
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

/* Chunk k: 96 pixels = 6 blocks; block b of chunk k starts at pixel
 * k*96 + b*16, on row (that / 320). Its 8 planar bytes land at
 * fb_cart_offset(k*48 + b*8). */
static void __not_in_flash_func(doom_c2p_chunks)(unsigned k0, unsigned k1) {
  for (unsigned k = k0; k < k1; k++) {
    const unsigned pix0 = k * 96u;
    const uint32_t cart0 = fb_cart_offset(k * (uint32_t)CART_FB_CHUNK_BYTES);
    for (unsigned b = 0; b < 6u; b++) {
      const unsigned pix = pix0 + b * 16u;
      doom_c2p_block((uint32_t *)(s_cart_fb + cart0 + b * 8u),
                     fb_chunked_buffer + pix, pix / FB_CHUNKED_W);
    }
  }
}

static void __not_in_flash_func(doom_c2p_bottom_job)(void *arg) {
  (void)arg;
  doom_c2p_chunks(CART_FB_CHUNK_COUNT / 2u, CART_FB_CHUNK_COUNT);
}

void doom_video_publish(void) {
  s_cart_fb = (uint8_t *)fb_screen.framebuffer;

  /* The cart FB is written in place, so only in the m68k's post-blit
   * slack: wait for its ack, convert on both cores, mark ready. */
  fb_wait_blit_ack();
  const uint32_t t0 = time_us_32();

  fb_core1_dispatch(doom_c2p_bottom_job, NULL);
  doom_c2p_chunks(0u, CART_FB_CHUNK_COUNT / 2u);
  /* Tail: the last 64 pixels, natural order at the end of the FB. */
  for (unsigned b = 0; b < CART_FB_CHUNK_TAIL / 8u; b++) {
    const unsigned pix = 2u * CART_FB_CHUNK_COVERED + b * 16u;
    doom_c2p_block((uint32_t *)(s_cart_fb + CART_FB_CHUNK_COVERED + b * 8u),
                   fb_chunked_buffer + pix, pix / FB_CHUNKED_W);
  }
  fb_core1_wait();

  s_convert_us = time_us_32() - t0;
  fb_frame_done();
}

uint32_t doom_video_last_convert_us(void) { return s_convert_us; }
