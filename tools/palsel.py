#!/usr/bin/env python3
# Copyright (C) 2026 Neil Rackett
# SPDX-License-Identifier: GPL-3.0-or-later
"""palsel.py -- offline experiments on the adaptive palette's selector.

Every variant chooses 16 pens for the view (rows 0..167) and 16 for the
status bar (168..199), pen 0 black in both, as doom_video.c does with the
split, and is scored by S-CIELAB against the 256-colour original (see
tools/bandpal.py, whose colour science and frame dumps it uses). The bar
is always drawn nearest-pen, as in the firmware.

  V0   the firmware before: k-means in 2:4:3 weighted RGB, each channel
       snapped to its level, the two nearest pens by redmean, the mix
       level from a projection in sRGB
  G2   the pair (among the four nearest pens) and level whose mix, in
       linear light, is nearest the colour in OKLab
  G2c  what the RP can afford of G2 (doom_video.c's best_pair): the
       level from a projection in linear light and the levels either
       side, the mix back in sRGB through a table, compared by redmean
  +refine  then the pens moved to suit the mixes (adapt_refine)

Measured on dumps from E1M1, E1M3, E1M4, E1M6, E1M7 (STE, 4x4 Bayer,
view): V0 2.99, G2 2.48, G2c 2.50, G2c + one refine pass 2.28. OKLab or
CIELAB k-means, perceptual snapping and count^p weighting were tried as
well and did not help. Without a dither G2c is worse than the nearest
pen, which the firmware keeps for that mode.

  tools/palsel.py FRAMES_DIR [ste|st] [EVERY] [bayer4|bayer2|nearest]
"""
import glob
import os
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import bandpal as bp  # noqa: E402

W = np.array([2.0, 4.0, 3.0])

# ---- OKLab -------------------------------------------------------------
M1 = np.array([[0.4122214708, 0.5363325363, 0.0514459929],
               [0.2119034982, 0.6806995451, 0.1073969566],
               [0.0883024619, 0.2817188376, 0.6299787005]])
M2 = np.array([[0.2104542553, 0.7936177850, -0.0040720468],
               [1.9779984951, -2.4285922050, 0.4505937099],
               [0.0259040371, 0.7827717662, -0.8086757660]])
def rgb_to_ok(rgb):
    lms = bp.srgb_to_lin(rgb) @ M1.T
    return np.cbrt(lms) @ M2.T
def ok_to_rgb(ok):
    lms = (ok @ np.linalg.inv(M2).T) ** 3
    return bp.lin_to_srgb(lms @ np.linalg.inv(M1).T)

# ---- the firmware's histogram ----------------------------------------
def hist(frame, y0, y1):
    h = np.zeros(256)
    for y in range(y0, y1, 2):
        row = frame[y, ((y >> 1) & 1)::2]
        np.add.at(h, row, 1)
    idx = np.nonzero(h)[0]
    return idx, h[idx]

# ---- k-means with pen 0 frozen at black, farthest-point start --------
def kmeans(pts, w, black, k=16, iters=8):
    cen = [black]
    d2 = ((pts - black) ** 2).sum(1)
    for _ in range(k - 1):
        i = int(np.argmax(w * d2))
        cen.append(pts[i].copy())
        d2 = np.minimum(d2, ((pts - pts[i]) ** 2).sum(1))
    cen = np.array(cen, float)
    for _ in range(iters):
        a = ((pts[:, None] - cen[None]) ** 2).sum(2).argmin(1)
        for j in range(1, k):
            m = a == j
            if m.any():
                cen[j] = (pts[m] * w[m, None]).sum(0) / w[m].sum()
            else:
                d = ((pts[:, None] - cen[None]) ** 2).sum(2).min(1)
                cen[j] = pts[np.argmax(w * d)]
    return cen

def levels(chip):
    n = 16 if chip == 'ste' else 8
    return np.array([bp.snap(np.array([[i * 255.0 / (n - 1)] * 3]), chip)[0, 0] for i in range(n)])

def snap_channel(rgb, chip):
    return bp.snap(rgb, chip)

def snap_perceptual(rgb, chip):
    """Of the grid colours around each centre (the levels below and above
    in each channel), the one nearest in OKLab."""
    lv = levels(chip)
    out = []
    for c in np.clip(rgb, 0, 255):
        cand = []
        for ch in range(3):
            i = np.searchsorted(lv, c[ch])
            cand.append(sorted({lv[max(i - 1, 0)], lv[min(i, len(lv) - 1)]}))
        grid = np.array(np.meshgrid(*cand, indexing='ij')).reshape(3, -1).T
        d = ((rgb_to_ok(grid) - rgb_to_ok(c[None])) ** 2).sum(1)
        out.append(grid[np.argmin(d)])
    return np.array(out, int)

# ---- pixel -> pen ------------------------------------------------------
def lut_redmean(pal, refs, dither):
    return bp.build_lut(pal, refs, dither)

def lut_ok(pal, refs, dither):
    """Two nearest pens and the projection, both in OKLab."""
    P, R = rgb_to_ok(pal), rgb_to_ok(refs)
    d = ((P[:, None] - R[None]) ** 2).sum(2)
    o = np.argsort(d, 1, kind='stable'); a, b = o[:, 0], o[:, 1]
    v = R[b] - R[a]
    t = ((P - R[a]) * v).sum(1) / np.maximum((v * v).sum(1), 1e-12)
    t16 = np.clip(np.floor(t * 16), 0, 16).astype(int)
    thr = bp.thresholds(dither)
    if thr is None:
        return np.tile(a, (16, 1))
    return np.where(t16[None] > thr[:, None], b[None], a[None])

def lut_redmean_linear(pal, refs, dither):
    """The firmware's two nearest pens (redmean), but the mix level from a
    projection in linear light, which is where the eye averages a dither."""
    n = len(refs)
    d = bp.redmean(pal[:, None, :].repeat(n, 1), refs[None, :, :].repeat(256, 0))
    o = np.argsort(d, 1, kind='stable'); a, b = o[:, 0], o[:, 1]
    P, R = bp.srgb_to_lin(pal), bp.srgb_to_lin(refs)
    v = R[b] - R[a]
    t = ((P - R[a]) * v * [0.2126, 0.7152, 0.0722]).sum(1) / np.maximum((v * v * [0.2126, 0.7152, 0.0722]).sum(1), 1e-12)
    t16 = np.clip(np.round(t * 16), 0, 16).astype(int)
    thr = bp.thresholds(dither)
    if thr is None:
        return np.tile(a, (16, 1))
    return np.where(t16[None] > thr[:, None], b[None], a[None])

def lut_bestpair(pal, refs, dither, cands=4):
    """Of the pairs among the `cands` nearest pens, the pair and level whose
    mix (in linear light, at 1/16 steps) is nearest the colour in OKLab."""
    n = len(refs)
    d = bp.redmean(pal[:, None, :].repeat(n, 1), refs[None, :, :].repeat(256, 0))
    o = np.argsort(d, 1, kind='stable')[:, :cands]
    P, R = bp.srgb_to_lin(pal), bp.srgb_to_lin(refs)
    Pok = rgb_to_ok(pal)
    ts = np.arange(17) / 16.0
    best_a = o[:, 0].copy(); best_b = o[:, 0].copy(); best_t = np.zeros(256, int)
    best_e = ((rgb_to_ok(refs[o[:, 0]]) - Pok) ** 2).sum(1)
    for i in range(cands):
        for j in range(i + 1, cands):
            A, B = o[:, i], o[:, j]
            mix = R[A][:, None, :] * (1 - ts[None, :, None]) + R[B][:, None, :] * ts[None, :, None]
            mok = np.cbrt(np.clip(mix @ M1.T, 0, None)) @ M2.T
            e = ((mok - Pok[:, None, :]) ** 2).sum(2)
            k = e.argmin(1); ek = e[np.arange(256), k]
            better = ek < best_e
            best_e = np.where(better, ek, best_e); best_a = np.where(better, A, best_a)
            best_b = np.where(better, B, best_b); best_t = np.where(better, k, best_t)
    thr = bp.thresholds(dither)
    if thr is None:
        return np.tile(np.where(best_t >= 8, best_b, best_a), (16, 1))
    return np.where(best_t[None] > thr[:, None], best_b[None], best_a[None])

LIN2SRGB = np.round(bp.lin_to_srgb(np.arange(1024) / 1023.0)).astype(int)
def lut_bestpair_cheap(pal, refs, dither, cands=4):
    """As lut_bestpair, but what the RP can afford: for each pair the mix
    level comes from a projection in linear light (plus the levels either
    side), and the error is redmean between the colour and the mix taken
    back to sRGB through a 1024-entry table."""
    n = len(refs)
    d = bp.redmean(pal[:, None, :].repeat(n, 1), refs[None, :, :].repeat(256, 0))
    o = np.argsort(d, 1, kind='stable')[:, :cands]
    P, R = bp.srgb_to_lin(pal), bp.srgb_to_lin(refs)
    best = (d[np.arange(256), o[:, 0]].astype(float), o[:, 0].copy(), o[:, 0].copy(), np.zeros(256, int))
    be, ba, bb, bt = [x.copy() for x in best]
    for i in range(cands):
        for j in range(i + 1, cands):
            A, B = o[:, i], o[:, j]
            v = R[B] - R[A]
            t = ((P - R[A]) * v).sum(1) / np.maximum((v * v).sum(1), 1e-12)
            t0 = np.clip(np.round(t * 16), 0, 16).astype(int)
            for dt in (-1, 0, 1):
                tt = np.clip(t0 + dt, 0, 16)
                mix = R[A] * (1 - tt[:, None] / 16.0) + R[B] * (tt[:, None] / 16.0)
                m = LIN2SRGB[np.clip(np.round(mix * 1023), 0, 1023).astype(int)]
                e = bp.redmean(pal, m).astype(float)
                better = e < be
                be = np.where(better, e, be); ba = np.where(better, A, ba)
                bb = np.where(better, B, bb); bt = np.where(better, tt, bt)
    thr = bp.thresholds(dither)
    if thr is None:
        return np.tile(np.where(bt >= 8, bb, ba), (16, 1))
    return np.where(bt[None] > thr[:, None], bb[None], ba[None])

def refine(pal, idx, w, refs, chip, rounds=3):
    """Move the pens to suit dithering: each colour takes its best pair and
    level (lut_bestpair_cheap's rule), then each pen moves, in linear
    light, to the weighted least-squares best for the mixes it is in."""
    refs = refs.astype(float)
    for _ in range(rounds):
        R = bp.srgb_to_lin(np.clip(refs, 0, 255))
        lut = lut_bestpair_cheap(pal, np.clip(np.round(refs), 0, 255).astype(int), 'bayer4')
        # recover pair and level per index from the 16 cells
        P = bp.srgb_to_lin(pal[idx])
        num = np.zeros((16, 3)); den = np.zeros(16)
        cells = lut[:, idx]                       # 16 x n pens
        for n_i in range(len(idx)):
            col = cells[:, n_i]
            a, b = col.min(), col.max()
            if a == b:
                num[a] += w[n_i] * P[n_i]; den[a] += w[n_i]
                continue
            t = (col == b).mean()
            # c ~ (1-t) A + t B: solve for each end with the other fixed
            num[a] += w[n_i] * (1 - t) * (P[n_i] - t * R[b]); den[a] += w[n_i] * (1 - t) ** 2
            num[b] += w[n_i] * t * (P[n_i] - (1 - t) * R[a]); den[b] += w[n_i] * t ** 2
        for k in range(1, 16):
            if den[k] > 0:
                refs[k] = bp.lin_to_srgb(np.clip(num[k] / den[k], 0, 1))
        refs = bp.snap(refs, chip).astype(float); refs[0] = 0
    return refs.astype(int)

def render(pal, frame, view_refs, hud_refs, lutf, dither, hud_nearest=True):
    out = np.zeros(frame.shape + (3,), int)
    lv = lutf(pal, view_refs, dither)
    pv = lv[bp.CELL[:168], frame[:168]]
    out[:168] = view_refs[pv]
    lh = lut_redmean(pal, hud_refs, 'nearest')
    ph = lh[bp.CELL[168:], frame[168:]]
    out[168:] = hud_refs[ph]
    return out

# ---- selectors -----------------------------------------------------------
def select(pal, frame, y0, y1, chip, space, snapf, power):
    idx, w = hist(frame, y0, y1)
    w = w ** power
    if space == 'rgbw':
        pts = pal[idx] * np.sqrt(W); black = np.zeros(3)
        cen = kmeans(pts, w, black) / np.sqrt(W)
    elif space == 'lin':
        pts = bp.srgb_to_lin(pal[idx]) * np.sqrt([0.2126, 0.7152, 0.0722]) * 255; black = np.zeros(3)
        cen = bp.lin_to_srgb(kmeans(pts, w, black) / np.sqrt([0.2126, 0.7152, 0.0722]) / 255)
    elif space == 'ok':
        pts = rgb_to_ok(pal[idx]); black = rgb_to_ok(np.zeros((1, 3)))[0]
        cen = ok_to_rgb(kmeans(pts, w, black))
    else:
        pts = bp.rgb_to_lab(pal[idx]); black = np.zeros(3)
        cen = bp.lab_to_rgb(kmeans(pts, w, black))
    refs = snapf(np.clip(cen, 0, 255), chip)
    refs[0] = 0
    return refs

VARIANTS = {
    'V0 firmware before (wRGB, channel snap, redmean)': ('rgbw', snap_channel, 1.0, lut_redmean),
    'G2 best pair, linear mix, OKLab error':            ('rgbw', snap_channel, 1.0, lut_bestpair),
    'G2c best pair, cheap (doom_video.c best_pair)':    ('rgbw', snap_channel, 1.0, lut_bestpair_cheap),
    'G2c + refine x1 (adapt_refine)':                    ('rgbw', snap_channel, 1.0, lut_bestpair_cheap),
}

if __name__ == '__main__':
    frames_dir = sys.argv[1]
    chip = sys.argv[2] if len(sys.argv) > 2 else 'ste'
    every = int(sys.argv[3]) if len(sys.argv) > 3 else 4
    DITHER = sys.argv[4] if len(sys.argv) > 4 else 'bayer4'
    fs = sorted(glob.glob(os.path.join(frames_dir, '*.bin')))[::every]
    res = {k: [] for k in VARIANTS}
    for n, f in enumerate(fs):
        pal, frame = bp.load(f)
        if not bp.has_status_bar(pal, frame):
            continue
        ref = bp.scielab(pal[frame], 15)
        for name, (space, snapf, power, lutf) in VARIANTS.items():
            vr = select(pal, frame, 0, 168, chip, space, snapf, power)
            if 'refine' in name:
                idx, w = hist(frame, 0, 168)
                vr = refine(pal, idx, w, vr, chip, rounds=1)
            hr = select(pal, frame, 168, 200, chip, space, snapf, power)
            shown = render(pal, frame, vr, hr, lutf, DITHER)
            de = np.sqrt(((bp.scielab(shown, 15) - ref) ** 2).sum(-1))
            res[name].append((de[:168].mean(), de[168:].mean(), np.percentile(de[:168], 95)))
        print('\r%d/%d' % (n + 1, len(fs)), end='', flush=True)
    print()
    print('%-44s %8s %8s %8s' % ('variant (%s, %s)' % (chip, DITHER), 'view dE', 'bar dE', 'view p95'))
    for name, v in res.items():
        a = np.array(v)
        print('%-44s %8.2f %8.2f %8.2f' % (name, a[:, 0].mean(), a[:, 1].mean(), a[:, 2].mean()))
