/*
 * Copyright (C) 2026 Neil Rackett
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Based on rp2040-doom's pico/i_system.c:
 * Copyright(C) 1993-1996 Id Software, Inc.
 * Copyright(C) 2005-2014 Simon Howard
 * Copyright(C) 2021-2022 Graham Sanderson
 * GPL-2.0-or-later.
 *
 * File: i_system.c
 * Description: Doom's system interface on the MD framework: the zone
 *              heap, malloc routed into it, exit and error handling.
 */

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "pico.h"
#include "pico/stdlib.h"

#include "config.h"
#include "d_event.h"
#include "deh_str.h"
#include "doomkeys.h"
#include "doomtype.h"
#include "i_input.h"
#include "i_sound.h"
#include "i_system.h"
#include "i_timer.h"
#include "i_video.h"
#include "m_argv.h"
#include "m_config.h"
#include "m_misc.h"
#include "w_wad.h"
#include "z_zone.h"

#include "debug.h"
#include "fb.h"
#include "ikbd.h"

extern void I_InputInit(void);

/* Nothing runs at exit: I_Quit hands the machine back to GEM and the
 * cartridge idles, so there is no list to keep. */
void I_AtExit(atexit_func_t func, boolean run_on_error) {
  (void)func;
  (void)run_on_error;
}

void I_Tactile(int on, int off, int total) {
  (void)on; (void)off; (void)total;
}

/* ------------------------------------------------------------------ */
/* Zone heap                                                           */

/* The zone takes everything between the C heap's current break and the
 * top of RAM, minus a margin for what newlib still hands out afterwards
 * (FatFs opens a level pack with a ~2 KB transient buffer). Nothing
 * stops newlib growing past that margin, so keep boot-time allocations
 * before this point and the margin honest. */
#define ZONE_HEAP_MARGIN 4096u

static uint8_t *s_zone_base;
static uint8_t *s_zone_end;

byte *I_ZoneBase(int *size) {
  extern char __StackLimit;
  uintptr_t brk = (uintptr_t)sbrk(0);
  brk = (brk + ZONE_HEAP_MARGIN + 7u) & ~(uintptr_t)7u;
  s_zone_base = (uint8_t *)brk;
  s_zone_end = (uint8_t *)&__StackLimit;
  *size = (int)(s_zone_end - s_zone_base);
  DPRINTF("zone: %p .. %p (%d bytes)\n", s_zone_base, s_zone_end, *size);
  return s_zone_base;
}

/* malloc/calloc/realloc/free routed into the zone once it exists, so the
 * engine's own allocations (and everything newlib does after boot) come
 * out of the one pool. Pointers from before the zone existed -- the
 * settings library, stdio -- are handed back to newlib. */
extern void *__real_malloc(size_t size);
extern void *__real_calloc(size_t count, size_t size);
extern void *__real_realloc(void *ptr, size_t size);
extern void __real_free(void *ptr);

static inline bool in_zone(const void *p) {
  return s_zone_base && (const uint8_t *)p >= s_zone_base &&
         (const uint8_t *)p < s_zone_end;
}

void *__wrap_malloc(size_t size) {
  if (!s_zone_base) return __real_malloc(size);
  return Z_MallocNoUser((int)size, PU_STATIC);
}

void *__wrap_calloc(size_t count, size_t size) {
  if (!s_zone_base) return __real_calloc(count, size);
  void *rc = Z_MallocNoUser((int)(count * size), PU_STATIC);
  if (rc) memset(rc, 0, count * size);
  return rc;
}

void *__wrap_realloc(void *ptr, size_t size) {
  if (!s_zone_base || (ptr && !in_zone(ptr))) return __real_realloc(ptr, size);
  if (!ptr) return Z_MallocNoUser((int)size, PU_STATIC);
  if (!size) {
    Z_Free(ptr);
    return NULL;
  }
  /* The zone has no realloc: allocate, copy what fits, free. Rare here
   * (I_Realloc is only used by the WAD loader upstream). */
  void *n = Z_MallocNoUser((int)size, PU_STATIC);
  if (n) {
    memcpy(n, ptr, size);
    Z_Free(ptr);
  }
  return n;
}

void __wrap_free(void *ptr) {
  if (!ptr) return;
  if (in_zone(ptr)) {
    Z_Free(ptr);
  } else {
    __real_free(ptr);
  }
}

/* ------------------------------------------------------------------ */

void I_PrintBanner(const char *msg) { DPRINTF("%s\n", msg); }
void I_PrintDivider(void) {}
void I_PrintStartupBanner(const char *gamedescription) {
  DPRINTF("%s\n", gamedescription);
}

void I_Init(void) { I_InputInit(); }

/* Quit: tell the m68k to return to GEM and idle. Nothing on the RP side
 * needs tearing down -- the ST takes its screen back and the cartridge
 * keeps serving the (now static) framebuffer. */
void __attribute__((noreturn)) I_Quit(void) {
  DPRINTF("I_Quit: back to GEM\n");
  for (;;) {
    ikbd_request_boot_gem();
    fb_pump_rom3();
    sleep_ms(20);
  }
}

#if !NO_IERROR
void I_Error(const char *error, ...) {
  va_list argptr;
  va_start(argptr, error);
#if defined(_DEBUG) && (_DEBUG != 0)
  vfprintf(stderr, error, argptr);
  fputc('\n', stderr);
#endif
  va_end(argptr);
  /* Fatal: there is no console on the ST to show it on yet. Hand the
   * machine back to GEM rather than hanging with a frozen picture. */
  I_Quit();
}
#endif

void *I_Realloc(void *ptr, size_t size) {
  void *new_ptr = realloc(ptr, size);
  if (size != 0 && new_ptr == NULL) {
    I_Error("I_Realloc: failed on reallocation of %" PRIuPTR " bytes", size);
  }
  return new_ptr;
}

/* DOS null-pointer dereference emulation, as in upstream. */
#define DOS_MEM_DUMP_SIZE 10
static const unsigned char mem_dump_dos622[DOS_MEM_DUMP_SIZE] = {
    0x57, 0x92, 0x19, 0x00, 0xF4, 0x06, 0x70, 0x00, 0x16, 0x00};

boolean I_GetMemoryValue(unsigned int offset, void *value, int size) {
  const unsigned char *d = mem_dump_dos622;
  switch (size) {
    case 1:
      *((unsigned char *)value) = d[offset];
      return true;
    case 2:
      *((unsigned short *)value) = d[offset] | (d[offset + 1] << 8);
      return true;
    case 4:
      *((unsigned int *)value) = d[offset] | (d[offset + 1] << 8) |
                                 (d[offset + 2] << 16) | (d[offset + 3] << 24);
      return true;
  }
  return false;
}
