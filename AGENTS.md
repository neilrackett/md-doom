# AGENTS.md — md-doom

This file is the playbook for coding agents working in this repository. It is
the single source of truth; `CLAUDE.md` is a one-line `@AGENTS.md` import, so
Claude Code loads this file too. Edit this file, not `CLAUDE.md`.

MD/DOOM is built on the **md-framebuffer-template** as carried in md-lynx;
most of what follows describes that platform, and the app-specific parts are
under "MD/DOOM" below.

See also: `README.md` (the user-facing guide), `CHANGELOG.md` (what each
version changed and why), `programming.md` (the **upstream**
`md-microfirmware-template` reference — useful for background, but its
shared-region table describes the upstream layout, *not* this template's:
the table below and `cart_shared.h` are authoritative here).

## What this repo is

**MD/DOOM — Doom for the Atari ST / STE / MegaST(E)**, a UF2 microfirmware
for the SidecarTridge Multi-device: the RP2040 in the cartridge slot runs the
game and hands the ST a finished 320×200 16-colour screen every VBL, while
also handling SD card I/O, keyboard/joystick input and audio. The ST is a
display and an input device; everything else happens on the Pico.

**Status: playable.** The shareware episode runs on real hardware (v0.1.0,
7 Sep 2026); since then the palettes, blue-noise dither, saved settings,
the Core 1 audio interrupt and the STE DMA handover fix (v0.2.x). What is
here and builds:

- The framebuffer platform (below), taken from md-lynx.
- `rp/src/doom/` — kilograham/rp2040-doom, with the MD platform layer under
  `rp/src/doom/md/` (see "The engine port" below).
- `doom_video.c` — 256 → 16 colour reduction with ordered dither, fused with
  the chunky-to-planar conversion, split across both cores.
- `doom_input.c` — IKBD scancode → Doom key codes (original PC bindings),
  joystick + Xpad gamepad folded into a Doom-shaped joystick state.
- `doom_sound.c` — the framework-era 8-channel PCM mixer (test card only;
  the game mixes in `doom/md/i_mdsound.c`) and the Ghostbusters YM LUT.
- `pack.c` — SD card → `PACK_FLASH` programmer for per-level asset packs.
- `tools/levelpack.py` + `tools/whd_gen/` — the offline pack builder;
  `tools/png_to_mono.py` — the backdrop converter; `tools/gen_bluenoise.py`
  — the blue-noise tile.
- `doomapp.c` — a pipeline **test card** (build with `MDDOOM_TEST_CARD=1`
  in `rp/src/CMakeLists.txt`) that drives the platform without the engine.

The framebuffer platform underneath provides:
- **Dual (page-flipped) framebuffer on the Atari ST side** — tear-free display, fully managed for you.
- **Real 50 Hz**, locked to the ST's vertical blank.
- **~19 ms of compute every VBL** for the app to draw its frame.
- **Chunked drawing on the RP2040** — one byte per pixel; the framework does the chunked → Atari ST planar conversion for you, straight into the cart framebuffer.
- **Atari ST keyboard and joystick handled on the RP2040** — decoded scancodes and stick state delivered straight to your app.
- **STE DMA sound with a YM2149 fallback**, chosen at runtime from what the machine has.

Network plumbing (WiFi / lwIP / mbedTLS / httpc) was deliberately stripped — apps that need it bring it back from `md-microfirmware-template` upstream.

## Build

Top-level build is driven by `build.sh` in the repo root:

```bash
# <board_type> = pico | pico_w | sidecartos_16mb
# <build_type> = debug | release   (note: always compiled as MinSizeRel — see below)
# <app_uuid_key> = UUID4 identifying this app, must match desc/app.json
./build.sh pico_w release 123e4567-e89b-12d3-a456-426614174000
```

`make build` (release, no version bump, UUID from `uuid.txt`), `make debug`,
`make uart` are the day-to-day entry points.

Required host environment:
- ARM GNU Toolchain 14.2 or newer — export `PICO_TOOLCHAIN_PATH` to its `arm-none-eabi/bin` dir. (15.2.rel1 is what this repo was first built with.)
- `atarist-toolkit-docker` (`stcmd`) — needed for the m68k target. `stcmd` requires a PTY (`pty=true`) and Docker running.
- SDK paths (auto-set from the repo if unset): `PICO_SDK_PATH`, `PICO_EXTRAS_PATH`, `FATFS_SDK_PATH`.
- Git + GNU Make. VS Code users: the C/C++ Extension Pack, CMake Tools and Cortex-Debug.
- Clone with `--recursive` (or `git submodule update --init --recursive`): the Xpad ABI comes from `lib/xpad` and both halves of the build need it.

For on-hardware debugging:
- A Raspberry Pi Debug Probe / Picoprobe wired to the Multi-device header — TX, RX **and both GND pins** must be connected.
- Optional helpers: `ARM_GDB_PATH` (the `arm-none-eabi/bin` dir) and `PICO_OPENOCD_PATH` (OpenOCD's `tcl` dir).

Build flow (orchestrated by `build.sh`):
1. Runs `tools/bump_version.sh`, which increments the patch in `version.txt` and syncs it to `rp/version.txt`, `target/version.txt` and `target/atarist/version.txt`. Set `SKIP_VERSION_BUMP=1` to release `version.txt` as-is (this is what `make build` does).
2. Builds the Atari ST target (`target/atarist/build.sh`) via `stcmd make`. Enforces a **16 KB hard limit** on `BOOT.BIN` (the cartridge code budget — `CART_CARTRIDGE_CODE_SIZE` in `rp/src/include/cart_shared.h`, mirrored as `CARTRIDGE_CODE_SIZE` in `target/atarist/src/main.s`); a build that exceeds it aborts with `ERROR: cartridge code is N bytes; limit is 16384`. A separate copy (`FIRMWARE.IMG`) is then padded to 64 KB to fill the entire shared region, and `firmware.py` converts it into `rp/src/include/target_firmware.h` (a C byte array embedded in the RP firmware). `FORCE_NO_DMA=1` in the environment forces the m68k to report "no DMA sound" so the YM path can be tested on an STE.
3. Builds the RP firmware (`rp/build.sh`): pins submodule versions (pico-sdk 2.2.0, pico-extras sdk-2.2.0, fatfs-sdk at a specific commit), runs CMake, produces `rp/dist/rp-<board>.uf2`. The FatFs configuration lives at `rp/src/ff/ffconf.h` and shadows the submodule's default via `target_include_directories(... BEFORE PRIVATE)` in `rp/src/CMakeLists.txt`, so the `fatfs-sdk` submodule stays pristine.
4. Computes MD5, renames to `dist/<APP_UUID>-<VERSION>.uf2`, and substitutes UUID/MD5/version into `dist/<APP_UUID>.json` from the `desc/app.json` template.

### Build gotchas
- **CMake always builds with `-DCMAKE_BUILD_TYPE=MinSizeRel`** regardless of the `<build_type>` argument. `<build_type>` only controls the `DEBUG_MODE` macro and the dist filename.
- Harmless VASM warnings during the m68k build (`target data type overflow`, `trailing garbage after option -D`) can be ignored, as can the `memset ... overflows the destination` warning from `memfunc.h`.
- VASM/`stcmd` errors like `the input device is not a TTY` mean `stcmd` was invoked without a PTY. `target/atarist/build.sh` already exports `STCMD_NO_TTY=1` for every `stcmd` call it makes; you only need to export it yourself if invoking `stcmd` directly from a non-TTY context. Without it the m68k build can fail silently and the previous `BOOT.BIN` survives — leading to a working RP firmware that displays garbage on the ST because `target_firmware.h` is stale.

### Troubleshooting

| Symptom | Fix |
| --- | --- |
| `the input device is not a TTY` when using `stcmd` | `target/atarist/build.sh` already sets `STCMD_NO_TTY=1` for every stcmd call. If you invoke `stcmd` directly from a non-TTY context, export `STCMD_NO_TTY=1` first. |
| `arm-none-eabi-gcc not found` | Ensure `PICO_TOOLCHAIN_PATH` points to the Arm GNU toolchain bin dir. |
| `ERROR: cartridge code is N bytes; limit is 16384` | The m68k cartridge grew past 16 KB. Trim `target/atarist/src/` (`main.s` / `userfw.s` and their includes), or move data into `APP_FREE` / `SHARED_VARIABLES` rather than embedding it in the cartridge image. |
| `ERROR: .../lib/xpad/src/xpad.inc missing` | Run `git submodule update --init --recursive`. |
| Final steps fail copying the UF2 | An upstream compile failed — scroll back for the first error before the copy step. |
| `region RAM overflowed` at link, or the app bails to Booster at boot | RAM ran out. Check the `__bss_end__` .. `__StackLimit` window in `rp/build-*/rp.elf.map`. See "Memory layout". |
| `region FLASH overflowed` at link | Code outgrew the 372 KB `FLASH` region. Move the `FLASH` / `PACK_FLASH` boundary in `memmap_rp.ld` and lower `tools/levelpack.py`'s `--budget` to match. |
| The Atari ST display shows garbage | Almost always a stale `target_firmware.h`. Confirm `target/atarist/dist/BOOT.BIN` was regenerated by the current build (compare its timestamp to the rest of `dist/`). |
| Test card's `F3` says `PACK FAILED` | No `/doom/E1M1.whx` on the card, or it is larger than the 780 KB window. Build packs with `tools/levelpack.py` (see `tools/whd_gen/README.md`). |

### CI / release
- `.github/workflows/build.yml` builds `pico_w` Release on manual dispatch.
- `.github/workflows/release.yml` (manual dispatch; uncomment the `v*` tag trigger to automate): builds, attaches UF2 + JSON to the GitHub Release, uploads to `s3://atarist.sidecartridge.com/` when the `APP_UUID` secret is set.
- `make tag` tags HEAD with the contents of `version.txt` and pushes the tag.

### Tests
There is no test suite for the firmware. "Verification" is: build succeeds, UF2 boots on hardware, the game plays — plus the serial debug console (`make uart`), where debug builds print a line every 64 frames with the c2p time, the longest frame, the audio interrupt's counters and the per-phase maxima (`md_prof.c`); that line is what every hardware bug so far was diagnosed from, so ask for it. The fused LUT + c2p in `doom_video.c` was checked on the host against a bit-level reference (a random 256-colour frame through a random LUT, compared word for word with a naive planar encoder); repeat that if you touch `doom_c2p_chunks` or `doom_c2p_block`.

## Architecture

The firmware is a **two-target build**: m68k assembly that runs on the Atari ST is compiled into a ROM image, embedded as a C array inside the RP2040 firmware, and served back to the Atari over the cartridge bus that the RP2040 emulates via PIO + DMA.

### Framebuffer pipeline (the headline feature)

End-to-end, every visible pixel on the ST goes through this path each VBL:

1. **RP draws into a chunked off-screen buffer** at RP `0x20000000+` (`fb_chunked_buffer`, 320×200 bytes = 64 KB, one byte per pixel). For the framework's own path the palette index is in the low nibble; **for MD/DOOM the byte is a full PLAYPAL index** and the buffer doubles as the engine's video buffer (see "Video" below). `fb_font.c` writes text; `fb_blit.c` writes rectangles / bitmaps / sprites with color-key.
2. **RP publishes via chunky-to-planar conversion, straight into the cart FB.** Both paths first block on the m68k's blit ack (`fb_wait_blit_ack`, a cart-bus read at `$FB8400` after each blit) and then write the cart FB at `$FA8300` (RP `0x20030000 + 0x8300`) inside the m68k's ~3 ms post-blit slack, ending with `fb_frame_done` (the frame counter bump). There is **no planar scratch buffer** — md-lynx's 32 KB went to the game — so the write is in place and must never spill past the next VBL or the m68k blits a torn frame. The framework path (`fb_publish` → `fb_chunky_to_planar`) transposes the low nibbles with the Thumb worker `fb_c2p_half` (for each 4-pixel uint32 group, plane K's 4-bit nibble = `((q >> K) & 0x01010101) * 0x80402010 >> 28`), one 96-pixel MOVEM chunk at a time; MD/DOOM's path (`doom_video_publish`) does the same transpose in C but maps each byte through the 16-colour dither LUT on the way in. Either way chunk k of the image lands at cart chunk (665-k) — the 48-byte m68k MOVEM chunks **pre-reversed** so the predec store in step 3 lands each at its natural position (see "Framebuffer chunk layout for the m68k MOVEM blit" in `cart_shared.h`; `fb_cart_offset()` is the mapping) — and the work is split between **Core 0 (chunks 0–332) and Core 1 (333–665)**, synced via the inter-core FIFO. ~1 ms per frame.
3. **m68k blits cart FB → ST screen page** inside `userfw.s`'s VBL loop via the `FBDRV_INLINE` macro — a fully unrolled `movem.l (a6)+, d0-d7/a1-a4` load + `movem.l d0-d7/a1-a4, -(a5)` predec store (**12 longwords / 48 bytes per iter, 666 iterations** for the full 200-line low-res FB, plus a `d16(a5)` MOVEM tail copying the last 32 bytes). A0 and A7 are intentionally omitted from the MOVEM list: A0 is the dedicated Timer-B audio cursor, and A7 keeps the supervisor SP valid so IRQs can fire safely during the macro. Pure 68000 CPU, no STE blitter, no `_MCH` cookie dispatch — same code runs on plain ST / STE / MegaSTE / TT / Falcon.
4. **m68k flips video base** to the page it just wrote. `userfw.s` toggles between `$70000` and `$78000` every frame (`UFW_SCREEN_PAGE` XOR `UFW_SCREEN_XOR`), so the display always reads from a stable page while the next blit fills the other.
5. **PALETTE_IDX0 (`$FFFF8240`) doubles as a timing tape-measure** when `FBDRV_DEBUG_MARKS` is 1 — `userfw.s` writes it at three points so the ST border shows the blit cost as colored bands.

The number of lines copied by `FBDRV_INLINE` is parameterized by `FB_COPY_LINES` (default 200). Leaving rows untouched lets an app keep a static status bar in the destination ST page that the FB blit never overwrites. Adjust the tail MOVEM register list manually when changing it (the comments in `userfw.s` list the working values).

### IKBD pipeline (keyboard + joystick input, ESC exit)

`userfw.s` owns the Atari ST IKBD ACIA at `$FFFFFC00`/`02` end-to-end,
replacing TOS's handlers so the m68k VBL blit runs interrupt-free apart from
the two IRQs the app actually needs:

1. **m68k boot stubs 4 IRQ vectors** — HBL (`$68`), Timer-A (`$134`), Timer-C (`$114`) and Timer-D (`$110`) all point at a 1-instruction `userfw_dummy_irq` (just `rte`). MFP Timer A/C/D are disabled+masked at IERA/IERB so they never fire; HBL is masked by SR=$2300. Timer-B (`$120`) is owned by the YM audio path; it is left idle when STE DMA sound is in use. On a DMA-sound machine Timer-A is then claimed by `userfw_snd_irq` (event-count mode, one event per DMA frame end; see the audio pipeline).
2. **The ACIA vector (`$118`) gets a real handler**, `userfw_acia_irq`, enabled at IERB/IMRB bit 6. It reads each IKBD byte the instant it arrives and forwards it to the RP with a cart-bus read at `IKBD_WINDOW_BASE + byte` (`$FB8200..$FB82FF`). Interrupt-driven rather than polled: the MC6850 has a one-byte receive buffer, and polling drops bytes, which desynchronises the multi-byte joystick packets. The MIDI ACIA shares the same MFP interrupt, so the handler drains it too. MFP is in auto-EOI mode, so the handler needs no in-service acknowledge; it saves only D0/A1, which makes it safe to fire in the middle of `FBDRV_INLINE`.
3. **m68k boot configures the IKBD** — `$12` (disable mouse reporting) → `$14` (joystick event reporting). The byte stream is then keyboard scancodes plus `$FE`/`$FF`/`$FD` joystick packets.
4. **RP captures via commemul** — the 1 KB ROM3 DMA ring (`commemul.{c,pio}`) records every read in `$FB0000`–`$FBFFFF`. The main loop drains it with `fb_pump_rom3()`, whose callback routes each sample to the IKBD demux, the Xpad receiver, the VBL frame-sync detector and the sound-capability decoder.
5. **RP demux** (`ikbd_pump()`) classifies each raw byte: `$00..$7F` = key press, `$80..$F1` = key release (scancode = `byte & $7F`), `$FD/$FE/$FF` = joystick packet headers whose following state byte(s) are framed into `s_joy_state[]`. Only port 1 is reported by `ikbd_get_joystick()` (bit0 up, 1 down, 2 left, 3 right, 7 fire). Apps drain key events via `ikbd_pop_key()`; scancode `$00` is suppressed.
6. **ESC** — `ikbd.c` posts `CART_CMD_BOOT_GEM` to the sentinel slot on an ESC press+release within 200 ms unless the app calls `ikbd_set_esc_auto_exit(false)`. The test card leaves it on (ESC = back to GEM); the game will turn it off (ESC = Doom's menu) and exit through `ikbd_request_boot_gem()` from a menu item. `ikbd_clear_command()` at the top of the main loop re-arms the slot to `CMD_NOP` so the exit is a one-shot and does not re-trigger after an ST reset.

### Audio pipeline (STE DMA sound, YM2149 fallback)

The back-end is chosen at **runtime** from what the machine actually has, and
both paths are compiled in. The RP produces whichever sample format the m68k
asks for; the fill callback keys off `audio_get_mode()`.

1. **Detection.** At boot `userfw.s` walks the cookie jar (`_p_cookies` at `$5A0`) for `_SND` and tests bit 1 (DMA / PCM sound), storing the result in `UFW_HAS_DMA`. Building with `FORCE_NO_DMA=1` forces the fallback so the YM path can be exercised on an STE.
2. **Reporting.** Every VBL the m68k does one cart-bus read at `SNDCAP_WINDOW_BASE + has_dma` (`$FB8600`). `audio_consume_rom3_sample()` decodes it and calls `audio_set_mode(AUDIO_MODE_DMA | AUDIO_MODE_YM)`. Before the first report the mode is `AUDIO_MODE_SILENT`.
3. **STE DMA path (preferred).** Mono, 8-bit **signed** PCM at 25,033 Hz = ~500 bytes per PAL VBL. The DMA runs in loop mode over a double buffer parked in the unused tails of the two ST screen pages (`$77D00` / `$7FD00`). Each VBL the m68k reads which buffer the DMA is playing and copies the fresh bytes from the cart audio buffer into the *other* one. **The loop's START/END registers are re-pointed from `userfw_snd_irq`**, an MFP Timer-A event-count interrupt that fires at the DMA's own frame end: the chip has just latched START/END and switched buffers, and the handler points them at the buffer it left, a full frame before the next latch. They must not be written from the VBL loop: the chip latches them whenever it reaches END, the six byte writes are not atomic, and a latch landing between the START and END writes plays from one buffer's start to the other's end, i.e. 32 KB of screen memory as ~1.3 s of noise, which happened every few minutes as the chip's phase drifted through the window.
4. **YM2149 fallback.** MFP Timer-B in /4 delay mode with TBDR=110 → 5,585.45 Hz, ~112 fires per PAL VBL = **224 bytes per VBL** of (vA, vB) volume pairs. Pairing two channels through the 1988 Ghostbusters demo's 64-entry LUT extracts ~6 effective bits from the YM's logarithmic volume curve where one channel would give 4. A0 is the dedicated Timer-B read cursor, reset to the buffer base by `userfw_vbl` every vsync.
5. **RP side.** The fill callback is asked for exactly the current mode's byte count and writes into the 1 KB cart buffer at `CART_AUDIO_BUFFER_OFFSET` (`$FA4100`). **MD/DOOM drives it from a 1 ms timer interrupt on Core 1** (`audio_start_vbl_timer(1)`, an alarm pool created on Core 1 so its IRQ is bound there): the handler peeks at the ROM3 ring (`commemul_scan`, non-consuming) for the m68k's end-of-blit ack at `$FB8400` and fills right after it, i.e. just after the m68k has copied the previous buffer and before it will copy the next. That keeps the refill on every VBL however long a frame takes, and it can never tear under the m68k's copy. Nothing on the game path calls `audio_render_frame()` (the test card still does). The engine's sfx module (`i_mdsound.c`) is the callback; a spinlock (`PICO_SPINLOCK_ID_OS1`) guards the channel state between the game on Core 0 and the mixer's interrupt. **The interrupt must not run on Core 0**: measured on hardware, the same 1 ms timer on Core 0 (`audio_start_vbl_timer(0)`) makes the renderer's pure-compute phases stall for 100–200 ms every second or two, although the handler itself costs under 1 ms per VBL and fires at the expected rate; the mechanism was not identified. Keypad `-` cycles off / Core 0 / Core 1 at run time for A/B tests. Core 1's park job (`fb_core1_park`) masks interrupts while parked so the refill cannot execute flash code while a pack is being programmed.

**The STE buffer length is measured, not assumed.** The sound DMA and the video run off different oscillators, so the samples the chip eats per frame is about 501.5 and machine-dependent. `userfw.s` steers the length ±1 byte every fourth frame from the drift of the DMA's frame counter and sends it to the RP each VBL through `SNDLEN_WINDOW_BASE` (`$FB8C00`), biased by `STE_SND_LEN_MIN`; `audio_set_fill_bytes()` applies it. **This is why the mixer must produce however many samples it is asked for**, which `doom_sound_fill` does by resampling every channel onto the requested count. `AUDIO_SNDLEN_BIAS` in `audio.c` must match `STE_SND_LEN_MIN` in `userfw.s`; `TIMERB_COUNT` must stay in step with `AUDIO_FILL_BYTES_YM`.

### Atari ST side (`target/atarist/`)
- `src/main.s` — m68k cartridge header + boot dispatcher. Lives at `$FA0000` (ROM4 cartridge region). `pre_auto` (CA_INIT bit 27) runs in supervisor mode, copies `start_rom_code` below ST screen memory, jumps there, checks resolution (high-res falls to GEM with a warning), then **`jmp USERFW` directly**.
- **No Atari RAM**: the cartridge code deliberately uses none of the ST's RAM (no `.bss`, no heap). Data lives in registers or the shared region. Adding naive BSS to `userfw.s` will silently corrupt system variables — keep state in `APP_FREE` / `SHARED_VARIABLES`, the cart code itself (which is **read-only** from m68k — writes bus-error), or registers.
- `src/userfw.s` — **the primary extension point for app-specific m68k code.** `src/userfw.ld` places `main.s` at offset `0x0000` (≤ 2 KB) and `userfw.s` at offset `0x0800`. Total cart code ≤ 16 KB; the current build is ~8.5 KB. Runs the FB blit + audio (STE DMA or YM) + interrupt-driven IKBD VBL loop, the optional Xpad consumer (`XPAD_ENABLE`, off by default), and exits on the RP's `CMD_BOOT_GEM`.
- Built via `stcmd make release` (m68k assembler in Docker). A 64 KB padded copy of the BOOT.BIN is then converted to `target_firmware.h` for inclusion in the RP build.

### Shared 64 KB cartridge region
The Atari ST sees a 64 KB window at `$FA0000`–`$FAFFFF` (mirrored RP-side at `0x20030000`). This is the **single source of truth** for any cross-target data layout — both sides derive every offset symbolically from constants in `rp/src/include/cart_shared.h` (RP-side) and `target/atarist/src/main.s` (m68k side). **Apps must never hard-code an address inside this region** — always reference the named offset/symbol.

| Offset | Symbol | Size | Purpose |
| --- | --- | --- | --- |
| `$FA0000` | cartridge image | 16 KB | m68k header + main.s (≤ 2 KB) + userfw.s at `$800`. Read-only from m68k. |
| `$FA4000` | `CMD_MAGIC_SENTINEL_ADDR` | 4 B | RP→m68k command word (`CMD_NOP`, `CMD_RESET`, `CMD_BOOT_GEM`, `CMD_START`). |
| `$FA4004` | (reserved) | 8 B | Former handshake slots; unused. |
| `$FA400C` | `FB_FRAME_COUNTER_ADDR` | 4 B | RP-incremented dirty-frame counter; `userfw.s` only blits when this changes. |
| `$FA4010` | `SHARED_VARIABLES` | 240 B | 60 × 4 B indexed slots. **Slots 12–19 are the palette below — not free.** |
| `$FA4040` | `PALETTE` (`CART_PALETTE_OFFSET`) | 32 B | 16 ST colour words (`0000.0RRR.0GGG.0BBB`, STE low bit in bit 3 of each nibble). The m68k VBL handler copies these to `$FFFF8240..$FFFF825E` every frame; `doom_video.c` writes them via `palette_set()`. |
| `$FA4100` | `AUDIO_BUFFER_ADDR` | 1024 B | Audio buffer, refilled by the RP once per VBL in whichever format the detected back-end wants. |
| `$FA4500` | `APP_FREE_ADDR` | ~15.5 KB | Contiguous arena, ends at FRAMEBUFFER. The ST never reads it, so the `CART_APP_FREE` linker region overlays it and RP-side buffers are parked there (`__cart_app_free`) — see "Memory layout". |
| `$FA8300` | `FRAMEBUFFER_ADDR` | 32000 B | 320×200×4bpp planar framebuffer, chunk-reversed. Read by `FBDRV_INLINE` every VBL. |

### RP2040 side (`rp/src/`)
- `main.c` — only sets voltage then clock, calls `gconfig_init` then `aconfig_init`, and hands off to `emul_start()`. If config init fails it jumps to the **Booster** app. **Don't add features to `main.c`.**
- `emul.c` — the boot sequence: erase + copy firmware to RAM, init IKBD + Xpad receivers, init romemul, init commemul, init fb (launches Core 1), palette, audio + `audio_start_vbl_timer(1)`, mount SD, configure SELECT, then hand over to `doomgame_start()` (`md/md_main.c`), which never returns — the engine's `I_GetEvent` and `I_MD_PresentFrame` do per frame what a framework main loop would. Only with `MDDOOM_TEST_CARD=1` does `emul.c` run its own loop `{ikbd_clear_command(); fb_pump_rom3(); ikbd_pump(); drain keys → doomapp_handle_key(); doomapp_render_frame();}`. The `cart_check()` canary compares the first 16 bytes of the served cart region against the embedded image at each boot stage — the first thing to look at if the ST bombs.
- `fb.c` / `fb.h` — owns the 32 KB planar framebuffer at `$FA8300` + boot splash. `fb_publish()` = `fb_wait_blit_ack()` + `fb_chunky_to_planar()` + `fb_frame_done()`; the two bracketing calls are public for `doom_video_publish`. `fb_rom3_dispatch` routes ROM3 samples to ikbd / xpadin / audio / VBL-sync.
- `fb_chunked.c` / `fb_chunked.h` — `fb_chunked_buffer` (64 KB, also Doom's video buffer) + the Core 1 job dispatcher (`fb_core1_dispatch` / `fb_core1_wait`, one dispatch per wait, never straddling a publish) + `fb_core1_park` / `fb_core1_unpark` (hold Core 1 in RAM, interrupts masked, while flash is programmed; `fb_core1_park` returns only once Core 1 has acknowledged, and like any dispatch it must be called with no job outstanding -- in the game that means the tics phase or the end of `I_MD_PresentFrame`, never from the input poll, which the renderer runs mid-frame).
- `fb_chunked_asm.S` — the Thumb c2p worker (`fb_c2p_half`) and the chunk-reversed copy (`fb_chunk_reverse_copy48`).
- `fb_font.c` / `fb_blit.c` — text and bitmap primitives into the chunked buffer. Note `font_set_color` masks to 4 bits (the framework's 16-colour assumption), so on the Doom path text can only use PLAYPAL indices 0..15 — the test palette keeps a grey ramp there for that reason.
- `doom_video.c` / `doom_input.c` / `doom_sound.c` / `pack.c` / `doomapp.c` — the app layer; see "MD/DOOM" below.
- `romemul.c` / `romemul.pio` — the cartridge ROM bus emulator (PIO + DMA, no CPU). `commemul.c` / `commemul.pio` — the ROM3 capture ring.
- `gconfig.c` / `aconfig.c` — global vs per-app configuration in dedicated flash sectors, on top of `settings/`. `aconfig.c`'s `FOLDER` (default `/doom`) is where packs live on the SD card. **Do not trim the global-config defaults** — they mirror the Booster app.
- `sdcard.c`, `hw_config.c` — FatFs over SPI. `select.c`, `reset.c` — SELECT button, jump-to-Booster. `xpadin.c` — Xpad report receiver (`$FB8800`/`$FB8A00`); the ABI comes from the `lib/xpad` submodule.
- `ikbd.c` / `ikbd.h` — IKBD ingest + demux, key-event ring, joystick state, ESC handling.

### Memory layout (`rp/src/memmap_rp.ld`)
The RP2040's 2 MB flash is sliced into named regions, and code is responsible for not stomping on them:

| Region | Origin | Length | Purpose |
| --- | --- | --- | --- |
| `FLASH` | `0x10000000` | 372 K | App code (~342 K release / ~379 K debug at v0.2.2 -- debug has ~2 K left; trim its printf chatter before moving the pack boundary) |
| `PACK_FLASH` | `0x1005D000` | 780 K | The current level's asset pack, programmed from SD (see below) |
| `BOOSTER_APP_FLASH` | `0x10120000` | 768 K | Reserved for the Booster app (do not write from this app) |
| `CONFIG_FLASH` | `0x101E0000` | 120 K | 30 sectors of per-app config |
| `GLOBAL_LOOKUP_FLASH` | `0x101FE000` | 4 K | UUID → config-sector lookup |
| `GLOBAL_CONFIG_FLASH` | `0x101FF000` | 4 K | Global config |
| `RAM` | `0x20000000` | 192 K | Normal RAM |
| `ROM_IN_RAM` | `0x20030000` | 64 K | The shared cartridge region the m68k reads |
| `CART_APP_FREE` | `0x20034800` | 15,104 B | Overlay on the unused hole inside `ROM_IN_RAM` |

**The 1152 KB microfirmware slot is the whole of `FLASH` + `PACK_FLASH`** and
it is the reason this port is built around level packs rather than one WHX:
see "Flash budget" under MD/DOOM. Move the boundary between the two regions
if the code ends up a different size, and keep `tools/levelpack.py --budget`
(default = `PACK_FLASH` length) in step.

**RAM is the binding constraint.** With the engine in, the build leaves a
**~48 KB heap window** (`__bss_end__` .. `__StackLimit`, 48,428 B in the
v0.2.2 debug build), of which newlib's boot-time allocations (the settings
library and friends) and `ZONE_HEAP_MARGIN` (4 KB, for FatFs while a pack
loads) come off the top: **Doom's zone is 32,768 B on hardware**
(`0x20028000..0x20030000` in the boot log), less than the ~38 KB estimated
from the map. Upstream rp2040-doom reports its zone using up to ~45 KB on
the busiest levels; E1M1 and E1M2 play, so watch the UART for `Z_Malloc`
errors on later maps. What already went: the engine renders
single-buffered into `fb_chunked_buffer` (upstream double-buffers
2 × 54 KB), the 32 KB planar scratch is gone (direct c2p),
`RENDER_COL_MAX` is 3000 (upstream 3600; overflow degrades to black
columns), the renderer's `visplane_bit`, patch decoder buffers and
`column_heads` (12.6 KB) live in `CART_APP_FREE`, the vpatch lists live in
the unused USB DPRAM, the sfx mix buffer (1 KB) in scratch X below Core 1's
2 KB stack. Left to pull if needed: `RENDER_COL_MAX` lower still, the 4 KB
dither LUT into scratch X, freeing the settings contexts after boot.

- **`CART_APP_FREE`** overlays the 15,104-byte hole between `CART_APP_FREE_OFFSET` (`$4500`) and `CART_FRAMEBUFFER_OFFSET` (`$8300`) inside the shared region — ordinary SRAM that neither the ST nor the framebuffer path reads. Buffers tagged `__cart_app_free("name")` live there (the commemul ring and 12.6 KB of renderer scratch; ~1.4 KB is free). It is `NOLOAD`, so only park things first touched after `emul_start()` has called `ERASE_FIRMWARE_IN_RAM()`. `emul_start()` re-checks the window against `cart_shared.h` at boot, and the linker script asserts the section starts on `ORIGIN`.
- If you add a `static` array, check the link: `__bss_end__` .. `__StackLimit` in `rp/build-*/rp.elf.map` is the whole heap window. The boot-time settings library needs ~8.4 KB of it; "the app launches then lands straight back in Booster" is a heap failure, not a crash. `DPRINT_HEAP()` in `debug.h` reports the window at boot.

The build assumes Core 0 owns flash writes (`PICO_FLASH_ASSUME_CORE0_SAFE=1`). **Core 1 is parked in `fb_core1_loop`** (`fb_chunked.c`), a generic job dispatcher; the c2p bottom half rides it, and so does the RAM-resident park job `pack.c` uses to hold Core 1 still while it programs flash. **Core 1 is otherwise free**: the cartridge bus is served by PIO + DMA with no CPU involvement (the original scoping assumption that one core is reserved for the cartridge interface does not hold), so upstream's Core 1 render split is kept via `fb_core1_dispatch`. The one other thing on Core 1 is the audio refill interrupt (see "Audio pipeline"), which is why it must never be moved to Core 0.

Core 0 runs at **400 MHz at `VREG_VOLTAGE_1_30`** (`RP2040_CLOCK_FREQ_KHZ` in `constants.h`); the cart-bus PIO programs keep their proven 225 MHz cycle timing via the `SAMPLE_DIV_FREQ` clock divider, and the QSPI flash divider is raised to /4 (`PICO_FLASH_SPI_CLKDIV=4`) so flash SCK stays at 100 MHz. That is lower XIP bandwidth than upstream rp2040-doom's 135 MHz, and WHX decompression is XIP-heavy — one of the things to measure once the engine runs. If a board fails to boot at 400 MHz (silicon lottery), step down: 360000 @ `VREG_VOLTAGE_1_25`, then 300000 @ `VREG_VOLTAGE_1_15`, then 225000 @ `VREG_VOLTAGE_1_10` (a /2 flash divider is only safe at 225).

### App identity
`CURRENT_APP_UUID_KEY` (set from the `APP_UUID_KEY` env var at CMake time, with a placeholder default) is the app's UUID4. It must match the `uuid` field in `desc/app.json` and is used as the key into `GLOBAL_LOOKUP_FLASH` to find this app's config sector. Mismatch → app jumps to Booster. `uuid.txt` (gitignored) holds this app's.

## MD/DOOM

### Flash budget — why level packs

Measured on 6 Sep 2026 with `whd_gen` built from rp2040-doom and the
shareware `DOOM1.WAD`:

| WHX | Bytes |
| --- | --- |
| doom1.whx, whole shareware WAD | 1,800,312 |
| minus music and demos | 1,696,056 |
| minus all maps but E1M1 | 1,404,848 |
| minus sound effects as well | 1,129,388 |
| no maps at all, no sfx (i.e. the shared graphics alone) | 1,110,100 |

Against a 1152 KB slot that also has to hold the code, nothing built from the
whole WAD fits, and the graphics alone are over. Per-level packs are the only
shape that works: `tools/levelpack.py` derives what each map needs (things →
mobj states → sprites, sidedefs → textures → patches, sectors → flats,
animation and switch partners, the monsters' sounds; the player's weapons and
projectiles always) and writes one WAD per map, which `whd_gen` turns into
a WHX. With everything in, E1M3 is still 1,142 KB, so three levers are needed:

| Lever | E1M3 pack |
| --- | --- |
| everything the level uses | 1,142,012 |
| `--no-ui` (HELP1/2, CREDIT out; TITLEPIC and WIMAP0 stay) | ~1,030,000 |
| + `--no-rotations` (monsters always face the player; ~120 KB) | 843,428 |
| + `--sfx-rate 5000` (half-rate sound effects; ~120 KB) | 725,248 |

With all three, every E1 pack fits the 780 KB window, the largest with
about 2 KB to spare (see the tool's output for current figures). Sounds
referenced by name in the game code (weapons, doors, the player's own pain
and death) are always kept; only a monster's own sounds go with it. The title
and intermission screens ride in every pack; help and credits do not. The two visible compromises — no sprite
rotations and 5 kHz effects — are the user's call; `--no-ui` plus one of
them is 843/862 KB, over by a little, so both are needed unless the code
shrinks enough to move the boundary.

### Level packs at run time (`pack.c`)
`pack_load(path, progress)` copies a file from the SD card into `PACK_FLASH`
32 KB at a time, borrowing `fb_chunked_buffer` as the staging area, with one
block erase up front and Core 1 parked + interrupts off around each flash
call (SD reads need XIP live, so the two cannot overlap). The display holds
the last published frame throughout, and the progress callback may draw and
publish between chunks. A 700 KB pack takes a few seconds. The engine calls
it from `I_MD_LoadLevelPack` at the top of `P_SetupLevel` before touching
any lump (see "The engine port"), and tolerates a WHX that lacks lumps the
full WAD has (other maps, other monsters' sprites, demos, music). Only ever
call it with Core 1 idle (it parks Core 1): the tics phase or the end of
`I_MD_PresentFrame`, never from the input poll.

### Video (`doom_video.c`)
Doom writes 320×200 PLAYPAL indices into `fb_chunked_buffer`;
`doom_video_publish()` maps each pixel through a per-dither-cell LUT to one
of 16 pens and packs the ST planes in the same pass, top half on Core 0 and
bottom half on Core 1, between `fb_wait_blit_ack()` and `fb_frame_done()`.
The LUT is rebuilt whenever the PLAYPAL page or a mode changes
(`doom_video_set_playpal` — Doom's `I_SetPalette` — is expected on every
tint, and the rebuild is cheap enough not to cache the 14 pages).

The 16 colours come from one of eight sources (`doom_video_palette_t`):
STDOOM's hand-picked subset of PLAYPAL indices (Jonas Eschenburg's, refined
over a long time on the shareware WAD; the default), a median-cut + k-means
palette generated from the current page, a 16-step grey ramp, and the fixed
EGA, CGA (palette 1, four colours — the reducer handles a reference set
smaller than 16), C64, ZX Spectrum and PICO-8 palettes. Dither modes:
nearest, 2×2 Bayer, 4×4 Bayer (default), 4×4 clustered-dot, and blue noise
(md-mjpeg's 32×32 void-and-cluster tile, `bluenoise.h`, regenerated by
`tools/gen_bluenoise.py`; it cannot be a per-cell LUT so it takes a
per-pixel path with a per-index (nearest, second, level) table). Measured
on hardware the whole LUT + c2p pass takes ~2.1 ms with Bayer and ~3.4 ms
with blue noise, against the m68k's ~3 ms post-blit slack — blue noise is
over, and no tearing has been reported yet, but it is the first suspect if
any is. Keypad `*` and `/` cycle the modes in-game and the choice is saved
in the app config (`DITHER` / `PALETTE`, `aconfig.c`) and restored at boot.
A colour is drawn as its nearest
reference on some cells and its second-nearest on the others, split by where
it falls on the line between them (`t` in 0..16 against the cell threshold).
Distances use the redmean approximation; the reference colours are snapped
to what the STE actually displays (`(v >> 4) * 17`, not the nibble ×17 — the
STE's nibble bit order is not the value's). All of this is the DOOM
Accelerator reduction from atarist-stdoom carried across; STDOOM's own
software path (4-colour mixing weights searched offline in `palette-opt`,
`mix_weights_lorez`) is the next quality step if wanted.

### Input (`doom_input.c`)
`doom_input_translate(scancode)` returns the Doom key code (doomkeys.h
values) for an IKBD scancode. Bindings are original PC Doom's — cursors,
Ctrl fire, Space use, Alt strafe, Shift run, 1-7 weapons, Esc, Tab, F1-F10,
-/= — with Help → F11 (gamma), Undo → Pause, and keypad digits acting as the
cursor cluster. `doom_input_poll_joystick()` folds the port-1 stick and an
Xpad pad (`xpadin.c`) into `doom_joy_state_t` (x/y in -1..1 plus a button
mask: fire, use, strafe, run, weapon prev/next, menu, map, strafe l/r).
`md/i_input.c` posts the keys as `ev_keydown`/`ev_keyup` and turns the
stick and pad into key presses on their edges.

### Sound (`doom_sound.c`, `md/i_mdsound.c`)
The game's mixer is `md/i_mdsound.c` (upstream's channel model and ADPCM
decoder, eight channels), resampled onto however many output samples the
m68k asks for each VBL — signed 8-bit at 25,033 Hz on the STE path,
Ghostbusters (vA, vB) pairs at 5,585 Hz on the YM path — from the
VBL-synced timer interrupt on Core 1 (see "Audio pipeline"). Volume 0..127
and stereo separation fold to a mono gain. No music. `doom_sound.c` is the
framework-era PCM mixer the test card uses; the game only takes its
Ghostbusters LUT (`doom_sound_ghost_lut`) from it.

### The test card (`doomapp.c`)
A stand-in for the engine that exercises the pipeline on real hardware: a
synthetic 256-colour palette (grey ramp at 0..15, 6×6×6 cube, hue wheel)
rendered as swatches through the reducer; F1 cycles the dither, F2 the
palette source, F3 programs `/doom/E1M1.whx` from the SD card into the pack
window with a progress bar; Ctrl, joystick fire or pad South plays a test
sweep through the mixer; the top band shows the version, the detected sound
chip, the last Doom key name, the stick/pad state, the c2p microseconds and
the frame rate. ESC returns to GEM.

### The engine port (`rp/src/doom/`)

Upstream is kilograham/rp2040-doom's `doom_tiny` configuration (the full
define list is in `rp/src/CMakeLists.txt`, mirroring upstream's
`small_doom_common` / `add_doom_tiny` / `tiny_settings` / `render_newhope`).
Layout: `rp/src/doom/` holds the shared engine files, `rp/src/doom/doom/`
the game, `rp/src/doom/md/` the MD platform layer that replaces upstream's
`pico/`. Not compiled: scanvideo, I2S, OPL/emu8950 and the music modules,
piconet, TinyUSB, `picoflash`, the flash save slots, `blit.S`. Every
vendored file altered for the port carries `MD/DOOM:` comments at the
change; the platform files under `md/` say which upstream file they derive
from.

- `md/i_video.c` — `I_VideoBuffer` is `fb_chunked_buffer`; both of
  upstream's `frame_buffer[2]` resolve to it (`pd_render.cpp`'s
  "other buffer minus 32 rows" tricks are patched to plain rows 168..199).
  `I_MD_PresentFrame()`, called at the end of `pd_end_frame`, drains the
  `render_frame_ready` / `display_frame_freed` semaphores, draws the overlay
  vpatch list (status bar, HUD, menus) into the frame with `V_DrawPatchList`,
  turns the requested PLAYPAL page into 16 colours + LUT
  (`doom_video_set_playpal`; pages 1–13 are derived from page 0 the way
  upstream's scan-out does) and calls `doom_video_publish()`. Full-screen
  pages are repainted every frame (`maybe_draw_single_screen`) because
  overlays land in the same buffer. No melt wipe (`wipe_start` forced 0).
- `md/i_input.c` — `I_GetEvent` = `ikbd_clear_command` + `fb_pump_rom3` +
  `ikbd_pump`, keys through `doom_input_translate`, the joystick/pad as
  edge-triggered key events (stick = cursors + Ctrl; pad South/East/West/North
  = Ctrl/Space/Alt/Shift, Start = Esc, Select = Tab). Keypad `*` and `/`
  cycle the dither mode and palette source (`doom_video`) with a HUD
  message, as the DOOM Accelerator did, and flag the settings for saving
  (`I_MD_SaveVideoSettings`; the flash write itself happens at frame end,
  see `md_main.c`). Keypad `-` cycles the audio interrupt off / Core 0 /
  Core 1 for A/B tests. `I_GetEvent` is also called by the renderer
  mid-frame (`NetUpdate`), so nothing in it may park Core 1 or touch
  flash. ESC auto-exit is off; quitting is `I_Quit` →
  `ikbd_request_boot_gem()`.
- `md/i_system.c` — the zone is everything from the C heap's break plus
  `ZONE_HEAP_MARGIN` to `__StackLimit`; `malloc`/`calloc`/`realloc`/`free`
  are `--wrap`ped into it once it exists (`SKIP_PICO_MALLOC` keeps the SDK's
  own wrappers out), pointers from before then go back to newlib. `I_Error`
  logs and quits to GEM.
- `md/i_mdsound.c` — upstream's channel model and ADPCM decoder with the
  I2S pool replaced by `I_MD_SoundFill`, the framework fill callback: mono,
  resampled onto whatever count the m68k asks for, DMA PCM or YM pairs. Runs
  from the VBL-synced timer interrupt on Core 1; the mix buffer lives in
  scratch X.
- `md/md_main.c` — `doomgame_start()` (boot palette, E1M1 pack, `I_Init`,
  `D_DoomMain`), the firmware's own screens (the 1-bit backdrop from
  `desc/bg-mono.png`, converted by `tools/png_to_mono.py` into
  `rp/src/include/bg_mono.h`, with "LOADING E1Mx" + a bar or the
  packs-missing message in the clear band at rows 81..194) and
  `I_MD_LoadLevelPack(ep, map)`, called at the top of
  `P_SetupLevel`: stops sounds, programs `/doom/E?M?.whx` with a loading
  screen, then re-points what was resolved against the old pack
  (`W_AddFile("")`, `R_InitData()`, sfx lump numbers, `V_ResetSharedPalettes`,
  the PLAYPAL pointer). It also reads the saved dither/palette at boot and
  owns `I_MD_FlushVideoSettings()`, called at the end of
  `I_MD_PresentFrame` (Core 1 idle) to write them. Save-game slots are
  stubbed to "none".
- `md/md_prof.c` — debug-only per-phase frame timer (tics / render / pre /
  cols / join / overlay / present), marked from `d_main.c` and
  `pd_render.cpp`, reported on the 64-frame debug line.
- `pd_render.cpp` — Core 1's `pd_core1_loop` runs as a framework job
  dispatched in `pd_begin_frame` and joined after `core1_done`; the
  per-frame scratch arrays are `__cart_app_free`; `RENDER_COL_MAX` 3000.
- `w_file_memory.c` — the WHX is `pack_base()`, re-read at every open.
- `d_main.c` — the demo loop only ever shows the title page (no DEMO or
  CREDIT lumps in the packs); `m_menu.c` hides Read This! and ignores F1;
  `d_loop.c`,
  `m_menu.c`, `g_game.c`, `p_saveg.c` have their piconet / flash-save
  references guarded out.

### Improvements backlog

MD/DOOM is a showcase built on "get it working, make it better". Every
shortcut taken on the way to a running game is listed here so it can be
revisited in order of visible payoff.

- **Sprite rotations.** Packs are built with `--no-rotations`, so monsters
  always face the player. Restoring them needs ~120 KB per pack: a
  streamed-from-SD sound path, or a smaller code region.
- **Sound effects at 5 kHz** (`--sfx-rate 5000`). Full 11 kHz needs the
  same ~120 KB. Streaming effects from the SD card at play time would free
  both this and the rotations budget.
- **Help and credits screens** are left out of level packs (the title and
  intermission screens are in every pack, which is why the title works), so
  the menu's Read This! item is hidden and F1 does nothing. A separate
  title pack that occupies the window while no level is loaded would bring
  them back, along with the demo loop.
- **Palette quality.** The reducer dithers between the two nearest of 16
  colours. STDOOM's software path mixes up to four with weights searched
  offline (`palette-opt`, `mix_weights_lorez` in `atari_c2p.c`); porting
  those tables gives smoother gradients for the default palette. A
  per-level generated palette could be precomputed offline too.
- **No music.** The YM2149 is free on STE-class machines while DMA sound
  plays; STDOOM's `atari_ym.c` MUS player is a template.
- **Save games** go nowhere at first; the SD card is the place.
- **Demos and the finale cast** are skipped; they need lumps the packs
  leave out.
- **The player sprite (`PLAY`) is left out of packs** (~57 KB). Nothing
  draws it in single player because the player's own mobj is culled at the
  near plane before the sprite lookup, but a look in a mirror-like
  situation (a second player, a corpse view) would want it back.
- **Direct c2p tearing.** The frame is written into the cart FB in the
  m68k's ~3 ms post-blit slack. Measured: ~2.1 ms for the LUT modes,
  ~3.4 ms for blue noise, so blue noise can tear; none has been seen yet.
  A faster blue-noise path (a per-row LUT slice) would close it.
- **`RENDER_COL_MAX` 3000** (upstream 3600): very busy views drop columns
  to black. Raise it if RAM allows.
- **Zone heap 32 KB** (measured; ~38 KB was the estimate). E1M1 and E1M2
  load; if a later map fails (`Z_Malloc` errors in the UART log), the next
  levers are listed under "Memory layout".
- **A freeze on picking up a clip in E1M2** was seen once on v0.1.x and
  has not recurred since the park handshake and the deferred settings
  write were fixed (v0.2.2); not confirmed fixed.
- **Melt wipe** is off; it needs a second frame buffer.
- **ST high resolution** (640×400 mono, low priority). Today the m68k bails
  to GEM in high-res. It would need a 1-bit reduction (the 4x4 dither
  already produces thresholds; the two-nearest step collapses to
  black/white), a 2x pixel-doubled 640×400 planar output (32,000 bytes,
  the same cart FB size), and a high-res branch in `userfw.s` for the
  screen base and a 1-plane blit. Mono users would get a sharp Doom.
- **Network download of the packs** was considered and rejected: no flash
  for lwIP/TLS beside the engine, and a throwaway downloader overlay in
  the pack window was judged a week of risky work for a one-shot install.
  The packs-missing screen (`missing_pack_screen` in `md_main.c`) points at
  the download URL instead.

## Editing guardrails

- **Never modify** `pico-sdk/`, `pico-extras/`, `fatfs-sdk/` or `lib/xpad/` — they are git submodules pinned to specific upstream revisions, and the build re-pins them on every run. To change FatFs configuration, edit `rp/src/ff/ffconf.h`.
- Don't touch `main.c` for feature work — boot changes go in `emul.c`, game-side changes in `rp/src/doom/md/` (or `doomapp.c` for the test card).
- Match the existing C style (clang-format config in `.clang-format`, clang-tidy in `.clang-tidy` — both wired up via CMake when the binaries are on `PATH`).
- Keep `cart_shared.h` and `main.s` in step: they are the two halves of one layout.

---

## Working style

These behavioral guidelines bias toward caution over speed. For trivial tasks, use judgment.

### 1. Think before coding

Before implementing:
- State your assumptions explicitly. If uncertain, ask.
- If multiple interpretations exist, present them — don't pick silently.
- If a simpler approach exists, say so. Push back when warranted.
- If something is unclear, stop. Name what's confusing. Ask.

### 2. Simplicity first

Minimum code that solves the problem. Nothing speculative.
- No features beyond what was asked.
- No abstractions for single-use code.
- No "flexibility" or "configurability" that wasn't requested.
- No error handling for impossible scenarios.
- If you write 200 lines and it could be 50, rewrite it.

Ask: "Would a senior engineer say this is overcomplicated?" If yes, simplify.

### 3. Surgical changes

Touch only what you must. Clean up only your own mess.
- Don't "improve" adjacent code, comments, or formatting.
- Don't refactor things that aren't broken.
- Match existing style, even if you'd do it differently.
- If you notice unrelated dead code, mention it — don't delete it.
- When your changes orphan an import/variable/function, remove it. Don't remove pre-existing dead code unless asked.

The test: every changed line should trace directly to the user's request.

### 4. Goal-driven execution

Define success criteria. Loop until verified.
- "Add validation" → "Write tests for invalid inputs, then make them pass"
- "Fix the bug" → "Write a test that reproduces it, then make it pass"
- "Refactor X" → "Ensure tests pass before and after"

For multi-step tasks, state a brief plan with a verification check per step.

### 5. No AI attribution

Never add AI-tool attribution to commits, PR descriptions, code comments,
docs, or any other artifact. This means **no**:
- "Generated with Claude Code", "Co-authored by Claude", "Made with ChatGPT",
  or any similar phrasing.
- `Co-Authored-By: Claude …`, `Co-Authored-By: ChatGPT …`, or any other
  AI co-author trailer.
- "AI-assisted", "written with the help of an LLM", etc., as comments or
  changelog entries.

Write the message as the human author. Do not mention AI tools used to
produce the work.

### 6. Copyright headers

Always include the appropriate copyright notice at the top of every source file
that is created or modified in the following format, where `[AUTHOR_NAME]` is
the name of the human author:

```c
/*
 * Copyright (C) 2026 [AUTHOR_NAME]
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

```
