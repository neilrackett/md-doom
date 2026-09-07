/*
 * Copyright (C) 2026 Neil Rackett
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * File: md_prof.h
 * Description: Debug-build frame profiler: the longest time each phase
 *              of a frame took since the last report. Compiled to nothing
 *              in release builds.
 */
#ifndef MD_PROF_H
#define MD_PROF_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
  MD_PROF_TICS,     /* TryRunTics: game logic                          */
  MD_PROF_RENDER,   /* pd_begin_frame .. pd_end_frame: BSP, column lists */
  MD_PROF_PRE,      /* pd_end_frame up to draw_regular_columns          */
  MD_PROF_COLS,     /* draw_regular_columns on Core 0                   */
  MD_PROF_JOIN,     /* waiting for Core 1                               */
  MD_PROF_OVERLAY,  /* fuzz, HUD, menu lists                            */
  MD_PROF_PRESENT,  /* I_MD_PresentFrame: overlays, palette, c2p        */
  MD_PROF_COUNT
};

#if defined(_DEBUG) && (_DEBUG != 0)
void md_prof_begin(int phase);
void md_prof_end(int phase);
/* Fill `buf` with "tics 12 render 30 ..." (max ms per phase) and reset. */
void md_prof_report(char *buf, int len);
#else
#define md_prof_begin(p) ((void)0)
#define md_prof_end(p) ((void)0)
#define md_prof_report(b, l) ((void)0)
#endif

#ifdef __cplusplus
}
#endif
#endif
