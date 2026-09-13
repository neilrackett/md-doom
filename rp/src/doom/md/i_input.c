/*
 * Copyright (C) 2026 Neil Rackett
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * File: i_input.c
 * Description: Doom's input interface on the MD framework. I_GetEvent
 *              drains the cart-bus capture ring, runs the IKBD demux and
 *              posts the decoded keys as Doom events; the port-1 joystick
 *              and an Xpad gamepad are turned into key presses on their
 *              edges so the vanilla key bindings cover them too. The ST
 *              mouse is posted as a real ev_mouse once per tic, from
 *              I_MD_PostMouseEvent.
 */

#include <string.h>

#include "d_event.h"
#include "doomkeys.h"
#include "doomtype.h"
#include "i_input.h"
#include "m_argv.h"

#include "audio.h"
#include "doom/doomstat.h"
#include "doom_input.h"
#include "doom_video.h"
#include "fb.h"
#include "ikbd.h"

/* Keypad * and / cycle the dither mode and the palette source at run
 * time, as the DOOM Accelerator did, and say so in the HUD. Neither key
 * means anything to Doom. */
#define SCAN_KP_MULTIPLY 0x66
#define SCAN_KP_DIVIDE 0x65
/* Debug builds only: keypad - cycles the audio refill interrupt off /
 * Core 0 / Core 1 for A/B tests. Core 0 stalls the renderer, so a
 * release build must not offer it one key away from * and /. */
#define SCAN_KP_MINUS 0x4A

extern void I_MD_SaveVideoSettings(void);

/* The next dither / palette to switch to, or -1 for "no change". The
 * key is seen from the input poll, which the renderer also runs
 * mid-frame from NetUpdate: rebuilding the 4 KB dither LUT there costs
 * a millisecond of the frame's budget and does it on the renderer's
 * own deep stack, so only the choice is recorded here and
 * I_MD_ApplyVideoMode does the work from I_MD_PresentFrame, between
 * frames, with Core 1 idle. */
static int8_t s_pending_dither = -1;
static int8_t s_pending_palette = -1;

static void cycle_video_mode(uint8_t scancode) {
  if (scancode == SCAN_KP_MULTIPLY) {
    int cur = s_pending_dither >= 0 ? s_pending_dither : (int)doom_video_get_dither();
    s_pending_dither = (int8_t)((cur + 1) % DOOM_VIDEO_DITHER_COUNT);
    players[consoleplayer].message =
        doom_video_dither_name((doom_video_dither_t)s_pending_dither);
  } else {
    int cur = s_pending_palette >= 0 ? s_pending_palette : (int)doom_video_get_palette_mode();
    s_pending_palette = (int8_t)((cur + 1) % DOOM_VIDEO_PAL_COUNT);
    players[consoleplayer].message =
        doom_video_palette_name((doom_video_palette_t)s_pending_palette);
  }
}

/* Apply a dither / palette change asked for since the last frame.
 * Called from I_MD_PresentFrame. */
void I_MD_ApplyVideoMode(void) {
  if (s_pending_dither < 0 && s_pending_palette < 0) return;
  if (s_pending_dither >= 0) {
    doom_video_set_dither((doom_video_dither_t)s_pending_dither);
    s_pending_dither = -1;
  }
  if (s_pending_palette >= 0) {
    doom_video_set_palette_mode((doom_video_palette_t)s_pending_palette);
    s_pending_palette = -1;
  }
  I_MD_SaveVideoSettings(); /* survives the next power-up (written at frame end) */
}

static bool s_shift_down;

static int typed_char(int key, bool shift) {
  if (key >= 'a' && key <= 'z') return shift ? key - 'a' + 'A' : key;
  if (key > ' ' && key < 0x7f) {
    if (!shift) return key;
    static const char *from = "1234567890-=[];'`\\,./";
    static const char *to = "!@#$%^&*()_+{}:\"~|<>?";
    const char *p = strchr(from, key);
    return p ? to[p - from] : key;
  }
  return key == ' ' ? ' ' : 0;
}

static void post_key(int key, bool down) {
  if (!key) return;
  event_t ev;
  ev.type = down ? ev_keydown : ev_keyup;
  ev.data1 = key;
  ev.data2 = down ? key : 0;
  ev.data3 = down ? typed_char(key, s_shift_down) : 0;
  ev.data4 = ev.data5 = 0;
  D_PostEvent(&ev);
}

/* Joystick / pad state as key presses: each bit of this mask maps to one
 * Doom key, and a change of the bit posts a press or a release. */
enum {
  JK_UP, JK_DOWN, JK_LEFT, JK_RIGHT, JK_FIRE, JK_USE, JK_STRAFE, JK_RUN,
  JK_MENU, JK_MAP, JK_STRAFE_L, JK_STRAFE_R, JK_COUNT
};
static const int s_joy_keys[JK_COUNT] = {
    KEY_UPARROW, KEY_DOWNARROW, KEY_LEFTARROW, KEY_RIGHTARROW,
    KEY_RCTRL,   ' ',           KEY_RALT,      KEY_RSHIFT,
    KEY_ESCAPE,  KEY_TAB,       ',',           '.',
};
static uint32_t s_joy_mask;

static void poll_joystick(void) {
  doom_joy_state_t j;
  doom_input_poll_joystick(&j);
  uint32_t m = 0;
  if (j.y < 0) m |= 1u << JK_UP;
  if (j.y > 0) m |= 1u << JK_DOWN;
  if (j.x < 0) m |= 1u << JK_LEFT;
  if (j.x > 0) m |= 1u << JK_RIGHT;
  if (j.buttons & DOOM_JOYB_FIRE) m |= 1u << JK_FIRE;
  if (j.buttons & DOOM_JOYB_USE) m |= 1u << JK_USE;
  if (j.buttons & DOOM_JOYB_STRAFE) m |= 1u << JK_STRAFE;
  if (j.buttons & DOOM_JOYB_RUN) m |= 1u << JK_RUN;
  if (j.buttons & DOOM_JOYB_MENU) m |= 1u << JK_MENU;
  if (j.buttons & DOOM_JOYB_MAP) m |= 1u << JK_MAP;
  if (j.buttons & DOOM_JOYB_STRAFE_L) m |= 1u << JK_STRAFE_L;
  if (j.buttons & DOOM_JOYB_STRAFE_R) m |= 1u << JK_STRAFE_R;
  const uint32_t changed = m ^ s_joy_mask;
  for (int i = 0; i < JK_COUNT && changed; i++) {
    if (changed & (1u << i)) post_key(s_joy_keys[i], (m >> i) & 1u);
  }
  s_joy_mask = m;
}

/* The ST mouse as Doom sees it: X turns, Y walks, the right button
 * fires and the left button held strafes -- the opposite way round to
 * a PC, because the ST wires joystick 1's fire to the right mouse
 * button. They are one line and cannot be told apart, so the right
 * button is a fire button whether we like it or not (it arrives as
 * joystick fire and needs nothing from us here), and strafe has to go
 * to the left button. Hence the only thing posted from the mouse
 * packet is the left button, as Doom's mouse button 2, which is
 * mousebstrafe.
 *
 * Posted once per tic from I_StartTic rather than from I_GetEvent:
 * G_Responder overwrites mousex/mousey with each event it sees, so a
 * second event in the same tic (the renderer polls input again from
 * NetUpdate) would throw the first one's movement away. The deltas
 * accumulate in ikbd.c until this reads them, so nothing is lost
 * either way. */
void I_MD_PostMouseEvent(void) {
  int16_t dx, dy;
  uint8_t buttons;
  ikbd_get_mouse(&dx, &dy, &buttons);

  static uint8_t s_last_buttons;
  if (!dx && !dy && buttons == s_last_buttons) return;
  s_last_buttons = buttons;

  event_t ev;
  ev.type = ev_mouse;
  /* Bit 1 is Doom's mouse button 2 = mousebstrafe; see above. */
  ev.data1 = (buttons & IKBD_MOUSE_BTN_LEFT) ? 0x02 : 0x00;
  ev.data2 = dx;
  ev.data3 = -dy; /* IKBD +Y is towards the user; Doom's +Y is forward */
  ev.data4 = ev.data5 = 0;
  D_PostEvent(&ev);
}

void I_GetEvent(void) {
  /* Re-arm the exit sentinel each frame so a posted BOOT_GEM is a
   * one-shot (see ikbd_clear_command). */
  ikbd_clear_command();
  fb_pump_rom3();
  ikbd_pump();

  ikbd_key_event_t k;
  while (ikbd_pop_key(&k)) {
    if (k.scancode == SCAN_KP_MULTIPLY || k.scancode == SCAN_KP_DIVIDE) {
      if (k.is_press) cycle_video_mode(k.scancode);
      continue;
    }
#if defined(_DEBUG) && (_DEBUG != 0)
    if (k.scancode == SCAN_KP_MINUS) {
      if (k.is_press) {
        const int c = audio_vbl_timer_core();
        audio_stop_vbl_timer();
        if (c < 0) audio_start_vbl_timer(0); else if (c == 0) audio_start_vbl_timer(1);
        players[consoleplayer].message = audio_vbl_timer_core() < 0 ? "AUDIO IRQ OFF"
                                         : audio_vbl_timer_core() == 0 ? "AUDIO IRQ CORE 0" : "AUDIO IRQ CORE 1";
      }
      continue;
    }
#endif
    const int key = doom_input_translate(k.scancode);
    if (key == KEY_RSHIFT) s_shift_down = k.is_press;
    post_key(key, k.is_press);
  }
  poll_joystick();
}

void I_GetEventTimeout(int key_timeout) {
  (void)key_timeout;
  I_GetEvent();
}

int GetTypedChar(int scancode, boolean shiftdown) {
  return typed_char(doom_input_translate((uint8_t)scancode), shiftdown);
}

void I_StartTextInput(int x1, int y1, int x2, int y2) {
  (void)x1; (void)y1; (void)x2; (void)y2;
}

void I_StopTextInput(void) {}

void I_BindInputVariables(void) {}

void I_InputInit(void) {
  /* The game owns ESC (its menu); leaving to GEM is I_Quit. */
  ikbd_set_esc_auto_exit(false);
}
