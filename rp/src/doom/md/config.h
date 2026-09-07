/*
 * Copyright (C) 2026 Neil Rackett
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * File: config.h
 * Description: The config.h the Chocolate Doom build normally generates
 *              from CMake, written out by hand for MD/DOOM.
 */

#ifndef MDDOOM_CONFIG_H
#define MDDOOM_CONFIG_H

#define PACKAGE_NAME "MD/DOOM"
#define PACKAGE_TARNAME "md-doom"
#define PACKAGE_VERSION RELEASE_VERSION
#define PACKAGE_STRING "MD/DOOM " RELEASE_VERSION
#define PROGRAM_PREFIX "md-"

#define HAVE_DECL_STRCASECMP 1
#define HAVE_DECL_STRNCASECMP 1
#define HAVE_MMAP 1

#endif
