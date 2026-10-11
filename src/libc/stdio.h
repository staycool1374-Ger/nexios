/*
 * NexIOS RTOS — Development Roadmap / Kernel Core
 * Copyright (C) 2026 Arnold Hasshold
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#pragma once

#include <sys/types.h>
#include <stdarg.h>

enum {
    _IOFBF = 0,
    _IOLBF = 1,
    _IONBF = 2,
};

int printf(const char* fmt, ...);
int sprintf(char* buf, const char* fmt, ...);
int snprintf(char* buf, size_t n, const char* fmt, ...);
/// @brief Bounded format into buf (issue #316 contract, verified by
/// userspace/printf-probe.c vectors P01-P16 on target).
/// Width contract (LP64, no exceptions): every conversion consumes EXACTLY
/// its declared width — `l`-flagged (`%lu`/`%lx`/`%ld`) reads 8 bytes,
/// unflagged (`%d`/`%u`/`%x`/`%c`) reads 4 bytes. Passing a 32-bit value
/// to `%lu` (or vice versa) desyncs va_arg stepping and garbles that AND
/// all following conversions. `ll` collapses to `l` (identical width on
/// LP64); there is no `z` length (`%zu` falls through to literal
/// passthrough).
/// Cosmetic notes: field width/`0` flags are parsed but not applied;
/// `%X` renders lowercase (same path as `%x`); `printf` truncates lines
/// at 255 bytes (single 256 B stack buffer, one write() call).
/// Shared contract (issue #286, `src/lib/fmt/term_fmt.hpp`): the RING-0
/// `fmt::mini_vsnprintf` implements this same conversion set with two
/// documented deltas — field width/`0` ARE applied there, and `%z` is
/// accepted as 64-bit (falls through to literal passthrough here).
/// LP64 widths, `ll`-collapses-`l`, `%X`-lowercase match on both sides.
int vsnprintf(char* buf, size_t n, const char* fmt, va_list ap);
int puts(const char* s);
int putchar(int c);
int getchar(void);
