# Console URC discipline (console-urc) — issue #407 item 2

Unsolicited result codes: background producers enqueue short text lines;
only the shell task pops and prints them at prompt redraw. Async output
never interleaves a typed line. Implementation: `src/kernel/console/urc.{hpp,cpp}`.

## Ownership

- Single console writer for URC text: producers only `emit()`; only the
  shell task `pop()`s and prints. No producer calls `Logger`/`Terminal`
  from inside the queue critical section.
- Prefix registry: compile-time tags `ELF`/`KILL`/`TST` (`Tag::Elf/Kill/Test`);
  late producers `register_prefix()` before `seal()` (shell boot, idempotent).
  Post-seal registration is a counted reject.

## Contracts

- `emit(tag, line)`: ISR-safe (`IrqSpinLockGuard`), bounded copy (≤122 B),
  freezes `"[PREFIX] payload"` at enqueue (truncated with `...`). Full
  queue drops newest + counts; unknown tag / null line are counted rejects.
  Never ENSURE, never blocks, never allocates.
- `pop(out, cap)`: copies out under the guard; printing happens unlocked.
- Drain budget: `kDrainMaxPerPass = 8` per prompt pass; overflow notice
  `"[URC] N dropped"` (coalesced via `take_dropped()`).

## Ordering and drop policy

One global FIFO; per-producer order preserved; drop-newest (the queued
prefix of history survives). Solicited output (command context) never
enters the queue — URCs print only between input lines.

## Redraw protocol (readline idle branch)

Drain (`flush_urcs()`: separator iff mid-line → drop notice → lines) then,
iff anything printed or the legacy serial guard fired, reprint prompt +
typed buffer (`redraw_after_async()`) and re-snapshot `prompt_serial`.
The legacy `Serial::write_count()` guard stays for direct serial writers;
it is x86-blind (raw COM1 bypass), so the queue is the reliable detector.

## Lock order

`scheduler_lock_ -> urc lock`; the urc lock is a leaf (bounded
`__builtin_memcpy` + two index stores, no calls out). The reap path
(`note_termination_and_free`, up to 64 emissions per pass) widens the
IRQ-off window by a bounded sub-µs copy per emission.

## Arch notes

TX-only discipline: no IER/IMSC change, no new IRQ source, riscv64 stays
poll-fed. Generic module, compiled on all three arches (no HAL edits).
URC lines are output-only and survive the prompt-time `serial_input::reset()`
(input reset must never discard queued output).

## Non-goals

Logger console sink, panic/fault paths (stay immediate), shell capture
buffer, ANSI handling. Machine record for task death: dmesg `TASK+17`
(`docs/specs/dmesg.md` §4); user-fault (`kernel.cpp`) and integrity-kill
paths stay immediate by design.
