# Changelog

## v0.1.0 (2026-09-07)

First release: the shareware episode plays on real hardware. Built on the
md-framebuffer-template as carried in MD/Lynx.

- Framework: 320×200 16-colour framebuffer with dual-core chunky-to-planar,
  interrupt-driven IKBD keyboard + port-1 joystick, Xpad gamepad consumer,
  STE/MegaSTE DMA sound with measured buffer length and a YM2149 fallback,
  detected at boot. Core 0 at 400 MHz.
- `doom_video`: 256 → 16 colour reduction (STDOOM subset, generated, or
  grey), nearest / 2×2 / 4×4 Bayer / halftone dither, fused with the c2p and
  split across both cores. Verified against a bit-level reference.
- `doom_input`: ST scancodes → Doom key codes with original PC bindings;
  joystick and gamepad folded into a Doom joystick state.
- `doom_sound`: 8-channel sfx mixer producing STE DMA PCM or YM pairs at
  whatever length the m68k asks for.
- `pack`: SD card → flash programmer for per-level asset packs, with the
  `PACK_FLASH` window (768 KB) reserved in the memory map.
- `tools/levelpack.py` + `tools/whd_gen/`: per-level WAD subsetting and WHX
  conversion, with the size levers needed to fit the shareware episode.
- Test card app driving all of the above on hardware (now behind
  `MDDOOM_TEST_CARD=1`).
- The rp2040-doom engine vendored under `rp/src/doom` with an MD platform
  layer: single 320×200 frame in the chunked buffer, overlays composited
  into it, per-level pack swap at map start, sfx mixed on Core 1, malloc
  routed into the zone. Boots to the Doom title screen (the level packs
  carry TITLEPIC; the attract loop shows only the title). No help screens,
  wipe, music or saves yet.
- Loading and missing-pack screens on a 1-bit backdrop (`desc/bg-mono.png`).

---
