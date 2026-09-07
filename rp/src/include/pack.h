/*
 * Copyright (C) 2026 Neil Rackett
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * File: pack.h
 * Description: Level asset packs -- SD card file -> the PACK_FLASH window.
 *
 * The microfirmware slot is far too small for the whole shareware WHX
 * (1758 KB against 1152 KB for code and data together), so MD/DOOM
 * plays from per-level packs: one WHX per map holding that map plus
 * only the sprites, textures, flats and sounds it uses, built offline
 * by tools/levelpack.py. When a level starts, its pack is copied from
 * the SD card into the PACK_FLASH window (memmap_rp.ld) and the engine
 * reads it in place over XIP, the same way MD/Lynx programs a
 * cartridge image into flash at load time.
 *
 * Programming needs XIP to stay live for the SD reads, so the erase and
 * each 32 KB chunk are bracketed by interrupts-off + Core 1 parked in
 * RAM, with fb_chunked_buffer borrowed as the staging area -- so the
 * display holds whatever frame was last published (the "LOADING"
 * screen) while this runs.
 */

#ifndef PACK_H_INCLUDED
#define PACK_H_INCLUDED

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Progress callback: bytes done of total. Called between chunks, from
 * the main loop context, with interrupts on and XIP live -- it may draw
 * and publish a frame. */
typedef void (*pack_progress_cb_t)(uint32_t done, uint32_t total);

/* Program the file at `path` (relative to the mounted SD root) into the
 * PACK_FLASH window. Fails without touching flash if the file is missing
 * or larger than the window. The window's previous contents are gone
 * either way once programming starts. */
bool pack_load(const char *path, pack_progress_cb_t progress);

/* Start of the window in the XIP address space, and its size. */
const uint8_t *pack_base(void);
uint32_t pack_capacity(void);

/* Bytes programmed by the last successful pack_load(), 0 if none. */
uint32_t pack_loaded_size(void);

/* True if the window currently starts with the WHX magic ("IWHX"), i.e.
 * a pack survived in flash from a previous run. */
bool pack_present(void);

#ifdef __cplusplus
}
#endif

#endif /* PACK_H_INCLUDED */
