# whd_gen

`whd_gen` is rp2040-doom's converter from a Doom IWAD to the compressed
WHX format the engine reads in place from flash. MD/DOOM does not vendor
it; this directory builds it from a checkout of the upstream repository
without the SDL2 packages the upstream CMake insists on.

```bash
git clone --depth 1 https://github.com/kilograham/rp2040-doom /tmp/rp2040-doom
cmake -S tools/whd_gen -B build/whd_gen -DRP2040_DOOM_SRC=/tmp/rp2040-doom/src
cmake --build build/whd_gen
# -> build/whd_gen/whd_gen
```

`make packs` (`tools/build_data.sh`) does all of this, with the WAD and
the upstream commit pinned, and zips the result as the release ships it.
By hand, build the per-level packs:

```bash
python3 tools/levelpack.py DOOM1.WAD packs \
    --doom-src /tmp/rp2040-doom/src/doom \
    --whd-gen build/whd_gen/whd_gen \
    --no-ui --no-rotations --sfx-rate 5000
```

and copy `packs/E1M*.whx` and the music, `packs/D_*.MUS` and
`packs/GENMIDI.OP2`, into the `/doom` folder of the SD card.
