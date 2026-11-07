#!/bin/bash

#
# Copyright (C) 2026 Neil Rackett
# SPDX-License-Identifier: GPL-3.0-or-later
#

# Build the SD card data -- the level packs and the music -- and zip them
# as the release ships them, under doom/:
#
#   tools/build_data.sh [OUT.zip]     (default dist/mddoom-level-packs.zip)
#
# Everything it needs it fetches into build/data, pinned: the shareware
# DOOM1.WAD by its checksum, and whd_gen's source (rp2040-doom, which
# MD/DOOM does not vendor; see tools/whd_gen) by its commit. The same
# inputs give byte-identical packs. DOOM1_WAD=path uses a WAD you have,
# if it is that one. `make packs` runs this; so does the release.
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
WORK=$ROOT/build/data
OUT=${1:-$ROOT/dist/mddoom-level-packs.zip}

# The shareware DOOM1.WAD, v1.9 (4,196,020 bytes), from ataritt-doom's
# release, whose own workflow never replaces it.
WAD_URL=https://github.com/neilrackett/ataritt-doom/releases/download/latest/DOOM1.WAD
WAD_SHA256=1d7d43be501e67d927e415e0b8f3e29c3bf33075e859721816f652a526cac771
# whd_gen's source.
RP2040_DOOM_URL=https://github.com/kilograham/rp2040-doom.git
RP2040_DOOM_COMMIT=29a453c980918a03e40fc8b69b024e7a3bdb5dc2

sha256() {
    python3 -c 'import hashlib, sys; print(hashlib.sha256(open(sys.argv[1], "rb").read()).hexdigest())' "$1"
}

mkdir -p "$WORK"

WAD=${DOOM1_WAD:-$WORK/DOOM1.WAD}
if [ -z "${DOOM1_WAD:-}" ] && { [ ! -f "$WAD" ] || [ "$(sha256 "$WAD")" != "$WAD_SHA256" ]; }; then
    echo "Fetching DOOM1.WAD"
    curl -fsSL -o "$WAD" "$WAD_URL"
fi
if [ "$(sha256 "$WAD")" != "$WAD_SHA256" ]; then
    echo "ERROR: $WAD is not the shareware DOOM1.WAD v1.9 (SHA-256 $WAD_SHA256)"
    exit 1
fi

SRC=$WORK/rp2040-doom
if [ "$(git -C "$SRC" rev-parse HEAD 2>/dev/null || true)" != "$RP2040_DOOM_COMMIT" ]; then
    echo "Fetching rp2040-doom ${RP2040_DOOM_COMMIT:0:7}"
    rm -rf "$SRC"
    git init -q "$SRC"
    git -C "$SRC" fetch -q --depth 1 "$RP2040_DOOM_URL" "$RP2040_DOOM_COMMIT"
    git -C "$SRC" -c advice.detachedHead=false checkout -q FETCH_HEAD
fi

echo "Building whd_gen"
cmake -S "$ROOT/tools/whd_gen" -B "$WORK/whd_gen" -DRP2040_DOOM_SRC="$SRC/src" > /dev/null
cmake --build "$WORK/whd_gen" --parallel > /dev/null

# The budget is the window the firmware programs a pack into.
KB=$(sed -nE 's/^ *PACK_FLASH\(rw\) *: *ORIGIN *= *0x[0-9A-Fa-f]+, *LENGTH *= *([0-9]+)k.*/\1/p' \
    "$ROOT/rp/src/memmap_rp.ld")
if [ -z "$KB" ]; then
    echo "ERROR: no PACK_FLASH length in rp/src/memmap_rp.ld"
    exit 1
fi

rm -rf "$WORK/doom"
python3 "$ROOT/tools/levelpack.py" "$WAD" "$WORK/doom" \
    --doom-src "$ROOT/rp/src/doom/doom" --whd-gen "$WORK/whd_gen/whd_gen" \
    --budget $((KB * 1024)) --no-ui --no-rotations --sfx-rate 5000
rm -f "$WORK"/doom/*.wad # whd_gen's input, one per map

mkdir -p "$(dirname "$OUT")"
rm -f "$OUT"
(cd "$WORK" && python3 -m zipfile -c "$OUT" doom)
echo "Wrote $OUT"
