# OOM / Resource-Exhaustion RT-Safety Specification

**Semantics:** contracts for guaranteeing that admission-control, allocation
failure, and WCET behave gracefully under memory/resource exhaustion.
Synthesis of the historical OOM RT-safety plan and WCET analysis (originals
removed 2026-11 after extraction — see git history), the
`CONFIG_MEMORY_BUDGET` mechanism, and the v0.3.12 Alloc/Free return-value audit
(ROADMAP).  `[IMPLEMENTED]` = code-verified.

## 1. The Problem (semantic)

A high-priority task that fails to allocate can monopolize the CPU (retry/spin)
and starve lower-priority tasks.  Every allocation failure must (a) be detected
at the call site, (b) propagate a defined error, and (c) never leave the
scheduler/allocator in a corrupt state.

## 2. Admission Control (`CONFIG_MEMORY_BUDGET`) [IMPLEMENTED, default ON]

```
 init_memory_budget(total_pages)  (global budget after PMM init, issue #20 —
        kernel.cpp sizes it from PMM capacity right after PMM::init)
        │
 TaskControlBlock::create()
        │ deduct CONFIG_DEFAULT_STACK_PAGES from global budget
        │ insufficient → return nullptr (OOM, admission denied)
        ▼
 task runs ──▶ PMM::alloc_page()/alloc_contiguous() check task budget
        │ over budget → return 0 (+ optional OOM handler)
        ▼
 TaskControlBlock::cleanup() returns pages to the budget
```
- `CONFIG_MEMORY_BUDGET` default 1 (enabled, issue #20) — sized at boot;
  per-task `memory_budget_pages_ == 0` keeps unlimited semantics.
- `memory_budget_pages_`/`memory_used_pages_` on the TCB (set 0 at create).
- **`clone_kernel_pml4` rollback [IMPLEMENTED]:** a real
  `JARVIS_TEST(vmm_clone_failure_rollback)` replaces the historical STUB-8;
  the fork path must roll back cleanly on OOM (no partial PML4).

## 3. Allocation-Failure Contract (v0.3.12 audit)

**`ENSURE()` panics unconditionally; `PMM::free_page()` silently no-ops on
double-free (so a double-free pushes the same page onto the free list twice —
corruption with no diagnostic).**

Binding rules for every alloc/free site:
1. **Never ignore an alloc return.** An unchecked `PMM::alloc_*()` return that
   feeds a deref/map is a NULL/0-deref (v0.3.12 CRITICAL items).
2. **Free is idempotent-but-tracking.** A double-free must be diagnosed, not
   silently re-pushed.
3. **OOM handler callbacks** release the allocator lock before invoking the
   handler and re-acquire for retry.

### v0.3.12 open items — landed/closed disposition
- **A1** LANDED — `init_kstack_window` (task.cpp:455/467/479): each
  `alloc_page_table()` is guarded by `panic("init_kstack_window: ... OOM")`
  before the physical address is stored or mapped.
- **A2** LANDED — `Scheduler::init` (scheduler.cpp:563):
  `if (!idle_task_) panic("Scheduler::init: idle task OOM");` before any
  `idle_task_->` deref.
- **A3** LANDED — `Scheduler::reap_orphans` (scheduler.cpp:1624-1643): when
  idle re-create fails, the old idle is re-inserted into `all_tasks_` /
  `id_table_` and the warn "idle recreate OOM — keeping old idle" is logged —
  the idle task is never freed on failure.
- **A4** LANDED — `map_page_in_pml4` RV64 (vmm.cpp:483/485) and x86_64
  (vmm.cpp:510/512): `get_table()` results are null-guarded (`if (!l1) return;`
  / `if (!l2) return;` / `if (!pdpt) return;` / `if (!pd) return;`).

Verified 2026-08-11 against commit `55fde391` (v0.3.12 milestone definition).
Regression coverage: ROADMAP_done.md v0.3.10/v0.3.11 entries (OOM guards,
G1-G3 of this binding).

## 4. WCET / Bounding Contract

- No `UINT64_MAX` pause-spin in kernel paths reachable by tasks; blocking uses
  BLOCKED + `reschedule()` (see `specs/ipc.md` VULN-W2).
- `sys_receive` has a `timeout_ticks` variant (VULN-W3, `specs/boundary.md`).
- Every per-tick scheduler pass is bounded O(n_tasks) / O(1) bitmap ops.

## 6. R-MEM unstable-state warning (#575)

Exhaustion is unrecoverable; the manager emits one latched warning
(diagnostic + supervisor Notify) instead of failing silently.

Trip-site table (all call `mem_pressure::latch`, zero-alloc, lock-free):

| Site | Reason | Fires when |
|---|---|---|
| `pmm.cpp` frozen deny legs | `PMM_FROZEN_DENY` | post-freeze alloc under `CONFIG_STATIC_POOLS_ONLY` |
| `pmm.cpp` `run_oom_handler_locked` | `PMM_OOM` | installed OOM handler ran and refused retry |
| `pmm.cpp` `is_over_budget` true legs | `BUDGET_DENY` | per-task budget exceeded (all 11 deny sites) |
| `pmm.cpp` waterline (`service_pending`) | `PMM_PRESSURE` | post-freeze free < (100 − threshold)% of baseline |
| `mempool.cpp` `alloc`/`alloc_err` | `MEMPOOL_EMPTY` | pool class `free_count == 0` (too-large requests excluded) |
| `buffer_pool.cpp` both legs | `BUFPOOL_EMPTY` | cache exhausted / non-USER page, no PMM fallback |
| `untyped_handover.cpp` | `HANDOVER_CAPPED` | handover run table full |

Record: `reason`, `count` (monotone, every caller bumps), `latch_tick`
(winner's `arch::Timer::ticks()`), `observed_tick` (supervisor poke).
First caller wins the state; exactly one `PMM+4` winner row is emitted;
subsystem rows are budgeted 1-per-64 ordinals (`latch_notable()`), so
sustained exhaustion cannot flood the fixed ring.

Latch/clear: one latched warning per boot; production never clears
(reboot is the only recovery, Q12-A). `clear()`/`snapshot_reset()` are
test-isolation seams only. A pending Notify with no registered
supervisor is retained, never dropped.

Bounds: waterline baseline is post-freeze free memory (never total RAM —
handover would trip every bootstrap boot); trip iff
`free_now * 100 <= baseline * (100 - threshold_pct())`,
`CONFIG_MEM_PRESSURE_THRESHOLD_PCT` default 90. No division, no
overflow for any sane RAM size.

Not covered (explicit boundary): supervisor fail-safe drive and the
mem-mgr-PD-side pool-dry report (no SUPERVISOR/MEM_MGR `PdRole`, no SDF
fail-safe validation — follow-ups); per-client OOM policy (typed
errors stand); `SHORT_RANGE` / `CAP_ERR_OBJECT_EXHAUSTED`
(client-policy, unlatched by design).

## 5. Gaps

- **`memory_determinism` test class** (exhaust a pool/budget, verify
  policy-defined failure) — phase-3 item, verification status unconfirmed.
- **Double-free diagnostic** — no poisoned-free-list detection yet.
