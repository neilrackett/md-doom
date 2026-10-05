/*
 * Copyright (C) 2026 Neil Rackett
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * File: i_mdmusic.c
 * Description: Doom's music on the YM2149. With STE DMA sound the
 *              sound effects use the DMA, so the YM is free; without it
 *              the YM plays the sound effects, unless keypad ) gives it
 *              to the music instead (I_MD_MusicSetYm, saved with the
 *              video options), which the m68k picks up from
 *              CART_YM_MUSIC_OFFSET. The songs are register streams in
 *              STDL's STM format,
 *              made offline from the MUS lumps by tools/ymmusic.py and
 *              read from the pack folder on the SD card (D_E1M1.STM and
 *              so on); a song that is not there is simply not played.
 *
 *              An STM file is a 12-byte header ("STM1", then big-endian
 *              frame rate, frame count, loop frame and a reserved word)
 *              and one frame per tick: a 16-bit mask of the registers
 *              that change, then a byte for each, register 0 first. The
 *              first frame sets all of them, so a song loops by going
 *              back to it.
 *
 *              The game core streams the file into a 1 KB ring once a
 *              frame (I_MD_MusicPump: the renderer calls the engine's
 *              own music poll mid-frame, which is no place for the SD
 *              card). The audio interrupt on Core 1 steps it by elapsed
 *              time, so the tempo holds at 50, 60 or 71 Hz (I_MD_MusicTick),
 *              and publishes the registers to the cart (CART_YM_PACKET_
 *              OFFSET) for the m68k to write to the YM in its next VBL.
 */
#include "config.h"

#include <stdio.h>
#include <string.h>

#include "audio.h"
#include "cart_shared.h"
#include "constants.h"
#include "debug.h"
#include "ff.h"
#include "hardware/sync.h"
#include "i_sound.h"
#include "pico/time.h"
#if PICO_ON_DEVICE
#include "hardware/regs/addressmap.h"
#endif

#define STM_HEADER 12u
#define RING_SIZE 1024u                  /* a power of two */
#define RING_MASK (RING_SIZE - 1u)
#define RING_READ 512u                   /* refill a block at a time */
#define MAX_STEP_US 100000u              /* catch up at most this after a stall */
#define YM_REGS 14u

/* The first 1 KB of the USB controller's RAM is free (the renderer's
 * vpatch lists start at +0x400) and costs the zone nothing. */
#if PICO_ON_DEVICE
#define s_ring ((uint8_t *)USBCTRL_DPRAM_BASE)
#else
static uint8_t s_ring[RING_SIZE];
#endif

typedef enum { MUSIC_STOPPED = 0, MUSIC_PLAYING, MUSIC_ENDED } music_state_t;

/* Guarded by s_lock: shared between the game core and the interrupt. */
static spin_lock_t *s_lock;
static music_state_t s_state;
static uint32_t s_head, s_tail;          /* ring byte counts, free-running */
static bool s_eof;                       /* the file is all in the ring */
static bool s_paused;
static uint8_t s_atten;                  /* volume steps taken off; 16 = mute */
static bool s_republish;                 /* volume or pause changed */
static uint32_t s_frame_us;
static uint32_t s_acc_us, s_last_us;
static uint8_t s_shadow[YM_REGS];        /* the song's registers */
static uint16_t s_changed;               /* since the last publish */
static uint16_t s_seq;
static bool s_silent = true;             /* last publish was silence (or none yet) */

/* No DMA sound: the YM is the music's (the player asked for it, and the
 * song is there), as the m68k is told through CART_YM_MUSIC_OFFSET.
 * Written by the game core, read by the interrupt. */
static volatile bool s_ym_owned;

/* Game core only. */
static bool s_ym_music;                  /* keypad ): music rather than effects */
static FIL s_file;
static bool s_file_open;
static bool s_looping;
static bool s_pending;                   /* a song to open at the next pump */
static char s_name[9];
static char s_path[48];

static const char *s_folder = "/doom";

/* The music volume slider (0..15) as YM steps of about 3 dB, so the
 * slider is roughly linear in amplitude: steps = -2 log2(slider / 15). */
static const uint8_t k_volume_atten[16] = {16, 8, 6, 5, 4, 3, 3, 2, 2, 1, 1, 1, 1, 0, 0, 0};

void I_MD_MusicSetFolder(const char *folder) { s_folder = folder; }

/* Whether the music has the YM: always with DMA sound, and without it
 * only once the YM has been handed over. */
static bool ym_is_ours(void) {
  const audio_mode_t mode = audio_get_mode();
  return mode == AUDIO_MODE_DMA || (mode == AUDIO_MODE_YM && s_ym_owned);
}

/* Whether a song may be started at all. */
static bool may_play(void) {
  const audio_mode_t mode = audio_get_mode();
  return mode == AUDIO_MODE_DMA || (mode == AUDIO_MODE_YM && s_ym_music);
}

/* Hand the YM to the music or back to the sound effects (no DMA sound;
 * the m68k does the switching). */
static void set_ym_owner(bool music) {
  if (s_ym_owned == music) return;
  s_ym_owned = music;
  *(volatile uint16_t *)((uint8_t *)&__rom_in_ram_start__ + CART_YM_MUSIC_OFFSET) = music ? 1u : 0u;
}

/* The registers as the m68k should write them. R0..R10 go every time,
 * so a lost or repeated packet cannot leave a wrong value behind; the
 * envelope registers only when they change, as writing R13 restarts
 * the envelope. Never R14/R15 (the floppy and printer ports), and R7
 * keeps both ports outputs. Called with s_lock held. */
static void publish(bool silent) {
  volatile uint16_t *pkt =
      (volatile uint16_t *)((uint8_t *)&__rom_in_ram_start__ + CART_YM_PACKET_OFFSET);
  for (uint32_t r = 0; r < YM_REGS; r++) {
    uint8_t v = s_shadow[r];
    if (r == 7) {
      v |= 0xC0u;
    } else if (r >= 8u && r <= 10u) {
      const uint8_t level = v & 0x0Fu;
      v = (silent || level <= s_atten) ? 0u : (uint8_t)(level - s_atten);
    }
    pkt[2 + r] = v;
  }
  pkt[1] = (uint16_t)(0x07FFu | (s_changed & 0x3800u));
  __dmb(); /* the registers before the sequence number that announces them */
  pkt[0] = ++s_seq;
  s_changed = 0;
  s_silent = silent;
}

/* One frame from the ring into the shadow, or false if it is not all
 * there yet. Called with s_lock held. */
static bool step_frame(void) {
  const uint32_t avail = s_head - s_tail;
  if (avail < 2u) return false;
  const uint16_t mask = (uint16_t)(((uint16_t)s_ring[s_tail & RING_MASK] << 8) |
                                   s_ring[(s_tail + 1u) & RING_MASK]) & 0x3FFFu;
  const uint32_t n = (uint32_t)__builtin_popcount(mask);
  if (avail < 2u + n) return false;
  uint32_t t = s_tail + 2u;
  for (uint32_t r = 0; r < YM_REGS; r++) {
    if (mask & (1u << r)) s_shadow[r] = s_ring[t++ & RING_MASK];
  }
  s_tail = t;
  s_changed |= mask;
  return true;
}

/* Once per VBL from the audio interrupt (I_MD_SoundFill), just after
 * the m68k's ack: it writes what this publishes in its next VBL. */
void I_MD_MusicTick(void) {
  if (!s_lock || !ym_is_ours()) return;
  const uint32_t ints = spin_lock_blocking(s_lock);
  const uint32_t now = time_us_32();
  bool silent = s_state != MUSIC_PLAYING || s_paused || s_atten >= 16u;
  if (s_state == MUSIC_PLAYING && !s_paused) {
    uint32_t dt = now - s_last_us;
    if (dt > MAX_STEP_US) dt = MAX_STEP_US;
    s_acc_us += dt;
    while (s_acc_us >= s_frame_us) {
      if (!step_frame()) {
        if (s_eof && s_head == s_tail) {
          s_state = MUSIC_ENDED; /* a song that does not loop is over */
          silent = true;
        }
        s_acc_us = 0; /* starved: wait for the game core, then go on */
        break;
      }
      s_acc_us -= s_frame_us;
    }
  }
  s_last_us = now;
  if (silent) {
    if (!s_silent || s_republish) publish(true);
  } else if (s_changed || s_republish || s_silent) {
    publish(false);
  }
  s_republish = false;
  spin_unlock(s_lock, ints);
}

/* Open s_path and check its header; the frame rate, or 0 if it is not
 * an STM file (and is then closed again). Game core only. */
static uint32_t open_song(void) {
  if (f_open(&s_file, s_path, FA_READ) != FR_OK) return 0;
  uint8_t hdr[STM_HEADER];
  UINT got = 0;
  const uint32_t hz = (f_size(&s_file) > STM_HEADER &&
                       f_read(&s_file, hdr, sizeof(hdr), &got) == FR_OK && got == sizeof(hdr) &&
                       memcmp(hdr, "STM1", 4) == 0)
                          ? ((uint32_t)hdr[4] << 8 | hdr[5])
                          : 0u;
  if (hz == 0 || hz > 1000u) {
    f_close(&s_file);
    return 0;
  }
  return hz;
}

/* Top the ring up from the file, a block at a time. */
static void fill_ring(void) {
  if (!s_file_open) return;
  for (;;) {
    uint32_t ints = spin_lock_blocking(s_lock);
    const uint32_t head = s_head, used = s_head - s_tail;
    spin_unlock(s_lock, ints);
    if (RING_SIZE - used < RING_READ) return;
    const uint32_t at = head & RING_MASK;
    const uint32_t want = (RING_SIZE - at < RING_READ) ? RING_SIZE - at : RING_READ;
    if (f_eof(&s_file)) {
      /* Round again from the top (reopened, which costs less flash
       * than f_lseek). */
      f_close(&s_file);
      if (s_looping && open_song()) continue;
      ints = spin_lock_blocking(s_lock);
      s_eof = true;
      spin_unlock(s_lock, ints);
      s_file_open = false;
      return;
    }
    UINT got = 0;
    const FRESULT fr = f_read(&s_file, s_ring + at, want, &got);
    if (fr != FR_OK || got == 0) {
      DPRINTF("music: %s read failed (%d)\n", s_name, (int)fr);
      ints = spin_lock_blocking(s_lock);
      s_eof = true;
      spin_unlock(s_lock, ints);
      f_close(&s_file);
      s_file_open = false;
      return;
    }
    ints = spin_lock_blocking(s_lock);
    s_head += got;
    spin_unlock(s_lock, ints);
  }
}

/* Game core only, at the end of a frame (and between the steps of a
 * melt): the only place the SD card is read. A new song is opened here
 * rather than in PlaySong, which can be reached from the renderer (the
 * IDMUS cheat, through the input poll) with its deep stack. */
void I_MD_MusicPump(void) {
  if (!s_pending) {
    fill_ring();
    return;
  }
  s_pending = false;
  const uint32_t hz = open_song();
  if (audio_get_mode() == AUDIO_MODE_YM) set_ym_owner(hz != 0); /* no song: the effects keep it */
  if (hz == 0) {
    DPRINTF("music: no %s\n", s_path); /* or not an STM file */
    return;
  }
  s_file_open = true;
  DPRINTF("music: %s at %u Hz%s\n", s_path, (unsigned)hz, s_looping ? ", looping" : "");
  fill_ring();

  const uint32_t ints = spin_lock_blocking(s_lock);
  memset(s_shadow, 0, sizeof(s_shadow));
  s_shadow[7] = 0x3F; /* all tones and noise off until the first frame */
  s_changed = 0x3FFFu;
  s_frame_us = 1000000u / hz;
  s_acc_us = s_frame_us; /* the first frame goes out at the next VBL */
  s_last_us = time_us_32();
  s_paused = false;
  s_state = MUSIC_PLAYING;
  spin_unlock(s_lock, ints);
}

static void stop_song(void) {
  s_pending = false;
  const uint32_t ints = spin_lock_blocking(s_lock);
  s_state = MUSIC_STOPPED;
  s_head = s_tail = 0;
  s_eof = false;
  spin_unlock(s_lock, ints);
  if (s_file_open) {
    f_close(&s_file);
    s_file_open = false;
  }
}

/* Stop, and wait until the m68k has had the silence: before anything
 * that holds Core 1 still (programming a pack), which would otherwise
 * leave the last notes sounding until it is done. */
void I_MD_MusicWaitSilent(void) {
  if (!s_lock) return;
  const bool sounding = !s_silent;
  stop_song();
  if (!sounding || !ym_is_ours()) return;
  const absolute_time_t until = make_timeout_time_ms(200);
  for (;;) {
    const uint32_t ints = spin_lock_blocking(s_lock);
    const bool done = s_silent;
    spin_unlock(s_lock, ints);
    if (done || time_reached(until)) break;
    sleep_ms(2);
  }
  sleep_ms(30); /* the m68k writes it in its next VBL */
}

/* The music module. RegisterSong is handed the lump's name ("d_e1m1")
 * rather than its data: the level packs carry no music (see
 * S_ChangeMusic). */

static boolean MD_InitMusic(void) {
  s_lock = spin_lock_instance(PICO_SPINLOCK_ID_OS1); /* the sfx mixer's: same interrupt */
  s_atten = k_volume_atten[8];
  s_shadow[7] = 0x3F; /* all tones and noise off */
  return true;
}

static void MD_ShutdownMusic(void) { stop_song(); }

static void MD_SetMusicVolume(int volume) {
  const uint32_t ints = spin_lock_blocking(s_lock);
  s_atten = k_volume_atten[(volume >> 3) & 15];
  s_republish = true;
  spin_unlock(s_lock, ints);
}

static void MD_PauseMusic(void) {
  const uint32_t ints = spin_lock_blocking(s_lock);
  s_paused = true;
  spin_unlock(s_lock, ints);
}

static void MD_ResumeMusic(void) {
  const uint32_t ints = spin_lock_blocking(s_lock);
  s_paused = false;
  s_last_us = time_us_32();
  s_republish = true;
  spin_unlock(s_lock, ints);
}

static void *MD_RegisterSong(should_be_const void *data, int len) {
  (void)len;
  snprintf(s_name, sizeof(s_name), "%s", (const char *)data);
  return s_name;
}

static void MD_UnRegisterSong(void *handle) { (void)handle; }

/* Ask for s_name to be opened by the next I_MD_MusicPump. */
static void request_song(void) {
  snprintf(s_path, sizeof(s_path), "%s/%s.STM", s_folder, s_name);
  for (char *p = s_path + strlen(s_folder); *p; p++) {
    if (*p >= 'a' && *p <= 'z') *p = (char)(*p - 'a' + 'A');
  }
  s_pending = true;
}

static void MD_PlaySong(void *handle, boolean looping) {
  (void)handle;
  stop_song();
  s_looping = looping; /* kept for keypad ), should the YM be given to the music later */
  if (may_play()) request_song(); /* else the YM is playing the sound effects */
}

/* Keypad ), without DMA sound: the YM to the music (true) or back to the
 * sound effects. Game core only, between frames (it opens the song).
 * Returns whether the music has the YM now: false for the effects, and
 * also if there is no song to play, which leaves the effects with it.
 * With DMA sound both play anyway, and this only records the choice. */
bool I_MD_MusicSetYm(bool music) {
  s_ym_music = music;
  if (!s_lock || audio_get_mode() != AUDIO_MODE_YM) return music; /* at boot, or DMA sound */
  if (!music) {
    stop_song();
    set_ym_owner(false);
    return false;
  }
  if (s_name[0]) {
    stop_song();
    request_song();
    I_MD_MusicPump();
  }
  if (!s_ym_owned && s_name[0]) s_ym_music = false; /* no song files: keep the effects */
  return s_ym_music;
}

bool I_MD_MusicGetYm(void) { return s_ym_music; }

/* For the loading screen: whether this machine would play music but the
 * map's song (D_E1M2.STM for "E1M2") is not on the card -- level data
 * from before there was music. Game core only. */
bool I_MD_MusicFileMissing(const char *map) {
  if (!may_play()) return false;
  char path[48];
  snprintf(path, sizeof(path), "%s/D_%s.STM", s_folder, map);
  return f_stat(path, NULL) != FR_OK;
}

static void MD_StopSong(void) { stop_song(); }

static boolean MD_MusicIsPlaying(void) { return s_pending || s_state == MUSIC_PLAYING; }

static const snddevice_t music_md_devices[] = {SNDDEVICE_GENMIDI};

const music_module_t music_md_module = {
    music_md_devices,
    sizeof(music_md_devices) / sizeof(music_md_devices[0]),
    MD_InitMusic,
    MD_ShutdownMusic,
    MD_SetMusicVolume,
    MD_PauseMusic,
    MD_ResumeMusic,
    MD_RegisterSong,
    MD_UnRegisterSong,
    MD_PlaySong,
    MD_StopSong,
    MD_MusicIsPlaying,
    NULL, /* Poll: the renderer calls it mid-frame; see I_MD_MusicPump */
};
