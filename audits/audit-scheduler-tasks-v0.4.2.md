# NexIOS Audit Report — Scheduler / Tasks (scheduler.cpp, scheduler.hpp)

**Scope:** `src/kernel/task/scheduler.cpp`, `src/kernel/task/scheduler.hpp`
**Date:** 2026-08-22 (original findings from prior review; re-formatted and structured) · **Baseline:** v0.4.2-dev
**Method:** static review for correctness, concurrency/atomic usage, API/implementation mismatches, duplication, maintainability
**Status:** ✅ **Re-verified against HEAD (`624b9e5f`, 2026-08-22)** — see "Re-Verification" section at the bottom. Original findings from a prior review pass; several remain open.

---

## Findings — HIGH

### H-1. Header/implementation contract mismatch: `alloc_id()` behavior
**Problem:** scheduler.hpp documents that `alloc_id()` should return `TASK_INVALID` when the ID table is full. scheduler.cpp's implementation simply returns `next_task_id_++` with no table-full check or wrap-around. This can cause ID reuse or overflow and violates the promised contract.

**Fix:** Implement `alloc_id()` to search for an unused id slot (probing `id_table_` or using a free-id bitmap) and return `TASK_INVALID` when no IDs are available; make `next_task_id_` atomic or ensure callers hold `scheduler_lock_` while calling. Provide an `alloc_id_err` that performs the same logic under lock and returns `SCHED_ERR_TABLE_FULL`.

**Rationale:** Prevents duplicate task IDs and hard-to-debug `find_task` failures and UAFs.

### H-2. `next_task_id_` non-atomic mutation / visibility
**Problem:** `next_task_id_` is incremented without atomic operations. If `alloc_id()` can be called from multiple contexts that may race (even single-core has contexts with interrupts enabled), this can race.

**Fix:** Either make `next_task_id_` atomic (`std::atomic<uint64_t>`) or require `alloc_id()` calls to take `scheduler_lock_` (document and enforce). Ensure memory-ordering semantics are correct for allocation visibility.

**Rationale:** Removes subtle races and non-deterministic duplicate IDs.

### H-3. Format-string mismatches in logging
**Problem:** Several `Logger::info`/`warn` calls use `%u` for task.id and other 64-bit values. Undefined/incorrect on many ABIs.

**Fix:** Cast to `unsigned long long` and use `%llu`, or add `Logger` overloads accepting `uint64_t`, or use `PRIu64` from `<cinttypes>`.

**Rationale:** Prevents truncated/wrong values in logs and confusing debug output.

### H-4. `alloc_id_err` documentation inconsistency and missing table-full handling
**Problem:** `alloc_id_err` currently does `out_id = next_task_id_++; return SCHED_ERR_OK`. It must check availability and return `SCHED_ERR_TABLE_FULL` if none available.

**Fix:** Implement probing or free-list semantics similar to `id_table_insert` and return the proper error code.

## Findings — MEDIUM

### M-1. `ID_TABLE_SIZE` power-of-two assumption unchecked
**Problem:** The code relies on `ID_TABLE_MASK = ID_TABLE_SIZE - 1`, implying ID_TABLE_SIZE is a power of two. No static_assert enforces this.

**Fix:** Add `static_assert((ID_TABLE_SIZE & (ID_TABLE_SIZE - 1)) == 0, "ID_TABLE_SIZE must be power of two");` in the header.

**Rationale:** Prevents subtle hashing/probing errors when build-time `CONFIG_MAX_TASKS` changes.

### M-2. `constinit` globals: unclear atomicity invariants
**Problem:** `next_task_id_`, `sporadic_task_count_` and other globals are updated/read from multiple contexts, some non-atomically. Some are updated under locks, others (e.g. `sporadic_task_count_` read by the on_tick loop) may be concurrently accessed.

**Fix:** Audit each global; mark genuinely concurrently accessed counters as atomic (or always access under `scheduler_lock_`/`IrqGuard`). Add comments documenting locking/atomicity invariants.

### M-3. Atomic memory-order / consistency review
**Problem:** Many `__atomic_store_n`/`__atomic_load_n` calls use RELEASE/ACQUIRE appropriately, but the specific memory-order choices where multiple variables are written in sequence deserve an audit to ensure cross-CPU visibility ordering.

**Fix:** Add comments where ordering is critical (e.g. store load_rsp, bump generation, then store save_rsp_to). Consider a helper that publishes the pair to guarantee the sequence matches the spec in one place.

## Findings — LOW (maintainability / hardening)

- **Duplicate `#pragma once` and duplicate includes** — scheduler.hpp has `#pragma once` twice; scheduler.cpp includes assert.hpp twice. Remove duplicates.
- **Repeated iret-frame validation logic** — similar frame validation duplicated in `switch_to_task` and `switch_away_from_terminating`. Extract `bool validate_iret_frame(const TaskControlBlock &next, bool harness_allowed)` to centralize.
- **Magic numeric offsets for frame parsing** — hard-coded `136/8`, `152/8`, `160/8`, `168/8` represent arch-specific register-layout offsets. Define named constants (`constexpr size_t IRET_RIP_IDX = 136/8;` etc.) and document "created" vs "isr_common" orderings.
- **Extract deferred-switch cancel/restore helper** — several call sites repeat the clear load/save/next/generation sequence. A single "drop_arm_and_restore_current" helper avoids drift.
- **Replace `TASK_STACK_PTR` macro with inline function** — macros depending on arch defines are error-prone; a `constexpr inline` function returning the correct field per arch is type-safe.
- **Split large functions** — `on_tick` and `switch_to_task` are large; extract diagnostics, accounting, sporadic-server handling, and deadline scan into smaller static helpers.
- **Precondition docstrings** — add one-line locking/IRQ preconditions per function ("Requires: scheduler_lock_ held", "Runs with IF=0 — must not re-enable IRQs") to simplify auditing.

## Suggested Tests & Diagnostics

- Unit/kernel tests for `id_table_insert`/`find`/`remove` under wrap-around and tombstones.
- `alloc_id`/`alloc_id_err` behavior when the table is full and id reclaim after remove.
- Deferred-kill list saturation (`MAX_DEFERRED_KILLS` overflow path).
- Schedule/reschedule behavior across harness test-mode boundaries (`is_test_active` gating).
- Snapshot capture/restore and harness RSP preservation.
- More invariant assertions: `static_assert(ID_TABLE_SIZE > MAX_TASKS)`; runtime `ENSURE()` that `next_task_id_` never equals a reserved value.
- Trace-level logging macro for arm/publish/drop sequences; an invariant-check function validating ready-queue consistency in diagnostics mode.

## Risk Assessment

- **High** — `alloc_id` mismatch and logging format issues (duplicate IDs, crashes, incorrect logs).
- **Medium** — atomicity/ordering subtleties around `next_task_id_` and other globals (intermittent races).
- **Low** — duplicate macros/includes and refactors (low-risk, improves maintainability).

---

## Re-Verification against HEAD (`624b9e5f`, 2026-08-22)

Each original finding was checked against the current source:

| Finding | Status at HEAD | Evidence |
|---|---|---|
| H-1 `alloc_id()` no table-full check | ❌ **STILL OPEN** | scheduler.cpp:993–995 — still `return next_task_id_++;`, no probing, no `TASK_INVALID` on full |
| H-2 `next_task_id_` non-atomic | ❌ **STILL OPEN** | scheduler.hpp — plain `uint64_t`, not atomic; incremented unlocked at scheduler.cpp:994, 3367 |
| H-3 `%u` format for 64-bit IDs | ❌ **STILL OPEN** | scheduler.cpp:688, 1713, 1727, 2381, 2483 — still `%u` for task IDs; no `PRIu64`/`%llu` anywhere in the file |
| H-4 `alloc_id_err` missing table-full | ❌ **STILL OPEN** | scheduler.cpp:3367 — still `out_id = next_task_id_++` with `SCHED_ERR_OK` |
| M-1 missing power-of-two static_assert | ❌ **STILL OPEN** | scheduler.hpp:524–526 — `CONFIG_MAX_TASKS * 2` with mask, no static_assert |
| M-2 non-atomic `constinit` globals | ❌ **STILL OPEN** | scheduler.cpp:597 — `next_task_id_` still plain |
| Low: duplicate `#pragma once` | ❌ **STILL OPEN** | scheduler.hpp contains `#pragma once` twice |
| Low: duplicate `assert.hpp` include | ❌ **STILL OPEN** | scheduler.cpp:39 and :74 |
| Low: `validate_iret_frame` helper | ❌ **NOT DONE** | no such helper exists |
| Low: `TASK_STACK_PTR` macro→inline fn | ❌ **STILL OPEN** | still a macro (test_isolate.cpp:72–76); scheduler.cpp:2711 uses it |

**Conclusion: none of the original scheduler findings have been addressed.** The file has evolved since the review (H2 deferred-switch fixes, aarch64 portability groundwork, per-task memory reporting), but the review's correctness items — ID allocation contract, atomicity of `next_task_id_`, format strings — are unchanged. Recommend treating H-1/H-2/H-4 (ID reuse under table pressure) as a single fix package, since they share the same root cause.
