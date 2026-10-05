# STDL - Planar Display Library for Atari ST
# Copyright (C) 2026 Neil Rackett
# SPDX-License-Identifier: LGPL-2.1-or-later
"""STDL's MIDI to YM2149 synth, carried into MD/DOOM.

Copied from atarist-stdl's tools/stdlconv/stdlconv.py (commit 7c10ffa,
the "midi" subcommand) so that MD/DOOM's songs are rendered by the same
synth STDL games use. Two changes, both marked MD/DOOM: midi_to_frames
takes events rather than a file, and can be given the song's exact end.
Keep the rest in step with STDL rather than tuning it here.
"""

import math
import struct
import sys


def die(msg):
    sys.exit("stdl_ym: " + msg)


# MIDI -> YM register stream (STM)
#
# A General MIDI score reduced to the YM2149's 3 square voices and
# noise, tuned by ear against a GeneralUser GS rendering of a game
# score. Each instrument (channel) gets one voice before any gets
# two, going by its General MIDI program: the bass first, then the
# melodic instruments, then the pads, and only then a second note
# of a chord; a part plays its lowest note if a bass, else its
# highest, and a note keeps the voice it has. Notes below C2 play an
# octave up (a lower square wave is a buzz more than a note), with a
# short accent, a settle on long melodic notes, a swell on pads, a
# fade on plucked and struck notes and a vibrato on held ones. Drums
# are short noise or falling-tone bursts over a free voice, else
# over the bass (a kick or tom) or the least important note, so the
# parts play on. Levels follow the perceived loudness of each
# program and octave. The synth runs on a 120Hz clock, sampled at
# the stream's frame rate.

YM_CLOCK = 125000            # 2MHz / 16: tone period unit
TICK_HZ = 50
SYNTH_HZ = 120               # the clock the times below count in

NOTE_FLOOR = 36              # C2: lower notes play an octave up
ACCENT = 6                   # ticks of the accent starting a note
SETTLE = 30                  # ticks after which a melodic note settles
SWELL = 6                    # ticks per level of a pad's swell, 3 levels
VIBRATO_DELAY = 40           # ticks before a held note gets a vibrato
VIBRATO_TOP = 80             # notes from G#5 up get none
CRASH_OVER = 8               # ticks a cymbal may replace a melody/bass
HEADROOM = 2                 # levels under full scale, so the loudest
                             # program still fits in 15
VIBRATO = (0, 1, 2, 1, 0, -1, -2, -1)   # in 1/16 semitone

# levels of a note velocity, and taken off by a channel's volume or
# expression: about 3dB a level
VEL_LEVEL = [0] + [max(0, 15 - round(-40 * math.log10(v / 127) / 3))
                   for v in range(1, 128)]
ATTENUATION = [15] + [min(15, round(-40 * math.log10(v / 127) / 3))
                      for v in range(1, 128)]

# what a General MIDI program is, for the voice allocation
PAD, LEAD, BASS = 1, 2, 3
PRIORITY = 0x07
DECAY = 0x08                 # plucked or struck: the note fades
DRUM = 0x10                  # percussion: played as a drum
_L, _LD, _B, _P, _D = LEAD, LEAD | DECAY, BASS, PAD, DRUM
PROGRAMS = ([_LD] * 8                             # pianos
            + [_LD] * 8                           # chromatic percussion
            + [_L] * 8                            # organs
            + [_LD] * 8                           # guitars
            + [_B] * 8                            # basses
            + [_L, _L, _L, _B, _L, _LD, _LD, _D]  # strings, harp, timpani
            + [_P] * 7 + [_LD]                    # ensembles, orchestra hit
            + [_L] * 32                           # brass, reeds, pipes, leads
            + [_P] * 16                           # synth pads and effects
            + [_LD] * 5 + [_L] * 3                # ethnic
            + [_D] * 8                            # percussive
            + [0] * 8)                            # sound effects: left out

KICK, SNARE, CLAP, TOM, HIHAT, OPEN_HIHAT, CYMBAL, CLICK = range(8)
# noise period (0 none), ticks, level lost per tick in 1/4, level
# added to the velocity's, tone period (0 none), period added per
# tick (1 = a 16th of the period): kick and tom are falling tones
DRUM_KINDS = ((0, 8, 4, 0, 400, 160), (10, 10, 5, 0, 0, 0),
              (6, 5, 8, -1, 0, 0), (0, 12, 4, 0, 700, 1),
              (1, 3, 10, -3, 0, 0), (1, 14, 3, -3, 0, 0),
              (2, 36, 1, -1, 0, 0), (4, 4, 8, -2, 0, 0))
# the drum of General MIDI percussion notes 35 to 81
DRUM_NOTES = (
    KICK, KICK, CLAP, SNARE, CLAP, SNARE,                          # 35
    TOM, HIHAT, TOM, HIHAT, TOM, OPEN_HIHAT,                       # 41
    TOM, TOM, CYMBAL, TOM, OPEN_HIHAT, CYMBAL,                     # 47
    OPEN_HIHAT, HIHAT, CYMBAL, CLICK, CYMBAL, CLICK,               # 53
    OPEN_HIHAT, CLICK, CLICK, CLICK, CLICK, CLICK,                 # 59
    CLICK, CLICK, CLICK, CLICK, HIHAT, HIHAT,                      # 65
    CLICK, CLICK, HIHAT, HIHAT, CLICK, CLICK,                      # 71
    CLICK, CLICK, CLICK, HIHAT, HIHAT)                             # 77

# level of each program at C1 C2 .. C7 next to a lead instrument: its
# perceived (A-weighted) loudness in the GeneralUser GS SoundFont,
# minus that of the square wave playing the note, over 3dB a level.
# Deep strings and pads sit in the background, high notes, where
# square waves are harshest, lower.
LOUDNESS = (
    (0, 1, 1, -1, -2, -4, -8), (0, 1, 0, -1, -2, -4, -7),
    (-2, -1, -1, -1, -3, -2, -3), (0, 1, 0, -1, -2, -4, -7),
    (-4, -1, -2, 0, -1, -1, -2), (-3, -2, -2, -1, -2, -2, -2),
    (-1, 0, 0, -2, -3, -2, -4), (1, 1, 1, 0, 0, -1, -3),
    (-8, -4, -2, -1, -2, -2, -3), (-2, 0, 0, -1, -2, -5, -6),
    (-7, -4, -3, -3, -3, -4, -5), (0, 1, 1, 0, 0, 0, 0),
    (-1, -2, -2, -1, -2, -4, -7), (-1, -2, -4, -5, -8, -8, -8),
    (0, 2, 2, 1, -1, -2, -4), (-1, 0, 0, -2, -2, -5, -4),
    (-3, 0, 0, -1, -1, -1, -1), (-3, -1, -1, 0, -1, -1, -1),
    (-3, -1, 0, -1, -1, -1, 0), (-2, -1, 0, 0, -1, -1, -2),
    (-1, 0, 0, 1, -1, 0, -4), (-2, 0, 0, -1, -1, -1, -2),
    (-8, -3, -1, 0, -1, 0, 0), (-2, 0, 0, 0, -1, 0, 0),
    (0, 0, 0, -2, -3, -4, -4), (0, 0, -2, -1, -2, -4, -6),
    (-2, 0, -1, -2, -1, -2, -2), (-2, 0, 0, 0, -1, -2, -3),
    (-3, -1, -6, -7, -8, -8, -8), (1, 2, 1, 0, 0, 0, 1),
    (2, 2, 2, 1, 1, 0, -1), (-2, 2, 2, 2, 2, 1, -4),
    (-3, -2, -2, -3, -3, -4, -7), (-2, 0, 0, 0, -1, -2, -5),
    (-2, 0, -1, -2, -3, -3, -5), (-1, -1, 1, 1, 0, -1, -8),
    (-1, -1, 0, 0, -1, -2, -8), (-1, -1, -2, -2, -5, -7, -8),
    (-1, 0, 0, 0, -1, -2, -6), (0, 1, -1, -2, -3, -8, -8),
    (-4, -1, 1, 2, 1, 0, -1), (0, 2, 1, -1, -1, -1, -4),
    (-2, 1, 0, 0, 0, -1, 0), (-2, -1, -1, 0, 0, -1, -1),
    (-2, 0, -1, -1, -2, -1, -2), (-7, -4, -4, -5, -7, -8, -8),
    (-4, -2, 0, -1, -2, -2, -6), (-1, 0, -1, -1, -3, -4, -8),
    (-2, 0, 0, 0, -1, -1, -2), (-2, 0, -1, -1, -2, -1, -2),
    (-2, -1, 0, -1, -1, -2, -3), (-6, -6, -4, -3, -4, -4, -5),
    (-3, -1, -1, -1, -1, -1, -1), (-4, -2, -1, -1, 0, -1, 0),
    (-4, -3, -2, -2, -2, -2, -1), (-4, -2, 0, -1, -2, -5, -8),
    (-3, 1, 1, 1, 1, -2, -3), (-1, 1, 1, 1, 0, 0, 0),
    (-1, 0, 1, 1, 1, -3, -5), (-2, 1, -1, -2, 0, -2, -2),
    (-2, 1, 2, 2, 1, 1, 1), (-1, 2, 2, 1, 0, 0, -1),
    (-1, 0, 0, 0, -1, -1, -1), (-1, 0, 0, -1, 0, -1, -1),
    (-7, -1, 0, -1, 0, 1, 0), (0, 1, 1, 0, 0, 1, 1),
    (-1, 1, 1, 1, 1, -1, -1), (0, 1, 0, 0, 0, 0, -1),
    (-3, 1, 1, 0, 0, 1, 1), (-6, -1, 0, 0, 0, 1, 1),
    (1, 2, 2, 2, 1, 0, 0), (-6, 1, 2, 1, 1, 1, 1),
    (-8, -6, -3, 0, 1, 2, 1), (-8, -2, 0, 0, 1, 2, 0),
    (-8, -5, -2, -1, 0, 1, 1), (-8, -5, -2, -1, 0, 0, 0),
    (-1, -1, -2, -1, 0, 0, -2), (-1, 2, 2, 0, -2, -1, -2),
    (-6, -2, 0, 2, 2, 2, 2), (-8, -2, 0, 1, 1, 2, 2),
    (-1, 0, 0, 0, 0, -1, -1), (-1, 0, 0, 0, 0, 0, -1),
    (0, 0, 0, 1, 1, 1, -3), (-3, -2, -1, 0, -1, -1, -1),
    (0, 0, 0, 0, -1, -1, -4), (-2, 0, 1, 1, 2, 0, 1),
    (-2, -1, -1, -1, -1, -2, -1), (-1, 0, 0, 1, 0, 1, 1),
    (-1, 0, 0, 0, 0, -1, -2), (-4, -2, -3, -4, -6, -5, -6),
    (-2, -1, -1, -1, -1, -2, -2), (-2, 0, 0, 0, -1, -1, -1),
    (-2, -2, -2, 0, -2, -3, -4), (0, 1, -2, 0, -1, -1, -2),
    (-2, -1, -1, -1, -2, -3, -3), (-3, -2, -2, -3, -3, -4, -4),
    (-1, -2, -2, -2, -2, -2, -3), (-2, -2, -1, -2, -1, -6, -8),
    (-2, -1, -1, -1, 0, 0, 0), (-1, 0, 0, 0, -1, -3, -4),
    (-1, 0, 1, 0, 0, 0, -1), (-4, -7, -8, -6, -5, -3, -5),
    (-1, 0, 1, 1, 0, -1, -3), (-3, -1, 0, 1, 0, 1, 0),
    (1, 2, 2, 1, -2, -5, -8), (0, 0, -1, -2, -5, -8, -8),
    (-3, -3, -3, -4, -5, -8, -8), (2, 2, 1, 0, -2, -6, -8),
    (-4, -5, -4, -3, -3, -2, -2), (-1, 1, 1, 2, 1, 0, -2),
    (-4, 0, 1, 2, 0, 0, -1), (1, 2, 2, 2, 0, -1, -2),
    (-3, 0, 0, 0, -1, -2, -4), (-2, -1, -2, -5, -8, -8, -8),
    (-3, 1, 2, 0, -3, -8, -8), (-7, -8, -8, -8, -8, -8, -8),
    (-3, -3, -3, -4, -5, -7, -4), (-5, -5, -6, -6, -8, -8, -8),
    (-2, -3, -5, -6, -8, -8, -8), (-4, -3, -4, -5, -5, -5, -5),
    (1, 1, 0, -3, -6, -8, -8), (-8, -8, -8, -8, -8, -8, -8),
    (-2, 0, -1, -2, -4, -5, -5), (2, 2, 1, 0, -2, -3, -3),
    (2, 2, 1, 0, -2, -2, -2), (-7, -8, -8, -8, -8, -8, -8),
    (-4, -1, -2, -3, -4, -5, -5), (2, 1, -1, -3, -5, -7, -8),
)


class _Note:
    """A keyed note."""
    __slots__ = ("count", "ch", "note", "vel", "level", "kind", "stamp",
                 "tick")


def _period(pitch):
    """YM period of a pitch in (fractional) MIDI notes."""
    p = round(YM_CLOCK / (440.0 * 2.0 ** ((pitch - 69) / 12.0)))
    return max(1, min(0xFFF, p))


def _note_first(kind, n1, n2):
    """Whether a part plays note n1 before n2: a bass its lowest,
    the others their highest."""
    return n1 < n2 if (kind & PRIORITY) == BASS else n1 > n2


def midi_to_frames(division, events, end_tick=None):
    """Render MIDI events to a list of 14-byte YM register frames.

    MD/DOOM: takes parse_smf's (division, events) rather than a file,
    so that tools/ymmusic.py can feed it Doom's MUS lumps directly, and
    `end_tick` (in MIDI ticks) is where the song ends -- a loop needs
    the exact length, not the last event plus half a second."""

    # event times in synth ticks, through the tempo map
    us_per_qn = 500000
    timed = []
    last_tick = 0
    t_us = 0.0
    for ev in events:
        t_us += (ev[0] - last_tick) * us_per_qn / division
        last_tick = ev[0]
        if ev[1] == "tempo":
            us_per_qn = ev[2]
            continue
        timed.append((int(t_us * SYNTH_HZ / 1e6 + 0.5),) + ev[1:])
    if not timed:
        die("no notes in MIDI file")
    total = timed[-1][0] * TICK_HZ // SYNTH_HZ + TICK_HZ // 2 + 1
    if end_tick is not None:
        end_us = t_us + (end_tick - last_tick) * us_per_qn / division
        total = max(1, int(end_us * TICK_HZ / 1e6 + 0.5))

    program = [0] * 16
    volume = [100] * 16          # General MIDI's defaults
    expression = [127] * 16
    bend = [0.0] * 16            # in 1/16 semitone
    notes = []                   # keyed notes
    voices = [None, None, None]  # the note each voice plays
    periods = [0, 0, 0]
    noise = 0
    drum = None
    stamp = 0

    def level_of(ch, vel):
        return (VEL_LEVEL[vel] - ATTENUATION[volume[ch]]
                - ATTENUATION[expression[ch]] - HEADROOM)

    def rank(n):
        # higher priority, then louder when keyed, then the part's own
        # order (see _note_first), then newer
        return (n.kind & PRIORITY, n.level,
                -n.note if (n.kind & PRIORITY) == BASS else n.note,
                n.stamp)

    def drum_level(now):
        """The drum's level now, 0 once over or faded out."""
        if drum is None:
            return 0
        age = now - drum["tick"]
        fade = DRUM_KINDS[drum["kind"]][2] * age
        if age >= drum["ticks"] or drum["level"] <= fade:
            return 0
        return (drum["level"] - fade) >> 2

    def drum_hit(t, ch, note, vel, kind):
        nonlocal drum
        # a hi-hat does not cut a kick or a snare short
        if drum_level(t) and drum["kind"] <= TOM < kind:
            return
        k = DRUM_KINDS[kind]
        level = level_of(ch, vel) + k[3]
        if level <= 0:
            return
        period = k[4]
        if kind == TOM and ch != 9:
            # a timpani is tuned
            while note < NOTE_FLOOR:
                note += 12
            period = _period(note)
        # timed from the next frame, so a drum shorter than a frame
        # still sounds its attack
        t = -(-t * TICK_HZ // SYNTH_HZ) * SYNTH_HZ // TICK_HZ
        drum = {"kind": kind, "ch": ch, "level": level << 2,
                "period": period, "ticks": k[1], "tick": t,
                "sweep": (period >> 4) if k[5] == 1 else k[5]}

    def note_on(t, ch, note, vel):
        nonlocal stamp
        if ch == 9:
            drum_hit(t, ch, note, vel, DRUM_NOTES[note - 35]
                     if 35 <= note < 35 + len(DRUM_NOTES) else HIHAT)
            return
        prog = program[ch]
        kind = PROGRAMS[prog]
        if kind & DRUM:
            # timpani, taiko / melodic tom / synth drum, reverse
            # cymbal, or bells and blocks
            drum_hit(t, ch, note, vel,
                     TOM if prog == 47 else
                     KICK if 116 <= prog <= 118 else
                     CYMBAL if prog == 119 else HIHAT)
            return
        if kind == 0:
            return
        for n in notes:
            if n.ch == ch and n.note == note:
                n.count += 1
                break
        else:
            n = _Note()
            n.count, n.ch, n.note = 1, ch, note
            notes.append(n)
        stamp += 1
        n.vel, n.level = vel, level_of(ch, vel)
        n.kind, n.stamp, n.tick = kind, stamp, t

    def note_off(ch, note):
        for n in notes:
            if n.ch == ch and n.note == note:
                n.count -= 1
                if n.count == 0:
                    notes.remove(n)
                return

    def channel_off(ch):
        nonlocal drum
        notes[:] = [n for n in notes if n.ch != ch]
        if drum is not None and drum["ch"] == ch:
            drum = None

    def allocate():
        # one voice per part first, then the other notes
        parts = {}
        for n in notes:
            p = parts.get(n.ch)
            if p is None or _note_first(n.kind, n.note, p.note):
                parts[n.ch] = n
        chosen = []
        while len(chosen) < 3:
            pool = [n for n in parts.values() if n not in chosen]
            if not pool:
                pool = [n for n in notes if n not in chosen]
            if not pool:
                break
            chosen.append(max(pool, key=rank))
        # a note keeps its voice; the others take a free one from A up
        for v in range(3):
            if voices[v] not in chosen:
                voices[v] = None
        for n in chosen:
            if n not in voices:
                voices[voices.index(None)] = n

    def drum_voice():
        # a free voice, else for a kick or tom the bass, else the one
        # playing the least important note
        host, host_prio = 0, 0xFF
        for v in (2, 1, 0):
            n = voices[v]
            if n is None:
                return v
            p = n.kind & PRIORITY
            if drum["period"] and p == BASS:
                p = 0
            if p < host_prio:
                host, host_prio = v, p
        return host

    # the synth runs tick by tick, sharing the voices again after each
    # tick's messages; the frames sample its registers
    ei = 0
    out = []
    for now in range((total - 1) * SYNTH_HZ // TICK_HZ + 1):
        while ei < len(timed) and timed[ei][0] <= now:
            ev = timed[ei]
            ei += 1
            kind, ch = ev[1], ev[2]
            if kind == "on":
                note_on(ev[0], ch, ev[3], ev[4])
            elif kind == "off":
                note_off(ch, ev[3])
            elif kind == "cc":
                if ev[3] == 7:
                    volume[ch] = ev[4]
                elif ev[3] == 11:
                    expression[ch] = ev[4]
                elif ev[3] in (120, 123):       # sound / notes off
                    channel_off(ch)
                elif ev[3] == 121:              # reset controllers
                    expression[ch] = 127
                    bend[ch] = 0.0
            elif kind == "prog":
                program[ch] = ev[3]
            elif kind == "bend":                # +/- 2 semitones
                bend[ch] = ev[3] / 256.0
        allocate()
        if now != len(out) * SYNTH_HZ // TICK_HZ:
            continue

        levels = [0, 0, 0]
        mix = 0x3F                       # all off (1 = disabled)
        for v in range(3):
            mix &= ~(1 << v)             # tone on: silent at level 0
            n = voices[v]
            if n is None:
                continue
            age = now - n.tick
            prio = n.kind & PRIORITY

            # a held melodic note gets a vibrato after a while
            vib = 0
            if (not n.kind & DECAY and prio in (LEAD, PAD)
                    and n.note < VIBRATO_TOP and age >= VIBRATO_DELAY):
                vib = VIBRATO[((age - VIBRATO_DELAY) * 3 >> 3) & 7]
            note = n.note
            while note < NOTE_FLOOR:
                note += 12
            periods[v] = _period(note + (bend[n.ch] + vib) / 16.0)

            octave = 0
            nn = n.note
            while nn >= 30 and octave < 6:
                nn -= 12
                octave += 1
            level = level_of(n.ch, n.vel) + LOUDNESS[program[n.ch]][octave]

            # plucked and struck notes fade, the others have an
            # accent, melodic notes settle and pads swell
            if n.kind & DECAY:
                level -= min(age >> 4, 6)
            elif prio == PAD:
                level -= 1 + sum(age < SWELL * i for i in (1, 2, 3))
            elif prio == LEAD:
                level -= (age >= ACCENT) + (age >= SETTLE)
            elif prio == BASS:
                level -= age >= ACCENT
            levels[v] = max(0, min(15, level))

        # the drum sounds over a voice
        dl = drum_level(now)
        if drum is not None and dl == 0:
            drum = None
        if drum is not None:
            k = DRUM_KINDS[drum["kind"]]
            age = now - drum["tick"]
            v = drum_voice()
            if drum["period"]:
                # a kick or tom: its own falling tone
                periods[v] = min(drum["period"] + drum["sweep"] * age,
                                 0xFFF)
                levels[v] = min(15, dl)
                if k[0]:
                    mix &= ~(8 << v)
            else:
                # noise: on a free voice, or instead of the least
                # important note (noise over a tone sounds its pitch),
                # a cymbal only briefly instead of a melody or the bass
                n = voices[v]
                if (n is None or age < CRASH_OVER
                        or (n.kind & PRIORITY) < LEAD
                        or drum["kind"] < OPEN_HIHAT):
                    mix |= 1 << v
                    mix &= ~(8 << v)
                    levels[v] = min(15, dl)
            if mix & 0x38 != 0x38:
                noise = k[0]

        regs = [0] * 14
        for v in range(3):
            regs[2 * v] = periods[v] & 0xFF
            regs[2 * v + 1] = (periods[v] >> 8) & 0x0F
            regs[8 + v] = levels[v]
        regs[6] = noise
        regs[7] = mix
        out.append(regs)
    return out


def delta_encode(frames):
    """Pack register frames as (mask, changed bytes) deltas."""
    shadow = [None] * 14
    blob = bytearray()
    for regs in frames:
        mask = 0
        payload = bytearray()
        for r in range(14):
            if regs[r] != shadow[r]:
                mask |= 1 << r
                payload.append(regs[r])
                shadow[r] = regs[r]
        blob += struct.pack(">H", mask) + payload
    return blob
