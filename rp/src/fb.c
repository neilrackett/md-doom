/*
 * Copyright (C) 2026 Neil Rackett
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/**
 * File: fb.c
 * Description: Framebuffer module — owns `fb_screen` and brings up the
 *              single 32 KB cartridge framebuffer the m68k reads each
 *              VBL (single-FB design).
 *
 * The framebuffer base is derived from the linker symbol
 * `__rom_in_ram_start__` plus `CART_FRAMEBUFFER_OFFSET`, so the
 * layout stays the single source of truth — apps must never hard-code
 * the address.
 */

#include "fb.h"

#include <stdbool.h>
#include <string.h>

#include "audio.h"
#include "cart_shared.h"
#include "commemul.h"
#include "debug.h"
#include "fb_chunked.h"
#include "fb_font.h"
#include "font8x8.h"            /* defines `font8x8` (FB_FONT instance) */
#include "ikbd.h"
#include "xpadin.h"
#include "pico/time.h"          /* time_us_32 for the timing overlay */

/* VBL frame-sync. The m68k does a cart-bus read at
 * $FB8400 after each blit (see VBLSYNC_ADDR in userfw.s); the
 * commemul ring captures it with low-16 = 0x84xx. fb_pump_rom3
 * routes ROM3 samples to both the IKBD demux and this detector;
 * fb_publish() blocks until s_vbl_seen advances before overwriting
 * the cart FB. The timeout below keeps the RP from hanging if the
 * m68k isn't emitting acks (e.g. before it boots). Only $FB8400 itself
 * is a frame ack: when a frame takes two VBLs (mono, or no DMA sound)
 * the m68k reads $FB8401 after copying the first half, which is a VBL
 * (the audio refill takes it) but leaves half the cart FB still to be
 * read. Either ack has 2 added when YM sample bank 1 is playing
 * (VBLSYNC_BANK in userfw.s), which only the audio refill cares about. */
#define FB_VBLSYNC_HIBYTE   0x8400u
#define FB_VBLSYNC_BANK     0x0002u
/* Display report, the first read of every VBL: $FB9000 + 1 when the ST
 * is in high res on a mono monitor (VIDEO_WINDOW_BASE in userfw.s). It
 * also closes the window a frame ack opens: the m68k is about to read
 * the cart FB and the palette slots again. */
#define FB_VIDEO_HIBYTE     0x9000u
/* Relocation heartbeat, one read per VBL from the m68k -- see
 * RELOC_WINDOW_BASE in userfw.s. Emitted only from the relocated copy
 * running in ST RAM, so it is proof that nothing is executing from the
 * cartridge code area and that 16 KB can be reclaimed. It doubles as
 * the ST's liveness signal. */
#define FB_RELOC_HIBYTE     0x8E00u
#define FB_RELOC_PROTOCOL   0x01u
/* 256 colours (cart_shared.h): the display report's bit 1 says eight
 * planes and bit 2 a Falcon's palette; the m68k acks each part it has
 * copied at $FB9200 + the part's sequence number, low byte
 * (PARTACK_WINDOW_BASE in userfw.s), and reports the rows of a part it
 * copies in a VBL at $FB9400 + that (ROWS_WINDOW_BASE). */
#define FB_REPORT_EIGHT     0x02u
#define FB_REPORT_FALCON    0x04u
#define FB_PARTACK_HIBYTE   0x9200u
#define FB_ROWS_HIBYTE      0x9400u
/* A part's ack takes as many VBLs as the m68k needs to copy 100 rows;
 * this is only for an m68k that has stopped, which the ST-loss check
 * deals with. */
#define FB_PART_TIMEOUT_US  500000u
/* Every ROM3 window is a 256-byte page, so the high byte is the
 * discriminator and the low byte the payload. */
#define FB_WINDOW_HIMASK    0xFF00u
/* Safety net only: must comfortably exceed the worst-case latency from
 * "counter bumped" to "m68k VBLSYNC" (up to ~2 VBLs = 40 ms when c2p
 * overruns the slack and the m68k skips a frame). Firing early would
 * let the next c2p race the blit -- so keep it generous; it should
 * never fire while the m68k is actually running. */
#define FB_VSYNC_TIMEOUT_US 60000u

static volatile uint32_t s_vbl_seen;
static volatile uint32_t s_reloc_seen;
static uint32_t s_vbl_published;
static volatile bool s_st_mono;
static volatile bool s_st_eight, s_st_falcon;
static volatile bool s_in_window; /* a frame ack, and no VBL since */
static volatile uint8_t s_part_acked;   /* the last part ack's sequence byte */
static volatile uint32_t s_part_acks;   /* how many part acks so far */
static volatile uint8_t s_rows_per_vbl;

/* Boot splash, painted once by fb_init (defined below). */
static void fb_render_frame(void);

/* Frames published so far; also the value written to the cart-side
 * dirty-frame counter the m68k VBL loop compares against
 * (fb_frame_done). */
static uint32_t fb_frame_tick = 0;

const struct FB_MODE fb_mode_320x200 = {320, 200, 4};

struct FB_SCREEN fb_screen;

/* RP-incremented dirty-frame counter at $FA400C. The m68k userfw reads
 * this each VBL and only blits cart->ST screen when the value differs
 * from what it saw last iteration. Zeroed in fb_init, bumped by
 * fb_frame_done after all FB writes commit. */
static volatile uint32_t *fb_frame_counter;

int fb_init(const struct FB_MODE *mode) {
  if (mode == NULL) {
    return -1;
  }
  fb_screen.framebuffer =
      (unsigned int *)((uintptr_t)&__rom_in_ram_start__ +
                       CART_FRAMEBUFFER_OFFSET);
  fb_screen.width = mode->h_pixels;
  fb_screen.height = mode->v_pixels;
  fb_screen.color_bits = mode->color_bits;
  fb_frame_counter =
      (volatile uint32_t *)((uint8_t *)&__rom_in_ram_start__ +
                            CART_FB_FRAME_COUNTER_OFFSET);

  fb_frame_tick = 0;

  /* Zero the dirty-frame counter so the m68k userfw VBL loop sees a
   * clean baseline. The full shared region is already zeroed by
   * emul_start's ERASE_FIRMWARE_IN_RAM, so this is defensive against
   * future callers that might re-init fb without erasing. (Used to
   * live in chandler_init; relocated when chandler was removed.) */
  *fb_frame_counter = 0;

  /* Launch Core 1 with the chunky-to-planar bottom-half worker. */
  fb_chunked_init();

  /* Paint the boot splash + run the conversion once so the cart
   * framebuffer is in a deterministic state before the main loop
   * spins up. */
  fb_render_frame();

  DPRINTF("FB: %dx%d, %d bpp at %p (size=%u)\n", fb_screen.width,
          fb_screen.height, fb_screen.color_bits, fb_screen.framebuffer,
          (unsigned int)CART_FRAMEBUFFER_SIZE);
  return 0;
}

/* Boot splash rendered via the chunked path.
 *
 * fb_font's render_text writes directly into fb_chunked_buffer (one byte
 * per pixel, palette index in low nibble); fb_chunky_to_planar publishes
 * the whole buffer into the cart FB. The app (doomapp.c) owns every
 * frame after boot -- these two entry points only paint the one splash
 * frame fb_init needs so the ST has something deterministic to blit
 * before the app is up. */

static void fb_render_static(void) {
  font_set_font(&font8x8);
  font_set_color(0);
  font_set_border(0, 0);
  font_align(FONT_ALIGN_CENTER);

  font_move(160, 88);
  font_print("MD/DOOM");
  font_move(160, 104);
  font_print("STARTING UP...");
}

static void fb_render_frame(void) {
  /* Black background (palette index 15 on the default palette; the app
   * replaces the palette once it is running). */
  fb_chunked_clear(15);
  fb_render_static();
  fb_publish();
}

/* ROM3 ring dispatch: route each captured cart-bus read to the IKBD
 * demux, the Xpad report receiver, the VBL frame-sync detector and the
 * sound-capability decoder. Each looks only at its own window. */
static void fb_rom3_dispatch(uint16_t sample) {
  ikbd_consume_rom3_sample(sample);
  xpadin_consume_rom3_sample(sample);
  audio_consume_rom3_sample(sample);
  if ((sample & ~FB_VBLSYNC_BANK) == FB_VBLSYNC_HIBYTE) {
    s_vbl_seen++;
    s_in_window = true;
  }
  if ((sample & FB_WINDOW_HIMASK) == FB_VIDEO_HIBYTE) {
    s_st_mono = (sample & 1u) != 0u;
    s_st_eight = (sample & FB_REPORT_EIGHT) != 0u;
    s_st_falcon = (sample & FB_REPORT_FALCON) != 0u;
    s_in_window = false;
  }
  if ((sample & FB_WINDOW_HIMASK) == FB_PARTACK_HIBYTE) {
    s_part_acked = (uint8_t)sample;
    s_part_acks++;
  }
  if ((sample & FB_WINDOW_HIMASK) == FB_ROWS_HIBYTE) {
    s_rows_per_vbl = (uint8_t)sample;
  }
  if ((sample & FB_WINDOW_HIMASK) == FB_RELOC_HIBYTE &&
      (sample & 0xFFu) == FB_RELOC_PROTOCOL) {
    s_reloc_seen++;
  }
}

bool fb_st_relocated(void) { return s_reloc_seen != 0u; }

bool fb_st_mono(void) { return s_st_mono; }

bool fb_st_eight(void) { return s_st_eight; }

bool fb_st_falcon(void) { return s_st_falcon; }

unsigned fb_eight_rows_per_vbl(void) { return s_rows_per_vbl; }

bool fb_wait_st_reloc(uint32_t timeout_ms) {
  /* Block until the m68k says it is running from ST RAM. This is the
   * only point at which the RP can know the ST exists at all: the ack
   * wait below times out rather than blocking, so nothing else in the
   * boot path proves it. Must be called before anything could write
   * into the cartridge code area. */
  const uint32_t start_count = s_reloc_seen;
  const uint32_t start = time_us_32();
  while (s_reloc_seen == start_count) {
    fb_pump_rom3();
    if ((time_us_32() - start) / 1000u >= timeout_ms) {
      return false;
    }
    sleep_us(200);
  }
  return true;
}

/* --- Has the ST gone? ---------------------------------------------
 *
 * The heartbeat stopping means the machine was reset: TOS is back in
 * its memory test and everything that was running from ST RAM is gone.
 * That matters because by then the firmware may be using the
 * cartridge's code area as memory, so the cartridge the ST is about to
 * read has to be put back before it looks.
 *
 * The test is a counter delta, never time since the ring was last
 * drained: a level-pack load erases flash with interrupts off for the
 * better part of a second and nothing reads the ring meanwhile, but the
 * count has moved by the next check. Core 0 only -- the audio interrupt
 * scans the ring without consuming it, and the recovery reads flash. */
#define FB_ST_LOSS_US 250000u

static void (*s_st_lost_cb)(void);
static uint32_t s_live_count;
static uint32_t s_live_us;

void fb_set_st_lost_handler(void (*fn)(void)) {
  s_live_count = s_reloc_seen;
  s_live_us = time_us_32();
  s_st_lost_cb = fn;
}

static void fb_check_st_alive(void) {
  if (!s_st_lost_cb) {
    return;
  }
  const uint32_t n = s_reloc_seen;
  if (n != s_live_count) {
    s_live_count = n;
    s_live_us = time_us_32();
    return;
  }
  if (time_us_32() - s_live_us < FB_ST_LOSS_US) {
    return;
  }
  void (*cb)(void) = s_st_lost_cb;
  s_st_lost_cb = NULL; /* one shot: the handler does not come back */
  cb();
}

void fb_pump_rom3(void) {
  commemul_poll(fb_rom3_dispatch);
  fb_check_st_alive();
}

static uint32_t s_dbg_wait_max_us;
uint32_t fb_debug_wait_max_us(void) {
  const uint32_t v = s_dbg_wait_max_us;
  s_dbg_wait_max_us = 0;
  return v;
}

void fb_wait_blit_ack(void) {
  /* Block until the cart FB is free: the m68k has acked a frame since
   * the last publish (VBLSYNC) and has not started another VBL since
   * (its display report, the first thing it reads). Any ack since the
   * last publish used to do, however old, so a frame finished late in
   * a VBL was written while the m68k copied it, and tore. The ring is
   * captured as it happens, so once it is drained the last of the two
   * says where the m68k is. Drain it meanwhile so IKBD stays live; the
   * timeout is a safety net for "m68k not running" (e.g. at boot). */
  uint32_t t_wait = time_us_32();
  fb_pump_rom3();
  while (!s_in_window || s_vbl_seen == s_vbl_published) {
    fb_pump_rom3();
    if (time_us_32() - t_wait > FB_VSYNC_TIMEOUT_US) {
      break;
    }
  }
  s_vbl_published = s_vbl_seen;
  const uint32_t waited = time_us_32() - t_wait;
  if (waited > s_dbg_wait_max_us) s_dbg_wait_max_us = waited;
}

void fb_frame_done(void) {
  /* Mark the frame ready as the last write (barrier first). */
  fb_frame_tick++;
  __sync_synchronize();
  *fb_frame_counter = fb_frame_tick;
}

/* ST high res: 640x400 at one bit a pixel, and no palette, so each pen
 * is drawn as black ink or white paper by its brightness in the cart
 * palette slot, every pixel doubled both ways. Only the boot splash
 * (and the test card) come this way; the game's frames are doom_video's.
 * One core: it is not on any per-frame path that matters. */
static void fb_chunky_to_mono(uint8_t *cart) {
  const uint16_t *pal = (const uint16_t *)((uintptr_t)&__rom_in_ram_start__ +
                                           CART_PALETTE_OFFSET);
  uint32_t ink = 0;
  for (unsigned i = 0; i < CART_PALETTE_ENTRIES; i++) {
    const unsigned v = pal[i];
    if (((v >> 8) & 7u) + ((v >> 4) & 7u) + (v & 7u) < 11u) ink |= 1u << i;
  }
  for (unsigned y = 0; y < FB_CHUNKED_H; y++) {
    const uint8_t *src = fb_chunked_buffer + y * FB_CHUNKED_W;
    for (unsigned w = 0; w < FB_CHUNKED_W / 8u; w++, src += 8) {
      uint16_t bits = 0;
      for (unsigned i = 0; i < 8u; i++) {
        if ((ink >> (src[i] & 15u)) & 1u) bits |= (uint16_t)(3u << (14u - 2u * i));
      }
      const uint32_t o = y * 160u + w * 2u; /* rows 2y and 2y + 1, 80 bytes each */
      *(uint16_t *)(cart + fb_cart_offset(o)) = bits;
      *(uint16_t *)(cart + fb_cart_offset(o + 80u)) = bits;
    }
  }
}

/* --- 256 colours ----------------------------------------------------
 * See cart_shared.h. The picture goes in two parts of 100 rows, each
 * filling the cart FB; the m68k copies a part and acks it, and only
 * then is the FB written again. */
_Static_assert(CART_EIGHT_ROWS * CART_EIGHT_ROW_BYTES == CART_FRAMEBUFFER_SIZE,
               "a part is not the cart FB");
_Static_assert(2 * CART_EIGHT_ROWS == FB_CHUNKED_H, "two parts are not the picture");
_Static_assert(CART_PAL8_OFFSET + 2 * CART_PAL8_ENTRIES == CART_APP_FREE_OFFSET,
               "the 8-plane palette is not YM bank 1 and the YM packet");
_Static_assert((CART_PART_SEQ_OFFSET & 3) == 0 && CART_PART_IDX_OFFSET == CART_PART_SEQ_OFFSET + 2,
               "the part sequence and index share slot 3");

uint16_t fb_eight_colour(uint8_t r, uint8_t g, uint8_t b) {
  if (s_st_falcon) {
    const unsigned r6 = (r * 63u + 127u) / 255u, g6 = (g * 63u + 127u) / 255u,
                   b6 = (b * 63u + 127u) / 255u;
    return (uint16_t)((r6 << 10) | ((b6 >> 4) << 8) | (g6 << 2) | ((b6 >> 2) & 3u));
  }
  const unsigned r4 = (r * 15u + 127u) / 255u, g4 = (g * 15u + 127u) / 255u,
                 b4 = (b * 15u + 127u) / 255u;
  return (uint16_t)((r4 << 8) | (g4 << 4) | b4);
}

/* Plane k's four bits for the four pixels of q, pixel 0 (the low byte)
 * in the nibble's top bit: the 4-plane conversion's trick, run for all
 * eight planes. */
#define FB_NIB(q, k) (((((q) >> (k)) & 0x01010101u) * 0x80402010u) >> 28)

/* `rows` rows of 320 palette indices from `src` to 8 interleaved planes
 * at `dst`: each 16 pixels make 8 words, plane 0 first, the leftmost
 * pixel in bit 15, stored as little-endian uint32 pairs so that the cart
 * bus presents them to the m68k in plane order. */
static void __not_in_flash_func(fb_c2p8_rows)(uint32_t *dst, const uint8_t *src, unsigned rows) {
  for (unsigned n = rows * (FB_CHUNKED_W / 16u); n; n--) {
    uint32_t p01 = 0, p23 = 0, p45 = 0, p67 = 0;
    for (unsigned g = 0; g < 4u; g++) {
      const uint32_t q = (uint32_t)src[0] | ((uint32_t)src[1] << 8) |
                         ((uint32_t)src[2] << 16) | ((uint32_t)src[3] << 24);
      src += 4;
      const unsigned sh = 12u - 4u * g;
      p01 |= (FB_NIB(q, 0) << sh) | (FB_NIB(q, 1) << (sh + 16u));
      p23 |= (FB_NIB(q, 2) << sh) | (FB_NIB(q, 3) << (sh + 16u));
      p45 |= (FB_NIB(q, 4) << sh) | (FB_NIB(q, 5) << (sh + 16u));
      p67 |= (FB_NIB(q, 6) << sh) | (FB_NIB(q, 7) << (sh + 16u));
    }
    dst[0] = p01;
    dst[1] = p23;
    dst[2] = p45;
    dst[3] = p67;
    dst += 4;
  }
}

#define FB_EIGHT_HALF (CART_EIGHT_ROWS / 2u)

/* Core 1's share of a part: its second 50 rows. */
static void __not_in_flash_func(fb_c2p8_job)(void *arg) {
  const unsigned part = (unsigned)(uintptr_t)arg;
  fb_c2p8_rows((uint32_t *)fb_screen.framebuffer + FB_EIGHT_HALF * (CART_EIGHT_ROW_BYTES / 4u),
               fb_chunked_buffer + (part * CART_EIGHT_ROWS + FB_EIGHT_HALF) * FB_CHUNKED_W,
               FB_EIGHT_HALF);
}

static uint16_t s_part_seq, s_pal8_seq;
static bool s_part_out;          /* a part posted, its ack not yet seen */
static uint32_t s_part_acks_at_post;

/* Posts what is in the cart FB with part index idx: the index, then the
 * sequence last, as the m68k takes it (and the palette) when it sees the
 * sequence change. */
static void fb_eight_post(unsigned idx) {
  uint8_t *cart = (uint8_t *)&__rom_in_ram_start__;
  *(volatile uint16_t *)(cart + CART_PART_IDX_OFFSET) = (uint16_t)idx;
  __sync_synchronize();
  *(volatile uint16_t *)(cart + CART_PART_SEQ_OFFSET) = ++s_part_seq;
  s_part_acks_at_post = s_part_acks;
  s_part_out = true;
}

/* Until the m68k has copied the part last posted. */
static void fb_eight_wait(void) {
  if (!s_part_out) return;
  const uint32_t t0 = time_us_32();
  fb_pump_rom3();
  while (s_part_acks == s_part_acks_at_post || s_part_acked != (uint8_t)s_part_seq) {
    fb_pump_rom3();
    if (time_us_32() - t0 > FB_PART_TIMEOUT_US) break;
  }
  s_part_out = false;
  const uint32_t waited = time_us_32() - t0;
  if (waited > s_dbg_wait_max_us) s_dbg_wait_max_us = waited;
}

/* Converts rows (idx & 1) * 100 on of fb_chunked_buffer into the cart FB
 * and posts them with part index idx (cart_shared.h: the part, and
 * CART_PART_TO_N for the melt's new-frame buffer). */
static uint32_t fb_eight_part(unsigned idx, void (*palette)(uint16_t *slot)) {
  const unsigned part = idx & 1u;
  fb_eight_wait();
  uint8_t *cart = (uint8_t *)&__rom_in_ram_start__;
  if (palette) {
    palette((uint16_t *)(cart + CART_PAL8_OFFSET));
    *(volatile uint16_t *)(cart + CART_PAL8_SEQ_OFFSET) = ++s_pal8_seq;
  }
  const uint32_t t0 = time_us_32();
  fb_core1_dispatch(fb_c2p8_job, (void *)(uintptr_t)part);
  fb_c2p8_rows((uint32_t *)fb_screen.framebuffer,
               fb_chunked_buffer + part * CART_EIGHT_ROWS * FB_CHUNKED_W, FB_EIGHT_HALF);
  fb_core1_wait();
  const uint32_t us = time_us_32() - t0;
  fb_eight_post(idx);
  return us;
}

uint32_t fb_eight_publish(void (*palette)(uint16_t *slot)) {
  const uint32_t us = fb_eight_part(0u, palette);
  return us + fb_eight_part(1u, NULL);
}

/* --- 256 colours: the melt (cart_shared.h, "the melt") --------------- */

#define MELT8_COLS 160u
#define MELT8_H    (2u * CART_EIGHT_ROWS)

/* The heights the page on screen shows, and the page being drawn (valid
 * from the second step: before it, it holds whatever was there). */
static uint8_t s_melt_front[MELT8_COLS], s_melt_back[MELT8_COLS];
static bool s_melt_back_valid;

/* MELT8_PLAN_BEGIN (extracted by the host test) */
/* One melt step's display list into out: yk the heights to show, yf the
 * page on screen's, yb the page being drawn's when back_valid, w the
 * screen writes a row costs (2 on a TT, whose rows are doubled). A row r
 * of a 2-pixel column c shows the new frame if r < yk[c], else the old
 * frame moved down by yk[c] -- which the page on screen has at row
 * r - (yk[c] - yf[c]). The page being drawn is only written, so nothing
 * depends on order. Where it already shows a column right (yk == yb), and
 * above every other column's yb (the new frame there already), nothing
 * is written; below, each group is cut at its columns' heights and at
 * row 100 (the new-frame buffer's two halves), and every column of a
 * piece is written from its own source. Returns the words written. */
static unsigned melt8_plan(uint16_t *out, const uint8_t *yk, const uint8_t *yf,
                           const uint8_t *yb, bool back_valid, unsigned w) {
  uint16_t *o = out;
  for (unsigned g = 0; g < MELT8_COLS / 8u; g++) {
    const unsigned c0 = 8u * g;
    unsigned lo = MELT8_H;
    for (unsigned c = c0; c < c0 + 8u; c++) {
      if (!back_valid) lo = 0;
      else if (yk[c] != yb[c] && yb[c] < lo) lo = yb[c];
    }
    for (unsigned a = lo; a < MELT8_H;) {
      unsigned b = (a < CART_EIGHT_ROWS) ? CART_EIGHT_ROWS : MELT8_H;
      for (unsigned c = c0; c < c0 + 8u; c++) {
        if (yk[c] > a && yk[c] < b) b = yk[c];
      }
      /* Rows a..b-1: a column is the new frame throughout, or the old
       * one throughout, moved by its own amount. */
      uint16_t nmask = 0, fd[8], fm[8];
      unsigned nf = 0;
      for (unsigned i = 0; i < 8u; i++) {
        const uint16_t m = (uint16_t)(0xC000u >> (2u * i));
        if (yk[c0 + i] >= b) {
          nmask |= m;
          continue;
        }
        const uint16_t d = (uint16_t)(yk[c0 + i] - yf[c0 + i]);
        unsigned j = 0;
        while (j < nf && fd[j] != d) j++;
        if (j == nf) {
          fd[nf] = d;
          fm[nf++] = 0;
        }
        fm[j] |= m;
      }
      const unsigned k = (nmask ? 1u : 0u) + nf;
      const unsigned per_row = k + w;
      const unsigned most = (CART_MELT_REC_MAX_COST - CART_MELT_REC_COST) / per_row;
      for (unsigned t = a; t < b; t += most) {
        const unsigned n = (b - t < most) ? b - t : most;
        *o++ = (uint16_t)(n * per_row + CART_MELT_REC_COST);
        *o++ = (uint16_t)n;
        *o++ = (uint16_t)t;
        *o++ = (uint16_t)(g * 16u);
        *o++ = (uint16_t)k;
        if (nmask) {
          *o++ = CART_MELT_N;
          *o++ = nmask;
        }
        for (unsigned j = 0; j < nf; j++) {
          *o++ = fd[j];
          *o++ = fm[j];
        }
      }
      a = b;
    }
  }
  *o++ = 0;
  return (unsigned)(o - out);
}
/* MELT8_PLAN_END */

void fb_eight_melt_begin(void (*palette)(uint16_t *slot)) {
  (void)fb_eight_part(CART_PART_TO_N, palette);
  (void)fb_eight_part(CART_PART_TO_N | 1u, NULL);
  memset(s_melt_front, 0, sizeof(s_melt_front)); /* the old picture, unmoved */
  s_melt_back_valid = false;
}

void fb_eight_melt_step(const int16_t *y) {
  uint8_t yk[MELT8_COLS];
  for (unsigned c = 0; c < MELT8_COLS; c++) {
    const int v = y[c];
    yk[c] = (uint8_t)(v < 0 ? 0 : v > (int)MELT8_H ? (int)MELT8_H : v);
  }
  fb_eight_wait(); /* the cart FB is free once the last step is copied */
  /* At most ~10 K words, well inside the cart FB's 16 K. */
  (void)melt8_plan((uint16_t *)fb_screen.framebuffer, yk, s_melt_front, s_melt_back,
                   s_melt_back_valid, s_st_falcon ? 1u : 2u);
  fb_eight_post(CART_PART_MELT);
  memcpy(s_melt_back, s_melt_front, sizeof(s_melt_back));
  memcpy(s_melt_front, yk, sizeof(s_melt_front));
  s_melt_back_valid = true;
}

/* The framework's 16 ST colours (palette_set) as the first 16 of 256,
 * the rest black: the boot splash and the test card. An STE colour
 * nibble keeps its lowest bit in bit 3. */
static void fb_eight_st_palette(uint16_t *slot) {
  const uint16_t *pal = (const uint16_t *)((uintptr_t)&__rom_in_ram_start__ +
                                           CART_PALETTE_OFFSET);
  for (unsigned i = 0; i < CART_PAL8_ENTRIES; i++) {
    uint8_t c[3] = {0, 0, 0};
    if (i < CART_PALETTE_ENTRIES) {
      for (unsigned k = 0; k < 3u; k++) {
        const unsigned n = (pal[i] >> (8u - 4u * k)) & 15u;
        c[k] = (uint8_t)((((n & 7u) << 1) | (n >> 3)) * 17u);
      }
    }
    slot[i] = fb_eight_colour(c[0], c[1], c[2]);
  }
}

void fb_publish(void) {
  if (s_st_eight) {
    (void)fb_eight_publish(fb_eight_st_palette);
    return;
  }
  fb_wait_blit_ack();
  /* Chunky -> planar straight into the cart FB, in the m68k's post-blit
   * slack (~1 ms on both cores against ~3 ms available). */
  if (s_st_mono) {
    fb_chunky_to_mono((uint8_t *)fb_screen.framebuffer);
  } else {
    fb_chunky_to_planar((uint16_t *)fb_screen.framebuffer);
  }
  fb_frame_done();
}
