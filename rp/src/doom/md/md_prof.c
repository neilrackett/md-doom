/*
 * Copyright (C) 2026 Neil Rackett
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * File: md_prof.c
 * Description: See md_prof.h.
 */
#include "md_prof.h"

#if defined(_DEBUG) && (_DEBUG != 0)
#include <stdio.h>

#include "pico/time.h"

static uint32_t s_start[MD_PROF_COUNT];
static uint32_t s_max[MD_PROF_COUNT];
static const char *const s_names[MD_PROF_COUNT] = {
    "tics", "render", "pre", "cols", "join", "overlay", "present"};

void md_prof_begin(int phase) { s_start[phase] = time_us_32(); }

void md_prof_end(int phase) {
  const uint32_t dt = time_us_32() - s_start[phase];
  if (dt > s_max[phase]) s_max[phase] = dt;
}

void md_prof_report(char *buf, int len) {
  int n = 0;
  for (int i = 0; i < MD_PROF_COUNT && n < len; i++) {
    n += snprintf(buf + n, (size_t)(len - n), "%s %lu ", s_names[i],
                  (unsigned long)((s_max[i] + 500u) / 1000u));
    s_max[i] = 0;
  }
}
#endif
