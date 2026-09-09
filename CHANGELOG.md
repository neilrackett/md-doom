# Changelog

## v0.4.0 (2026-09-09)

The release build.

- Tidy-up before release. Unused code is gone: the audio module's loop
  player, YMS file streamer and polled refill, the framework's sprite
  blit module, the IKBD ring's drop counter, the reducer's nearest-pen
  table and the exit-handler list. Keypad `-`, which cycles the audio
  interrupt between cores for A/B tests and can stall the renderer,
  now exists only in debug builds. The SELECT button was configured
  twice and the pack folder looked up twice; once each now. Comments
  left over from the framework template and earlier designs (a planar
  scratch buffer, YM-only audio, demo apps) brought up to date.

## v0.3.4 (2026-09-09)

- Finishing E1M8 no longer drops to GEM after the closing text. The
  finale's art screen (the shareware order screen, HELP2) is not in the
  packs, so the game now returns to the title instead of failing the
  lump lookup.

## v0.3.3 (2026-09-08)

- The melt no longer rewrites rows that cannot have changed (the old
  picture before a column starts moving, the new one once it has been
  revealed). A step measured 3.9 ms on hardware when every row was
  rewritten, past the m68k's post-blit slack, so a melt frame could tear.

## v0.3.1 (2026-09-08)

- The screen melt is back: level to intermission, intermission to level,
  title to game. It runs in place on the cart framebuffer, which still
  holds the last frame, so it needs no second buffer. At a level start
  it melts from the loading screen rather than the intermission.

## v0.2.4 (2026-09-08)

- Retro palettes: the brightness stretch eased from a gamma of 0.5 to
  0.75. At 0.5 the walls came out as white dither; this keeps the colour
  and puts the shading back.

## v0.2.3 (2026-09-08)

- The fixed retro palettes (EGA, CGA, C64, ZX Spectrum, PICO-8) no longer
  render the game as black with a few highlights. Doom's dark browns and
  greys were all nearest black by distance; they now pick a pen by hue,
  stretch their brightness, and dither along a ladder of black plus the
  pens of that hue, so walls and floors come out in colour.

## v0.2.2 (2026-09-07)

- Changing the palette or dither mid-frame no longer freezes: the settings
  write is deferred to the end of the frame, when Core 1 is idle and can
  be parked.
- Core 1's park is acknowledged before flash is touched; previously an
  erase could start while Core 1 was still finishing an audio refill.
- Debug line reports the audio mode and fill length.
- The loud 1-2 s burst of noise every few minutes on STE DMA sound: the
  DMA loop's start/end registers were rewritten from the VBL loop and a
  frame end landing mid-update played 32 KB of screen memory as audio.
  They are now set from a Timer-A frame-end interrupt, a full frame
  away from the next latch.

## v0.2.0 (2026-09-07)

- Loading screen drawn in plain black and white, whatever tint the game was
  showing when the level ended.
- Keypad `*` and `/` cycle the dither mode and palette source in-game.
- Fixed palettes for fun: EGA, CGA (palette 1), C64, ZX Spectrum, PICO-8.
- Blue-noise dither mode (md-mjpeg's 32×32 void-and-cluster tile).
- Intermission background painted every frame (the stats were drawn over
  the last level frame).
- Sound refilled from a VBL-synced timer interrupt on Core 1 instead of
  Core 1's idle time, which fell silent on heavy frames and looped the
  buffer (a 50 Hz buzz). The same interrupt on Core 0 stalled the renderer
  for 100–200 ms at a time; keypad `-` switches between off / Core 0 /
  Core 1 for comparison.
- Dither and palette choices are saved and restored at the next boot.

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
