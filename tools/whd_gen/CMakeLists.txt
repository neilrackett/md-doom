# Copyright (C) 2026 Neil Rackett
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Host build of rp2040-doom's whd_gen (the WAD -> WHX converter) on its
# own, without the SDL2 dependencies the upstream top-level CMake pulls in
# for the whole of Chocolate Doom. Point RP2040_DOOM_SRC at the `src`
# directory of a kilograham/rp2040-doom checkout:
#
#   cmake -S tools/whd_gen -B build/whd_gen -DRP2040_DOOM_SRC=/path/to/rp2040-doom/src
#   cmake --build build/whd_gen
#
# Use a Release build (the default here): whd_gen deliberately lowers its
# ADPCM search quality in debug builds.
cmake_minimum_required(VERSION 3.13)
project(whd_gen_host C CXX)
set(CMAKE_CXX_STANDARD 14)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()
set(RP2040_DOOM_SRC "" CACHE PATH "rp2040-doom src directory")
if(NOT RP2040_DOOM_SRC)
  message(FATAL_ERROR "Set -DRP2040_DOOM_SRC=/path/to/rp2040-doom/src")
endif()
set(SRC ${RP2040_DOOM_SRC})
add_executable(whd_gen
  ${SRC}/whd_gen/whd_gen.cpp ${SRC}/whd_gen/wad.cpp ${SRC}/whd_gen/mus2seq.cpp
  ${SRC}/whd_gen/huff.cpp ${SRC}/whd_gen/lodepng.cpp ${SRC}/whd_gen/compress_mus.cpp
  ${SRC}/tiny_huff.c ${SRC}/musx_decoder.c ${SRC}/image_decoder.c ${SRC}/adpcm-xq/adpcm-lib.c)
target_compile_definitions(whd_gen PRIVATE IS_WHD_GEN=1)
# Recent clang rejects upstream's `x.template f()` on non-templates; it is
# harmless, so silence it (and the deprecation noise) rather than patch
# the checkout.
target_compile_options(whd_gen PRIVATE -w -Wno-missing-template-arg-list-after-template-kw)
target_include_directories(whd_gen PRIVATE ${SRC}/whd_gen ${SRC} ${SRC}/doom ${SRC}/adpcm-xq)
