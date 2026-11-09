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
 *  1. Palette work, for every frame: choose 16 colours for the ones on
 *     screen (the adaptive palette, below), snapped to what the ST can
 *     display, and build s_lut[16][256] -- for each of the 16 dither
 *     cells, the pen every PLAYPAL index maps to. A dithered index is
 *     drawn as one pen on some cells and another on the rest, the split
 *     decided by its mix level (t in 0..16) against the cell's
 *     threshold (see best_pair).
 *
 *  2. Per frame: doom_video_publish() walks fb_chunked_buffer 16 pixels
 *     at a time, maps each pixel through the LUT for its cell, and packs
 *     the four plane words with the same multiply transpose the
 *     framework's asm worker uses. The bottom half of the screen runs on
 *     Core 1 through fb_core1_dispatch, straight into the cart FB in the
 *     m68k's post-blit slack (there is no planar scratch buffer).
 *
 * In ST high res (a mono monitor) both halves change: the reduction is
 * to brightness alone, and the frame is 640x400 at one bit a pixel. See
 * "ST high res" below.
 */

#include "doom_video.h"

#include <string.h>

#include "audio.h"
#include "bluenoise.h" /* 32x32 threshold tile, 16 levels, in flash */
#include "gamma.h"     /* sRGB <-> linear light, in flash          */
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
static doom_video_dither_t s_dither = DOOM_VIDEO_DITHER_BAYER4;

static bool s_mono;               /* ST high res: 640x400, one bit a pixel */

static uint8_t s_ref_rgb[16][3];  /* the 16 colours as the ST shows them */
static uint16_t s_st_colors[16];  /* the same, as ST palette words       */

/* [cell][index] -> pen, cell = ((y & 3) << 2) | (x & 3). 4 KB. In ST
 * high res the same 4 KB holds the mono table instead (build_mono). */
static union {
  uint8_t pen[16][256];
  uint16_t mono[2][4][256];
} s_lut __attribute__((aligned(4)));

/* For the blue-noise mode, which cannot be a per-cell LUT (1024 cells):
 * per index, nearest pen (bits 0..3), second pen (bits 4..7) and the
 * 0..16 mix level (bits 8..12); a pixel takes the second pen where the
 * level beats the tile's threshold. In ST high res, the index's
 * brightness level 0..16 alone. */
static uint16_t s_pair[256];

static uint32_t s_convert_us;

/* Everything from here to the public API runs per palette or per frame,
 * not per pixel: size matters more than speed (the debug build's flash
 * ran out at -O3). */
#pragma GCC push_options
#pragma GCC optimize("Os")

/* ------------------------------------------------------------------ */
/* Colour helpers                                                      */

/* Perceptual distance, the "redmean" approximation: green-weighted and
 * red-adjusted so skies and browns pick sensible neighbours. */
static long color_dist(int r1, int g1, int b1, int r2, int g2, int b2) {
  long rmean = ((long)r1 + (long)r2) / 2;
  long dr = r1 - r2, dg = g1 - g2, db = b1 - b2;
  return (((512 + rmean) * dr * dr) >> 8) + 4 * dg * dg +
         (((767 - rmean) * db * db) >> 8);
}

/* The pens to draw colour c with, as s_pair holds them: one pen (bits
 * 0..3), another (4..7) and the 0..16 mix level (8..12), the second pen
 * taking the cells whose threshold the level beats.
 *
 * A dither is two pens side by side and the eye averages them as light,
 * not as 0..255 values (half black and half white looks like sRGB 188,
 * not 128), so a pair is judged by the colour its mix really makes: of
 * the pairs among the four nearest pens, the level from a projection in
 * linear light (and the levels either side), the mix taken back to sRGB
 * and compared with c by redmean; the nearest pen alone is the baseline.
 * Offline against the original frames (S-CIELAB, STE, 4x4 Bayer) this
 * took the error from 2.99 to 2.50 against the two nearest pens and a
 * projection in sRGB, which places every mix between distant pens too
 * light. Without a dither it is simply the nearest pen. */
#define PAIR_CANDS 4

static uint16_t best_pair(const uint8_t *c, const uint8_t (*rgb)[3], const uint16_t (*lin)[3],
                          int n, bool dither) {
  uint8_t cand[PAIR_CANDS];
  long cd[PAIR_CANDS];
  int nc = 0;
  for (int k = 0; k < n; k++) {
    const long d = color_dist(c[0], c[1], c[2], rgb[k][0], rgb[k][1], rgb[k][2]);
    int j = nc < PAIR_CANDS ? nc++ : PAIR_CANDS;
    while (j > 0 && cd[j - 1] > d) {
      if (j < PAIR_CANDS) {
        cd[j] = cd[j - 1];
        cand[j] = cand[j - 1];
      }
      j--;
    }
    if (j < PAIR_CANDS) {
      cd[j] = d;
      cand[j] = (uint8_t)k;
    }
  }
  unsigned a = cand[0], b = cand[0], t = 0;
  if (!dither) return (uint16_t)(a | (a << 4));
  long best = cd[0];
  const int32_t cl[3] = {gamma_lin[c[0]], gamma_lin[c[1]], gamma_lin[c[2]]};
  for (int i = 0; i < nc; i++) {
    for (int j = i + 1; j < nc; j++) {
      const uint16_t *la = lin[cand[i]], *lb = lin[cand[j]];
      int32_t v[3], num = 0, den = 0;
      for (int ch = 0; ch < 3; ch++) {
        v[ch] = (int32_t)lb[ch] - (int32_t)la[ch];
        num += (cl[ch] - (int32_t)la[ch]) * v[ch];
        den += v[ch] * v[ch];
      }
      if (den <= 0) continue;
      int32_t t0 = num <= 0 ? 0 : (num * 16 + den / 2) / den;
      if (t0 > 16) t0 = 16;
      for (int32_t tt = t0 - 1; tt <= t0 + 1; tt++) {
        if (tt < 0 || tt > 16) continue;
        uint8_t m[3];
        for (int ch = 0; ch < 3; ch++) m[ch] = gamma_srgb[(la[ch] + (v[ch] * tt) / 16) >> 2];
        const long e = color_dist(c[0], c[1], c[2], m[0], m[1], m[2]);
        if (e < best) {
          best = e;
          a = cand[i];
          b = cand[j];
          t = (unsigned)tt;
        }
      }
    }
  }
  return (uint16_t)(a | (b << 4) | (t << 8));
}

/* ------------------------------------------------------------------ */
/* Dither thresholds                                                   */

static const uint8_t s_bayer4[16] = {0, 8,  2,  10, 12, 4,  14, 6,
                                     3, 11, 1,  9,  15, 7,  13, 5};
static const uint8_t s_bayer2[4] = {2, 10, 14, 6}; /* scaled to 0..15 */

static uint8_t dither_threshold(doom_video_dither_t mode, uint8_t cell) {
  switch (mode) {
    case DOOM_VIDEO_DITHER_BAYER2:
      return s_bayer2[(((cell >> 2) & 1u) << 1) | (cell & 1u)];
    default:
      return s_bayer4[cell & 15u];
  }
}

/* ------------------------------------------------------------------ */
/* Adaptive palette: 16 pens for each frame                            */

/* A fixed set of 16 for the whole of PLAYPAL spends pens on colours that
 * are not on screen. These are chosen for the frame being shown: a
 * histogram of the PLAYPAL indices in it, clustered into 16 by weighted
 * k-means, each centre snapped to a colour the shifter can show (16
 * levels a channel on an STE, 8 on an ST, told apart by DMA sound).
 * Measured offline against the original 256-colour frames
 * (tools/bandpal.py), it more than halves the error of STDOOM's
 * hand-picked subset, the fixed set this replaced, on an STE.
 *
 * Pens chosen afresh every frame would shimmer, so a frame starts from
 * the last one's centres and takes ADAPT_ITERS k-means steps, and a
 * channel only moves to another level once its centre is ADAPT_STICKY
 * sixteenths of a level past the halfway mark. A still picture settles
 * and stays put.
 *
 * The status bar gets a palette of its own (the "split", see
 * CART_SPLIT_OFFSET): the view's pens move with the view, and the bar,
 * sharing them, would shimmer. Its rows are
 * drawn nearest-pen through s_hud_pen, as 16 pens chosen for a few
 * dozen flat colours leave nothing worth dithering, and a second 4 KB
 * LUT is more RAM than the bar deserves.
 *
 * Pen 0 stays black in both: it is also the colour of the border all
 * round the picture, which must not change with the frame, and Doom
 * always has black on screen anyway.
 *
 * Distances are 2 dR^2 + 4 dG^2 + 3 dB^2, which the offline test found
 * as good as CIELAB here at a fraction of the cost. Centres are kept in
 * sixteenths so the hysteresis has something to work with, and the
 * histogram samples every other pixel of every other row. */

#define ADAPT_ITERS 2      /* k-means steps a frame, from the last frame's */
#define ADAPT_COLD_ITERS 8 /* from scratch, or into a melt                 */
#define ADAPT_STICKY 5     /* sixteenths of a level past halfway to move   */
#define ADAPT_BLACK 1u     /* pen 0, never moved                           */
#define SPLIT_ROW CART_SPLIT_LINE

/* One palette's worth: the view's, and with the split the bar's. */
typedef struct {
  int16_t cen[16][3]; /* centres, RGB in sixteenths                   */
  uint8_t lvl[16][3]; /* the level each channel is snapped to         */
  uint8_t lvl_max;    /* 15 (STE) or 7 (ST); 0 = nothing chosen yet    */
  uint8_t rgb[16][3]; /* the pens as the shifter shows them           */
  uint16_t st[16];    /* and as palette words                         */
} adapt_t;

static adapt_t s_view, s_hud;
static uint16_t s_hist[256];  /* one region's histogram at a time         */
static bool s_lut_dirty;      /* the page changed under the view's pens   */
static bool s_hud_dirty;      /* or under the bar's                       */
static uint16_t s_frozen;     /* view pens k-means may not move (a melt)  */
static uint16_t s_view_used;  /* view pens the last frame used            */
static bool s_status_bar;     /* the game says the bar is on screen       */
static bool s_split;          /* and this frame has its own palette for it */
static uint8_t s_hud_pen[256]; /* PLAYPAL index -> the bar's nearest pen   */

static void adapt_histogram(unsigned y0, unsigned y1) {
  memset(s_hist, 0, sizeof(s_hist));
  for (unsigned y = y0; y < y1; y += 2u) {
    const uint8_t *p = fb_chunked_buffer + y * FB_CHUNKED_W + ((y >> 1) & 1u);
    for (unsigned x = 0; x < FB_CHUNKED_W; x += 2u) s_hist[p[x]]++;
  }
}

static int32_t adapt_dist(const int16_t *c, const uint8_t *rgb) {
  const int32_t dr = ((int32_t)rgb[0] << 4) - c[0];
  const int32_t dg = ((int32_t)rgb[1] << 4) - c[1];
  const int32_t db = ((int32_t)rgb[2] << 4) - c[2];
  return 2 * dr * dr + 4 * dg * dg + 3 * db * db;
}

/* Put each centre not in `live` on the colour the live ones serve
 * worst, weighted by how much of the screen it covers, one at a time:
 * with none live it is a farthest-point start from the commonest. */
static void adapt_reseed(adapt_t *a, uint16_t live) {
  for (unsigned k = 0; k < 16u; k++) {
    if (live & (1u << k)) continue;
    uint64_t best = 0;
    int bi = -1;
    for (unsigned i = 0; i < 256u; i++) {
      if (!s_hist[i]) continue;
      uint32_t d = live ? UINT32_MAX : 1u;
      for (unsigned j = 0; j < 16u; j++) {
        if (!(live & (1u << j))) continue;
        const uint32_t dj = (uint32_t)adapt_dist(a->cen[j], &s_playpal[i * 3u]);
        if (dj < d) d = dj;
      }
      const uint64_t score = (uint64_t)d * s_hist[i];
      if (bi < 0 || score > best) {
        best = score;
        bi = (int)i;
      }
    }
    if (bi < 0) return; /* nothing on screen */
    for (unsigned ch = 0; ch < 3u; ch++) a->cen[k][ch] = (int16_t)(s_playpal[bi * 3 + (int)ch] << 4);
    live |= (uint16_t)(1u << k);
  }
}

/* One k-means step over the histogram. Frozen centres take colours but
 * do not move; a centre left with none is re-seeded. */
static void adapt_step(adapt_t *a, uint16_t frozen) {
  int32_t sum[16][3];
  uint32_t cnt[16];
  memset(sum, 0, sizeof(sum));
  memset(cnt, 0, sizeof(cnt));
  for (unsigned i = 0; i < 256u; i++) {
    const uint32_t w = s_hist[i];
    if (!w) continue;
    const uint8_t *c = &s_playpal[i * 3u];
    int32_t best = INT32_MAX;
    unsigned bk = 0;
    for (unsigned k = 0; k < 16u; k++) {
      const int32_t d = adapt_dist(a->cen[k], c);
      if (d < best) {
        best = d;
        bk = k;
      }
    }
    for (unsigned ch = 0; ch < 3u; ch++) sum[bk][ch] += (int32_t)(w * c[ch]);
    cnt[bk] += w;
  }
  uint16_t live = frozen;
  for (unsigned k = 0; k < 16u; k++) {
    if (!cnt[k]) continue;
    live |= (uint16_t)(1u << k);
    if (frozen & (1u << k)) continue;
    for (unsigned ch = 0; ch < 3u; ch++) {
      a->cen[k][ch] = (int16_t)((sum[k][ch] * 16 + (int32_t)(cnt[k] / 2u)) / (int32_t)cnt[k]);
    }
  }
  if (live != 0xFFFFu) adapt_reseed(a, live);
}

/* Level -> displayed channel value / palette nibble, for 15 or 7 levels. */
static uint8_t level_value(unsigned l, unsigned max) {
  return (uint8_t)(max == 15u ? l * 17u : (l * 255u + 3u) / 7u);
}

/* Pens with these levels, as shown and in linear light. */
static void level_pens(const uint8_t lvl[16][3], unsigned max, uint8_t rgb[16][3], uint16_t lin[16][3]) {
  for (unsigned k = 0; k < 16u; k++) {
    for (unsigned ch = 0; ch < 3u; ch++) {
      rgb[k][ch] = level_value(lvl[k][ch], max);
      lin[k][ch] = gamma_lin[rgb[k][ch]];
    }
  }
}

/* The colour index i's pair (best_pair) makes, in sRGB. */
static void pair_mix(uint16_t v, const uint16_t (*lin)[3], uint8_t *m) {
  const uint16_t *la = lin[v & 15u], *lb = lin[(v >> 4) & 15u];
  const int32_t t = v >> 8;
  for (int ch = 0; ch < 3; ch++) {
    m[ch] = gamma_srgb[(la[ch] + (((int32_t)lb[ch] - (int32_t)la[ch]) * t) / 16) >> 2];
  }
}

/* How badly pens with these levels serve the histogram, weighted by
 * count: each colour's distance to its nearest pen, or with `mix` to
 * the mix of its best pair, which is how it will be drawn. */
static uint64_t adapt_error(const uint8_t lvl[16][3], unsigned max, bool mix) {
  uint8_t rgb[16][3];
  uint16_t lin[16][3];
  level_pens(lvl, max, rgb, lin);
  uint64_t err = 0;
  for (unsigned i = 0; i < 256u; i++) {
    if (!s_hist[i]) continue;
    const uint8_t *c = &s_playpal[i * 3u];
    uint8_t m[3];
    pair_mix(best_pair(c, (const uint8_t(*)[3])rgb, (const uint16_t(*)[3])lin, 16, mix),
             (const uint16_t(*)[3])lin, m);
    err += (uint64_t)color_dist(c[0], c[1], c[2], m[0], m[1], m[2]) * s_hist[i];
  }
  return err;
}

/* k-means puts each pen at the middle of its colours, which is right for
 * drawing every colour in its nearest pen; drawn in mixes of two, the
 * pens do better as the ends the mixes are made from. One pass: each
 * colour takes its best pair and level against the pens as they stand
 * (the centres, snapped), and each pen moves, in linear light, to the
 * weighted least-squares best for the mixes it is part of. Offline
 * (S-CIELAB, STE, 4x4 Bayer) 2.50 -> 2.28; more passes add nothing. The
 * centres themselves stay as k-means left them, for the next frame. */
static void adapt_refine(const adapt_t *a, uint16_t frozen, int16_t out[16][3]) {
  const uint8_t max = (audio_get_mode() == AUDIO_MODE_YM) ? 7u : 15u;
  uint8_t lvl[16][3], rgb[16][3];
  uint16_t lin[16][3];
  for (unsigned k = 0; k < 16u; k++) {
    for (unsigned ch = 0; ch < 3u; ch++) {
      const int32_t l = ((int32_t)a->cen[k][ch] * max + 2040) / 4080;
      lvl[k][ch] = (uint8_t)(l < 0 ? 0 : l > (int32_t)max ? (int32_t)max : l);
    }
  }
  level_pens(lvl, max, rgb, lin);
  int64_t num[16][3];
  int64_t den[16];
  memset(num, 0, sizeof(num));
  memset(den, 0, sizeof(den));
  for (unsigned i = 0; i < 256u; i++) {
    const int64_t w = s_hist[i];
    if (!w) continue;
    const uint8_t *c = &s_playpal[i * 3u];
    const uint16_t v = best_pair(c, (const uint8_t(*)[3])rgb, (const uint16_t(*)[3])lin, 16, true);
    const unsigned pa = v & 15u, pb = (v >> 4) & 15u;
    const int64_t t = v >> 8, u = 16 - t;
    /* c ~ (u A + t B) / 16, each end solved with the other held. */
    for (int ch = 0; ch < 3; ch++) {
      const int64_t c16 = 16 * (int64_t)gamma_lin[c[ch]];
      if (u) num[pa][ch] += w * u * (c16 - t * lin[pb][ch]);
      if (t) num[pb][ch] += w * t * (c16 - u * lin[pa][ch]);
    }
    if (u) den[pa] += w * u * u;
    if (t) den[pb] += w * t * t;
  }
  for (unsigned k = 0; k < 16u; k++) {
    for (unsigned ch = 0; ch < 3u; ch++) {
      if ((frozen & (1u << k)) || !den[k]) { /* held, or in no mix */
        out[k][ch] = a->cen[k][ch];
        continue;
      }
      int64_t l = num[k][ch] / den[k];
      if (l < 0) l = 0;
      if (l > 4095) l = 4095;
      out[k][ch] = (int16_t)(gamma_srgb[l >> 2] << 4);
    }
  }
}

/* Snap the centres to levels, with hysteresis, and fill in the pens.
 * Frozen pens keep theirs. A new set is only taken if it serves the
 * histogram ADAPT_GATE better than the one on screen: offline (on a
 * run through E1M1) this cut the changes from 22% of frames to 7%, and
 * the flicker of still parts of the picture with them, for an error 2%
 * higher. True if any pen changed. */
#define ADAPT_GATE 16 /* in sixteenths: 15/16 of the error, ~6% better */

static bool adapt_snap(adapt_t *a, const int16_t (*cen)[3], uint16_t frozen, bool mix) {
  const uint8_t max = (audio_get_mode() == AUDIO_MODE_YM) ? 7u : 15u;
  const bool warm = (a->lvl_max == max);
  uint8_t lvl[16][3];
  memcpy(lvl, a->lvl, sizeof(lvl));
  for (unsigned k = 0; k < 16u; k++) {
    if (frozen & (1u << k)) continue;
    for (unsigned ch = 0; ch < 3u; ch++) {
      const int32_t pos = ((int32_t)cen[k][ch] * max + 127) / 255; /* sixteenths */
      int32_t l = (pos + 8) >> 4;
      if (warm) {
        const int32_t off = pos - ((int32_t)a->lvl[k][ch] << 4);
        if (off > -(8 + ADAPT_STICKY) && off < 8 + ADAPT_STICKY) l = a->lvl[k][ch];
      }
      if (l < 0) l = 0;
      if (l > max) l = max;
      lvl[k][ch] = (uint8_t)l;
    }
  }
  if (warm) {
    if (!memcmp(lvl, a->lvl, sizeof(lvl))) return false;
    if (!(frozen & ~ADAPT_BLACK) &&
        adapt_error(lvl, max, mix) * ADAPT_GATE >= adapt_error(a->lvl, max, mix) * (ADAPT_GATE - 1)) {
      return false;
    }
  }
  a->lvl_max = max;
  memcpy(a->lvl, lvl, sizeof(lvl));
  for (unsigned k = 0; k < 16u; k++) {
    uint16_t w = 0;
    for (unsigned ch = 0; ch < 3u; ch++) {
      const unsigned l = a->lvl[k][ch];
      /* An STE level's lowest, finest bit is the nibble's bit 3. */
      w = (uint16_t)((w << 4) | (max == 15u ? ((l >> 1) | ((l & 1u) << 3)) : l));
      a->rgb[k][ch] = level_value(l, max);
    }
    a->st[k] = w;
  }
  return true;
}

/* The pens for rows [y0, y1), drawn in mixes (`mix`, a dither) or in
 * their nearest pen. True if they changed. Nothing chosen yet means from
 * scratch: black, then a farthest-point start for the rest. */
static bool adapt_update(adapt_t *a, unsigned y0, unsigned y1, uint16_t frozen, int iters, bool mix) {
  frozen |= ADAPT_BLACK;
  adapt_histogram(y0, y1);
  if (!a->lvl_max) {
    memset(a->cen[0], 0, sizeof(a->cen[0]));
    memset(a->lvl[0], 0, sizeof(a->lvl[0]));
    adapt_reseed(a, frozen);
    iters = ADAPT_COLD_ITERS;
  }
  while (iters-- > 0) adapt_step(a, frozen);
  if (!mix) return adapt_snap(a, (const int16_t(*)[3])a->cen, frozen, false);
  int16_t ends[16][3];
  adapt_refine(a, frozen, ends);
  return adapt_snap(a, (const int16_t(*)[3])ends, frozen, true);
}

/* The view's pens are the reducer's references. */
static void adapt_install_view(void) {
  memcpy(s_ref_rgb, s_view.rgb, sizeof(s_ref_rgb));
  memcpy(s_st_colors, s_view.st, sizeof(s_st_colors));
}

static void build_hud_pens(void) {
  for (unsigned i = 0; i < 256u; i++) {
    const uint8_t *c = &s_playpal[i * 3u];
    long best = 0x7FFFFFFFL;
    for (unsigned k = 0; k < 16u; k++) {
      const long d = color_dist(c[0], c[1], c[2], s_hud.rgb[k][0], s_hud.rgb[k][1], s_hud.rgb[k][2]);
      if (d < best) {
        best = d;
        s_hud_pen[i] = (uint8_t)k;
      }
    }
  }
}

/* ------------------------------------------------------------------ */
/* ST high res                                                         */

/* A mono monitor shows 640x400 at one bit a pixel and has no palette: a
 * set bit is black ink, a clear one white paper (userfw.s sets colour 0
 * that way). Doom's 320x200 frame is doubled both ways, but the dither
 * runs at the full 640x400, so each Doom pixel is a 2x2 block of output
 * pixels that can take up to four different thresholds, and a flat
 * colour gets all 17 levels of a 4x4 matrix rather than 17 levels'
 * worth of blocks. An output pixel is white where the colour's level,
 * 0..16, beats the threshold under it. There is no palette to choose.
 *
 * The level is the colour's brightest channel, not its luminance.
 * Saturated colours look brighter than their luminance says, and Doom
 * leans on it: the status bar's red numerals have about the luminance
 * of the grey bar behind them (levels 2..4 against 4..6) and vanish by
 * luminance, where by their brightest channel they stand out at 7..13.
 * Greys are the same either way, and the browns most of Doom is made of
 * move only a level or two.
 *
 * s_lut.mono[y & 1][x & 3][index] is one Doom pixel's four output bits,
 * placed for a group of four: the low byte is the group's 8 output
 * pixels in the upper of its two rows (pixel x & 3 = k at bits 7-2k and
 * 6-2k), the high byte the same in the row below. Four lookups ORed
 * together make a group's two output bytes. */

static uint8_t mono_threshold(unsigned yy, unsigned xx) {
  if (s_dither == DOOM_VIDEO_DITHER_NEAREST) return 7;
  return dither_threshold(s_dither, (uint8_t)(((yy & 3u) << 2) | (xx & 3u)));
}

static void build_mono(void) {
  for (uint32_t i = 0; i < 256u; i++) {
    const uint8_t *c = &s_playpal[i * 3u];
    int m = c[0];
    if (c[1] > m) m = c[1];
    if (c[2] > m) m = c[2];
    s_pair[i] = (uint16_t)((m * 16 + 127) / 255);
  }
  for (unsigned yp = 0; yp < 2u; yp++) {
    for (unsigned k = 0; k < 4u; k++) {
      uint8_t t[2][2]; /* [output row][output column] of this pixel */
      for (unsigned r = 0; r < 2u; r++) {
        for (unsigned c = 0; c < 2u; c++) t[r][c] = mono_threshold(2u * yp + r, 2u * k + c);
      }
      for (uint32_t i = 0; i < 256u; i++) {
        uint16_t e = 0;
        for (unsigned r = 0; r < 2u; r++) {
          for (unsigned c = 0; c < 2u; c++) {
            if (s_pair[i] <= t[r][c]) e |= (uint16_t)(1u << (8u * r + 7u - 2u * k - c));
          }
        }
        s_lut.mono[yp][k][i] = e;
      }
    }
  }
}

/* s_lut and s_pair from s_ref_rgb and the installed page. */
static void build_lut(void) {
  uint16_t lin[16][3];
  for (int k = 0; k < 16; k++) {
    for (int ch = 0; ch < 3; ch++) lin[k][ch] = gamma_lin[s_ref_rgb[k][ch]];
  }
  const bool dither = (s_dither != DOOM_VIDEO_DITHER_NEAREST);
  for (uint32_t i = 0; i < 256u; i++) {
    const uint8_t *c = &s_playpal[i * 3u];
    const uint16_t v = best_pair(c, (const uint8_t(*)[3])s_ref_rgb, (const uint16_t(*)[3])lin, 16, dither);
    s_pair[i] = v;
    const uint8_t a = v & 15u, b = (v >> 4) & 15u;
    for (uint8_t cell = 0; cell < 16u; cell++) {
      s_lut.pen[cell][i] = ((int)(v >> 8) > (int)dither_threshold(s_dither, cell)) ? b : a;
    }
  }
}

/* The palette words go to the cart slot from the publish, in the same
 * window as the pixels, never from here: the m68k latches the slot as
 * it copies the frame, and a write at any other time would be shown
 * with the wrong picture. */
static void rebuild(void) {
  s_mono = fb_st_mono();
  if (!s_have_playpal) return;
  if (s_mono) {
    build_mono(); /* no palette to publish: the m68k does not copy it */
    return;
  }
  s_lut_dirty = s_hud_dirty = true; /* the pens come from the frame: built at the publish */
}

#pragma GCC pop_options

/* ------------------------------------------------------------------ */
/* Public palette API                                                  */

void doom_video_init(void) {
  s_have_playpal = false;
  s_dither = DOOM_VIDEO_DITHER_BAYER4;
  s_convert_us = 0;
}

void doom_video_set_playpal(const uint8_t *rgb768) {
  memcpy(s_playpal, rgb768, sizeof(s_playpal));
  s_have_playpal = true;
  rebuild();
}

void doom_video_set_dither(doom_video_dither_t mode) {
  if (mode >= DOOM_VIDEO_DITHER_COUNT) return;
  s_dither = mode;
  rebuild();
}

doom_video_dither_t doom_video_get_dither(void) { return s_dither; }

void doom_video_set_status_bar(bool shown) { s_status_bar = shown; }

const char *doom_video_dither_name(doom_video_dither_t mode) {
  switch (mode) {
    case DOOM_VIDEO_DITHER_NEAREST: return "NEAREST";
    case DOOM_VIDEO_DITHER_BAYER2: return "BAYER 2X2";
    case DOOM_VIDEO_DITHER_BAYER4: return "BAYER 4X4";
    case DOOM_VIDEO_DITHER_BLUENOISE: return "BLUE NOISE";
    default: return "?";
  }
}

const uint16_t *doom_video_st_palette(void) { return s_st_colors; }

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

static unsigned s_split_pix = ~0u; /* first pixel of the bar's rows, latched per frame */

/* `pix` is the block's first pixel. The status bar's rows, with the
 * split, go through its nearest-pen table instead: the same table for
 * every cell, and no dither. */
static inline void __not_in_flash_func(doom_c2p_block)(uint32_t *dst, const uint8_t *src,
                                                       unsigned pix) {
  const uint8_t *l0, *l1, *l2, *l3;
  if (pix >= s_split_pix) {
    l0 = l1 = l2 = l3 = s_hud_pen;
  } else {
    const unsigned y = pix / FB_CHUNKED_W;
    l0 = s_lut.pen[((y & 3u) << 2) | 0u];
    l1 = s_lut.pen[((y & 3u) << 2) | 1u];
    l2 = s_lut.pen[((y & 3u) << 2) | 2u];
    l3 = s_lut.pen[((y & 3u) << 2) | 3u];
  }
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

#pragma GCC push_options
#pragma GCC optimize("Os")

/* Wait for the m68k's window and send the palette words with the frame:
 * the view's, the bar's, and the split flag. The m68k latches them as
 * it starts copying the frame, so they are shown with it. */
static void publish_begin(void) {
  fb_wait_blit_ack();
  if (s_mono) return;
  uint8_t *cart = (uint8_t *)fb_screen.framebuffer - CART_FRAMEBUFFER_OFFSET;
  palette_set(s_st_colors);
  if (s_split) {
    memcpy(cart + CART_HUD_PALETTE_OFFSET, s_hud.st, sizeof(s_hud.st));
  } else {
    memset(cart + CART_HUD_PALETTE_OFFSET, 0, sizeof(s_hud.st));
  }
  *(volatile uint16_t *)(cart + CART_SPLIT_OFFSET) = s_split;
}

#pragma GCC pop_options

static inline void __not_in_flash_func(doom_c2p_block_any)(uint32_t *dst, const uint8_t *src,
                                                           unsigned pix) {
  if (s_use_bn && pix < s_split_pix) {
    doom_c2p_block_bn(dst, src, pix % FB_CHUNKED_W, pix / FB_CHUNKED_W);
  } else {
    doom_c2p_block(dst, src, pix);
  }
}

/* Chunk k: 96 pixels = 6 blocks; block b of chunk k starts at pixel
 * k*96 + b*16, on row (that / 320). Its 8 planar bytes land at
 * fb_cart_offset(k*48 + b*8).
 *
 * Last chunk first: that is the cart FB's order, which is the order the
 * m68k copies it in from the next VBL. Core 1's half starts at the cart
 * FB's first byte and stays ahead of the copy (a chunk in ~6 us against
 * the m68k's ~26), and Core 0's half is not reached until ~9 ms into
 * the copy, so a conversion only has to start before the VBL, not
 * finish by it. */
static void __attribute__((noinline)) __not_in_flash_func(doom_c2p_chunks)(unsigned k0, unsigned k1) {
  for (unsigned k = k1; k-- > k0;) {
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

/* ST high res: 16 Doom pixels on row y -> the 32 output pixels they
 * cover in each of output rows 2y (*top) and 2y + 1 (*bot), each as a
 * uint32 of two screen words. The cart bus presents an RP halfword as
 * the m68k word at the same address, so the left word is the low half,
 * and a word's leftmost pixel is its bit 15: group g of four Doom
 * pixels lands in bits 15..8, 7..0, 31..24, 23..16 for g = 0..3.
 *
 * Not RAM-resident themselves: they are inlined into mono_rows, which
 * is, and the melt's copies stay in flash with it. RAM is the zone's. */
static inline void mono_block(uint32_t *top, uint32_t *bot, const uint8_t *src, unsigned y) {
  const uint16_t(*t)[256] = s_lut.mono[y & 1u];
  uint32_t a = 0, b = 0;
  for (unsigned g = 0; g < 4u; g++) {
    const uint32_t e = (uint32_t)t[0][src[0]] | t[1][src[1]] | t[2][src[2]] | t[3][src[3]];
    src += 4;
    const unsigned sh = (g ^ 1u) * 8u;
    a |= (e & 0xFFu) << sh;
    b |= (e >> 8) << sh;
  }
  *top = a;
  *bot = b;
}

/* Blue-noise variant, per output pixel against the tile. The block's 32
 * output pixels are one whole row of it, starting at column 0: word h
 * of each row is tile columns 16h..16h+15, Doom pixels 8h..8h+7. Kept
 * rolled; unrolled it was 1.5 KB of RAM. */
static inline void mono_block_bn(uint32_t *top, uint32_t *bot, const uint8_t *src, unsigned y) {
  const uint8_t *nt = &bluenoise[((2u * y) & (BLUENOISE_N - 1u)) * BLUENOISE_N];
  const uint8_t *nb = nt + BLUENOISE_N; /* 2y & 31 is even, so 2y + 1 is the next row */
  uint32_t a = 0, b = 0;
#pragma GCC unroll 1
  for (unsigned i = 0; i < 16u; i++) {
    const uint16_t l = s_pair[src[i]];
    const unsigned bit = 16u * (i >> 3) + 15u - 2u * (i & 7u); /* output pixel 2i */
    a |= ((uint32_t)(l <= nt[2u * i]) << bit) | ((uint32_t)(l <= nt[2u * i + 1u]) << (bit - 1u));
    b |= ((uint32_t)(l <= nb[2u * i]) << bit) | ((uint32_t)(l <= nb[2u * i + 1u]) << (bit - 1u));
  }
  *top = a;
  *bot = b;
}

static inline void mono_block_any(uint32_t *top, uint32_t *bot, const uint8_t *src, unsigned y) {
  if (s_use_bn) {
    mono_block_bn(top, bot, src, y);
  } else {
    mono_block(top, bot, src, y);
  }
}

/* Doom rows [y0, y1) into the cart FB: row y is output rows 2y and
 * 2y + 1, 80 bytes each, so 160y and 160y + 80 bytes into the image.
 * Every store is 4-byte aligned and so never straddles a 48-byte chunk.
 * One copy, shared by both cores. Bottom row first, the cart FB's
 * order (see doom_c2p_chunks). */
static void __attribute__((noinline)) __not_in_flash_func(mono_rows)(unsigned y0, unsigned y1) {
  for (unsigned y = y1; y-- > y0;) {
    const uint8_t *src = fb_chunked_buffer + y * FB_CHUNKED_W;
    for (unsigned x = 0; x < FB_CHUNKED_W; x += 16u, src += 16) {
      uint32_t top, bot;
      mono_block_any(&top, &bot, src, y);
      const uint32_t o = y * 160u + x / 4u;
      *(uint32_t *)(s_cart_fb + fb_cart_offset(o)) = top;
      *(uint32_t *)(s_cart_fb + fb_cart_offset(o + 80u)) = bot;
    }
  }
}

static void __not_in_flash_func(mono_bottom_job)(void *arg) {
  (void)arg;
  mono_rows(FB_CHUNKED_H / 2u, FB_CHUNKED_H);
}

#pragma GCC push_options
#pragma GCC optimize("Os")

void doom_video_publish(void) {
  /* The ST says which it is long before the first frame; rebuilding on
   * a change here just means the LUT and the layout can never disagree. */
  if (fb_st_mono() != s_mono) rebuild();
  s_cart_fb = (uint8_t *)fb_screen.framebuffer;
  s_use_bn = (s_dither == DOOM_VIDEO_DITHER_BLUENOISE);

  /* The adaptive pens are this frame's: chosen before the wait, in the
   * game's time rather than the m68k's slack. With the split the view
   * and the bar each get their own. */
  s_split = false;
  if (!s_mono && s_have_playpal) {
    s_split = s_status_bar;
    const unsigned view_h = s_split ? SPLIT_ROW : FB_CHUNKED_H;
    const bool mix = (s_dither != DOOM_VIDEO_DITHER_NEAREST);
    if (adapt_update(&s_view, 0u, view_h, 0u, ADAPT_ITERS, mix) || s_lut_dirty) {
      adapt_install_view();
      build_lut();
      s_lut_dirty = false;
    }
    /* What the melt will need to keep, should this frame be melted. */
    s_view_used = ADAPT_BLACK;
    for (unsigned i = 0; i < 256u; i++) {
      if (s_hist[i]) s_view_used |= (uint16_t)((1u << (s_pair[i] & 15u)) | (1u << ((s_pair[i] >> 4) & 15u)));
    }
    if (s_split && (adapt_update(&s_hud, SPLIT_ROW, FB_CHUNKED_H, 0u, ADAPT_ITERS, false) || s_hud_dirty)) {
      build_hud_pens();
      s_hud_dirty = false;
    }
  }
  s_split_pix = s_split ? SPLIT_ROW * FB_CHUNKED_W : ~0u;

  /* The cart FB is written in place, so only in the m68k's post-blit
   * slack: wait for its ack, convert on both cores, mark ready. The
   * palette goes in the same window, so the m68k latches it with these
   * pixels (see rebuild). */
  publish_begin();
  const uint32_t t0 = time_us_32();

  if (s_mono) {
    fb_core1_dispatch(mono_bottom_job, NULL);
    mono_rows(0u, FB_CHUNKED_H / 2u);
  } else {
    fb_core1_dispatch(doom_c2p_bottom_job, NULL);
    doom_c2p_chunks(0u, CART_FB_CHUNK_COUNT / 2u);
    /* Tail: the last 64 pixels, natural order at the end of the FB. */
    for (unsigned b = 0; b < CART_FB_CHUNK_TAIL / 8u; b++) {
      const unsigned pix = 2u * CART_FB_CHUNK_COVERED + b * 16u;
      doom_c2p_block_any((uint32_t *)(s_cart_fb + CART_FB_CHUNK_COVERED + b * 8u),
                         fb_chunked_buffer + pix, pix);
    }
  }
  fb_core1_wait();

  s_convert_us = time_us_32() - t0;
  fb_frame_done();

#ifdef EMUMD
  /* emu/framedump.c: the frame and its page, for offline experiments. */
  extern void emu_frame_dump(const uint8_t *frame, const uint8_t *playpal);
  emu_frame_dump(fb_chunked_buffer, s_playpal);
#endif

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

#pragma GCC pop_options

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

/* Plain RAM, not the cart hole: the full-screen size grew the
 * renderer's visplane bitmap by 1,280 bytes and the hole had 832 left,
 * so these 640 moved out to make room. */
static int16_t s_wipe_y[WIPE_COLS];   /* Doom's y[]: < 0 waiting, 200 done */
static int16_t s_wipe_off[WIPE_COLS]; /* offset drawn by the last step  */
static uint32_t s_wipe_rand;
static uint32_t s_wipe_max_us;

static uint32_t wipe_random(void) {
  s_wipe_rand ^= s_wipe_rand << 13;
  s_wipe_rand ^= s_wipe_rand >> 17;
  s_wipe_rand ^= s_wipe_rand << 5;
  return s_wipe_rand;
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

/* fb_cart_offset(y * 160 + g * 8) without the divide: (y*160)/48 is
 * (y*10)/3, and a group's 8 bytes never straddle a chunk. */
static inline void wipe_ptr_init(wipe_ptr_t *p, unsigned y, unsigned g) {
  uint32_t k = (y * 10u * 0xAAABu) >> 17;
  uint32_t r = y * 160u - k * 48u + g * 8u; /* 0..184 */
  const uint32_t add = (r >= 48u) + (r >= 96u) + (r >= 144u);
  k += add;
  r -= add * 48u;
  p->co = (CART_FB_CHUNK_COUNT - 1u - k) * CART_FB_CHUNK_BYTES + r;
  p->r = (int)r;
}

static inline uint32_t wipe_cart_off(unsigned y, unsigned g) {
  const uint32_t o = y * 160u + g * 8u;
  if (o >= (uint32_t)CART_FB_CHUNK_COVERED) return o;
  wipe_ptr_t p;
  wipe_ptr_init(&p, y, g);
  return p.co;
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

/* The melt in ST high res: the same columns, rules and bottom-up order
 * on the mono layout. A group is still 16 Doom pixels -- 8 columns -- but
 * now 32 output pixels, one uint32 of two screen words per output row; a
 * column is four output bits wide and moves two output rows for every
 * row of Doom's. Left in flash: it only runs during the melt, and the
 * arithmetic is simple enough to be well inside the slack from there. */
#define WIPE_MONO_H (2 * FB_CHUNKED_H)

static uint32_t wipe_mono_mask(unsigned c) {
  return (c < 4u) ? (0xF000u >> (4u * c)) : (0xF0000000u >> (4u * (c - 4u)));
}

static inline uint32_t *wipe_mono_word(uint8_t *fb, int yy, unsigned g) {
  return (uint32_t *)(fb + fb_cart_offset((uint32_t)yy * 80u + g * 4u));
}

static void wipe_compose_mono(unsigned g0, unsigned g1) {
  uint8_t *const fb = s_cart_fb;
  for (unsigned g = g0; g < g1; g++) {
    /* As wipe_compose_groups, in output rows. */
    int16_t on[8];
    int ymin_static = WIPE_MONO_H;
    int ymax = 0;
    unsigned nd = 0;
    int16_t dd[8], dlo[8];
    uint32_t dm[8];
    for (unsigned c = 0; c < 8u; c++) {
      int v = s_wipe_y[g * 8u + c];
      if (v < 0) v = 0;
      if (v > FB_CHUNKED_H) v = FB_CHUNKED_H;
      v *= 2;
      on[c] = (int16_t)v;
      if (v > ymax) ymax = v;
      const int prev = 2 * s_wipe_off[g * 8u + c];
      if (prev < ymin_static) ymin_static = prev;
      const int16_t d = (int16_t)(v - prev);
      if (!d) continue;
      const uint32_t m = wipe_mono_mask(c);
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
    if (!nd) continue;

    for (unsigned j = 0; j < nd; j++) {
      const uint32_t m = dm[j];
      for (int y = WIPE_MONO_H - 1; y >= dlo[j]; y--) {
        uint32_t *dst = wipe_mono_word(fb, y, g);
        *dst = (*dst & ~m) | (*wipe_mono_word(fb, y - dd[j], g) & m);
      }
    }

    for (int y = ymax - 1; y >= ymin_static; y--) {
      uint32_t mnew = 0;
      for (unsigned c = 0; c < 8u; c++) {
        if (on[c] > y) mnew |= wipe_mono_mask(c);
      }
      uint32_t top, bot;
      const unsigned sy = (unsigned)y >> 1;
      mono_block_any(&top, &bot, fb_chunked_buffer + sy * FB_CHUNKED_W + g * 16u, sy);
      const uint32_t t = (y & 1) ? bot : top;
      uint32_t *dst = wipe_mono_word(fb, y, g);
      *dst = (*dst & ~mnew) | (t & mnew);
    }
    for (unsigned c = 0; c < 8u; c++) s_wipe_off[g * 8u + c] = (int16_t)(on[c] / 2);
  }
}

/* The old frame's status bar, drawn in the bar's own pens, into the
 * melt's: every pixel of rows SPLIT_ROW..199 through s_remap, in place.
 * Once, in the first step's window, with the palette that needs it. */
static uint8_t s_remap[16];
static bool s_melt_remap;

static void remap_bar(void) {
  for (unsigned y = SPLIT_ROW; y < FB_CHUNKED_H; y++) {
    for (unsigned g = 0; g < FB_CHUNKED_W / 16u; g++) {
      uint16_t *w = (uint16_t *)(s_cart_fb + wipe_cart_off(y, g)); /* planes 0..3 */
      uint16_t out[4] = {0, 0, 0, 0};
      for (unsigned bit = 0; bit < 16u; bit++) {
        unsigned pen = 0;
        for (unsigned pl = 0; pl < 4u; pl++) pen |= ((w[pl] >> bit) & 1u) << pl;
        pen = s_remap[pen];
        for (unsigned pl = 0; pl < 4u; pl++) out[pl] |= (uint16_t)(((pen >> pl) & 1u) << bit);
      }
      memcpy(w, out, sizeof(out));
    }
  }
}

static void __not_in_flash_func(wipe_right_job)(void *arg) {
  (void)arg;
  if (s_mono) {
    wipe_compose_mono(10u, 20u);
  } else {
    wipe_compose_groups(10u, 20u);
  }
}

void doom_video_wipe_begin(void) {
  /* One palette serves both pictures, and the whole screen. The old
   * one is in the cart FB as pens, so with the adaptive source the view
   * pens it uses keep their colours for the melt, and the others are
   * chosen for the new frame. With all 16 in use the new frame is drawn
   * in the old one's; the next frame after the melt chooses its own.
   * If the old frame had the split, its bar is in the bar's pens: each
   * is mapped to the nearest of the melt's at the first step. */
  if (!s_mono && s_have_playpal) {
    if (s_view.lvl_max) {
      s_frozen = s_view_used;
      for (unsigned k = 0; k < 16u; k++) {
        if (!(s_frozen & (1u << k))) continue;
        for (unsigned ch = 0; ch < 3u; ch++) s_view.cen[k][ch] = (int16_t)(s_view.rgb[k][ch] << 4);
      }
    }
    adapt_update(&s_view, 0u, FB_CHUNKED_H, s_frozen, ADAPT_COLD_ITERS, s_dither != DOOM_VIDEO_DITHER_NEAREST);
    adapt_install_view();
    build_lut();
    s_lut_dirty = false;
    s_frozen = 0;
    s_melt_remap = s_split;
    for (unsigned k = 0; s_split && k < 16u; k++) {
      long best = 0x7FFFFFFFL;
      for (unsigned j = 0; j < 16u; j++) {
        const long d = color_dist(s_hud.rgb[k][0], s_hud.rgb[k][1], s_hud.rgb[k][2],
                                  s_view.rgb[j][0], s_view.rgb[j][1], s_view.rgb[j][2]);
        if (d < best) {
          best = d;
          s_remap[k] = (uint8_t)j;
        }
      }
    }
    s_split = false;
    s_split_pix = ~0u;
  }

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
  publish_begin();
  const uint32_t t0 = time_us_32();
  if (s_melt_remap) {
    remap_bar();
    s_melt_remap = false;
  }
  fb_core1_dispatch(wipe_right_job, NULL);
  if (s_mono) {
    wipe_compose_mono(0u, 10u);
  } else {
    wipe_compose_groups(0u, 10u);
  }
  fb_core1_wait();
  const uint32_t us = time_us_32() - t0;
  if (us > s_wipe_max_us) s_wipe_max_us = us;
  fb_frame_done();

  if (done) DPRINTF("melt done, step max %lu us\n", (unsigned long)s_wipe_max_us);
  return done;
}

#pragma GCC pop_options
