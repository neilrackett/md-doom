#!/usr/bin/env python3
#
# Copyright (C) 2026 Neil Rackett
# SPDX-License-Identifier: GPL-3.0-or-later
#
"""Build per-level asset packs for MD/DOOM.

The SidecarTridge Multi-device gives a microfirmware 1152 KB of flash for
code and data together; doom1.whx alone is 1758 KB. So instead of one
WHX, MD/DOOM loads one pack per level: a WHX holding that map plus only
the sprites, textures, flats and sound effects it needs, which the
firmware programs into its PACK_FLASH window from the SD card when the
level starts.

This tool reads the IWAD, works out what each map depends on (things ->
mobj states -> sprites; sidedefs -> textures -> patches; sectors ->
flats; animation and switch partners; monster sounds), writes one WAD
per map and, given whd_gen, converts each to a WHX and reports its size
against the flash budget.

    levelpack.py DOOM1.WAD OUTDIR --doom-src rp2040-doom/src/doom \\
        [--whd-gen path/to/whd_gen] [--maps E1M1,E1M2] [--budget 786432]
        [--no-ui] [--no-rotations] [--sfx-rate 5000] [--keep-player]

Levers, largest first (sizes measured on the shareware WAD):
  --no-ui         leave out HELP1/2 and CREDIT (a title pack can bring
                  them back)                                  ~-80 KB
  --no-rotations  keep only rotation 1 of every sprite frame, renamed to
                  rotation 0, so monsters always face the player  ~-120 KB
  --sfx-rate N    resample sound effects down to N Hz (Doom's are 11025;
                  5000 is what fits the shareware episode)   ~-130 KB

`--doom-src` points at the engine's doom/ directory (info.c and info.h
are parsed for the state and mobj tables, so the mapping follows the
vendored engine rather than a copy of it). whd_gen is built from
rp2040-doom's src/whd_gen -- see tools/whd_gen/README.md.
"""
import argparse
import collections
import os
import re
import struct
import subprocess
import sys

MAP_LUMPS = ['THINGS', 'LINEDEFS', 'SIDEDEFS', 'VERTEXES', 'SEGS', 'SSECTORS',
             'NODES', 'SECTORS', 'REJECT', 'BLOCKMAP']

# Things the engine spawns at run time rather than placing in the map, so
# the map's THINGS list does not mention them.
EXTRA_MOBJS = {
    'MT_TROOP': ['MT_TROOPSHOT'], 'MT_BRUISER': ['MT_BRUISERSHOT'],
    'MT_KNIGHT': ['MT_BRUISERSHOT'], 'MT_HEAD': ['MT_HEADSHOT'],
    'MT_POSSESSED': ['MT_CLIP'], 'MT_SHOTGUY': ['MT_SHOTGUN'],
    'MT_CHAINGUY': ['MT_CHAINGUN'], 'MT_CYBORG': ['MT_ROCKET'],
    'MT_PAIN': ['MT_SKULL'],
}
ALWAYS_MOBJS = ['MT_PUFF', 'MT_BLOOD', 'MT_TFOG', 'MT_IFOG', 'MT_ROCKET',
                'MT_TELEPORTMAN']
# The player's own weapons and projectiles carry over between levels, so
# every pack needs all of them.
WEAPON_SPRITES = {'PUNG', 'PISG', 'PISF', 'SHTG', 'SHTF', 'SHT2', 'CHGG', 'CHGF',
                  'MISG', 'MISF', 'SAWG', 'PLSG', 'PLSF', 'BFGG', 'BFGF', 'MISL',
                  'PLSS', 'PLSE', 'BFS1', 'BFE1', 'BFE2'}
TEX_ANIMS = [('SLADRIP1', 'SLADRIP3'), ('BLODGR1', 'BLODGR4'), ('BLODRIP1', 'BLODRIP4'),
             ('FIREWALA', 'FIREWALL'), ('GSTFONT1', 'GSTFONT3'), ('FIRELAV3', 'FIRELAVA'),
             ('FIREMAG1', 'FIREMAG3'), ('FIREBLU1', 'FIREBLU2'), ('ROCKRED1', 'ROCKRED3')]
FLAT_ANIMS = [('NUKAGE1', 'NUKAGE3'), ('FWATER1', 'FWATER4'), ('SWATER1', 'SWATER4'),
              ('LAVA1', 'LAVA4'), ('BLOOD1', 'BLOOD3')]
# Screens a level pack can do without: help and credits. The title screen
# stays so the game boots into it, and the intermission map (WIMAP0) is
# shown after every level.
UI_SCREENS = {'HELP1', 'HELP2', 'CREDIT'}
# Flats the game asks for outside any map: the episode finale backgrounds.
ALWAYS_FLATS = {'F_SKY1', 'FLOOR4_8', 'SFLR6_1', 'MFLR8_4', 'MFLR8_3'}


def read_wad(path):
    wad = open(path, 'rb').read()
    ident, num, off = struct.unpack('<4sII', wad[:12])
    lumps = []
    for i in range(num):
        o, s, n = struct.unpack('<II8s', wad[off + 16 * i:off + 16 * i + 16])
        lumps.append((n.rstrip(b'\0').decode(), wad[o:o + s]))
    return lumps


def write_wad(lumps, path):
    data = bytearray(b'IWAD') + struct.pack('<II', len(lumps), 0)
    entries = []
    for n, d in lumps:
        entries.append((len(data), len(d), n))
        data += d
        while len(data) % 4:
            data += b'\0'
    diroff = len(data)
    for o, s, n in entries:
        data += struct.pack('<II8s', o, s, n.encode().ljust(8, b'\0'))
    data[8:12] = struct.pack('<I', diroff)
    open(path, 'wb').write(data)
    return len(data)


class EngineTables:
    """States, sprites and mobj types parsed out of info.h / info.c."""

    def __init__(self, doom_src):
        infoh = open(os.path.join(doom_src, 'info.h')).read()
        infoc = open(os.path.join(doom_src, 'info.c')).read()
        self.states = self._enum(infoh, 'S_', 'NUMSTATES')
        self.sidx = {n: i for i, n in enumerate(self.states)}
        st = re.findall(r'STATE\(SPR_([A-Z0-9]+),[^,]*,[^,]*,[^,]*,(S_[A-Z0-9_]+)', infoc)
        if len(st) != len(self.states):
            sys.exit(f'info.c has {len(st)} STATE entries but info.h names {len(self.states)}')
        self.state_sprite = {self.states[i]: s for i, (s, n) in enumerate(st)}
        self.state_next = {self.states[i]: n for i, (s, n) in enumerate(st)}
        body = infoc[infoc.index('mobjinfo[NUMMOBJTYPES]'):]
        self.mobj = {}
        for name, blk in re.findall(r'\{\s*//\s*(MT_[A-Z0-9_]+)(.*?)\n    \}', body, re.S):
            first = [l for l in blk.split('\n') if l.strip()][0]
            doomed = int(re.match(r'\s*(-?\d+)', first).group(1))
            states = set(re.findall(r'\b(S_[A-Z0-9_]+)\b', blk)) - {'S_NULL'}
            sfx = set(re.findall(r'\b(sfx_[a-z0-9]+)\b', blk)) - {'sfx_None'}
            self.mobj[name] = (doomed, states, sfx)
        self.by_doomed = {v[0]: k for k, v in self.mobj.items() if v[0] >= 0}
        self.all_monster_sfx = set()
        for _, _, sfx in self.mobj.values():
            self.all_monster_sfx |= sfx
        # Sounds the game code starts by name (weapons, doors, the player's
        # pain and death, ...) must always be present, whatever mobj table
        # entry happens to own them. Only info.c's per-mobj sounds are
        # candidates for dropping.
        # p_enemy.c is the monsters' own behaviour, so a sound that only it
        # references belongs to whichever monster uses it and can go with it.
        self.code_sfx = set()
        for f in os.listdir(doom_src):
            if f.endswith('.c') and f not in ('info.c', 'sounds.c', 'p_enemy.c'):
                self.code_sfx |= set(re.findall(r'\b(sfx_[a-z0-9]+)\b', open(os.path.join(doom_src, f)).read()))
        self.code_sfx |= self.mobj['MT_PLAYER'][2]

    @staticmethod
    def _enum(text, prefix, end):
        m = re.search(r'\{([^}]*%s[^}]*)\}' % end, text, re.S)
        out = []
        for tok in re.findall(r'\b(%s[A-Z0-9_]+)\b' % prefix, m.group(1)):
            if tok != end and tok not in out:
                out.append(tok)
        return out

    def sprites_of(self, mt):
        seen, out, todo = set(), set(), list(self.mobj[mt][1])
        while todo:
            s = todo.pop()
            if s in seen or s == 'S_NULL':
                continue
            seen.add(s)
            out.add(self.state_sprite[s])
            todo.append(self.state_next[s])
        return out


class Packer:
    def __init__(self, lumps, tables, args):
        self.lumps = lumps
        self.t = tables
        self.a = args
        self.names = [n for n, _ in lumps]
        # Textures + patches
        pn = self.lump('PNAMES')
        npn = struct.unpack('<I', pn[:4])[0]
        self.pnames = [pn[4 + 8 * i:12 + 8 * i].rstrip(b'\0').decode().upper() for i in range(npn)]
        tex = self.lump('TEXTURE1')
        ntex = struct.unpack('<I', tex[:4])[0]
        offs = struct.unpack('<%dI' % ntex, tex[4:4 + 4 * ntex])
        self.textures = []
        for to in offs:
            name = tex[to:to + 8].rstrip(b'\0').decode().upper()
            pc = struct.unpack('<H', tex[to + 20:to + 22])[0]
            raw = tex[to:to + 22 + 10 * pc]
            pats = [self.pnames[struct.unpack('<H', raw[22 + 10 * j + 4:22 + 10 * j + 6])[0]]
                    for j in range(pc)]
            self.textures.append((name, raw, pats))
        self.texnames = [t[0] for t in self.textures]
        # Flats, in order
        self.flat_order = []
        insec = False
        for n, d in lumps:
            if n == 'F_START':
                insec = True
            elif n == 'F_END':
                insec = False
            elif insec and d:
                self.flat_order.append(n)

    def lump(self, name):
        return self.lumps[self.names.index(name)][1]

    @staticmethod
    def span(order, a, b):
        if a not in order or b not in order:
            return []
        return order[order.index(a):order.index(b) + 1]

    def build(self, mapname):
        li = self.names.index(mapname)
        maplumps = dict(self.lumps[li + 1:li + 11])
        things, sidedefs, sectors = maplumps['THINGS'], maplumps['SIDEDEFS'], maplumps['SECTORS']

        # --- mobjs -> sprites + sounds
        mts = set(ALWAYS_MOBJS)
        if self.a.keep_player:
            mts.add('MT_PLAYER')
        for i in range(len(things) // 10):
            t = struct.unpack('<hhhHH', things[10 * i:10 * i + 10])[3]
            if t in self.t.by_doomed:
                mts.add(self.t.by_doomed[t])
        for m in list(mts):
            mts.update(EXTRA_MOBJS.get(m, []))
        sprites = set(WEAPON_SPRITES)
        sfx = set()
        for m in mts:
            sprites |= self.t.sprites_of(m)
            sfx |= self.t.mobj[m][2]
        drop_sfx = {s.replace('sfx_', 'DS').upper() for s in (self.t.all_monster_sfx - sfx - self.t.code_sfx)}

        # --- textures -> patches
        want_tex = {'SKY1', self.texnames[0]}
        for i in range(len(sidedefs) // 30):
            for k in range(3):
                nm = sidedefs[30 * i + 4 + 8 * k:30 * i + 12 + 8 * k].rstrip(b'\0').decode().upper()
                if nm and nm != '-':
                    want_tex.add(nm)
        for a, b in TEX_ANIMS:
            r = set(self.span(self.texnames, a, b))
            if want_tex & r:
                want_tex |= r
        for t in list(want_tex):
            for src, dst in (('SW1', 'SW2'), ('SW2', 'SW1')):
                if t.startswith(src) and dst + t[3:] in self.texnames:
                    want_tex.add(dst + t[3:])
        keep_tex = [t for t in self.textures if t[0] in want_tex]
        want_pat = set()
        for t in keep_tex:
            want_pat.update(t[2])

        # --- flats
        want_flat = set(ALWAYS_FLATS)
        for i in range(len(sectors) // 26):
            want_flat.add(sectors[26 * i + 4:26 * i + 12].rstrip(b'\0').decode().upper())
            want_flat.add(sectors[26 * i + 12:26 * i + 20].rstrip(b'\0').decode().upper())
        for a, b in FLAT_ANIMS:
            r = set(self.span(self.flat_order, a, b))
            if want_flat & r:
                want_flat |= r

        # --- rebuild TEXTURE1 / PNAMES over the kept subset (order preserved,
        # which keeps animation ranges contiguous)
        new_pn = sorted(want_pat, key=self.pnames.index)
        pnidx = {p: i for i, p in enumerate(new_pn)}
        tdata, toffs = b'', []
        for name, raw, pats in keep_tex:
            raw = bytearray(raw)
            for j, p in enumerate(pats):
                raw[22 + 10 * j + 4:22 + 10 * j + 6] = struct.pack('<H', pnidx[p])
            toffs.append(4 + 4 * len(keep_tex) + len(tdata))
            tdata += bytes(raw)
        new_tex1 = (struct.pack('<I', len(keep_tex)) +
                    struct.pack('<%dI' % len(keep_tex), *toffs) + tdata)
        new_pnames = struct.pack('<I', len(new_pn)) + b''.join(p.encode().ljust(8, b'\0') for p in new_pn)

        # --- assemble
        out = []
        sec = None
        skip = False
        for n, d in self.lumps:
            if re.match(r'^E\dM\d$', n) or re.match(r'^MAP\d\d$', n):
                skip = (n != mapname)
            elif n not in MAP_LUMPS:
                skip = False
            if skip:
                continue
            if n in ('S_START', 'P_START', 'F_START'):
                sec = n[0]
            if n in ('S_END', 'P_END', 'F_END'):
                sec = None
            if n.startswith('D_') or n.startswith('DEMO') or n.startswith('DP') or n in ('GENMIDI', 'DMXGUS'):
                continue  # no music, no demos, no PC speaker
            if n == 'TEXTURE1':
                d = new_tex1
            elif n == 'PNAMES':
                d = new_pnames
            if sec == 'S' and d:
                if n[:4] not in sprites:
                    continue
                if self.a.no_rotations and len(n) >= 6 and n[5] != '0':
                    if n[5] != '1':
                        continue
                    n = n[:5] + '0'
            if sec == 'P' and d and n.upper() not in want_pat and not n.startswith(('P1_', 'P2_', 'P3_')):
                continue
            if sec == 'F' and d and n.upper() not in want_flat and not n.startswith(('F1_', 'F2_', 'F3_')):
                continue
            if n.startswith('DS'):
                if n in drop_sfx:
                    continue
                if self.a.sfx_rate and len(d) > 8:
                    d = resample_sfx(d, self.a.sfx_rate)
            if self.a.no_ui and n in UI_SCREENS:
                continue
            out.append((n, d))

        monsters = sorted(m for m in mts if m not in ALWAYS_MOBJS and not m.startswith('MT_MISC'))
        return out, dict(mobjs=len(mts), sprites=len(sprites), textures=len(keep_tex),
                         patches=len(new_pn), flats=len(want_flat), monsters=monsters)


def resample_sfx(lump, rate):
    fmt, srate, n = struct.unpack('<HHI', lump[:8])
    pcm = lump[8:8 + n]
    if rate >= srate:
        return lump
    step = srate / rate
    out = bytes(pcm[int(i * step)] for i in range(int(n / step)))
    return struct.pack('<HHI', fmt, rate, len(out)) + out


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('wad')
    ap.add_argument('outdir')
    ap.add_argument('--doom-src', required=True, help="engine's doom/ dir (info.c, info.h)")
    ap.add_argument('--whd-gen', help='path to whd_gen; converts each WAD to WHX')
    ap.add_argument('--maps', help='comma-separated map names (default: all)')
    ap.add_argument('--budget', type=int, default=798720,
                    help='PACK_FLASH size in bytes (default 780 KB, memmap_rp.ld)')
    ap.add_argument('--no-ui', action='store_true')
    ap.add_argument('--no-rotations', action='store_true')
    ap.add_argument('--sfx-rate', type=int, default=0)
    ap.add_argument('--keep-player', action='store_true',
                    help='keep the PLAY sprite (only visible in multiplayer)')
    args = ap.parse_args()

    os.makedirs(args.outdir, exist_ok=True)
    lumps = read_wad(args.wad)
    tables = EngineTables(args.doom_src)
    packer = Packer(lumps, tables, args)
    maps = [n for n, _ in lumps if re.match(r'^E\dM\d$', n) or re.match(r'^MAP\d\d$', n)]
    if args.maps:
        maps = [m for m in maps if m in set(args.maps.split(','))]

    worst = 0
    for m in maps:
        out, info = packer.build(m)
        wadpath = os.path.join(args.outdir, m + '.wad')
        size = write_wad(out, wadpath)
        line = f'{m}: {len(out)} lumps, WAD {size:,} B; sprites {info["sprites"]} tex {info["textures"]} flats {info["flats"]}; {", ".join(info["monsters"])}'
        if args.whd_gen:
            whx = os.path.join(args.outdir, m + '.whx')
            r = subprocess.run([args.whd_gen, wadpath, whx], capture_output=True, text=True)
            if r.returncode != 0:
                line += f'\n  whd_gen FAILED: {r.stdout[-400:]}{r.stderr[-400:]}'
            else:
                wsz = os.path.getsize(whx)
                worst = max(worst, wsz)
                line += f'\n  WHX {wsz:,} B = {wsz * 100 // args.budget}% of budget' + \
                        ('' if wsz <= args.budget else f'  ** OVER by {wsz - args.budget:,} B **')
        print(line)
    if args.whd_gen and maps:
        print(f'largest pack {worst:,} B against a budget of {args.budget:,} B'
              + (' -- all fit' if worst <= args.budget else ' -- DOES NOT FIT'))


if __name__ == '__main__':
    main()
