/*
 * Copyright (C) 2026 Neil Rackett
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * File: doomapp.h
 * Description: The MD/DOOM application front-end, as driven by emul.c's
 *              main loop: init, key events in, one frame out per VBL,
 *              audio fill.
 *
 * Until the Doom engine is vendored this is a pipeline test card: it
 * pushes a 256-colour test image through the palette reduction, dither
 * and chunky-to-planar path every frame, shows which Doom key the ST
 * keyboard produced, reports the joystick / gamepad and the detected
 * sound chip, plays a test sound, and can program a level pack from the
 * SD card into flash -- everything the engine will lean on, exercised
 * on real hardware without the engine.
 */

#ifndef DOOMAPP_H_INCLUDED
#define DOOMAPP_H_INCLUDED

#include <stdint.h>

#include "ikbd.h"

#ifdef __cplusplus
extern "C" {
#endif

void doomapp_init(void);
void doomapp_handle_key(const ikbd_key_event_t *k);
void doomapp_render_frame(void);
void doomapp_audio_fill(uint8_t *buf, uint32_t bytes);

#ifdef __cplusplus
}
#endif

#endif /* DOOMAPP_H_INCLUDED */
