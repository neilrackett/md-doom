/*
 * Copyright (C) 2026 Neil Rackett
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * File: ikbd.h
 * Description: IKBD keyboard + joystick ingest + demux.
 *
 * The m68k's ACIA interrupt handler (userfw_acia_irq in
 * target/atarist/src/userfw.s) reads the keyboard ACIA at $FFFFFC00/02
 * and forwards every received byte to the RP with one cart-bus read at
 * IKBD_WINDOW_BASE + byte ($FB8200..$FB82FF).
 *
 * `ikbd_consume_rom3_sample(addr_lsb)` is called from the ROM3
 * dispatcher in fb.c for every captured cart-bus read; it keeps the
 * bytes in the IKBD window and pushes them onto a raw-byte ring.
 *
 * `ikbd_pump()` drains the ring and classifies each byte:
 *   $00..$7F -> key press scancode (scancode 0 suppressed)
 *   $80..$F1 -> key release scancode (byte & $7F)
 *   $FE / $FF / $FD -> joystick packet headers; the state byte(s) that
 *              follow are framed into the per-port joystick state
 *              (userfw.s puts the IKBD into joystick event reporting
 *              at boot, with the mouse off). Other headers are dropped.
 *
 * Apps drain decoded key events via `ikbd_pop_key` and read the port-1
 * stick with `ikbd_get_joystick`. ESC (scancode $01) press+release
 * within 200 ms posts CMD_BOOT_GEM to the cart command sentinel unless
 * the app has taken ESC for itself (ikbd_set_esc_auto_exit).
 */

#ifndef IKBD_H_INCLUDED
#define IKBD_H_INCLUDED

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Ring + filter constants exposed for tests / debug code. */
#define IKBD_RING_CAPACITY 64u
#define IKBD_WINDOW_LO16   0x8200u  /* low 16 bits of $FB8200 */
#define IKBD_WINDOW_MASK   0xFF00u  /* discriminator: high byte == 0x82 */

/* Reset the ring buffer + cached state. Safe to call multiple times. */
void ikbd_init(void);

/* commemul ring consumer. Passed as the callback to commemul_poll
 * for every ROM3 sample. Filters for the IKBD window, extracts the
 * low byte, and pushes it onto the raw-byte ring. Runs in main-loop
 * context. */
void ikbd_consume_rom3_sample(uint16_t addr_lsb);

/* Drain the raw-byte ring, feeding bytes through the keyboard demux
 * and pushing decoded events onto the key event ring. Also signals
 * CMD_BOOT_GEM via the cart command sentinel on ESC press+release.
 * Call once per main-loop iteration. */
void ikbd_pump(void);

/* Latest Atari ST joystick state decoded by ikbd_pump: bit0 up,
 * bit1 down, bit2 left, bit3 right, bit7 fire. Returns 0 unless the
 * m68k side is emitting joystick event packets (userfw.s puts the
 * IKBD into joystick event-reporting mode at boot). */
uint8_t ikbd_get_joystick(void);

/* Key press / release event. `scancode` is the IKBD scancode with
 * bit 7 stripped (0..127). `is_press` is true for make, false for
 * break. Scancode $00 is suppressed (not a valid IKBD key). */
typedef struct {
  uint8_t scancode;
  bool is_press;
} ikbd_key_event_t;

/* Pop the next decoded key event. Returns false if the ring is
 * empty. */
bool ikbd_pop_key(ikbd_key_event_t *out);

/* Enable / disable the built-in ESC press+release -> CMD_BOOT_GEM
 * sentinel write. Default = true (backward-compatible with the
 * ergonomic of "press ESC to exit"). Apps that want to own
 * the ESC key (e.g. the menu+demo dispatcher that uses ESC for
 * "back to menu") call ikbd_set_esc_auto_exit(false) once at boot.
 * ESC events are still delivered via ikbd_pop_key() regardless of
 * the setting. */
void ikbd_set_esc_auto_exit(bool enabled);

/* Request an exit to GEM by writing CMD_BOOT_GEM to the cart sentinel
 * (the same write the ESC auto-exit performs). Apps that own ESC use
 * this to exit on their own trigger -- e.g. an "Exit" menu item. */
void ikbd_request_boot_gem(void);

/* Re-arm the command sentinel to CMD_NOP. Call once per main-loop
 * iteration so a BOOT_GEM posted by ikbd_request_boot_gem() is a
 * one-shot: userfw consumes it during the exit frame's fb_publish, then
 * this wipes it. Without it, the value persists across an ST reset (the
 * m68k can't clear the RP-owned region; the RP zeroes it only at boot)
 * and the re-run userfw quits to GEM instead of running the app. */
void ikbd_clear_command(void);

#ifdef __cplusplus
}
#endif

#endif /* IKBD_H_INCLUDED */
