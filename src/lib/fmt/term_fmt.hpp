/// @file src/lib/fmt/term_fmt.hpp
/// @brief Unified terminal output core (issue #286): freestanding-safe
/// formatter + fixed-width table helpers + redraw discipline, usable from
/// RING-0 and RING-3. No STL, no exceptions, no heap, no syscalls;
/// caller-provided buffers only, bounded loops, fail-closed truncation.
#pragma once

#include <types.hpp>

namespace fmt {

/// @brief Canonical terminal width all tables are disciplined to.
static constexpr size_t kTermWidth = 80;
/// @brief Upper bound for parsed field widths (`%12d`).
static constexpr size_t kMaxWidth = 128;
/// @brief Upper bound for parsed precisions (`%.40s`).
static constexpr size_t kMaxPrecision = 128;
/// @brief Default scratch buffer capacity for one formatted line.
static constexpr size_t kFmtBufCap = 256;
/// @brief Minimum frame interval for top-style redraw loops (ms).
static constexpr unsigned long kTopMinFrameMs = 250;

/// @brief va_list spelling for the freestanding kernel (mirrors the
/// `__va_list` idiom in src/lib/logger.hpp; no <cstdarg> freestanding).
using VaList = __builtin_va_list;

/// @brief Format into `out` (issue #286 contract, frozen by owner).
/// Conversions: `%c %s %d %i %u %x %X %p %%`, flags `-`/`0`, decimal
/// field width, `.N` precision for `%s` only. Lengths: none = 32 bit,
/// `l`/`ll` = 64 bit (LP64), `z` = 64 bit, `hh`/`h` ignored (default
/// promotion applies). `%X` renders lowercase hex (in-tree libc quirk,
/// kept for a single shared contract). Unknown specifiers are copied
/// literally (fail-closed); overlong width/precision is clamped.
/// @return Needed length excluding NUL (vsnprintf-compatible); output is
/// truncated to `cap - 1` chars + NUL when `cap > 0`, untouched when 0.
int mini_vsnprintf(char *out, size_t cap, const char *fmt, VaList args);

/// @brief Variadic front-end over mini_vsnprintf (freestanding builtins,
/// no <cstdarg>).
inline int mini_snprintf(char *out, size_t cap, const char *fmt, ...) {
    __builtin_va_list args;
    __builtin_va_start(args, fmt);
    int ret = mini_vsnprintf(out, cap, fmt, args);
    __builtin_va_end(args);
    return ret;
}

/// @brief Char sink for streaming helpers (e.g. Terminal::putchar).
using Emit = void (*)(char);

/// @brief Emit one fixed-width field (truncate-then-pad with spaces).
/// `width` is clamped to `kTermWidth`; `right` selects alignment.
void emit_field(Emit emit, const char *text, size_t width, bool right);

/// @brief Emit a separator rule of exactly `min(count, kTermWidth)` `ch`.
void emit_separator(Emit emit, char ch, size_t count);

/// @brief Top-style redraw gate (issue #286 buffers discipline): true at
/// most once per `interval_ms` of the caller-provided monotonic `now_ms`
/// clock; updates `last_ms` on fire. Pure function, no clock coupling.
bool frame_due(unsigned long now_ms, unsigned long &last_ms,
               unsigned long interval_ms);

} // namespace fmt
