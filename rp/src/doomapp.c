/*
 * Copyright (C) 2026 Neil Rackett
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * File: doomapp.c
 * Description: The pipeline test card described in doomapp.h, built
 *              in place of the game with MDDOOM_TEST_CARD=1.
 *
 * Keys on the test card:
 *   F1        next dither mode        F2   next palette source
 *   F3        program /doom/E1M1.whx from SD into the pack window
 *   Ctrl / joystick fire / pad South  play the test sound
 *   Esc       back to GEM (in the game proper Esc will be the menu)
 * Everything else just shows its Doom key name, which is how to check
 * the keyboard mapping against a real ST.
 */

#include "doomapp.h"

#if MDDOOM_TEST_CARD


#include <stdio.h>
#include <string.h>

#include "audio.h"
#include "doom_input.h"
#include "doom_sound.h"
#include "doom_video.h"
#include "fb.h"
#include "fb_chunked.h"
#include "fb_font.h"
#include "pack.h"
#include "pico/stdlib.h"
#include "pico/time.h"


/* Pens used for the overlay text. The test palette below puts a
 * 16-step grey ramp at indices 0..15, so these are what they say. */
#define PEN_BLACK 0
#define PEN_WHITE 15
#define PEN_GREY 9

/* --------------------------------------------------------------------- */
/* Test palette + test card                                              */

/* A stand-in for PLAYPAL until the engine supplies the real one:
 * 0..15 grey ramp, 16..231 a 6x6x6 colour cube, 232..255 a saturated hue
 * wheel. Covers the reducer with the kinds of colours Doom has (lots of
 * near-greys and browns, a few saturated accents). */
static uint8_t s_playpal[768];

static void build_test_palette(void) {
  uint8_t *p = s_playpal;
  for (int i = 0; i < 16; i++) {
    *p++ = (uint8_t)(i * 17);
    *p++ = (uint8_t)(i * 17);
    *p++ = (uint8_t)(i * 17);
  }
  for (int r = 0; r < 6; r++) {
    for (int g = 0; g < 6; g++) {
      for (int b = 0; b < 6; b++) {
        *p++ = (uint8_t)(r * 51);
        *p++ = (uint8_t)(g * 51);
        *p++ = (uint8_t)(b * 51);
      }
    }
  }
  for (int i = 0; i < 24; i++) {
    /* Hue wheel, full saturation: sector 0..5, position within it. */
    int h = i * 6 * 255 / 24; /* 0..1530 */
    int sector = h / 255, f = h % 255;
    int r = 0, g = 0, b = 0;
    switch (sector) {
      case 0: r = 255; g = f; break;
      case 1: r = 255 - f; g = 255; break;
      case 2: g = 255; b = f; break;
      case 3: g = 255 - f; b = 255; break;
      case 4: r = f; b = 255; break;
      default: r = 255; b = 255 - f; break;
    }
    *p++ = (uint8_t)r;
    *p++ = (uint8_t)g;
    *p++ = (uint8_t)b;
  }
}

/* 16 x 16 swatches of every index, in the band below the text. */
#define CARD_Y0 40
#define CARD_ROWS 10
#define CARD_COLS 16
#define SWATCH_W (FB_CHUNKED_W / CARD_COLS)  /* 20 */
#define SWATCH_H 15

static void draw_test_card(void) {
  for (int y = 0; y < CARD_ROWS * SWATCH_H; y++) {
    uint8_t *row = fb_chunked_buffer + (CARD_Y0 + y) * FB_CHUNKED_W;
    const int sy = y / SWATCH_H;
    for (int x = 0; x < FB_CHUNKED_W; x++) {
      const int sx = x / SWATCH_W;
      int idx = sy * CARD_COLS + sx;
      /* Rows 0..15 hold all 256 indices; a 16-row card would not fit
       * under the text, so the wheel and the cube's top rows share the
       * last card row as a smooth gradient instead. */
      if (sy == CARD_ROWS - 1) idx = 232 + (x * 24) / FB_CHUNKED_W;
      row[x] = (uint8_t)idx;
    }
  }
  /* A horizontal grey ramp at the bottom, full width, to judge the
   * dither on a gradient. */
  for (int y = CARD_Y0 + CARD_ROWS * SWATCH_H; y < FB_CHUNKED_H; y++) {
    uint8_t *row = fb_chunked_buffer + y * FB_CHUNKED_W;
    for (int x = 0; x < FB_CHUNKED_W; x++) {
      /* 16..231 cube greys are at 16 + k*43 (k = 0..5); use the 0..15
       * ramp for a finer gradient. */
      row[x] = (uint8_t)((x * 16) / FB_CHUNKED_W);
    }
  }
}

/* --------------------------------------------------------------------- */
/* Test sound: a short falling sweep, 8-bit unsigned at 11,025 Hz         */

#define BLIP_RATE 11025u
#define BLIP_LEN (BLIP_RATE / 4u)
static uint8_t s_blip[BLIP_LEN];

static void build_blip(void) {
  uint32_t phase = 0;
  for (uint32_t i = 0; i < BLIP_LEN; i++) {
    /* Frequency slides 880 -> 220 Hz; amplitude decays linearly. */
    const uint32_t hz = 880u - (660u * i) / BLIP_LEN;
    phase += (hz << 16) / BLIP_RATE;
    const int32_t sq = (phase & 0x8000u) ? 1 : -1;
    const int32_t amp = (int32_t)(120u * (BLIP_LEN - i) / BLIP_LEN);
    s_blip[i] = (uint8_t)(128 + sq * amp);
  }
}

/* --------------------------------------------------------------------- */
/* State                                                                  */

static int s_last_key;
static bool s_last_key_down;
static int s_blip_channel = -1;
static uint32_t s_fps, s_fps_frames, s_fps_t0;
static bool s_fire_held;
static char s_status[40];

static void fmt_uint(char *dst, size_t n, uint32_t v) {
  snprintf(dst, n, "%lu", (unsigned long)v);
}

static void play_blip(void) {
  s_blip_channel = doom_sound_start(s_blip, BLIP_LEN, BLIP_RATE, 127, 128);
}

/* Progress screen while a pack is being programmed. Runs between chunks
 * with XIP live, so drawing and publishing are fine. */
static void pack_progress(uint32_t done, uint32_t total) {
  const int w = (int)((uint64_t)done * (FB_CHUNKED_W - 40) / (total ? total : 1));
  memset(fb_chunked_buffer + 96 * FB_CHUNKED_W, PEN_BLACK, FB_CHUNKED_W * 12);
  for (int y = 96; y < 108; y++) {
    memset(fb_chunked_buffer + y * FB_CHUNKED_W + 20, PEN_WHITE, (size_t)w);
  }
  doom_video_publish();
}

static void load_pack(void) {
  /* The staging buffer is the framebuffer, so the card is gone
   * afterwards; redraw it. Draw the banner first so the screen holds
   * something sensible through the erase. */
  fb_chunked_clear(PEN_BLACK);
  font_set_color(PEN_WHITE);
  font_align(FONT_ALIGN_CENTER);
  font_move(160, 80);
  font_print("LOADING E1M1.WHX");
  doom_video_publish();

  const bool ok = pack_load("/doom/E1M1.whx", pack_progress);
  snprintf(s_status, sizeof(s_status), ok ? "PACK OK %lu B" : "PACK FAILED",
           (unsigned long)pack_loaded_size());
  draw_test_card();
}

/* --------------------------------------------------------------------- */
/* Public                                                                 */

void doomapp_init(void) {
  build_test_palette();
  build_blip();
  doom_video_init();
  /* The synthetic palette has no meaningful STDOOM subset, so the card
   * starts on the generated palette; F2 cycles to the others. */
  doom_video_set_palette_mode(DOOM_VIDEO_PAL_GENERATED);
  doom_video_set_playpal(s_playpal);
  doom_sound_init();

  font_set_font(&font8x8);
  font_set_border(0, 0);
  fb_chunked_clear(PEN_BLACK);
  draw_test_card();

  snprintf(s_status, sizeof(s_status), pack_present() ? "PACK IN FLASH" : "NO PACK");
  s_fps_t0 = time_us_32();
}

void doomapp_handle_key(const ikbd_key_event_t *k) {
  const int key = doom_input_translate(k->scancode);
  if (key == 0) return;
  s_last_key = key;
  s_last_key_down = k->is_press;
  if (!k->is_press) return;

  switch (key) {
    case DOOM_KEY_F1:
      doom_video_set_dither((doom_video_get_dither() + 1) % DOOM_VIDEO_DITHER_COUNT);
      break;
    case DOOM_KEY_F2:
      doom_video_set_palette_mode((doom_video_get_palette_mode() + 1) %
                                  DOOM_VIDEO_PAL_COUNT);
      break;
    case DOOM_KEY_F3:
      load_pack();
      break;
    case DOOM_KEY_RCTRL:
      play_blip();
      break;
    default:
      break;
  }
}

void doomapp_render_frame(void) {
  char num[12];

  /* Joystick / pad: fire on the rising edge only. */
  doom_joy_state_t joy;
  doom_input_poll_joystick(&joy);
  const bool fire = (joy.buttons & DOOM_JOYB_FIRE) != 0;
  if (fire && !s_fire_held) play_blip();
  s_fire_held = fire;

  /* Text band at the top (rows 0..39): cleared and rewritten each
   * frame; the card below is static. */
  memset(fb_chunked_buffer, PEN_BLACK, FB_CHUNKED_W * CARD_Y0);
  font_set_color(PEN_WHITE);
  font_align(FONT_ALIGN_LEFT);

  font_move(4, 2);
  font_print("MD/DOOM " RELEASE_VERSION);
  font_align(FONT_ALIGN_RIGHT);
  font_move(316, 2);
  switch (audio_get_mode()) {
    case AUDIO_MODE_DMA: font_print("STE DMA"); break;
    case AUDIO_MODE_YM: font_print("YM2149"); break;
    default: font_print("NO SOUND"); break;
  }
  font_align(FONT_ALIGN_LEFT);

  font_set_color(PEN_GREY);
  font_move(4, 12);
  font_print("F1 ");
  font_set_color(PEN_WHITE);
  font_print(doom_video_dither_name(doom_video_get_dither()));
  font_set_color(PEN_GREY);
  font_move(124, 12);
  font_print("F2 ");
  font_set_color(PEN_WHITE);
  font_print(doom_video_palette_name(doom_video_get_palette_mode()));
  font_set_color(PEN_GREY);
  font_move(228, 12);
  font_print("F3 ");
  font_set_color(PEN_WHITE);
  font_print(s_status);

  font_set_color(PEN_GREY);
  font_move(4, 22);
  font_print("KEY ");
  font_set_color(PEN_WHITE);
  font_print(doom_input_key_name(s_last_key));
  if (s_last_key) font_print(s_last_key_down ? " DOWN" : " UP");
  font_set_color(PEN_GREY);
  font_move(124, 22);
  font_print(joy.pad_present ? "PAD " : "JOY ");
  font_set_color(PEN_WHITE);
  font_print(joy.x < 0 ? "L" : joy.x > 0 ? "R" : "-");
  font_print(joy.y < 0 ? "U" : joy.y > 0 ? "D" : "-");
  font_print((joy.buttons & DOOM_JOYB_FIRE) ? " FIRE" : "");
  font_print((joy.buttons & DOOM_JOYB_USE) ? " USE" : "");
  font_print((joy.buttons & DOOM_JOYB_STRAFE) ? " STRAFE" : "");
  font_print((joy.buttons & DOOM_JOYB_RUN) ? " RUN" : "");

  font_set_color(PEN_GREY);
  font_move(4, 32);
  font_print("C2P ");
  font_set_color(PEN_WHITE);
  fmt_uint(num, sizeof(num), doom_video_last_convert_us());
  font_print(num);
  font_print(" US");
  font_set_color(PEN_GREY);
  font_move(124, 32);
  font_print("FPS ");
  font_set_color(PEN_WHITE);
  fmt_uint(num, sizeof(num), s_fps);
  font_print(num);
  font_set_color(PEN_GREY);
  font_move(228, 32);
  font_print("SFX ");
  font_set_color(PEN_WHITE);
  font_print(doom_sound_is_playing(s_blip_channel) ? "PLAYING" : "CTRL/FIRE");

  doom_video_publish();

  /* Frames per second over the last second. */
  s_fps_frames++;
  const uint32_t now = time_us_32();
  if (now - s_fps_t0 >= 1000000u) {
    s_fps = s_fps_frames;
    s_fps_frames = 0;
    s_fps_t0 = now;
  }
}

void doomapp_audio_fill(uint8_t *buf, uint32_t bytes) {
  doom_sound_fill(buf, bytes);
}

#endif /* MDDOOM_TEST_CARD */
