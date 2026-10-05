#!/usr/bin/env python3
#
# Copyright (C) 2026 Neil Rackett
# SPDX-License-Identifier: GPL-3.0-or-later
#
"""Doom's music as YM2149 register streams for MD/DOOM.

    ymmusic.py DOOM1.WAD OUTDIR [--songs E1M1,INTER]

writes D_<NAME>.STM for every music lump in the WAD (D_E1M1, D_INTER,
...). Copy them into the /doom folder of the SD card next to the level
packs: on a machine with STE DMA sound, which leaves the YM2149 free,
MD/DOOM plays the song for each level, the intermission and the title
from there. Without them the game simply has no music. levelpack.py
writes them too, unless given --no-music.

The songs are rendered offline by STDL's MIDI synth (stdl_ym.py, from
atarist-stdl) and stored in its STM format: 50 frames a second, each a
mask of the registers that changed and their values. This tool only
reads the MUS lumps into the events that synth takes: MUS channel 15
becomes General MIDI's drum channel, and MUS's 140 Hz ticks are MIDI
ticks at 70 to the quarter note at the default 120 bpm.
"""
import argparse
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import stdl_ym  # noqa: E402

MUS_DIVISION = 70  # ticks per quarter note: 140 a second at 500000 us/qn

# MUS controller numbers -> MIDI controllers (0 is the program change)
MUS_CONTROLLERS = {1: 0, 2: 1, 3: 7, 4: 10, 5: 11, 6: 91, 7: 93, 8: 64, 9: 67}
# MUS system events -> MIDI channel mode messages
MUS_SYSTEM = {10: 120, 11: 123, 12: 126, 13: 127, 14: 121}


def mus_events(lump):
    """Decode a MUS lump into stdl_ym's events and the song's end tick."""
    if lump[:4] != b'MUS\x1a':
        raise ValueError('not a MUS lump')
    score_len, score_start = struct.unpack_from('<HH', lump, 4)
    p, end = score_start, score_start + score_len
    tick = 0
    velocity = [127] * 16  # a note-on without a volume reuses the last
    events = []
    while p < end:
        b = lump[p]
        p += 1
        kind, mch = (b >> 4) & 7, b & 15
        ch = 9 if mch == 15 else (mch + 1 if mch >= 9 else mch)
        if kind == 0:  # release note
            events.append((tick, 'off', ch, lump[p] & 0x7F))
            p += 1
        elif kind == 1:  # play note
            note = lump[p]
            p += 1
            if note & 0x80:
                velocity[ch] = lump[p] & 0x7F
                p += 1
            note &= 0x7F
            if velocity[ch]:
                events.append((tick, 'on', ch, note, velocity[ch]))
            else:
                events.append((tick, 'off', ch, note))
        elif kind == 2:  # pitch wheel, 128 = centre
            events.append((tick, 'bend', ch, (lump[p] - 128) * 64))
            p += 1
        elif kind == 3:  # system event
            cc = MUS_SYSTEM.get(lump[p])
            if cc is not None:
                events.append((tick, 'cc', ch, cc, 0))
            p += 1
        elif kind == 4:  # controller
            num, value = lump[p], lump[p + 1] & 0x7F
            p += 2
            if num == 0:
                events.append((tick, 'prog', ch, value))
            elif num in MUS_CONTROLLERS:
                events.append((tick, 'cc', ch, MUS_CONTROLLERS[num], value))
        elif kind == 6:  # score end
            break
        else:  # 5 and 7 are unused and carry no data
            pass
        if b & 0x80:  # a delay follows
            delay = 0
            while True:
                c = lump[p]
                p += 1
                delay = (delay << 7) | (c & 0x7F)
                if not c & 0x80:
                    break
            tick += delay
    order = {'off': 1, 'cc': 2, 'prog': 3, 'bend': 4, 'on': 5}
    events.sort(key=lambda e: (e[0], order[e[1]]))
    return events, tick


def mus_to_stm(lump):
    """A MUS lump as an STM file (bytes)."""
    events, end_tick = mus_events(lump)
    frames = stdl_ym.midi_to_frames(MUS_DIVISION, events, end_tick)
    blob = stdl_ym.delta_encode(frames)
    # Doom's songs loop from the start, which delta_encode makes a full
    # set of registers, so the loop frame is 0.
    return (b'STM1' + struct.pack('>HHHH', stdl_ym.TICK_HZ, len(frames), 0, 0)
            + bytes(blob)), len(frames)


def write_songs(lumps, outdir, songs=None):
    """Write D_*.STM into outdir for the music lumps among (name, data)
    pairs, or only those named in songs (without the D_). Returns the
    total bytes written."""
    total = 0
    for name, data in lumps:
        if not name.startswith('D_') or data[:4] != b'MUS\x1a':
            continue
        if songs and name[2:] not in songs:
            continue
        stm, nframes = mus_to_stm(data)
        with open(os.path.join(outdir, name + '.STM'), 'wb') as f:
            f.write(stm)
        total += len(stm)
        print(f'{name}.STM: {nframes / stdl_ym.TICK_HZ:.1f} s, {len(stm):,} B')
    return total


def read_lumps(path):
    wad = open(path, 'rb').read()
    _, num, off = struct.unpack('<4sII', wad[:12])
    lumps = []
    for i in range(num):
        o, s, n = struct.unpack('<II8s', wad[off + 16 * i:off + 16 * i + 16])
        lumps.append((n.rstrip(b'\0').decode().upper(), wad[o:o + s]))
    return lumps


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('wad')
    ap.add_argument('outdir')
    ap.add_argument('--songs', help='comma-separated song names, e.g. E1M1,INTER (default: all)')
    args = ap.parse_args()
    os.makedirs(args.outdir, exist_ok=True)
    songs = set(s.upper() for s in args.songs.split(',')) if args.songs else None
    total = write_songs(read_lumps(args.wad), args.outdir, songs)
    print(f'{total:,} B of music')


if __name__ == '__main__':
    main()
