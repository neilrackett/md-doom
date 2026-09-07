/*
 * Copyright (C) 2026 Neil Rackett
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * File: doom_input.c
 * Description: IKBD scancode -> Doom key translation, plus the joystick
 *              and Xpad gamepad fold. See doom_input.h for the bindings.
 */

#include "doom_input.h"

#include <stddef.h>

#include "xpad.h"
#include "xpadin.h"

/* Indexed by IKBD scancode. 0 = no Doom key. Letters are lower case and
 * digits are their ASCII values, which is what Doom's key bindings and
 * menu/cheat input expect (the engine applies its own shift transform
 * when it wants a typed character). */
static const uint8_t s_scan_to_doom[128] = {
    [0x01] = DOOM_KEY_ESCAPE,
    [0x02] = '1', [0x03] = '2', [0x04] = '3', [0x05] = '4', [0x06] = '5',
    [0x07] = '6', [0x08] = '7', [0x09] = '8', [0x0A] = '9', [0x0B] = '0',
    [0x0C] = DOOM_KEY_MINUS,
    [0x0D] = DOOM_KEY_EQUALS,
    [0x0E] = DOOM_KEY_BACKSPACE,
    [0x0F] = DOOM_KEY_TAB,
    [0x10] = 'q', [0x11] = 'w', [0x12] = 'e', [0x13] = 'r', [0x14] = 't',
    [0x15] = 'y', [0x16] = 'u', [0x17] = 'i', [0x18] = 'o', [0x19] = 'p',
    [0x1A] = '[', [0x1B] = ']',
    [0x1C] = DOOM_KEY_ENTER,
    [0x1D] = DOOM_KEY_RCTRL,    /* Control: fire */
    [0x1E] = 'a', [0x1F] = 's', [0x20] = 'd', [0x21] = 'f', [0x22] = 'g',
    [0x23] = 'h', [0x24] = 'j', [0x25] = 'k', [0x26] = 'l', [0x27] = ';',
    [0x28] = '\'', [0x29] = '`',
    [0x2A] = DOOM_KEY_RSHIFT,   /* left Shift: run */
    [0x2B] = '\\',
    [0x2C] = 'z', [0x2D] = 'x', [0x2E] = 'c', [0x2F] = 'v', [0x30] = 'b',
    [0x31] = 'n', [0x32] = 'm', [0x33] = ',', [0x34] = '.', [0x35] = '/',
    [0x36] = DOOM_KEY_RSHIFT,   /* right Shift: run */
    [0x38] = DOOM_KEY_RALT,     /* Alternate: strafe */
    [0x39] = ' ',               /* Space: use */
    [0x3A] = DOOM_KEY_CAPSLOCK,
    [0x3B] = DOOM_KEY_F1, [0x3C] = DOOM_KEY_F2, [0x3D] = DOOM_KEY_F3,
    [0x3E] = DOOM_KEY_F4, [0x3F] = DOOM_KEY_F5, [0x40] = DOOM_KEY_F6,
    [0x41] = DOOM_KEY_F7, [0x42] = DOOM_KEY_F8, [0x43] = DOOM_KEY_F9,
    [0x44] = DOOM_KEY_F10,
    [0x47] = DOOM_KEY_HOME,     /* Clr Home */
    [0x48] = DOOM_KEY_UPARROW,
    [0x4A] = '-',               /* keypad - */
    [0x4B] = DOOM_KEY_LEFTARROW,
    [0x4D] = DOOM_KEY_RIGHTARROW,
    [0x4E] = '+',               /* keypad + */
    [0x50] = DOOM_KEY_DOWNARROW,
    [0x52] = DOOM_KEY_INS,      /* Insert */
    [0x53] = DOOM_KEY_DEL,      /* Delete */
    [0x61] = DOOM_KEY_PAUSE,    /* Undo: the ST has no Pause key */
    [0x62] = DOOM_KEY_F11,      /* Help: gamma, as on the PC's F11 */
    [0x63] = '(',               /* keypad ( */
    [0x64] = ')',               /* keypad ) */
    [0x65] = '/',               /* keypad / */
    [0x66] = '*',               /* keypad * */
    /* Keypad digits double as the cursor cluster, as on a PC without
     * Num Lock, so the map and the menus work from the keypad too. */
    [0x67] = DOOM_KEY_HOME,       /* keypad 7 */
    [0x68] = DOOM_KEY_UPARROW,    /* keypad 8 */
    [0x69] = DOOM_KEY_PGUP,       /* keypad 9 */
    [0x6A] = DOOM_KEY_LEFTARROW,  /* keypad 4 */
    [0x6B] = DOOM_KEYP_5,         /* keypad 5 */
    [0x6C] = DOOM_KEY_RIGHTARROW, /* keypad 6 */
    [0x6D] = DOOM_KEY_END,        /* keypad 1 */
    [0x6E] = DOOM_KEY_DOWNARROW,  /* keypad 2 */
    [0x6F] = DOOM_KEY_PGDN,       /* keypad 3 */
    [0x70] = DOOM_KEY_INS,        /* keypad 0 */
    [0x71] = DOOM_KEY_DEL,        /* keypad . */
    [0x72] = DOOM_KEY_ENTER,      /* keypad Enter */
};

int doom_input_translate(uint8_t scancode) {
  return (scancode < 128u) ? s_scan_to_doom[scancode] : 0;
}

const char *doom_input_key_name(int k) {
  static const struct {
    int key;
    const char *name;
  } names[] = {
      {DOOM_KEY_RIGHTARROW, "RIGHT"}, {DOOM_KEY_LEFTARROW, "LEFT"},
      {DOOM_KEY_UPARROW, "UP"},       {DOOM_KEY_DOWNARROW, "DOWN"},
      {DOOM_KEY_ESCAPE, "ESC"},       {DOOM_KEY_ENTER, "ENTER"},
      {DOOM_KEY_TAB, "TAB"},          {DOOM_KEY_F1, "F1"},
      {DOOM_KEY_F2, "F2"},            {DOOM_KEY_F3, "F3"},
      {DOOM_KEY_F4, "F4"},            {DOOM_KEY_F5, "F5"},
      {DOOM_KEY_F6, "F6"},            {DOOM_KEY_F7, "F7"},
      {DOOM_KEY_F8, "F8"},            {DOOM_KEY_F9, "F9"},
      {DOOM_KEY_F10, "F10"},          {DOOM_KEY_F11, "F11"},
      {DOOM_KEY_F12, "F12"},          {DOOM_KEY_BACKSPACE, "BACKSPACE"},
      {DOOM_KEY_PAUSE, "PAUSE"},      {DOOM_KEY_EQUALS, "EQUALS"},
      {DOOM_KEY_MINUS, "MINUS"},      {DOOM_KEY_RSHIFT, "SHIFT"},
      {DOOM_KEY_RCTRL, "CTRL"},       {DOOM_KEY_RALT, "ALT"},
      {DOOM_KEY_CAPSLOCK, "CAPSLOCK"}, {DOOM_KEY_HOME, "HOME"},
      {DOOM_KEY_END, "END"},          {DOOM_KEY_PGUP, "PGUP"},
      {DOOM_KEY_PGDN, "PGDN"},        {DOOM_KEY_INS, "INS"},
      {DOOM_KEY_DEL, "DEL"},          {DOOM_KEYP_5, "NUM5"},
      {' ', "SPACE"},
  };
  /* Single printable characters: one static two-byte buffer is enough
   * for a diagnostics overlay that prints one key at a time. */
  static char single[2];
  for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
    if (names[i].key == k) return names[i].name;
  }
  if (k > ' ' && k < 0x7f) {
    single[0] = (char)((k >= 'a' && k <= 'z') ? (k - 'a' + 'A') : k);
    single[1] = '\0';
    return single;
  }
  return k ? "?" : "";
}

void doom_input_poll_joystick(doom_joy_state_t *out) {
  out->x = 0;
  out->y = 0;
  out->buttons = 0;
  out->pad_present = false;

  /* Port-1 ST joystick: bit0 up, bit1 down, bit2 left, bit3 right,
   * bit7 fire. */
  const uint8_t st = ikbd_get_joystick();
  if (st & 0x01u) out->y = -1;
  if (st & 0x02u) out->y = 1;
  if (st & 0x04u) out->x = -1;
  if (st & 0x08u) out->x = 1;
  if (st & 0x80u) out->buttons |= DOOM_JOYB_FIRE;

  /* Xpad gamepad, when a provider on the ST is publishing one. Only bits
   * 0..15 of the mask cross the cart bus, which is everything below. */
  uint16_t pad;
  if (xpadin_get(&pad)) {
    out->pad_present = true;
    if (pad & XPAD_UP) out->y = -1;
    if (pad & XPAD_DOWN) out->y = 1;
    if (pad & XPAD_LEFT) out->x = -1;
    if (pad & XPAD_RIGHT) out->x = 1;
    if (pad & XPAD_SOUTH) out->buttons |= DOOM_JOYB_FIRE;
    if (pad & XPAD_EAST) out->buttons |= DOOM_JOYB_USE;
    if (pad & XPAD_WEST) out->buttons |= DOOM_JOYB_STRAFE;
    if (pad & XPAD_NORTH) out->buttons |= DOOM_JOYB_RUN;
    if (pad & XPAD_TL) out->buttons |= DOOM_JOYB_PREVWEAP;
    if (pad & XPAD_TR) out->buttons |= DOOM_JOYB_NEXTWEAP;
    if (pad & XPAD_TL2) out->buttons |= DOOM_JOYB_STRAFE_L;
    if (pad & XPAD_TR2) out->buttons |= DOOM_JOYB_STRAFE_R;
    if (pad & XPAD_START) out->buttons |= DOOM_JOYB_MENU;
    if (pad & XPAD_SELECT) out->buttons |= DOOM_JOYB_MAP;
  }
}
