/**
 * File: audio.c
 * Description: Cart-shared audio buffer producer.
 *
 * The m68k reads sample bytes from the cart buffer at
 * CART_AUDIO_BUFFER_OFFSET (1024 B) once per VBL. The RP refills it
 * from a VBL-synced timer interrupt (audio_start_vbl_timer). The
 * library is format-agnostic -- it just dispatches to whatever fill
 * callback the app has installed. See audio.h for the public API.
 */

#include "audio.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "cart_shared.h"
#include "constants.h"
#include "debug.h"
#include "pico/stdlib.h"
#include "commemul.h"
#include "fb_chunked.h"
#include "pico/time.h"

/* Audio back-end is chosen at RUNTIME from the m68k's _SND detection,
 * reported over the cart bus (see fb.c / userfw.s). audio_set_mode()
 * switches the per-VBL refill size, and the fill callback keys off
 * audio_get_mode() to produce the matching format:
 *  - AUDIO_MODE_YM: 5,585 Hz, 112 samples/VBL x 2 B (vA,vB) = 224 B
 *    (m68k Timer-B, TBDR=110 /4 prescaler; ~800 B of the cart buffer
 *    stays as overrun headroom).
 *  - AUDIO_MODE_DMA: 25,033 Hz, 500 samples/VBL x 1 B (signed) = 500 B.
 *  - AUDIO_MODE_SILENT: 500 B of zeros until the first report arrives
 *    (zeros are silence for both the DMA and YM readers).
 * Default is SILENT so nothing plays garbage before the m68k reports. */
#define AUDIO_FILL_BYTES_YM  224u
#define AUDIO_FILL_BYTES_DMA 500u

static audio_mode_t s_audio_mode = AUDIO_MODE_SILENT;
static uint32_t s_fill_bytes = AUDIO_FILL_BYTES_DMA;

void audio_set_mode(audio_mode_t mode) {
  if (mode == s_audio_mode) {
    return;
  }
  s_audio_mode = mode;
  s_fill_bytes = (mode == AUDIO_MODE_YM) ? AUDIO_FILL_BYTES_YM
                                         : AUDIO_FILL_BYTES_DMA;
  DPRINTF("audio_set_mode: %s (%u B/VBL)\n",
          mode == AUDIO_MODE_DMA ? "STE DMA" : mode == AUDIO_MODE_YM
                                                   ? "YM"
                                                   : "silent",
          (unsigned)s_fill_bytes);
}

audio_mode_t audio_get_mode(void) { return s_audio_mode; }
uint32_t audio_get_fill_bytes(void) { return s_fill_bytes; }

/* Sound-capability report window (SNDCAP_WINDOW_BASE in userfw.s): the
 * m68k reads $FB8600 + has_dma once per VBL, having probed the _SND
 * cookie at boot. Decoding it here rather than in the ROM3 dispatcher
 * keeps the window address and the bit-to-back-end mapping with the
 * module that owns audio_mode_t and the refill sizes. */
#define AUDIO_SNDCAP_HIBYTE 0x8600u
#define AUDIO_WINDOW_HIMASK 0xFF00u

/* Buffer-length report (SNDLEN_WINDOW_BASE in userfw.s): while STE DMA
 * sound is running the m68k measures how much the chip actually eats
 * per frame -- it is about 501.5, not 500, and it varies by machine
 * because the DMA and the video run off different oscillators -- and
 * sends the length back as bytes/4 once a VBL. Producing exactly that
 * many is what stops the chip running a buffer dry and replaying it. */
#define AUDIO_SNDLEN_HIBYTE 0x8C00u

/* The report is biased by the m68k's minimum so it fits one byte; this
 * must match STE_SND_LEN_MIN in userfw.s. */
#define AUDIO_SNDLEN_BIAS 480u

/* Guard rails on a value that arrives over a bus. The m68k steers
 * within a narrow band around one VBL's worth, so anything well outside
 * it is a corrupt sample rather than a length, and is ignored. */
#define AUDIO_FILL_BYTES_MIN 448u
#define AUDIO_FILL_BYTES_MAX 576u

void audio_set_fill_bytes(uint32_t bytes) {
  if (s_audio_mode != AUDIO_MODE_DMA) {
    return; /* the YM rate is fixed by the Timer-B divider */
  }
  if (bytes < AUDIO_FILL_BYTES_MIN || bytes > AUDIO_FILL_BYTES_MAX ||
      bytes > CART_AUDIO_BUFFER_SIZE) {
    return;
  }
  s_fill_bytes = bytes;
}

void audio_consume_rom3_sample(uint16_t addr_lsb) {
  const uint16_t window = addr_lsb & AUDIO_WINDOW_HIMASK;
  if (window == AUDIO_SNDCAP_HIBYTE) {
    audio_set_mode((addr_lsb & 1u) ? AUDIO_MODE_DMA : AUDIO_MODE_YM);
  } else if (window == AUDIO_SNDLEN_HIBYTE) {
    audio_set_fill_bytes(AUDIO_SNDLEN_BIAS + (uint32_t)(addr_lsb & 0xFFu));
  }
}

static uint8_t *s_audio_buf;
static uint32_t s_last_frame_us;
static audio_fill_cb_t s_fill_cb;

void audio_init(void) {
  uint8_t *base = (uint8_t *)&__rom_in_ram_start__;
  s_audio_buf = base + CART_AUDIO_BUFFER_OFFSET;
  s_last_frame_us = 0;
  s_fill_cb = NULL;

  /* ERASE_FIRMWARE_IN_RAM at emul_start already zeroed the cart
   * buffer (= silence on YM). With no callback installed, the
   * buffer stays zero until an app calls audio_set_fill_callback(). */

  DPRINTF("audio_init: cart buffer %u B at offset $%04X, %u B/VBL refill\n",
          (unsigned)CART_AUDIO_BUFFER_SIZE,
          (unsigned)CART_AUDIO_BUFFER_OFFSET, (unsigned)s_fill_bytes);
}

void audio_set_fill_callback(audio_fill_cb_t cb) {
  s_fill_cb = cb;
}

/* VBL-synced refill (see audio.h). The scan cursor is the timer's own,
 * separate from commemul_poll's read index. A fill is never repeated
 * within 5 ms, in case an ack is seen twice across a scan boundary. */
#define AUDIO_VBLSYNC_HIBYTE 0x8400u
#define AUDIO_MIN_FILL_GAP_US 5000u

static repeating_timer_t s_vbl_timer;
static uint32_t s_scan_cursor;
static volatile uint32_t s_dbg_fill_max_us, s_dbg_fills, s_dbg_cb_max_us, s_dbg_cbs;

static bool __not_in_flash_func(audio_vbl_timer_cb)(repeating_timer_t *rt) {
  (void)rt;
  s_dbg_cbs++;
  const uint32_t t0 = time_us_32();
  if (commemul_scan(&s_scan_cursor, AUDIO_VBLSYNC_HIBYTE) && s_fill_cb) {
    if (t0 - s_last_frame_us >= AUDIO_MIN_FILL_GAP_US) {
      s_last_frame_us = t0;
      s_fill_cb(s_audio_buf, s_fill_bytes);
      const uint32_t dt = time_us_32() - t0;
      if (dt > s_dbg_fill_max_us) s_dbg_fill_max_us = dt;
      s_dbg_fills++;
    }
  }
  const uint32_t cb = time_us_32() - t0;
  if (cb > s_dbg_cb_max_us) s_dbg_cb_max_us = cb;
  return true;
}

void audio_debug_stats(uint32_t *fill_max_us, uint32_t *fills, uint32_t *cb_max_us, uint32_t *cbs) {
  *fill_max_us = s_dbg_fill_max_us;
  *fills = s_dbg_fills;
  *cb_max_us = s_dbg_cb_max_us;
  *cbs = s_dbg_cbs;
  s_dbg_fill_max_us = s_dbg_fills = s_dbg_cb_max_us = s_dbg_cbs = 0;
}

static int s_vbl_timer_core = -1; /* -1 off, else the core the IRQ runs on */
static alarm_pool_t *s_core1_pool;

/* Runs on Core 1 (as a framework job): a Core 1 alarm pool binds its
 * interrupt to Core 1, so the refill then never interrupts Core 0. */
static void core1_start_timer_job(void *arg) {
  (void)arg;
  if (!s_core1_pool) {
    s_core1_pool = alarm_pool_create_with_unused_hardware_alarm(4);
  }
  alarm_pool_add_repeating_timer_us(s_core1_pool, -1000, audio_vbl_timer_cb, NULL, &s_vbl_timer);
}

void audio_start_vbl_timer(int core) {
  if (s_vbl_timer_core >= 0) return;
  s_scan_cursor = 0;
  if (core == 1) {
    fb_core1_dispatch(core1_start_timer_job, NULL);
    fb_core1_wait();
  } else {
    add_repeating_timer_us(-1000, audio_vbl_timer_cb, NULL, &s_vbl_timer);
  }
  s_vbl_timer_core = core ? 1 : 0;
  DPRINTF("audio: VBL-synced refill timer started on core %d\n", s_vbl_timer_core);
}

void audio_stop_vbl_timer(void) {
  if (s_vbl_timer_core < 0) return;
  cancel_repeating_timer(&s_vbl_timer);
  s_vbl_timer_core = -1;
  memset(s_audio_buf, 0, CART_AUDIO_BUFFER_SIZE);
  DPRINTF("audio: VBL-synced refill timer stopped\n");
}

int audio_vbl_timer_core(void) { return s_vbl_timer_core; }
