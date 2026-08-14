# Background ELF Loader — Spec (ROADMAP 0.4.2/0.4.3 precursor)

**Doc ID:** NEX-SPEC-2026-08-14-001
**Status:** DRAFT → IMPLEMENTED + SIL 3 APPROVED (2026-08-14)
**Branch:** main, x86_64 primary

## 1. Overview & Goals

The system's future purpose is to load a general ELF file from the filesystem
and run it under NexIOS control. Loading a large ELF as one synchronous pass
(`kernel::elf::load`) holds the CPU for too long, violating deadline criteria
for the daemons and the deadline monitor. The fix: split loading into small
chunks executed by a **low-priority background task** that:

- is **preemptible** at any chunk boundary (higher-priority tasks run between
  chunks);
- is **cancellable** by a shell command (`cancel-load`) that performs all
  cleanup and reports to shell + dmesg;
- runs to completion in the background if not canceled, reporting
  `loading <name> <size> completed in <ticks>` to shell + dmesg;
- reports a meaningful error (shell + dmesg) and frees all partial state on
  failure — identical cleanup to cancel;
- has a future `runelf` hook: the completed TCB is retained by the loader
  singleton and can be made schedulable at a settled priority (NOT implemented
  in this milestone, but the state machine makes it reachable).

## 2. Architecture Decision

A **fixed kernel task** `elf_loader_task_main` (created at boot by
`ElfLoader::ensure_task()`) owns a singleton `ElfLoader` state object. The
shell task only sets request parameters + flags and posts a semaphore; the
loader task performs all chunk work and is the **single owner of all resource
cleanup**.

Why a kernel task, not `create_user()`: user tasks run with a user-mode
yield stub and cannot execute kernel C++ (VFS/PMM/VMM). The loader must call
`VnodeOps::read`, `PMM::alloc_user_page`, `VMM::map_page_in_pml4` — kernel
paths. The shell itself is the precedent (`create(shell_task_main, 2, 0)`,
kernel.cpp:309-310).

Why a fixed singleton task instead of a per-load spawned task: the shell
`load` command must return immediately; a persistent low-priority task that
blocks on a wake semaphore when idle costs zero CPU, and per-load state lives
in the singleton (survives across commands without IPC message passing).

**Priority:** the loader runs at **priority 1** — LOWER urgency than the
shell (2), so shell input/output always preempts it; higher urgency than init
background (0). It is aperiodic (`period_ticks = 0`), so the deadline monitor
skips it (`scan_deadlines` skips `period_ticks == 0`; `add_task` only inserts
`period_ticks > 0` tasks into the deadline list). It yields with
`Scheduler::reschedule()` after every chunk, letting the timer dispatch any
higher-priority task (daemons, deadline monitor, shell) between chunks.

## 3. State Machine

```
IDLE --request_load (shell)--> VALIDATING
VALIDATING --ok--> COPYING_SEGMENTS
VALIDATING --fail/read-error--> FAILED
VALIDATING --cancel observed--> CANCELED
COPYING_SEGMENTS --chunk done, more--> COPYING_SEGMENTS
COPYING_SEGMENTS --all segments--> MAPPING
COPYING_SEGMENTS --fail/OOM/read-error--> FAILED
COPYING_SEGMENTS --cancel observed--> CANCELED
MAPPING --ok--> DONE
MAPPING --fail/OOM--> FAILED
MAPPING --cancel observed (before TCB build)--> CANCELED
FAILED --cleanup done--> IDLE
CANCELED --cleanup done--> IDLE
DONE --release_completed()/reset()/next request_load--> IDLE
IDLE --request_cancel (shell)--> "not loading" (no state change)
```

- Only the loader task mutates state between IDLE and DONE/FAILED/CANCELED.
  The shell task performs exactly two state transitions: `request_load`
  (IDLE→VALIDATING) and `request_cancel` (any-non-IDLE→CANCELED).
- One transition max per chunk. Chunk work is bounded (~4 KiB copy + one page
  map) and is followed by `Scheduler::reschedule()` with NO locks held.
- Cancel is observed at: after header validation, after phdr validation,
  before MAPPING, and before each chunk.

## 4. Chunking Design

- `CHUNK_SIZE = 4096` (one page). The loader tracks a read cursor per segment:
  `file_off = phdr->offset + page_in_seg * CHUNK_SIZE`.
- Each PT_LOAD segment is streamed page-by-page: `ops->read(fd, chunk_buf,
  in_file, file_off)` → `PMM::alloc_user_page()` → `map_page_in_pml4(vaddr,
  phys, true, pml4)` → `memcpy(HHDM_OFFSET+phys, chunk_buf, in_file)` (+ zero
  the `memsz > filesz` tail). BSS-only pages (beyond filesz) are zero-filled.
- The whole file is never held in memory: only `sizeof(ELF64Header)` +
  `phnum * phentsize` (≤ 3584 B) of headers plus one 4 KiB chunk buffer, all
  in the singleton (kernel BSS), not the task stack.
- Per-chunk work bound ≈ one page-table walk + one 4 KiB copy + PMM/VMM
  internals — equivalent to the existing daemon budget-consume path.
- Yield contract: `Scheduler::reschedule()` after every chunk, with the
  singleton spinlock released and no IrqGuard held. On a single-core UP the
  timer ISR applies the deferred switch on the next tick; higher-priority
  tasks run first.

## 5. Preemption / Cancel Policy

- **Preemption:** the loader yields after every chunk. A higher-priority task
  (deadline monitor 127, daemons 20, shell 2) runs immediately. The loader is
  never in the ready queue while idle (blocked on the wake semaphore).
- **Cancel:** `request_cancel()` (shell) sets `state_ = CANCELED` +
  `cancel_requested_ = true` under the spinlock and returns immediately. The
  loader observes it at the next `check_cancel()` (per chunk / validation
  step / before MAPPING), then runs `cleanup_and_idle()`.
- **Single-owner cleanup:** the loader is the ONLY actor that frees
  resources. Every exit from `run_load()` funnels through `cleanup_and_idle()`
  with idempotent guards (`pml4_ == 0`, `fd_ == -1`), so a cancel arriving
  mid-chunk or between validation sub-steps cannot double-free.
- **Stale-post guard:** `load_generation_` bumps on every accepted request; a
  spurious wake (stale `wake_.post()` after a reset) is dropped by comparing
  the generation captured at `run_load()` entry.

## 6. Error Taxonomy

| Error | Shell/dmesg message | dmesg code | State | Cleanup |
|---|---|---|---|---|
| ALREADY_LOADING | "already loading" | 0xDB08 | no change | none |
| NOT_LOADING | "not loading" | 0xDB09 | no change | none |
| FILE_NOT_FOUND | "file not found" | 0xDB06 | FAILED→IDLE | cleanup_and_idle |
| READ_ERROR | "read error" | 0xDB07 | FAILED→IDLE | cleanup_and_idle |
| INVALID_ELF | "invalid elf-file" | 0xDB04 | FAILED→IDLE | cleanup_and_idle |
| OOM | "not enough memory" | 0xDB05 | FAILED→IDLE | cleanup_and_idle |
| (success) | "loading <name> <size> started" | 0xDB01 | VALIDATING | — |
| (success) | "loading <name> <size> completed in <t>" | 0xDB02 | DONE→IDLE | close fd, retain TCB |
| (cancel) | "loading <name> canceled" | 0xDB03 | CANCELED→IDLE | cleanup_and_idle |

## 7. Shell Command Contract

- `load <path.elf>`:
  - `argc < 2` → `Usage: load <path.elf>`
  - success → `loading <path.elf> <size> started` to terminal + dmesg 0xDB01,
    return immediately (no waiting).
  - `ALREADY_LOADING` → "load: <path> already loading" + dmesg 0xDB08.
  - `FILE_NOT_FOUND` → "load: <path> file not found" + dmesg 0xDB06.
- `cancel-load`:
  - `NOT_LOADING` → "cancel-load: not loading" + dmesg 0xDB09.
  - success → `loading <path> canceled` + dmesg 0xDB03, return immediately;
    the loader finishes cleanup asynchronously.
- Completion/cancel/failure messages are produced by the **loader task**
  (shell only produces the immediate start/not-loading replies).

## 8. Memory Accounting (zero ResourceTracker delta per cycle)

| Resource | Allocated | Freed by |
|---|---|---|
| fd + vnode ref | VALIDATING | `fd_table.free` (success + cleanup) |
| PML4 page | VALIDATING | `PMM::free_page(pml4_)` (cleanup) |
| segment/stack/heap user pages | per chunk / MAPPING | `VMM::free_user_pages(pml4_)` (cleanup) |
| TCB struct + kstack (DONE) | MAPPING | `release_completed()` / `reset()` / future runelf |
| header + phdr + chunk buffers | none (singleton BSS) | n/a |

Completion closes the fd; the completed TCB is the only retained resource and
is owned by the singleton until `take_completed()` / `release_completed()`.

## 9. Snapshot / Test-Isolation Contract

- The loader TCB is created in `kernel.cpp` before the test runner, so it is
  part of the snapshot baseline and survives `snapshot_restore` (task fields +
  kstack restored automatically).
- Loader state (singleton fields) is NOT snapshot-restored. Tests must call
  `ElfLoader::reset()` at test start and `ElfLoader::wait_loader_idle()` at
  test end (never leave a load in flight).
- `reset()`: if not IDLE, set CANCELED; caller spins `wait_loader_idle()`;
  release `completed_tcb_` if present; `wake_.init(0,1)` to drop stale counts.

## 10. Future `runelf` Hook (design-only, not implemented)

- `ElfLoader::take_completed()` returns the DONE TCB (ownership transfers to
  the caller); `release_completed()` cleans it up.
- The future `runelf <name>` command calls `take_completed()`, sets a settled
  priority/base_priority, and `Scheduler::add_task` — making the loaded image
  schedulable as a user task. The state machine's `DONE` state + singleton
  retention make this a one-command change.

## 11. Test Plan

New class `elf_loader` (test_elf_loader.cpp):
1. `loader_load_success` — load a tmpfs ELF; assert completed TCB, entry,
   page_table; release.
2. `loader_load_invalid_elf` — corrupt magic; expect INVALID_ELF.
3. `loader_load_truncated` — phdr bounds vs file size; expect INVALID_ELF.
4. `loader_cancel_mid_load` — 8-page load, cancel after 2-3 chunks; zero delta.
5. `loader_already_loading` — second request rejected; cancel; idle.
6. `loader_cancel_not_loading` — cancel with nothing in flight.
7. `loader_preemption_yield` — harness makes progress during an active load.
8. `loader_multiple_cycles` — 3 sequential load→release cycles, zero delta.

Validation: `elf` class (refactor guard), `elf_loader` class, `selftest`,
debug `all` (trace ON), release `all` (trace OFF); test-history rows.
