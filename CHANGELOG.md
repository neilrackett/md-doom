# Changelog

## v0.5.5 (2026-09-14)

- The two menu items MD/DOOM adds are drawn at the menu's own size
  instead of in the little font the messages use. Doom's menu is
  artwork, one picture per item, and the level packs carry those and no
  alphabet, so there is nothing in them to set new words in: these are
  drawn by the firmware in its own 8x8 font at double size, coloured
  with whichever palette entry is the most saturated red. The Options
  item is now "Level N", since "Start level" is too wide at that size.

## v0.5.4 (2026-09-14)

- A **Booster** item below Quit Game on the main menu. It asks the same
  way Quit does and then restarts the ST into the SidecarT Booster,
  which saves powering off and holding the SELECT button. The handover
  is ordered so the ST is never reading a cartridge that is being
  replaced: the m68k restores the machine and jumps through the reset
  vector, and only once it is away in its memory test does the RP2040
  reboot itself into the Booster.

## v0.5.2 (2026-09-14)

- Mouse buttons work. The right button fires and the left button held
  strafes. The last build reported only the left button and expected
  fire to arrive on the joystick's own line, which it never does: while
  the mouse is switched on, the keyboard chip puts the wire that
  joystick 1's trigger shares with the right mouse button into the
  mouse packet and leaves the joystick packet's fire bit clear. So the
  right button is the trigger, and nothing could shoot until it was
  mapped to fire.
- The mouse moves at a usable speed. An ST mouse reports on the order of
  a hundred counts an inch, where the mice Doom's defaults were written
  for manage thousands, and the game turns only eight angle units per
  count: a full sweep of the mat used to turn a few degrees. Turning is
  scaled by eight and walking by four.
- Options has a new **Start level** setting: the map a new game begins
  on, 1 to 9 and back to 1. It is for jumping straight to a level
  without playing through, so it is not saved and it returns to 1 as
  soon as a game starts.

## v0.4.7 (2026-09-13)

- Full screen. `+` gives the game the whole 320x200 and takes the
  status bar away, `-` brings it back; the choice is saved. This is the
  original's largest screen size, and the only one this renderer can
  add: the sizes in between need a narrower view, which means tables
  and border art the build and the level packs leave out.
- The two meanings of the 168-row constant had to be told apart first.
  Upstream has one number for "how tall the view is" and "where the
  status bar starts", because they were never different. They are now
  `MAIN_VIEWHEIGHT` and `STATUS_BAR_TOP`, and the tiny build learned to
  act on a size change at all, which it could not do before: the call
  that applies one was compiled out.
- Room for the taller view: the renderer's visplane bitmap grew by
  1,280 bytes, which the cart hole could not hold, so the melt's 640
  bytes of column state moved into ordinary RAM. That and the taller
  slope table cost the zone heap about 900 bytes, leaving it near
  31 KB. Watch the log for Z_Malloc errors on the later maps.

## v0.4.5 (2026-09-13)

- The ST mouse now plays the game: X turns, Y walks, the right button
  fires and the left button held strafes. The IKBD is left in its
  default state rather than being told anything, which is what reports
  mouse packets and joystick events together; the old boot-time
  "disable mouse" command is what had been switching the mouse off. The
  demux frames the three-byte packets and the movement is posted as a
  real Doom mouse event once per tic. The buttons are the other way
  round to a PC because the ST wires joystick 1's fire to the right
  mouse button: they are one line, so the right button is a fire button
  whether we want it or not, and strafe goes to the left. A side effect
  of all this is that the desktop mouse works again after quitting to
  GEM.
- Gamma correction (the Help key) does something at last. The level was
  tracked and its message printed, but the table was never applied to
  the palette; it now is, after the pain and pickup tints, and a change
  of level forces the palette to be rebuilt even when the page has not
  changed.
- Keypad `*` and `/` no longer risk a freeze. They were rebuilding the
  4 KB dither lookup table where the key was seen, which is also the
  input poll the renderer runs part-way through a frame, on the
  renderer's own deep stack. The keypress now only records the choice
  and the rebuild happens between frames, with Core 1 idle.
- The README marks the F-keys that do nothing in this build: help and
  detail, which this renderer has no screens or modes for, and save,
  load, quick-save and quick-load, which wait on save games.
- The `-` and `+` screen size keys now say "Screen size cannot be
  changed" instead of doing nothing at all, so they do not look broken.
  This renderer has no windowed view, and the menu item for it is
  already compiled out; see the backlog in `AGENTS.md` for what
  restoring it would take. The keys still zoom the automap.

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
