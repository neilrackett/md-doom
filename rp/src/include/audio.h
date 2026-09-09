/**
 * File: audio.h
 * Description: Cart-shared audio buffer producer + app-facing API.
 *
 * The m68k (target/atarist/src/userfw.s) reads sample bytes from a
 * 1024-byte cart buffer at CART_AUDIO_BUFFER_OFFSET once per VBL:
 * signed 8-bit PCM for STE DMA sound, (vA, vB) YM volume pairs
 * otherwise. The RP refills the buffer once per VBL from the VBL-synced
 * timer interrupt (audio_start_vbl_timer).
 *
 * Apps install audio content with audio_set_fill_callback(cb): the
 * library invokes `cb(buf, bytes)` once per refill with `bytes` set to
 * the m68k's per-VBL consumption, and the callback writes exactly that
 * many bytes into `buf` in the format audio_get_mode() names.
 *
 * If no callback is installed the cart buffer stays whatever
 * audio_init() left it (zero = silence). Calling
 * audio_set_fill_callback(NULL) re-enters this silent state.
 */

#ifndef AUDIO_H_INCLUDED
#define AUDIO_H_INCLUDED

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Audio back-end, chosen at runtime from the m68k's _SND detection
 * (reported over the cart bus; audio_consume_rom3_sample decodes the
 * report out of the ROM3 ring). Governs the per-VBL
 * refill size and the format the fill callback produces. Defaults to
 * SILENT until the first report arrives. */
typedef enum {
  AUDIO_MODE_SILENT = 0, /* zeros -- pre-report / no audio    */
  AUDIO_MODE_YM,         /* 224 B/VBL: 112 (vA,vB) pairs      */
  AUDIO_MODE_DMA         /* 500 B/VBL: 500 signed PCM samples */
} audio_mode_t;

/* Set the back-end (idempotent). Called by the cart-bus capability
 * decoder; changes the refill size the fill callback is asked for. */
void audio_set_mode(audio_mode_t mode);

/* Current back-end, for the fill callback to pick its output format. */
audio_mode_t audio_get_mode(void);
uint32_t audio_get_fill_bytes(void); /* bytes per VBL currently asked of the fill */

/* Set the per-VBL refill size. Only meaningful for STE DMA sound, where
 * the m68k measures what the chip actually consumes per frame and sends
 * it back -- that figure is near 501, not 500, and differs between
 * machines because the sound DMA and the video are clocked separately.
 * Ignored in YM mode, whose rate is fixed by the Timer-B divider, and
 * for values outside the cart buffer. */
void audio_set_fill_bytes(uint32_t bytes);

/* commemul ring consumer. Passed each captured ROM3 sample by the
 * dispatcher in fb.c; filters for the m68k's sound-capability and
 * buffer-length report windows and applies what it finds. */
void audio_consume_rom3_sample(uint16_t addr_lsb);

typedef void (*audio_fill_cb_t)(uint8_t *buf, uint32_t bytes);

/* Initialise the cart audio buffer pointer; clear any previously
 * installed callback. Call once during boot. */
void audio_init(void);

/* Refill from a 1 ms timer interrupt, each time the
 * m68k's end-of-blit ack ($FB8400) shows up in the ROM3 ring. That is
 * right after the m68k has copied the previous buffer, so the refill can
 * never tear under its copy, and it happens every VBL however long the
 * main loop's frames take. `core` is the core that takes the interrupt:
 * use 1 (a Core 1 alarm pool, created via a framework job, so call it
 * between publishes). On Core 0 the same interrupt stalls the game's
 * renderer for 100-200 ms at a time, cause unknown; it is kept for A/B
 * tests only. Call after commemul_init() and audio_init(). */
void audio_start_vbl_timer(int core); /* 0 or 1: which core takes the interrupt */
void audio_stop_vbl_timer(void);
int audio_vbl_timer_core(void);         /* -1 when stopped */

/* Debug counters since the last call: longest fill, number of fills,
 * longest timer callback (fill or not). */
void audio_debug_stats(uint32_t *fill_max_us, uint32_t *fills, uint32_t *cb_max_us, uint32_t *cbs);

/* Install (or clear, if cb == NULL) the fill callback. The callback
 * must write exactly `bytes` bytes (the library does not zero on entry)
 * and must not block: on the game path it runs from Core 1's timer
 * interrupt. */
void audio_set_fill_callback(audio_fill_cb_t cb);

#ifdef __cplusplus
}
#endif

#endif /* AUDIO_H_INCLUDED */
