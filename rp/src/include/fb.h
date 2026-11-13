/*
 * Copyright (C) 2026 Neil Rackett
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/**
 * File: fb.h
 * Description: Framebuffer module — owns the single 32 KB cartridge
 *              framebuffer the m68k reads each VBL.
 *
 * Single-FB design: there is exactly one framebuffer in
 * the shared region (`$FA8300`, 32 KB, 320x200x4bpp). Double-buffering
 * happens on the ST side; the RP just writes into this one buffer.
 *
 * fb.c defines `fb_screen`, brings the cart framebuffer up with a boot
 * splash, and owns the m68k blit handshake (fb_wait_blit_ack /
 * fb_frame_done) that every publish is bracketed by.
 */

#ifndef FB_H
#define FB_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct FB_MODE {
  unsigned short h_pixels;
  unsigned short v_pixels;
  unsigned char color_bits; /* bits per pixel */
};

/* Mirrors md-sprites-demo's VGA_SCREEN but with a single framebuffer
 * pointer instead of A/B/current/hidden (we don't keep a back buffer in
 * cartridge). The ported text renderer writes into
 * `framebuffer` directly. */
struct FB_SCREEN {
  unsigned int *framebuffer;
  uint16_t width;
  uint16_t height;
  uint8_t color_bits;
  uint8_t _pad;
};

extern struct FB_SCREEN fb_screen;

/* Built-in 320x200 4 bpp Atari ST low-res mode. Defined in fb.c. */
extern const struct FB_MODE fb_mode_320x200;

/**
 * @brief Populate `fb_screen` from a mode descriptor, launch Core 1's
 *        worker loop and publish the boot splash.
 *
 * @return 0 on success, -1 if `mode` is NULL.
 */
int fb_init(const struct FB_MODE *mode);

/* fb_init paints a one-frame boot splash internally; the app owns
 * every frame after that, so the splash renderers are static to fb.c
 * rather than public API. */

/** @brief Publish the current chunked buffer to the cart framebuffer,
 *         synchronized to the Atari's 50 Hz VBL.
 *
 *         Two steps: (1) BLOCK until the m68k acks it finished blitting
 *         the previous frame (a cart-bus read at $FB8400 captured by
 *         commemul) so the cart FB is free; (2) transpose chunked ->
 *         planar straight into the cart FB (~1 ms, both cores) and bump
 *         FB_FRAME_COUNTER. The write lands in the m68k's ~3 ms post-blit
 *         slack, so the m68k never reads the FB mid-write. A ~60 ms
 *         timeout (safety net for "m68k not running", e.g. at boot)
 *         keeps the RP from hanging. Drawing into `fb_chunked_buffer`
 *         (RP RAM) is unsynchronized; call this once per frame after
 *         drawing. */
void fb_publish(void);

/** @brief The two halves of fb_publish() for callers that write the
 *         cart FB by their own route (MD/DOOM's fused palette-reduce +
 *         c2p): block until the m68k has finished blitting the previous
 *         frame and not yet started its next VBL, then, after writing,
 *         mark the new frame ready. Anything written to the cart FB or
 *         the palette slots must start between the two, inside the
 *         m68k's post-blit slack. A frame finished just after the next
 *         VBL waits up to a VBL for the following one. */
void fb_wait_blit_ack(void);
void fb_frame_done(void);

/* Debug: longest fb_wait_blit_ack (or, in 256 colours, wait for a part's
 * ack) since the last call, in microseconds. */
uint32_t fb_debug_wait_max_us(void);

/** @brief Drain the ROM3 commemul ring once, routing each captured
 *         sample to BOTH the IKBD demux and the VBL frame-sync
 *         detector. Call from the main loop in place of a bare
 *         commemul_poll(); fb_publish() also calls it internally while
 *         waiting for the VBL ack, so IKBD/ESC stay responsive during
 *         the wait. */
void fb_pump_rom3(void);

/* Relocation heartbeat from the m68k (userfw.s's RELOC_WINDOW_BASE),
 * one per VBL once it is running from ST RAM rather than in place from
 * the cartridge. `fb_wait_st_reloc` blocks for the first one, pumping
 * the ROM3 ring, and returns false if it never arrives -- an ST that is
 * not there, or an older firmware image. fb.c keeps the count itself,
 * for the liveness check to watch for standing still; callers outside
 * only ever ask whether it has happened. */
bool fb_st_relocated(void);
bool fb_wait_st_reloc(uint32_t timeout_ms);

/* True when the ST is in high res on a mono monitor (640x400, one
 * plane, 71 Hz), from the display report the m68k sends every VBL. The
 * report comes before the relocation heartbeat, so it is known by the
 * time fb_wait_st_reloc returns. The cart FB is still 32000 bytes in
 * the same chunk-reversed layout -- 80-byte rows, 400 of them -- and a
 * frame is two VBLs long; fb_publish draws the right one either way. */
bool fb_st_mono(void);

/* 256 colours (cart_shared.h, "256 colours"): true when the ST is a TT
 * or a Falcon showing 8 interleaved bitplanes, 320x200, from the same
 * display report; fb_st_falcon() says which palette format. The pixels
 * of fb_chunked_buffer are then palette indices 0..255 as they are, and
 * fb_publish() sends them in two parts. */
bool fb_st_eight(void);
bool fb_st_falcon(void);

/* One 8-plane palette entry in the format the ST in front of us takes,
 * from 8-bit RGB: a TT's 0000RRRR GGGGBBBB, or a Falcon's RRRRRRbb
 * GGGGGGbb (cart_shared.h). */
uint16_t fb_eight_colour(uint8_t r, uint8_t g, uint8_t b);

/* Publish fb_chunked_buffer as 256 colours: wait until the m68k has
 * copied the last part, write the palette if `palette` is given (256
 * words, from fb_eight_colour, into the slot it is handed) and part 0,
 * then wait again and write part 1. Returns once part 1 is in the cart
 * FB, while the m68k is still copying it, so the buffer is free to draw
 * the next frame into. The result is the conversion's time in
 * microseconds, both parts, without the waits. */
uint32_t fb_eight_publish(void (*palette)(uint16_t *slot));

/* Debug: the rows of a part the m68k copies in a VBL, as it measured at
 * boot (0 before the first report). */
unsigned fb_eight_rows_per_vbl(void);

/* 256 colours, the melt (cart_shared.h, "the melt"): send the new frame,
 * in fb_chunked_buffer, to the ST's new-frame buffer (with the palette,
 * as fb_eight_publish), and then each step's heights -- Doom's y[] of 160
 * two-pixel columns, < 0 not yet moving, 200 done -- as a display list
 * the ST runs into the page being drawn before it flips. A step waits
 * for the one before to be on screen. */
void fb_eight_melt_begin(void (*palette)(uint16_t *slot));
void fb_eight_melt_step(const int16_t *y);

/* Called from fb_pump_rom3 when the heartbeat has been gone long enough
 * to mean the ST was reset. The handler does not return: the cartridge
 * image has to be put back and the RP restarted alongside the ST.
 * Registering also arms the check, so register it only once the first
 * heartbeat has been seen. */
void fb_set_st_lost_handler(void (*fn)(void));

#ifdef __cplusplus
}
#endif

#endif /* FB_H */
