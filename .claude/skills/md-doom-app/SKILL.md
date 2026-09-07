---
name: md-doom-app
description: >-
  Use when building, modifying, or debugging MD/DOOM -- Doom for the
  Atari ST on the SidecarTridge Multi-device, built on the
  md-framebuffer-template; the RP2040 in the cartridge draws a 320x200
  16-colour framebuffer and the firmware blits it to the ST each VBL.
  Covers the chunked framebuffer, the doom_video / doom_input /
  doom_sound / pack APIs, the main-loop pattern, the flash and RAM
  budgets, and the level-pack tooling. Triggers on tasks like "vendor
  the engine", "why is the picture wrong", "add a key", "the pack does
  not fit", or "why does my app show garbage".
---

# MD/DOOM on the md-framebuffer-template

**Read `AGENTS.md` first** -- it is the full playbook; the "MD/DOOM"
section covers the app layer, the measured flash budget and why the port
is built on per-level packs, the engine port and the improvements
backlog. This skill is the quick reference.

## The model

The app runs on the RP2040 in the cartridge. Doom draws **320x200 bytes
of PLAYPAL indices into `fb_chunked_buffer`** and the app calls
**`doom_video_publish()` once per frame**: it maps every pixel through a
16-colour dither LUT, packs the ST planes (both cores), waits for the
ST's VBL and hands the frame over tear-free at 50 Hz. The 16 hardware
colours travel with the frame. `doom_video_set_playpal()` is Doom's
`I_SetPalette`.

## Where the code lives

- `rp/src/doom/` -- the vendored rp2040-doom engine; `rp/src/doom/md/` is
  the MD platform layer (video, input, system, sound, level-pack swap).
  `emul.c` hands over to `doomgame_start()` in `md/md_main.c`.
- `rp/src/doomapp.c` -- the pipeline test card, compiled only with
  `MDDOOM_TEST_CARD=1`.
- `rp/src/doom_video.c` -- palette sources, dither modes, the fused LUT +
  c2p. `rp/src/doom_input.c` -- scancode -> Doom key, joystick + Xpad.
  `rp/src/doom_sound.c` -- the sfx mixer. `rp/src/pack.c` -- SD -> flash.
- `rp/src/emul.c` -- boot sequence and the main loop.
- `rp/src/memmap_rp.ld` -- the `FLASH` / `PACK_FLASH` split (372 K / 780 K).
- `target/atarist/src/userfw.s` -- the m68k side (per-VBL blit, palette,
  audio, IKBD). Only touch it for new m68k behaviour; it forces a Docker
  `stcmd` build.
- `tools/levelpack.py`, `tools/whd_gen/` -- the offline pack builder.

## The API

```c
// video: doom_video.h
doom_video_set_playpal(rgb768);          // rebuild 16 colours + LUT
doom_video_set_dither(mode); doom_video_set_palette_mode(mode);
doom_video_publish();                    // LUT + c2p + VBL-synced hand-off
// input: doom_input.h
int key = doom_input_translate(scancode);   // doomkeys.h value or 0
doom_input_poll_joystick(&joy);             // x, y, buttons, pad_present
// sound: doom_sound.h
int ch = doom_sound_start(pcm8, len, rate_hz, vol127, sep255);
doom_sound_fill(buf, bytes);             // the audio_set_fill_callback target
// packs: pack.h
pack_load("/doom/E1M1.whx", progress_cb); pack_base(); pack_capacity();
// framework: fb_chunked.h, fb_font.h, ikbd.h, audio.h, palette.h
```

## Per-frame contract

The game has no framework main loop: `emul.c` hands over to
`doomgame_start()` and the engine drives the frame itself.

```c
I_GetEvent()          // ikbd_clear_command + fb_pump_rom3 + ikbd_pump, keys -> events
                      // (also called mid-render by NetUpdate: never park Core 1 here)
pd_begin_frame()      // dispatches Core 1's render job
pd_end_frame()        // joins it, then I_MD_PresentFrame():
                      //   overlays -> palette -> doom_video_publish() (blocks on VBL)
                      //   -> I_MD_FlushVideoSettings() (Core 1 idle: flash is safe here)
```

Audio is refilled from a 1 ms timer interrupt on Core 1
(`audio_start_vbl_timer(1)`), synced to the m68k's VBL ack; nothing in
the frame calls it. The test card (`MDDOOM_TEST_CARD=1`) keeps the old
framework loop in `emul.c`.

## Critical constraints

- **Flash**: 1152 KB slot = 372 KB code + 780 KB pack window. Game data
  comes in per-level packs; `tools/levelpack.py --no-ui --no-rotations
  --sfx-rate 5000` is what fits the shareware episode. Check `--budget`
  against `PACK_FLASH` whenever the boundary moves.
- **RAM**: ~48 KB of heap window with the engine in, 32 KB of it Doom's
  zone on hardware. Check `__bss_end__`..`__StackLimit` in the map after
  adding any static; AGENTS.md lists the remaining levers.
- **Text on the Doom path** can only use PLAYPAL indices 0..15
  (`font_set_color` masks to 4 bits).
- **Core 1** runs the render job, the c2p bottom half and the audio
  interrupt; one `fb_core1_dispatch` per `fb_core1_wait`, never across a
  publish. The audio interrupt must stay on Core 1: on Core 0 it stalls
  the renderer (measured, cause unknown).
- **Flash programming** (`pack_load`, `settings_save`) parks Core 1 and
  disables interrupts; only from the tics phase or the end of
  `I_MD_PresentFrame`, never from `I_GetEvent`, Core 1 or an IRQ, and stop
  sounds first (the audio interrupt mixes from the pack).
- **STE DMA sound** start/end registers are written only from the m68k's
  Timer-A frame-end handler (`userfw_snd_irq`), never from the VBL loop.
- **The cart FB is written in place** by `doom_video_publish` between
  `fb_wait_blit_ack` and `fb_frame_done`; keep that window short.

## Verifying without hardware

Build: `make debug`. The fused LUT + c2p has a host test pattern
(random frame + random LUT vs. a naive planar encoder); reproduce it if
`doom_c2p_chunks` or `doom_c2p_block` changes. Everything else needs the
ST: ask for the UART log, whose 64-frame debug line (c2p time, longest
frame, audio counters, per-phase maxima) is how hardware bugs get found.
