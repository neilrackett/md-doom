/**
 * File: fb_chunked.c
 * Description: Chunked framebuffer storage + Core 0 / Core 1 dispatch
 *              for the chunky-to-planar conversion.
 *
 * `fb_c2p_half(dst, src, src_end)` lives in fb_chunked_asm.S -- the
 * multiplication-based bit-transpose worker that converts a range of
 * the chunked buffer into a contiguous slice of the planar destination.
 *
 * fb_chunky_to_planar() splits the work across both RP2040 cores:
 * Core 0 handles the top 100 rows (pixels 0..15999 of the chunked
 * buffer -> first 8000 uint16_t of planar), Core 1 handles the bottom
 * 100 rows. Sync is a one-word push/pop on the inter-core FIFO.
 *
 * Core 1 runs a perpetual loop in `fb_c2p_core1_loop` waiting for
 * each frame's dispatch. `fb_chunked_init()` launches it once at
 * boot via the pico-sdk's `multicore_launch_core1`.
 *
 * MD/DOOM: there is no planar scratch (RAM belongs to the game), so the
 * conversion writes the cart framebuffer directly, one 48-byte MOVEM
 * chunk at a time at its pre-reversed position -- the m68k FBDRV_INLINE
 * macro uses `movem.l list, -(a5)` predec stores, which land each chunk
 * in the destination ST page in reverse memory order. See the
 * "Framebuffer chunk layout for the m68k MOVEM blit" comment in
 * cart_shared.h for the full spec. Because the cart FB is written in
 * place, the caller must only run this in the m68k's post-blit slack
 * (fb_wait_blit_ack ... fb_frame_done in fb.c). ~1 ms dual-core.
 */

#include "fb_chunked.h"

#include <string.h>

#include "cart_shared.h"
#include "hardware/sync.h"
#include "pico/multicore.h"

/* Per-file -O3: the global build is MinSizeRel (-Os). The chunky->planar
 * conversion + chunk-reversed memcpy here run on the hot per-frame path
 * and are pure compute; the dual-core handshake uses blocking FIFO calls
 * (real SDK calls with barriers), so -O3 is safe. */
#pragma GCC optimize("O3")

uint8_t fb_chunked_buffer[FB_CHUNKED_SIZE] __attribute__((aligned(4)));

/* Asm worker (fb_chunked_asm.S). Processes pixels in [src, src_end)
 * into the planar layout starting at dst. */
extern void fb_c2p_half(uint16_t *dst,
                        const uint8_t *src,
                        const uint8_t *src_end);

/* Generic Core 1 worker loop (dual-core). Pops a job function
 * pointer + arg off the FIFO, runs it, signals completion. Both the c2p
 * bottom half and the demos' band rendering dispatch through this. The
 * fn/arg travel through the FIFO (not shared memory), so the FIFO's
 * push/pop barriers fully order the handoff. Placed in RAM so the loop
 * doesn't pay XIP cost on every dispatch. */
static void __not_in_flash_func(fb_core1_loop)(void) {
  for (;;) {
    fb_core1_job_t job = (fb_core1_job_t)(uintptr_t)multicore_fifo_pop_blocking();
    void *arg = (void *)(uintptr_t)multicore_fifo_pop_blocking();
    job(arg);
    multicore_fifo_push_blocking(0); /* done */
  }
}

void __not_in_flash_func(fb_core1_dispatch)(fb_core1_job_t job, void *arg) {
  multicore_fifo_push_blocking((uint32_t)(uintptr_t)job);
  multicore_fifo_push_blocking((uint32_t)(uintptr_t)arg);
}

void __not_in_flash_func(fb_core1_wait)(void) {
  (void)multicore_fifo_pop_blocking(); /* wait for done */
}

void fb_chunked_init(void) {
  /* multicore_launch_core1 blocks until Core 1 has entered the user
   * function, so by the time fb_init returns, Core 1 is parked in the
   * FIFO pop and ready to service the first frame. */
  multicore_launch_core1(fb_core1_loop);
}

/* Hold Core 1 somewhere RAM-resident so Core 0 can write flash.
 *
 * Core 1 normally waits in fb_core1_loop's FIFO pop, and that pop is a
 * flash-resident SDK call -- executing it with XIP disabled would fault.
 * The park job spins on a flag in RAM instead. Both the fact that the
 * idle loop lives in flash and the one-dispatch-one-wait protocol are
 * this module's business, so the mechanism lives here rather than in
 * whichever caller happens to need to erase a sector.
 *
 * Pairs like any other dispatch: park, do the flash work with interrupts
 * off, unpark. */
static volatile uint32_t s_core1_parked, s_core1_park_ack;

static void __not_in_flash_func(fb_core1_park_job)(void *arg) {
  volatile uint32_t *flag = (volatile uint32_t *)arg;
  /* MD/DOOM: with interrupts masked, so a Core 1 timer (the audio refill)
   * cannot run flash-resident code while Core 0 has XIP disabled. */
  uint32_t ints = save_and_disable_interrupts();
  s_core1_park_ack = 1;
  while (*flag) tight_loop_contents();
  restore_interrupts(ints);
}

void fb_core1_park(void) {
  s_core1_parked = 1;
  s_core1_park_ack = 0;
  fb_core1_dispatch(fb_core1_park_job, (void *)&s_core1_parked);
  /* MD/DOOM: only return once Core 1 is actually in the job with its
   * interrupts off. Before this, a flash erase could start while Core 1
   * was still finishing an audio refill from flash-resident code. */
  while (!s_core1_park_ack) tight_loop_contents();
}

void fb_core1_unpark(void) {
  s_core1_parked = 0;
  fb_core1_wait();
}

void fb_chunked_clear(uint8_t color) {
  memset(fb_chunked_buffer, color, FB_CHUNKED_SIZE);
}

/* MD/DOOM: chunk-reversed direct c2p. Image chunk k (96 pixels = 48
 * planar bytes) goes to cart chunk (COUNT-1-k); the 64-pixel tail keeps
 * its natural place. A chunk is a contiguous run of the chunked buffer
 * (rows are contiguous in memory, and 16-pixel blocks never straddle a
 * row), so the asm worker takes each run as one range. */
static uint16_t *s_direct_planar;

static void __not_in_flash_func(fb_c2p_chunks)(unsigned k0, unsigned k1) {
  uint8_t *cart = (uint8_t *)s_direct_planar;
  for (unsigned k = k0; k < k1; k++) {
    const uint8_t *src = fb_chunked_buffer + k * (2u * CART_FB_CHUNK_BYTES);
    fb_c2p_half((uint16_t *)(cart + (CART_FB_CHUNK_COUNT - 1u - k) * CART_FB_CHUNK_BYTES),
                src, src + 2u * CART_FB_CHUNK_BYTES);
  }
}

static void __not_in_flash_func(fb_c2p_chunks_bottom_job)(void *arg) {
  (void)arg;
  fb_c2p_chunks(CART_FB_CHUNK_COUNT / 2u, CART_FB_CHUNK_COUNT);
}

void __not_in_flash_func(fb_chunky_to_planar)(uint16_t *planar) {
  s_direct_planar = planar;
  fb_core1_dispatch(fb_c2p_chunks_bottom_job, NULL);
  fb_c2p_chunks(0u, CART_FB_CHUNK_COUNT / 2u);
  /* Tail: natural order at the end of the FB. */
  fb_c2p_half((uint16_t *)((uint8_t *)planar + CART_FB_CHUNK_COVERED),
              fb_chunked_buffer + 2u * CART_FB_CHUNK_COVERED,
              fb_chunked_buffer + FB_CHUNKED_SIZE);
  fb_core1_wait();
}
