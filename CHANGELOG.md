# Changelog

## v0.7.1 (2026-09-17)

- Fixed: quitting to the Booster reset the ST and then started MD/DOOM
  again instead of going to the Booster. MD/DOOM watches for the ST
  disappearing so the two can recover together after a reset, and that
  watch was still running while MD/DOOM deliberately reset the ST on its
  way out — so it treated its own shutdown as a fault and restarted
  itself. The watch now stands down before any planned reset.
- Gamepad support is off for now, and starting MD/DOOM from the desktop
  is only safe on a machine with nothing else resident. MD/DOOM claims a
  fixed region of memory for its screens without checking whether
  anything else is using it, so anything loaded from your AUTO folder —
  a gamepad driver, for instance — moves things into that region and
  MD/DOOM bombs shortly after the splash screen. Letting it start on its
  own from the cartridge is unaffected. The gamepad code is not at
  fault; the crash happens with it compiled out, which is why it is
  simply switched off until the memory claim is fixed.

## v0.7.0 (2026-09-16)

- MD/DOOM now runs its Atari ST firmware from the ST's own RAM instead
  of from the cartridge, which frees the cartridge's 16 KB of code space
  for Doom's memory pool — about 16 KB more for the levels to live in.
- The cartridge appears on the desktop as drive `c` holding
  `MDDOOM.TOS`, so you can start the game by double-clicking it after
  the machine has booted. That is what makes a gamepad work: an Xpad
  driver in your AUTO folder is loaded by then, where the automatic
  start happens too early to see it.
- Hold either SHIFT key at power-on to go to the desktop instead of
  starting the game.
- Gamepad support is on.
- Quitting returns you to the desktop, and the ST and the cartridge now
  recover together if you press reset mid-game.
- Boot messages: version, copyright and licence first, then whatever
  needs saying — that it needs a colour monitor or 512 KB, that SHIFT
  goes to the desktop, or how to start the game again after quitting.

## v0.6.21 (2026-09-16)

- Boot messages are sentence case after the `MD/DOOM:` prefix, with no
  trailing full stop.

## v0.6.19 (2026-09-16)

- Quitting a desktop-launched game used to come back up saying to hold
  SHIFT for the desktop, which is where that boot was going anyway. The
  skip-autostart check now runs before the hint, and that boot says how
  to get back into the game instead: run MDDOOM.TOS from drive c.

## v0.6.18 (2026-09-16)

- The boot messages now follow MD/Net's shape: the standard banner —
  version, copyright, licence and URL — prints first, and whatever else
  there is to say goes underneath it. The banner comes from a generated
  `version.inc`, the way MD/Net builds its, so the version on screen
  always matches the build. Every message either entry path produces now
  has the banner above it, including the three the desktop launcher can
  print.

## v0.6.17 (2026-09-16)

- The boot messages read consistently, and leaving for the desktop no
  longer announces itself. The high-resolution message also said to
  switch to medium and reboot, which stopped being true when MD/DOOM
  started switching to low resolution itself; it now just says it needs
  a colour monitor.

## v0.6.16 (2026-09-16)

- Starting MD/DOOM from the desktop in medium resolution no longer
  shows a stripy mess: it switches to low resolution first, which is
  what the game's picture needs. Starting from the desktop on a mono
  monitor says so and returns instead. The autostart never hit this,
  because the cartridge runs before your saved desktop resolution is
  applied.
- Quitting a game started from the desktop is now a single, definite
  sequence rather than something the firmware works out after the fact,
  which should stop the reboot loop.

## v0.6.12 (2026-09-15)

- Gamepad support works. It never could before, for two reasons that
  both needed fixing: MD/DOOM started before the AUTO folder, so a
  driver installed there did not exist yet when the game looked for one,
  and the game stopped the system timer the driver relies on. Start
  MD/DOOM from the desktop and a driver in your AUTO folder is found and
  kept running.
- Drivers that hook the vertical blank instead of the system timer are
  not supported: MD/DOOM needs the vertical blank for the screen.

## v0.6.11 (2026-09-15)

- MD/DOOM can be started from the desktop: open the cartridge icon and
  run MDDOOM.TOS. Worth doing if you have a gamepad, because programs in
  your AUTO folder have loaded by then and the game can see them, which
  it cannot when it starts straight from the cartridge.
- Quitting now restarts your ST and returns you to the desktop. The game
  takes the machine over completely while it runs, so there is nothing
  left to hand back to.

## v0.6.9 (2026-09-15)

- The game gets another 16 KB of memory, which is the point of
  everything since v0.6.1. The Atari ST firmware now runs from the ST's
  own RAM, so the 16 KB the cartridge reserved for it is free, and the
  game takes it. On the biggest map that is worth roughly 1,500 more
  rendered columns: E1M6 should now look like the rest of the episode
  rather than showing black holes.
- If you reset your ST, the SidecarT notices and restarts with it, so
  one press of reset restores both.

## v0.6.5 (2026-09-15)

- The ST and the SidecarT now tell each other they are both there. The
  ST sends a heartbeat every frame once it is running from its own RAM,
  and the firmware waits for the first one at startup. Nothing visible
  changes yet; it is what the memory reclaim will be gated on, and it
  gives the serial log a definite answer about whether an ST is
  attached.

## v0.6.3 (2026-09-15)

- The Atari ST firmware now runs from the ST's own RAM instead of being
  executed in place from the cartridge. Nothing about the game changes,
  but it is what lets the cartridge's 16 KB of code space become memory
  for the game to use, which is worth roughly a thousand more rendered
  columns on the biggest maps.
- The image carries a header so the copy can be checked at both ends,
  and the build refuses to produce one too large for the space reserved
  for it.

## v0.6.1 (2026-09-15)

- Hold either Shift key at power-on to boot to the desktop instead of the
  game. The boot screen says so.
- The cartridge icon is named MDDOOM.TOS and no longer crashes if you
  double-click it: it had no run address at all, which the desktop reads
  as a jump to address 0. It says the launcher is not ready yet, which
  is the next piece of work.
- MD/DOOM now says so and stops if the machine has less than 512 KB.
  The screen pages have always assumed it and nothing ever checked.

## v0.5.21 (2026-09-14)

- The level selector uses the ordinary message font, and the firmware no
  longer carries a way to draw menu-sized letters of its own. That
  existed for this one item, which is now a debug-build option, so it
  was 90 lines and a few hundred bytes for something nobody ships.

## v0.5.19 (2026-09-14)

- The Options menu's Level setting is a build-time option now, off by
  default: it is a testing aid rather than something to ship. Build with
  `MDDOOM_LEVEL_SELECT=1` to get it back, the way the test card works.
  The doubled-font text it needed goes with it.

## v0.5.17 (2026-09-14)

- The sky survives a level change. Starting a new game after playing a
  different level left the sky replaced by garbage: F_SKY1's flat number
  is worked out before the level's asset pack is swapped in, and since
  each pack carries only the flats its own map needs, the number then
  pointed at the wrong flat. It is worked out again once the new pack is
  in place.

## v0.5.15 (2026-09-14)

- The renderer and the level now share memory properly instead of
  splitting it at build time. The column buffer is taken from the Doom
  zone once the level is in, so it gets whatever that level did not
  need: the small maps get the full 3600 columns the engine was written
  for, and only the biggest map has to make do. Every fixed split tried
  before this was either too mean for E1M1, which left black holes over
  half the picture, or too mean for E1M6, which ran the zone out and
  panicked.
- Room for it came out of the debug build's flash: FatFs's formatting,
  find, expand and string helpers are switched off, fatal errors print
  through the compact printf rather than newlib's, and two config
  parsers use `strtol` where they used `sscanf`, which was pulling in
  12 KB of the scanf family.

## v0.5.9 (2026-09-14)

- E1M6 gets the memory it needs. The previous build gave the zone about
  37 KB and E1M6 filled every byte of it before it was finished
  building the level; every other map in the episode loads and plays.
  The renderer's column budget goes from 2400 to 1800 and the heap
  margin from 4 KB to 2 KB, which takes the zone to about 46 KB -- the
  figure upstream quotes for its own busiest levels. Very busy views
  may drop a few more columns to black in exchange.

## v0.5.8 (2026-09-14)

- Starting on one of the bigger maps no longer dies. E1M6 was running
  the Doom zone out of memory as it built the level, and in this build
  that is a panic rather than an error, which is why it looked like a
  freeze with the loading screen still up. The level data alone comes to
  about 31 KB on E1M6 -- twice E1M4, three times E1M1 -- against a zone
  of about the same size. The renderer's column budget, the only large
  tunable buffer left, gives the zone its memory, so it drops from 3000
  columns to 2400 and the zone goes to about 38 KB. Very busy views may
  drop a few more columns to black in exchange.
- Every level load now reports what is left of the zone, and running it
  out says how much was wanted and how much was free. The measured cost
  of all nine maps is in `AGENTS.md`.

## v0.5.7 (2026-09-14)

- Quit Game now asks where to go: Y for GEM, B for the Booster. The
  Booster moves here from the main menu, where an item of its own had
  to draw its own letters and looked out of place among the original's
  artwork.
- Loading a level pack no longer buzzes. The sound mixer reads its
  samples straight out of the pack window, which the load is busy
  erasing, and it runs from an interrupt on the core that is parked
  around every flash write -- so for the second or so the erase takes,
  nothing refilled the ST's audio buffer and it looped whatever was
  left in there. The mixer is now stopped for the duration, which
  silences the buffer as it goes.
- The Options item reads "LEVEL: N".

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
