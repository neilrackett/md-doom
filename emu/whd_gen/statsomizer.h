/*
 * Copyright (C) 2026 Neil Rackett
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * File: statsomizer.h
 * Description: The statistics rp2040-doom's renderer keeps when built for
 *              a host (pd_render.cpp includes ../whd_gen/statsomizer.h,
 *              found through emu/shim), kept to nothing: MD/DOOM does not
 *              report them.
 */

#ifndef STATSOMIZER_H
#define STATSOMIZER_H

#include <set>

struct statsomizer {
  explicit statsomizer(const char *name) { (void)name; }
  void record(int value) { (void)value; }
};

#endif
