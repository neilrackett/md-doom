#!/usr/bin/env python3
# Copyright (C) 2026 Neil Rackett
# SPDX-License-Identifier: GPL-3.0-or-later
"""bandpal.py -- offline test of raster palette bands.

Would switching the ST palette every 8 raster lines, so each band gets
pens chosen for its own content, reduce the dither enough to be worth
building? This answers it on real frames before anything touches
userfw.s or doom_video.c.

Frames come from the EmuMD build (emu/framedump.c): run it with
MDDOOM_FRAMEDUMP=DIR and each dump is the 768-byte PLAYPAL page then
the 320x200 frame of indices. Every variant goes through a copy of
doom_video.c's reducer (two nearest pens by redmean, the colour
projected onto the line between them, compared against the cell's
threshold), so only the pens differ:

  today      STDOOM's 16 PLAYPAL indices, one palette (the firmware now)
  playpal16  16 pens for the whole of PLAYPAL, unweighted (the firmware's
             GENERATED source, near enough)
  frame16    16 pens chosen for the whole frame: the control -- what a
             better palette gets without any raster work
  band16/H   16 pens per H-row band of the view, 16 for the status bar;
             band16/168 is the view and the status bar, one palette each
  band8+8/H  8 pens for the view, 8 more per band, 16 for the status bar

Pens are chosen by weighted k-means in CIELAB over the PLAYPAL indices
the region actually uses, then snapped to what the chip displays (STE:
16 levels a channel, ST: 8). Quality is S-CIELAB-style: both pictures
are blurred in an opponent colour space the way the eye blurs them at
a viewing distance (--ppd, pixels per degree), then compared as CIELAB
Delta E. Lower is better; about 1 is a just-noticeable difference.

The seam: a Timer-B interrupt fires at the end of a line's display and
has ~190 cycles to the next; the handler can be late by a MOVEM, an
exception and any other MFP handler, so the left of each band's first
row may still show the previous band's pens. --seam N simulates a
switch point uniform in [0, N) pixels, and the 8+8 variant can draw
that stretch with the 8 frame pens only (the "g8 seam" rows).

Pens chosen afresh every frame can flicker. --temporal FIRST:LAST
takes a run of consecutive dumps (MDDOOM_FRAMEDUMP_EVERY=1) and
measures how much the still parts of the picture change from frame to
frame, for pens chosen from scratch, from the last frame's centres, and
with hysteresis on the snapping.

  tools/bandpal.py DIR [--out OUT] [--every N] [--samples 0,10,...]
  tools/bandpal.py DIR --temporal 300:560
"""

import argparse
import glob
import os

import numpy as np
from PIL import Image
from scipy.ndimage import gaussian_filter, maximum_filter

SUBSET = [0, 201, 205, 117, 123, 185, 87, 100, 60, 95, 227, 104, 69, 79, 179, 159]
BAYER4 = np.array([0, 8, 2, 10, 12, 4, 14, 6, 3, 11, 1, 9, 15, 7, 13, 5])
BAYER2 = np.array([2, 10, 14, 6])
VIEW_H = 168

YY, XX = np.mgrid[0:200, 0:320]
CELL = ((YY & 3) << 2) | (XX & 3)


def thresholds(dither):
    """Per-cell threshold, as doom_video.c's dither_threshold."""
    cells = np.arange(16)
    if dither == 'bayer4':
        return BAYER4[cells]
    if dither == 'bayer2':
        return BAYER2[(((cells >> 2) & 1) << 1) | (cells & 1)]
    return None  # nearest


# ---------------------------------------------------------------------
# The chip

def snap(rgb, chip):
    """Arbitrary colours (floats 0..255) -> what the chip displays."""
    rgb = np.clip(np.asarray(rgb, dtype=float), 0, 255)
    if chip == 'ste':
        return (np.rint(rgb / 17.0) * 17).astype(int)
    return np.rint(np.rint(rgb * 7 / 255.0) * 255 / 7.0).astype(int)


def displayed(rgb, chip):
    """A PLAYPAL colour written as the firmware writes it, then shown."""
    rgb = np.asarray(rgb, dtype=int)
    if chip == 'ste':
        return (rgb >> 4) * 17
    return np.rint((rgb >> 5) * 255 / 7.0).astype(int)


# ---------------------------------------------------------------------
# The reducer, as doom_video.c's rebuild()

def redmean(c, r):
    c = c.astype(np.int64)
    r = r.astype(np.int64)
    rmean = (c[..., 0] + r[..., 0]) // 2
    dr, dg, db = (c - r).transpose(2, 0, 1) if c.ndim == 3 else (c - r).T
    return (((512 + rmean) * dr * dr) >> 8) + 4 * dg * dg + (((767 - rmean) * db * db) >> 8)


def build_lut(pal, refs, dither):
    """pal (256,3) PLAYPAL, refs (n,3) displayed pens -> (16 cells, 256) pens."""
    n = len(refs)
    d = redmean(pal[:, None, :].repeat(n, 1), refs[None, :, :].repeat(256, 0))
    order = np.argsort(d, axis=1, kind='stable')
    a = order[:, 0]
    b = order[:, 1] if n > 1 else a
    ra, rb = refs[a].astype(np.int64), refs[b].astype(np.int64)
    dv = rb - ra
    denom = (dv * dv).sum(1)
    dot = ((pal.astype(np.int64) - ra) * dv).sum(1)
    t16 = np.where((denom <= 0) | (dot <= 0), 0,
                   np.where(dot >= denom, 16, (dot * 16) // np.maximum(denom, 1)))
    thr = thresholds(dither)
    if thr is None:
        return np.tile(a, (16, 1))
    return np.where(t16[None, :] > thr[:, None], b[None, :], a[None, :])


# ---------------------------------------------------------------------
# Colour science

def srgb_to_lin(c):
    c = np.asarray(c, dtype=float) / 255.0
    return np.where(c <= 0.04045, c / 12.92, ((c + 0.055) / 1.055) ** 2.4)


def lin_to_srgb(c):
    c = np.clip(c, 0, 1)
    return 255.0 * np.where(c <= 0.0031308, c * 12.92, 1.055 * c ** (1 / 2.4) - 0.055)


M_XYZ = np.array([[0.4124, 0.3576, 0.1805], [0.2126, 0.7152, 0.0722], [0.0193, 0.1192, 0.9505]])
WHITE = M_XYZ.sum(1)


def xyz_to_lab(xyz):
    t = xyz / WHITE
    f = np.where(t > (6 / 29) ** 3, np.cbrt(t), t / (3 * (6 / 29) ** 2) + 4 / 29)
    return np.stack([116 * f[..., 1] - 16, 500 * (f[..., 0] - f[..., 1]),
                     200 * (f[..., 1] - f[..., 2])], -1)


def lab_to_xyz(lab):
    fy = (lab[..., 0] + 16) / 116
    f = np.stack([fy + lab[..., 1] / 500, fy, fy - lab[..., 2] / 200], -1)
    t = np.where(f > 6 / 29, f ** 3, 3 * (6 / 29) ** 2 * (f - 4 / 29))
    return t * WHITE


def rgb_to_lab(rgb):
    return xyz_to_lab(srgb_to_lin(rgb) @ M_XYZ.T)


def lab_to_rgb(lab):
    return lin_to_srgb(lab_to_xyz(lab) @ np.linalg.inv(M_XYZ).T)


# S-CIELAB (Zhang & Wandell): XYZ -> opponent, a sum of Gaussians per
# channel, back to XYZ, then CIELAB. Spreads in degrees of visual angle.
M_OPP = np.array([[0.279, 0.72, -0.107], [-0.449, 0.29, -0.077], [0.086, -0.59, 0.501]])
KERNELS = [[(1.003, 0.05), (0.114, 0.225)],          # luminance (7-degree lobe dropped)
           [(0.616, 0.0685), (0.384, 0.826)],        # red-green
           [(0.567, 0.0920), (0.433, 0.6451)]]       # blue-yellow


def scielab(rgb, ppd):
    opp = (srgb_to_lin(rgb) @ M_XYZ.T) @ M_OPP.T
    out = np.empty_like(opp)
    for ch, ks in enumerate(KERNELS):
        wsum = sum(w for w, _ in ks)
        out[..., ch] = sum(w / wsum * gaussian_filter(opp[..., ch], s * ppd, mode='nearest')
                           for w, s in ks)
    return xyz_to_lab(out @ np.linalg.inv(M_OPP).T)


# ---------------------------------------------------------------------
# Choosing pens

def kmeans(lab, w, k, frozen=None, iters=12, init=None):
    """Weighted k-means; `frozen` centres take part but do not move;
    `init` starts the free ones somewhere (the last frame's) rather than
    from scratch. Returns the k - len(frozen) free centres (Lab)."""
    frozen = np.zeros((0, 3)) if frozen is None else np.asarray(frozen, dtype=float)
    nfree = k - len(frozen)
    if nfree <= 0:
        return np.zeros((0, 3))
    if len(lab) <= nfree:
        extra = np.repeat(lab[:1], nfree - len(lab), 0) if len(lab) else np.zeros((nfree, 3))
        return np.concatenate([lab, extra])
    if init is not None:
        return _iterate(lab, w, frozen, np.array(init, dtype=float), iters)
    # Deterministic weighted farthest-point start.
    cen = list(frozen)
    free = []
    d2 = np.full(len(lab), np.inf)
    for c in cen:
        d2 = np.minimum(d2, ((lab - c) ** 2).sum(1))
    for _ in range(nfree):
        i = int(np.argmax(w if not cen and not free else w * d2))
        free.append(lab[i].copy())
        d2 = np.minimum(d2, ((lab - lab[i]) ** 2).sum(1))
    return _iterate(lab, w, frozen, np.array(free), iters)


def _iterate(lab, w, frozen, free, iters):
    nfree = len(free)
    for _ in range(iters):
        allc = np.concatenate([frozen, free])
        dist = ((lab[:, None, :] - allc[None, :, :]) ** 2).sum(2)
        lab_of = dist.argmin(1) - len(frozen)
        for j in range(nfree):
            m = lab_of == j
            if m.any():
                free[j] = (lab[m] * w[m, None]).sum(0) / w[m].sum()
            else:  # re-seed an empty cluster on the worst-served colour
                free[j] = lab[np.argmax(w * dist.min(1))]
    return free


def region_hist(frame, mask):
    idx, cnt = np.unique(frame[mask], return_counts=True)
    return idx, cnt.astype(float)


def pens_for(pal_lab, frame, mask, k, chip, frozen_rgb=None):
    idx, w = region_hist(frame, mask)
    frozen_lab = None if frozen_rgb is None else rgb_to_lab(frozen_rgb)
    free = kmeans(pal_lab[idx], w, k, frozen_lab)
    return snap(lab_to_rgb(free), chip)


# ---------------------------------------------------------------------
# Frames and variants

def load(path):
    b = open(path, 'rb').read()
    pal = np.frombuffer(b[:768], np.uint8).reshape(256, 3).astype(int)
    frame = np.frombuffer(b[768:768 + 64000], np.uint8).reshape(200, 320)
    return pal, frame


def has_status_bar(pal, frame):
    rgb = pal[frame[VIEW_H:]]
    return (np.ptp(rgb, axis=2) == 0).mean() > 0.5


def band_rows(hud, h):
    """Row ranges, one palette each."""
    top = VIEW_H if hud else 200
    rows = [(y, min(y + h, top)) for y in range(0, top, h)]
    if hud:
        rows.append((VIEW_H, 200))
    return rows


def palettes(variant, pal, frame, chip, hud):
    """-> list of (row range, displayed pens (n,3)); the frame pens for 8+8."""
    pal_lab = rgb_to_lab(pal)
    if variant == 'today':
        return [((0, 200), displayed(pal[SUBSET], chip))], None
    if variant == 'frame16':
        return [((0, 200), pens_for(pal_lab, frame, np.ones_like(frame, bool), 16, chip))], None
    if variant == 'playpal16':  # like the firmware's GENERATED: all 256, unweighted
        free = kmeans(pal_lab, np.ones(256), 16)
        return [((0, 200), snap(lab_to_rgb(free), chip))], None
    variant, h = variant.split('/')
    rows = band_rows(hud, int(h))
    view = np.zeros_like(frame, bool)
    view[:VIEW_H if hud else 200] = True
    g8 = pens_for(pal_lab, frame, view, 8, chip) if variant == 'band8+8' else None
    out = []
    for (y0, y1) in rows:
        m = np.zeros_like(frame, bool)
        m[y0:y1] = True
        if variant == 'band16' or (hud and y0 == VIEW_H):
            out.append(((y0, y1), pens_for(pal_lab, frame, m, 16, chip)))
        else:
            out.append(((y0, y1), np.concatenate([g8, pens_for(pal_lab, frame, m, 16, chip, g8)])))
    return out, g8


def render(pal, frame, bands, dither, seam=0, g8=None, rng=None):
    """Reduce `frame` and show it as the ST would. With `seam`, the first
    row of each band switches palette at a random x in [0, seam); with g8
    as well, the RP draws [0, seam) of those rows with the frame pens."""
    luts = [build_lut(pal, refs, dither) for _, refs in bands]
    pens = np.zeros(frame.shape, int)
    shown = np.zeros(frame.shape + (3,), int)
    for i, ((y0, y1), refs) in enumerate(bands):
        sl = slice(y0, y1)
        pens[sl] = luts[i][CELL[sl], frame[sl]]
    g8lut = build_lut(pal, g8, dither) if (seam and g8 is not None) else None
    for i, ((y0, y1), refs) in enumerate(bands):
        if seam and i > 0:
            # g8 pens are slots 0..7 in every band but the status bar's
            if g8lut is not None and np.array_equal(refs[:8], g8):
                pens[y0, :seam] = g8lut[CELL[y0, :seam], frame[y0, :seam]]
            x = int(rng.integers(0, seam))
            shown[y0, :x] = bands[i - 1][1][pens[y0, :x]]
            shown[y0, x:] = refs[pens[y0, x:]]
            shown[y0 + 1:y1] = refs[pens[y0 + 1:y1]]
        else:
            shown[y0:y1] = refs[pens[y0:y1]]
    return shown


class Tracker:
    """Pens for a run of frames, the way the firmware would have to
    choose them: per region, from the last frame's centres with a few
    iterations (`iters`), and a pen kept as it was unless its new
    colour is more than `hold` Delta E away. iters=None chooses every
    frame from scratch. `sticky` snaps with hysteresis instead: a
    channel keeps its level until the centre is more than 0.5 + sticky
    of a level away from it."""

    def __init__(self, h, chip, iters, hold, sticky=0.0):
        self.h, self.chip, self.iters, self.hold, self.sticky = h, chip, iters, hold, sticky
        self.cen, self.pens = {}, {}

    def palettes(self, pal, frame, hud):
        pal_lab = rgb_to_lab(pal)
        out = []
        for key, (y0, y1) in enumerate(band_rows(hud, self.h)):
            idx, w = region_hist(frame[y0:y1], np.ones((y1 - y0, 320), bool))
            init = None if self.iters is None else self.cen.get((hud, key))
            cen = kmeans(pal_lab[idx], w, 16, iters=12 if init is None else self.iters, init=init)
            self.cen[(hud, key)] = cen
            pens = snap(lab_to_rgb(cen), self.chip)
            old = self.pens.get((hud, key))
            if old is not None and self.sticky:
                step = 17.0 if self.chip == 'ste' else 255 / 7.0
                c = np.clip(lab_to_rgb(cen), 0, 255)
                keep = np.abs(c - old) < step * (0.5 + self.sticky)
                pens = np.where(keep, old, pens)
            if old is not None and self.hold:
                near = np.sqrt(((rgb_to_lab(pens) - rgb_to_lab(old)) ** 2).sum(1)) <= self.hold
                pens = np.where(near[:, None], old, pens)
            self.pens[(hud, key)] = pens
            out.append(((y0, y1), pens))
        return out


def temporal(files, args):
    """Flicker: how much the still parts of the picture change from one
    frame to the next. Per pixel whose index did not change, the mean
    Delta E and the share that changed colour at all; then as the eye
    sees it (S-CIELAB) where nothing within 12 pixels moved, the mean
    and the 99th percentile."""
    trackers = {'today': None}
    for h in (200, 168, 8):
        name = {200: 'frame16', 168: 'band16/168', 8: 'band16/8'}[h]
        trackers[name + ' cold'] = Tracker(h, 'ste', None, 0)
        trackers[name + ' warm2'] = Tracker(h, 'ste', 2, 0)
        trackers[name + ' warm2 sticky.3'] = Tracker(h, 'ste', 2, 0, 0.3)
        trackers[name + ' warm2 sticky.6'] = Tracker(h, 'ste', 2, 0, 0.6)
    prev = {}
    stats = {k: [] for k in trackers}
    for fi, path in enumerate(files):
        pal, frame = load(path)
        hud = has_status_bar(pal, frame)
        ref = scielab(pal[frame], args.ppd)
        for name, tr in trackers.items():
            if tr is None:
                bands = [((0, 200), displayed(pal[SUBSET], 'ste'))]
            else:
                if tr.h == 200:  # one palette for the frame: 'hud' off
                    bands = tr.palettes(pal, frame, False)
                else:
                    bands = tr.palettes(pal, frame, hud)
            shown = render(pal, frame, bands, 'bayer4')
            blur = scielab(shown, args.ppd)
            q = np.sqrt(((blur - ref) ** 2).sum(-1)).mean()
            if name in prev:
                pframe, pshown, pblur = prev[name]
                still = pframe == frame
                calm = ~maximum_filter(~still, size=25)
                if still.any():
                    d = np.sqrt(((rgb_to_lab(shown[still]) - rgb_to_lab(pshown[still])) ** 2).sum(-1))
                    db = np.sqrt(((blur - pblur) ** 2).sum(-1))[calm] if calm.any() else np.zeros(1)
                    stats[name].append((q, d.mean(), (d > 0).mean(), db.mean(), np.percentile(db, 99)))
            prev[name] = (frame, shown, blur)
        print('\r%d/%d' % (fi + 1, len(files)), end='', flush=True)
    print()
    print('%-28s %8s %10s %10s %10s %10s' % ('variant (STE, bayer4)', 'mean dE', 'flicker dE', 'changed %',
                                            'blur flick', 'p99 flick'))
    for name, v in stats.items():
        a = np.array(v)
        print('%-28s %8.2f %10.3f %10.2f %10.3f %10.3f' % (name, a[:, 0].mean(), a[:, 1].mean(), 100 * a[:, 2].mean(),
                                                     a[:, 3].mean(), a[:, 4].mean()))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('dir')
    ap.add_argument('--out', default='bandpal-out')
    ap.add_argument('--every', type=int, default=1, help='use every Nth frame')
    ap.add_argument('--ppd', type=float, default=15.0, help='pixels per degree of visual angle')
    ap.add_argument('--chips', default='ste,st')
    ap.add_argument('--dithers', default='nearest,bayer2,bayer4')
    ap.add_argument('--variants', default='today,frame16,band16/168,band16/8,band8+8/8')
    ap.add_argument('--seam', type=int, default=0, help='also test a late palette switch of up to N px')
    ap.add_argument('--samples', default='', help='frame numbers to save side-by-side pictures of')
    ap.add_argument('--temporal', default='', help='FIRST:LAST: consecutive frames, measure flicker')
    args = ap.parse_args()
    os.makedirs(args.out, exist_ok=True)
    files = sorted(glob.glob(os.path.join(args.dir, '*.bin')))
    if args.temporal:
        first, last = (int(x) for x in args.temporal.split(':'))
        return temporal(files[first:last], args)
    files = files[::args.every]
    samples = set(int(s) for s in args.samples.split(',') if s)
    variants = args.variants.split(',')
    dithers = args.dithers.split(',')
    rows = {}
    rng = np.random.default_rng(1)
    for fi, path in enumerate(files):
        pal, frame = load(path)
        hud = has_status_bar(pal, frame)
        ref = scielab(pal[frame], args.ppd)
        for chip in args.chips.split(','):
            pics = []
            for v in variants:
                bands, g8 = palettes(v, pal, frame, chip, hud)
                for d in dithers:
                    tests = [('', 0, None)]
                    if args.seam and v.startswith('band'):
                        tests.append((' seam', args.seam, None))
                        if g8 is not None:
                            tests.append((' g8 seam', args.seam, g8))
                    for tag, seam, gg in tests:
                        shown = render(pal, frame, bands, d, seam, gg, rng)
                        de = np.sqrt(((scielab(shown, args.ppd) - ref) ** 2).sum(-1))
                        rows.setdefault((chip, v + tag, d), []).append((de.mean(), np.percentile(de, 95)))
                        if fi in samples and not tag:
                            pics.append(((v, d), shown))
            if pics:
                save_sheet(os.path.join(args.out, 'frame%03d_%s.png' % (fi, chip)), pal[frame], pics)
        print('\r%d/%d' % (fi + 1, len(files)), end='', flush=True)
    print()
    print('%-5s %-18s %-8s %8s %8s' % ('chip', 'variant', 'dither', 'mean dE', 'p95 dE'))
    for (chip, v, d), vals in rows.items():
        a = np.array(vals)
        print('%-5s %-18s %-8s %8.2f %8.2f' % (chip, v, d, a[:, 0].mean(), a[:, 1].mean()))


def save_sheet(path, ref, pics, scale=2):
    """The original, then one row per variant, one column per dither."""
    variants = list(dict.fromkeys(v for (v, _), _ in pics))
    dithers = list(dict.fromkeys(d for (_, d), _ in pics))
    W, H = 320 * scale, 200 * scale
    sheet = Image.new('RGB', (W * len(dithers), H * (len(variants) + 1)))
    sheet.paste(Image.fromarray(ref.astype(np.uint8)).resize((W, H), Image.NEAREST), (0, 0))
    for (v, d), img in pics:
        im = Image.fromarray(img.astype(np.uint8)).resize((W, H), Image.NEAREST)
        sheet.paste(im, (dithers.index(d) * W, (variants.index(v) + 1) * H))
    sheet.save(path)


if __name__ == '__main__':
    main()
