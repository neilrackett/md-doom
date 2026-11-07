/*
 * Copyright (C) 2026 Neil Rackett
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * File: i_mdopl.h
 * Description: The DMX OPL2 music driver on emu8950 (i_mdopl.c), driven
 *              by i_mdmusic.c. Everything but opl_md_init and the
 *              instrument loading runs in the audio interrupt on Core 1.
 */
#ifndef I_MDOPL_H
#define I_MDOPL_H

#include <stdbool.h>
#include <stdint.h>

#define OPL_MD_RATE 49716u     /* the chip's own rate: 3,579,545 Hz / 72 */
#define OPL_MD_PERCUSSION 15u  /* MUS's percussion channel */
#define GENMIDI_INSTRS 175u    /* 128 melodic, then 47 percussion (keys 35..81) */
#define GENMIDI_INSTR_BYTES 36u

bool opl_md_init(void);

/* Instruments, from the GENMIDI lump: cleared and stored at song start,
 * by the game core while no song is playing. Index 0..174. */
void opl_md_clear_instruments(void);
bool opl_md_store_instrument(unsigned index, const uint8_t rec[GENMIDI_INSTR_BYTES]);

/* The audio interrupt's. Channels are MUS channels, 15 percussion. */
void opl_md_start(void);       /* every voice off, channels as new */
void opl_md_restart(void);     /* channels as new (a song going round) */
void opl_md_all_off(void);
void opl_md_channel_off(unsigned ch);
void opl_md_key_on(unsigned ch, unsigned key, unsigned volume);
void opl_md_key_off(unsigned ch, unsigned key);
void opl_md_program(unsigned ch, unsigned program);
void opl_md_volume(unsigned ch, unsigned volume);
void opl_md_bend(unsigned ch, unsigned mus_bend);
void opl_md_pause(void);       /* key off all but percussion, as DMX */
void opl_md_music_volume(unsigned volume); /* 0..127 */
bool opl_md_voices_busy(void);
void opl_md_render(int32_t *buf, uint32_t n); /* n <= OPL_MD_RUN */

#endif
