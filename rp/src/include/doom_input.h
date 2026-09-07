/*
 * Copyright (C) 2026 Neil Rackett
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * File: doom_input.h
 * Description: Atari ST input -> Doom input. Translates IKBD scancodes
 *              into the key codes Doom's event queue expects (the values
 *              match doomkeys.h in the engine, so a translated key can be
 *              posted straight to D_PostEvent as ev_keydown/ev_keyup) and
 *              folds the port-1 joystick and an Xpad gamepad into one
 *              Doom-shaped joystick state.
 *
 * The bindings are original PC Doom's: cursor keys move and turn, Ctrl
 * fires, Space uses, Alt strafes, Shift runs, 1-7 pick weapons, Esc is
 * the menu, Tab the automap, F1-F10 the function keys, - and = the
 * view size. Keys the ST does not have are stood in for by the ones it
 * has: Help is F11 (gamma), Undo is Pause. The keypad digits act as
 * the cursor cluster the way they do on a PC without Num Lock.
 */

#ifndef DOOM_INPUT_H_INCLUDED
#define DOOM_INPUT_H_INCLUDED

#include <stdbool.h>
#include <stdint.h>

#include "ikbd.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Doom key codes. These are doomkeys.h's values verbatim so the
 * translation table plugs into the engine unchanged. */
#define DOOM_KEY_RIGHTARROW 0xae
#define DOOM_KEY_LEFTARROW  0xac
#define DOOM_KEY_UPARROW    0xad
#define DOOM_KEY_DOWNARROW  0xaf
#define DOOM_KEY_ESCAPE     27
#define DOOM_KEY_ENTER      13
#define DOOM_KEY_TAB        9
#define DOOM_KEY_F1         (0x80 + 0x3b)
#define DOOM_KEY_F2         (0x80 + 0x3c)
#define DOOM_KEY_F3         (0x80 + 0x3d)
#define DOOM_KEY_F4         (0x80 + 0x3e)
#define DOOM_KEY_F5         (0x80 + 0x3f)
#define DOOM_KEY_F6         (0x80 + 0x40)
#define DOOM_KEY_F7         (0x80 + 0x41)
#define DOOM_KEY_F8         (0x80 + 0x42)
#define DOOM_KEY_F9         (0x80 + 0x43)
#define DOOM_KEY_F10        (0x80 + 0x44)
#define DOOM_KEY_F11        (0x80 + 0x57)
#define DOOM_KEY_F12        (0x80 + 0x58)
#define DOOM_KEY_BACKSPACE  0x7f
#define DOOM_KEY_PAUSE      0xff
#define DOOM_KEY_EQUALS     0x3d
#define DOOM_KEY_MINUS      0x2d
#define DOOM_KEY_RSHIFT     (0x80 + 0x36)
#define DOOM_KEY_RCTRL      (0x80 + 0x1d)
#define DOOM_KEY_RALT       (0x80 + 0x38)
#define DOOM_KEY_CAPSLOCK   (0x80 + 0x3a)
#define DOOM_KEY_HOME       (0x80 + 0x47)
#define DOOM_KEY_END        (0x80 + 0x4f)
#define DOOM_KEY_PGUP       (0x80 + 0x49)
#define DOOM_KEY_PGDN       (0x80 + 0x51)
#define DOOM_KEY_INS        (0x80 + 0x52)
#define DOOM_KEY_DEL        (0x80 + 0x53)
#define DOOM_KEYP_5         (0x80 + 0x4c)

/* Translate an IKBD scancode (0..127, bit 7 already stripped) to a Doom
 * key code. Returns 0 for keys Doom has no use for. */
int doom_input_translate(uint8_t scancode);

/* Human-readable name for a Doom key code, for diagnostics. Letters and
 * digits come back as themselves; special keys by name. Never NULL. */
const char *doom_input_key_name(int doom_key);

/* Joystick / gamepad state in Doom's own shape. `x` and `y` are -1, 0
 * or 1 (right and down positive, matching ev_joystick's data2/data3
 * sign convention), `buttons` is a bit mask. The port-1 ST joystick
 * gives the four directions and DOOM_JOYB_FIRE; an Xpad gamepad, when
 * a provider is present, adds the rest. */
#define DOOM_JOYB_FIRE     (1u << 0) /* stick fire, pad South      */
#define DOOM_JOYB_STRAFE   (1u << 1) /* pad West                   */
#define DOOM_JOYB_USE      (1u << 2) /* pad East                   */
#define DOOM_JOYB_RUN      (1u << 3) /* pad North                  */
#define DOOM_JOYB_PREVWEAP (1u << 4) /* pad left shoulder          */
#define DOOM_JOYB_NEXTWEAP (1u << 5) /* pad right shoulder         */
#define DOOM_JOYB_MENU     (1u << 6) /* pad Start                  */
#define DOOM_JOYB_MAP      (1u << 7) /* pad Select / Back          */
#define DOOM_JOYB_STRAFE_L (1u << 8) /* pad left trigger           */
#define DOOM_JOYB_STRAFE_R (1u << 9) /* pad right trigger          */

typedef struct {
  int8_t x;
  int8_t y;
  uint16_t buttons;
  bool pad_present; /* an Xpad provider is reporting a pad */
} doom_joy_state_t;

/* Sample the joystick and gamepad. Call once per frame after
 * ikbd_pump(). */
void doom_input_poll_joystick(doom_joy_state_t *out);

#ifdef __cplusplus
}
#endif

#endif /* DOOM_INPUT_H_INCLUDED */
