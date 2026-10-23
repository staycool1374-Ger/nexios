# Terminal Output Structure (terminal-output) — issue #286

Unified formatter + table/screen contracts for RING-0 and RING-3.
Implementation: `src/lib/fmt/term_fmt.{hpp,cpp}` (freestanding, no heap,
no syscalls). First consumer: kernel-shell `top` rendering
(`src/services/shell.cpp`); `Terminal::write/putchar` paths unchanged.

## Formatter (`fmt::mini_vsnprintf`, vsnprintf-compatible needed-length)

- Conversions: `%c %s %d %i %u %x %X %p %%`. Flags `-`/`0`, decimal
  field width, `.N` precision for `%s` only.
- Lengths (LP64): none = 32 bit, `l`/`ll` = 64 bit (`ll` collapses),
  `z` = 64 bit, `hh`/`h` ignored (promotion applies).
- `%X` renders lowercase hex (in-tree libc quirk, kept for one contract).
- Fail-closed: unknown specifiers copy literally (`%q` → `%q`),
  width/precision clamp to `kMaxWidth`/`kMaxPrecision` (128),
  truncation to `cap - 1` + NUL, `INT64_MIN`-safe magnitudes.
- Deltas vs in-tree libc (`src/libc/stdio.h`): libc parses-but-ignores
  width/`0` and lacks `%z` (literal passthrough); `term_fmt` applies
  width/`0` and accepts `%z` as 64-bit. Widths, `ll`-collapse and
  `%X`-lowercase match on both sides.

## Tables (80-col discipline, `kTermWidth = 80`)

- `emit_field(sink, text, width, right)`: truncate-then-pad with spaces;
  width clamped to 80. `emit_separator(ch, count)`: rule clamped to 80.
- Tables compose fixed-width columns; the task table keeps a 77-char
  row with NAME as the flexible column (shrinks below 77-col terms,
  identity at 11 above). No row may exceed 80 printable chars.

## Screens/buffers (`top` handling)

- `frame_due(now, last, interval)` gates redraws (default
  `kTopMinFrameMs = 250`) on a caller clock — no per-frame full `clear()`
  on slow paths (framebuffer is UC MMIO, serial is busy-wait).
- Rendered task rows are bounded by `Terminal::rows()` when known
  (no-op at 0 or ample height): no scroll-spam on short screens.

## Non-goals

No ANSI parser/emitter, no scrollback buffer, no Logger/dmesg
migration, no `userspace/top.c` rewrite (24-line stub stays).
Async (URC) console discipline lives in `docs/specs/console-urc.md`
(#407 item 2), not here.
