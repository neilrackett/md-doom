/*
 * Copyright (C) 2026 Neil Rackett
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * File: i_mdmusic.c
 * Description: Doom's music, synthesised on the RP2040 and mixed into the
 *              sound effects' stream, so the ST plays both from the one
 *              buffer: through the STE's DMA sound, or through the YM
 *              volume pairs of a plain ST. The songs are Doom's own MUS
 *              lumps, D_E1M1.MUS and so on, played the way DMX played them
 *              on an OPL2 (i_mdopl.c, with GENMIDI.OP2's instruments), and
 *              read from the pack folder on the SD card; a song that is
 *              not there is simply not played.
 *
 *              The game core streams the song's score into a 1 KB ring
 *              once a frame (I_MD_MusicPump: the renderer calls the
 *              engine's own music poll mid-frame, which is no place for
 *              the SD card), and at a song's start loads the instruments
 *              its header lists. The audio interrupt on Core 1, as it
 *              fills each VBL's buffer (I_MD_MusicMix), steps the score at
 *              MUS's 140 ticks a second and renders the chip at its own
 *              49,716 Hz, averaged down to the output rate. Its timing
 *              comes from the samples, so the tempo holds whatever the VBL
 *              rate.
 */
#include "config.h"

#include <stdio.h>
#include <string.h>

#include "audio.h"
#include "cart_shared.h"
#include "debug.h"
#include "emu8950.h"
#include "ff.h"
#include "hardware/sync.h"
#include "i_mdopl.h"
#include "i_sound.h"
#if PICO_ON_DEVICE
#include "hardware/regs/addressmap.h"
#endif

#define RING_SIZE 1024u                  /* a power of two */
#define RING_MASK (RING_SIZE - 1u)
#define RING_READ 512u                   /* refill a block at a time */
#define MUS_HEADER 16u
#define MUS_INSTR_LIST_MAX 128u          /* bytes of instrument list read */
#define MUS_TICK_HZ 140u
#define GENMIDI_NAME "GENMIDI.OP2"
#define GENMIDI_MAGIC "#OPL_II#"
#define TICK_STEP ((int32_t)(((uint64_t)OPL_MD_RATE << 16) / MUS_TICK_HZ))
#define QUIET_STOP OPL_MD_RATE           /* a second of release, then rest */
/* The chip's sum of nine voices into the 8-bit mix: about the level
 * upstream sends to its 16-bit DAC. */
#define MUSIC_SHIFT 6

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
static uint32_t s_gen;                   /* bumped when the ring is emptied */
static bool s_eof;                       /* the score is all in the ring */
static bool s_paused;
static bool s_loop;                      /* the playing song goes round */
static bool s_start_req, s_stop_req;
static int s_volume_req = -1;

/* The interrupt's own. */
static bool s_ready;                     /* the chip is set up */
static bool s_playing;                   /* stepping the score */
static bool s_was_paused;
static uint32_t s_delay;                 /* ticks to the next events */
static int32_t s_until_tick;             /* chip samples (16.16) to the next tick */
static uint32_t s_dec_frac;              /* averaging down to the output rate */
static int32_t s_dec_sum;
static uint32_t s_dec_n;
static uint32_t s_quiet;                 /* chip samples since anything sounded */
static uint32_t s_mix_head, s_mix_tail;
static bool s_mix_eof;
static uint8_t s_velocity[16];           /* each channel's last note volume */
static int32_t s_run[OPL_MD_RUN] __scratch_x_noload("oplrun");

/* Game core only. */
static FIL s_file;
static bool s_file_open;
static bool s_looping;
static bool s_pending;                   /* a song to open at the next pump */
static char s_name[9];
static char s_path[48];
static uint32_t s_score_start, s_score_len, s_score_left;

static const char *s_folder = "/doom";

void I_MD_MusicSetFolder(const char *folder) { s_folder = folder; }

/* Whether a song may play: once the m68k has said which sound it has. */
static bool may_play(void) { return audio_get_mode() != AUDIO_MODE_SILENT; }

/* ---- the score, in the interrupt ------------------------------------ */

static int peek(uint32_t k) {
  return (s_mix_head - s_mix_tail > k) ? s_ring[(s_mix_tail + k) & RING_MASK] : -1;
}

/* One MUS event, or false if it is not all in the ring yet. */
static bool mus_event(void) {
  const int b = peek(0);
  if (b < 0) return false;
  const unsigned type = ((unsigned)b >> 4) & 7u, ch = (unsigned)b & 15u;
  uint32_t len = 1;
  if (type == 0 || type == 2 || type == 3) {
    len = 2;
  } else if (type == 1) {
    const int note = peek(1);
    if (note < 0) return false;
    len = (note & 0x80) ? 3 : 2;
  } else if (type == 4) {
    len = 3;
  }
  uint32_t delay = 0, dlen = 0;
  if (b & 0x80) {
    for (;;) {
      const int x = peek(len + dlen);
      if (x < 0) return false;
      dlen++;
      delay = (delay << 7) | ((unsigned)x & 0x7fu);
      if (!(x & 0x80) || dlen == 4) break;
    }
  }
  if (peek(len - 1) < 0) return false;

  const unsigned d0 = (unsigned)peek(1), d1 = (unsigned)peek(2);
  switch (type) {
    case 0: /* release note */
      opl_md_key_off(ch, d0 & 0x7f);
      break;
    case 1: /* play note, with a new volume if the top bit says so */
      if (d0 & 0x80) s_velocity[ch] = (uint8_t)(d1 & 0x7f);
      opl_md_key_on(ch, d0 & 0x7f, s_velocity[ch]);
      break;
    case 2: /* pitch bend, 128 = none */
      opl_md_bend(ch, d0);
      break;
    case 3: /* system: all sounds / all notes off */
      if (d0 == 10 || d0 == 11) opl_md_channel_off(ch);
      break;
    case 4: /* controller: 0 instrument, 3 volume; pan is moot in mono */
      if (d0 == 0) {
        opl_md_program(ch, d1);
      } else if (d0 == 3) {
        opl_md_volume(ch, d1);
      }
      break;
    case 6: /* score end: round again (the ring goes on from the top) or stop */
      if (s_loop) {
        opl_md_restart();
      } else {
        s_playing = false;
        opl_md_all_off();
      }
      break;
    default: /* 5, end of measure; 7, unused */
      break;
  }
  s_mix_tail += len + dlen;
  if (b & 0x80) s_delay = delay;
  if (type == 6 && s_delay == 0) s_delay = 1; /* a tick before the top again */
  return true;
}

/* One MUS tick: the events due now, if they are in the ring. A score
 * the game core has not caught up with waits, and goes on from there. */
static void mus_tick(void) {
  if (s_delay > 0 && --s_delay > 0) return;
  while (s_delay == 0 && s_playing) {
    if (!mus_event()) {
      if (s_mix_eof && s_mix_head == s_mix_tail) {
        s_playing = false; /* the file ended without saying so */
        opl_md_all_off();
      }
      return;
    }
  }
}

/* Add n samples of music at `rate` to mix. Once per VBL, from the sound
 * effects' fill in the audio interrupt (i_mdsound.c). */
void I_MD_MusicMix(int16_t *mix, uint32_t n, uint32_t rate) {
  if (!s_lock || !s_ready || n == 0 || rate == 0 || rate > OPL_MD_RATE) return;
  uint32_t ints = spin_lock_blocking(s_lock);
  const bool start = s_start_req, stop = s_stop_req;
  s_start_req = s_stop_req = false;
  const int volume = s_volume_req;
  s_volume_req = -1;
  const bool paused = s_paused;
  const uint32_t gen = s_gen;
  if (stop) s_playing = false; /* before start: a song can follow another in a frame */
  if (start) s_playing = true;
  if (s_state != MUSIC_PLAYING) s_playing = false;
  s_mix_head = s_head;
  s_mix_tail = s_tail;
  s_mix_eof = s_eof;
  spin_unlock(s_lock, ints);

  if (stop) opl_md_all_off();
  if (volume >= 0) opl_md_music_volume((unsigned)volume);
  if (start) {
    opl_md_start();
    memset(s_velocity, 127, sizeof(s_velocity));
    s_delay = 0;
    s_until_tick = 0;
    s_quiet = 0;
  }
  if (paused && !s_was_paused) opl_md_pause();
  s_was_paused = paused;
  const bool stepping = s_playing && !paused;

  if (!stepping && !opl_md_voices_busy() && s_quiet >= QUIET_STOP) return; /* nothing to hear */

  const uint32_t step = (uint32_t)(((uint64_t)OPL_MD_RATE << 16) / rate);
  uint32_t out = 0, rendered = 0;
  while (out < n) {
    if (stepping && s_playing && s_until_tick <= 0) {
      mus_tick();
      s_until_tick += TICK_STEP;
      continue;
    }
    uint32_t k = (uint32_t)(((uint64_t)(n - out) * step - s_dec_frac + 0xffffu) >> 16);
    if (k > OPL_MD_RUN) k = OPL_MD_RUN;
    if (stepping && s_playing) {
      const uint32_t to_tick = ((uint32_t)s_until_tick + 0xffffu) >> 16;
      if (k > to_tick) k = to_tick;
    }
    if (k == 0) k = 1;
    opl_md_render(s_run, k);
    rendered += k;
    for (uint32_t i = 0; i < k; i++) {
      s_dec_sum += s_run[i];
      s_dec_n++;
      s_dec_frac += 0x10000u;
      if (s_dec_frac >= step) {
        s_dec_frac -= step;
        if (out < n) mix[out++] += (int16_t)((s_dec_sum / (int32_t)s_dec_n) >> MUSIC_SHIFT);
        s_dec_sum = 0;
        s_dec_n = 0;
      }
    }
    if (stepping && s_playing) s_until_tick -= (int32_t)(k << 16);
  }
  if (s_playing || opl_md_voices_busy()) {
    s_quiet = 0;
  } else if (s_quiet < QUIET_STOP) {
    s_quiet += rendered;
  }

  ints = spin_lock_blocking(s_lock);
  if (gen == s_gen) {
    s_tail = s_mix_tail;
    if (!s_playing && s_state == MUSIC_PLAYING && stepping) s_state = MUSIC_ENDED;
  }
  spin_unlock(s_lock, ints);
}

/* ---- the files, on the game core ------------------------------------ */

/* Open s_path and read its header: where the score is and how long, and
 * (into want) the GENMIDI instruments it lists. Leaves the file at the
 * score. */
static bool open_song(uint8_t want[(GENMIDI_INSTRS + 7) / 8]) {
  if (f_open(&s_file, s_path, FA_READ) != FR_OK) return false;
  uint8_t hdr[MUS_HEADER + MUS_INSTR_LIST_MAX];
  UINT got = 0;
  if (f_read(&s_file, hdr, MUS_HEADER, &got) != FR_OK || got != MUS_HEADER ||
      memcmp(hdr, "MUS\x1a", 4) != 0) {
    goto bad;
  }
  s_score_len = (uint32_t)hdr[4] | (uint32_t)hdr[5] << 8;
  s_score_start = (uint32_t)hdr[6] | (uint32_t)hdr[7] << 8;
  const uint32_t ninstr = (uint32_t)hdr[12] | (uint32_t)hdr[13] << 8;
  const uint32_t list = s_score_start - MUS_HEADER;
  if (s_score_start < MUS_HEADER + 2u * ninstr || list > MUS_INSTR_LIST_MAX) goto bad;
  if (list && (f_read(&s_file, hdr + MUS_HEADER, list, &got) != FR_OK || got != list)) goto bad;
  if (want) {
    want[0] |= 1u; /* a channel's instrument until the song picks one */
    for (uint32_t i = 0; i < ninstr; i++) {
      unsigned id = hdr[MUS_HEADER + 2u * i] | hdr[MUS_HEADER + 2u * i + 1u] << 8;
      if (id >= 135u && id <= 181u) id -= 7u; /* percussion, key + 100: GENMIDI's 128.. */
      if (id < GENMIDI_INSTRS) want[id >> 3] |= (uint8_t)(1u << (id & 7u));
    }
  }
  s_score_left = s_score_len;
  return true;
bad:
  f_close(&s_file);
  return false;
}

/* The instruments in want, from GENMIDI.OP2 beside the songs. Read
 * through rather than sought in: 6 KB, once a song. */
static bool load_instruments(const uint8_t want[(GENMIDI_INSTRS + 7) / 8]) {
  char path[48];
  snprintf(path, sizeof(path), "%s/" GENMIDI_NAME, s_folder);
  if (f_open(&s_file, path, FA_READ) != FR_OK) return false;
  uint8_t rec[GENMIDI_INSTR_BYTES];
  UINT got = 0;
  bool ok = f_read(&s_file, rec, 8, &got) == FR_OK && got == 8 && memcmp(rec, GENMIDI_MAGIC, 8) == 0;
  opl_md_clear_instruments();
  for (unsigned i = 0; ok && i < GENMIDI_INSTRS; i++) {
    ok = f_read(&s_file, rec, sizeof(rec), &got) == FR_OK && got == sizeof(rec);
    if (ok && (want[i >> 3] & (1u << (i & 7u)))) opl_md_store_instrument(i, rec);
  }
  f_close(&s_file);
  return ok;
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
    uint32_t want = (RING_SIZE - at < RING_READ) ? RING_SIZE - at : RING_READ;
    if (want > s_score_left) want = s_score_left;
    if (want == 0) {
      /* Round again from the top (reopened, which costs less flash
       * than f_lseek). */
      f_close(&s_file);
      if (s_looping && open_song(NULL)) continue;
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
    s_score_left -= got;
    ints = spin_lock_blocking(s_lock);
    s_head += got;
    spin_unlock(s_lock, ints);
  }
}

/* Open s_path: its instruments, then its score. */
static bool start_song(void) {
  uint8_t want[(GENMIDI_INSTRS + 7) / 8] = {0};
  if (!open_song(want)) return false;
  f_close(&s_file);
  if (!load_instruments(want)) {
    DPRINTF("music: no %s/" GENMIDI_NAME "\n", s_folder);
    return false;
  }
  if (!open_song(NULL)) return false;
  s_file_open = true;
  fill_ring();
  const uint32_t ints = spin_lock_blocking(s_lock);
  s_loop = s_looping;
  s_paused = false;
  s_start_req = true;
  s_state = MUSIC_PLAYING;
  spin_unlock(s_lock, ints);
  return true;
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
  if (!start_song()) {
    DPRINTF("music: no %s\n", s_path); /* or not a MUS file */
    return;
  }
  DPRINTF("music: %s%s\n", s_path, s_looping ? ", looping" : "");
}

static void stop_song(void) {
  s_pending = false;
  const uint32_t ints = spin_lock_blocking(s_lock);
  s_state = MUSIC_STOPPED;
  s_head = s_tail = 0;
  s_gen++;
  s_eof = false;
  s_stop_req = true;
  s_start_req = false;
  spin_unlock(s_lock, ints);
  if (s_file_open) {
    f_close(&s_file);
    s_file_open = false;
  }
}

/* Stop the song before a pack is programmed. The audio timer is stopped
 * for that, which silences the stream, so there is nothing to wait for. */
void I_MD_MusicWaitSilent(void) {
  if (s_lock) stop_song();
}

/* The music module. RegisterSong is handed the lump's name ("d_e1m1")
 * rather than its data: the level packs carry no music (see
 * S_ChangeMusic). */

static boolean MD_InitMusic(void) {
  s_lock = spin_lock_instance(PICO_SPINLOCK_ID_OS1); /* the sfx mixer's: same interrupt */
  s_ready = opl_md_init();
  if (!s_ready) DPRINTF("music: no memory for the OPL\n");
  return true;
}

static void MD_ShutdownMusic(void) { stop_song(); }

static void MD_SetMusicVolume(int volume) {
  const uint32_t ints = spin_lock_blocking(s_lock);
  s_volume_req = volume < 0 ? 0 : volume > 127 ? 127 : volume;
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
  snprintf(s_path, sizeof(s_path), "%s/%s.MUS", s_folder, s_name);
  for (char *p = s_path + strlen(s_folder); *p; p++) {
    if (*p >= 'a' && *p <= 'z') *p = (char)(*p - 'a' + 'A');
  }
  s_pending = true;
}

static void MD_PlaySong(void *handle, boolean looping) {
  (void)handle;
  stop_song();
  s_looping = looping;
  if (may_play()) request_song();
}

static bool song_playing(void) {
  const uint32_t ints = spin_lock_blocking(s_lock);
  const bool playing = s_state == MUSIC_PLAYING;
  spin_unlock(s_lock, ints);
  return playing;
}

/* For the loading screen: whether this machine would play music but the
 * map's song (D_E1M2.MUS for "E1M2") or the instruments are not on the
 * card -- level data from before there was music. Game core only. */
bool I_MD_MusicFileMissing(const char *map) {
  if (!may_play()) return false;
  char path[48];
  snprintf(path, sizeof(path), "%s/D_%s.MUS", s_folder, map);
  if (f_stat(path, NULL) != FR_OK) return true;
  snprintf(path, sizeof(path), "%s/" GENMIDI_NAME, s_folder);
  return f_stat(path, NULL) != FR_OK;
}

static void MD_StopSong(void) { stop_song(); }

static boolean MD_MusicIsPlaying(void) { return s_pending || song_playing(); }

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
