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
is built on per-level packs, and the engine port plan. This skill is the
quick reference.

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

## Main-loop contract (emul.c)

```c
while (true) {
  ikbd_clear_command();
  fb_pump_rom3();
  ikbd_pump();
  while (ikbd_pop_key(&k)) doomapp_handle_key(&k);
  doomapp_render_frame();      // ends in doom_video_publish(), blocks on VBL
  audio_render_frame();
}
```

## Critical constraints

- **Flash**: 1152 KB slot = 372 KB code + 780 KB pack window. Game data
  comes in per-level packs; `tools/levelpack.py --no-ui --no-rotations
  --sfx-rate 5000` is what fits the shareware episode. Check `--budget`
  against `PACK_FLASH` whenever the boundary moves.
- **RAM**: ~50 KB of heap window with the engine in, ~38 KB of it Doom's
  zone. Check `__bss_end__`..`__StackLimit` in the map after adding any
  static; AGENTS.md lists the remaining levers.
- **Text on the Doom path** can only use PLAYPAL indices 0..15
  (`font_set_color` masks to 4 bits).
- **Core 1** is free apart from the c2p bottom half; one
  `fb_core1_dispatch` per `fb_core1_wait`, never across a publish.
- **Flash programming** (`pack_load`) parks Core 1 and disables
  interrupts per chunk; never call it from Core 1 or from an IRQ, and stop
  sounds first (Core 1 mixes from the pack while idle).
- **The cart FB is written in place** by `doom_video_publish` between
  `fb_wait_blit_ack` and `fb_frame_done`; keep that window short.

## Verifying without hardware

Build: `make debug`. The fused LUT + c2p has a host test pattern
(random frame + random LUT vs. a naive planar encoder); reproduce it if
`doom_c2p_rows` changes. Everything else needs the ST or Hatari with the
SidecarT emulation.
