/*
 * Copyright (C) 2026 Neil Rackett
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * File: framedump.c
 * Description: Doom's frames as the renderer left them, for offline
 *              palette experiments (tools/bandpal.py). With
 *              MDDOOM_FRAMEDUMP=DIR in the environment, every
 *              MDDOOM_FRAMEDUMP_EVERY-th published frame (default 25) is
 *              written to DIR as frame_NNNNN.bin: the 768-byte PLAYPAL
 *              page in force, then the 320x200 frame of PLAYPAL indices.
 *              Called from doom_video_publish in EmuMD builds only.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

void emu_frame_dump(const uint8_t *frame, const uint8_t *playpal) {
  static const char *dir;
  static unsigned every, count, written;
  static int checked;
  if (!checked) {
    checked = 1;
    dir = getenv("MDDOOM_FRAMEDUMP");
    const char *e = getenv("MDDOOM_FRAMEDUMP_EVERY");
    every = e ? (unsigned)atoi(e) : 25u;
    if (!every) every = 1;
  }
  if (!dir || (count++ % every) != 0) return;

  char path[1024];
  snprintf(path, sizeof(path), "%s/frame_%05u.bin", dir, written++);
  FILE *f = fopen(path, "wb");
  if (!f) return;
  fwrite(playpal, 1, 768, f);
  fwrite(frame, 1, 320 * 200, f);
  fclose(f);
}
