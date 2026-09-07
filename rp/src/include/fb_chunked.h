/**
 * File: fb_chunked.h
 * Description: Chunked-pixel framebuffer + chunky-to-planar publish path.
 *
 * App code draws into `fb_chunked_buffer` (one byte per pixel, palette
 * index in the low nibble). A single conversion pass per frame
 * (`fb_chunky_to_planar`) transposes the chunked bytes into the Atari
 * ST 4 bpp planar layout that lives in the shared cart framebuffer at
 * $FA8300, where the m68k VBL blit picks it up.
 *
 * The conversion writes 16-bit plane WORDS into cart-mirrored RP RAM,
 * so the cart-bus byte-swap is transparent at the word level (same
 * property the older direct-planar `pixel_masks_flat` writes relied
 * on). Only the low 4 bits of each chunked byte are used; the high 4
 * bits are dropped during transposition.
 */

#ifndef FB_CHUNKED_H_INCLUDED
#define FB_CHUNKED_H_INCLUDED

#include <stdint.h>

#include "cart_shared.h"
#include "pico/stdlib.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Chunked framebuffer dimensions. Matches the Atari ST low-res screen
 * the planar conversion targets. */
#define FB_CHUNKED_W 320
#define FB_CHUNKED_H 200
#define FB_CHUNKED_SIZE (FB_CHUNKED_W * FB_CHUNKED_H)

/* Storage lives in regular RP RAM (`RAM` region at 0x20000000), not the
 * cart-mirrored ROM_IN_RAM region. Apps poke pixels here directly.
 *
 * MD/DOOM: this is also the Doom engine's 320x200 8 bpp video buffer.
 * The framework's own transpose (fb_publish) only looks at the low nibble
 * of each byte; the Doom path (doom_video_publish) instead reads the full
 * PLAYPAL index and maps it through the 16-colour dither LUT on the way
 * into the planar scratch, so the same 64 KB serves both. */
extern uint8_t fb_chunked_buffer[FB_CHUNKED_SIZE];

/* Launch Core 1 with the chunky-to-planar worker loop. Must be called
 * exactly once, from Core 0, before the first call to
 * fb_chunky_to_planar. fb_init() does this. A second call would
 * deadlock inside pico-sdk's launch handshake. */
void fb_chunked_init(void);

/* Run a job on Core 1, in parallel with Core 0 (dual-core).
 * `fb_core1_dispatch` hands `job(arg)` to the parked Core 1 worker and
 * returns immediately; the caller then does its own half of the work and
 * calls `fb_core1_wait()` to join. The fn+arg are passed through the
 * inter-core FIFO, whose push/pop carry the cross-core memory barriers,
 * so data prepared before dispatch is visible to the job and the job's
 * writes are visible after wait. Each dispatch MUST be paired with one
 * wait before the next dispatch (the c2p in fb_chunky_to_planar uses this too,
 * so demos must join their own job before fb_publish). */
typedef void (*fb_core1_job_t)(void *arg);
void __not_in_flash_func(fb_core1_dispatch)(fb_core1_job_t job, void *arg);
void __not_in_flash_func(fb_core1_wait)(void);

/* Hold Core 1 in a RAM-resident spin so Core 0 can erase or program
 * flash. Core 1's idle FIFO pop is a flash-resident SDK call and would
 * fault with XIP disabled. Park, do the flash work with interrupts off,
 * unpark; the pair counts as one dispatch/wait, so it must not straddle
 * an fb_publish(). */
void fb_core1_park(void);
void fb_core1_unpark(void);

/* Fill the entire chunked buffer with a single palette index. */
void fb_chunked_clear(uint8_t color);

/* Bounds-checked single-pixel plot; mostly useful for diagnostics. */
static inline void fb_chunked_plot(unsigned int x, unsigned int y,
                                   uint8_t color) {
  if (x < FB_CHUNKED_W && y < FB_CHUNKED_H) {
    fb_chunked_buffer[y * FB_CHUNKED_W + x] = color;
  }
}

/**
 * Transpose the chunked buffer into Atari ST 4 bpp planar format,
 * straight into the cart framebuffer in the chunk-reversed layout the
 * m68k's predec MOVEM blit expects (see cart_shared.h).
 *
 * Output layout per 16-pixel block (matches ST low-res):
 *   word 0 = plane 0 (LSB of palette index)
 *   word 1 = plane 1
 *   word 2 = plane 2
 *   word 3 = plane 3 (MSB)
 * Within each word, bit (15 - i) corresponds to pixel `i` of the block.
 *
 * MD/DOOM: there is no planar scratch buffer (the 32 KB went to the
 * game), so this writes the cart FB directly and MUST run only between
 * the m68k's blit ack and the next VBL -- fb_publish() and
 * doom_video_publish() bracket it with fb_wait_blit_ack() /
 * fb_frame_done(). Dual-core (~1 ms): **Core 0 only**, the bottom half
 * is dispatched to Core 1 and joined.
 *
 * @param planar  The cart framebuffer at $FA8300 (32 000 bytes).
 */
void __not_in_flash_func(fb_chunky_to_planar)(uint16_t *planar);

/* MD/DOOM: the cart-FB byte offset that image (natural, row-major
 * planar) byte offset `o` lands at, i.e. the m68k MOVEM chunk reversal.
 * Chunk K of the image goes to cart chunk (COUNT-1-K); the tail past the
 * last whole chunk stays in place. */
static inline uint32_t fb_cart_offset(uint32_t o) {
  if (o >= (uint32_t)CART_FB_CHUNK_COVERED) return o;
  const uint32_t chunk = o / CART_FB_CHUNK_BYTES;
  return (CART_FB_CHUNK_COUNT - 1u - chunk) * CART_FB_CHUNK_BYTES +
         (o - chunk * CART_FB_CHUNK_BYTES);
}

#ifdef __cplusplus
}
#endif

#endif /* FB_CHUNKED_H_INCLUDED */
