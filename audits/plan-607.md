# PLAN: #607 — fence the sys_waitpid publication window under one arch::IrqGuard (publish-side atomicity; the #590 seam guard nests inside it)

Base: branch `main`, tip `4daa93eaa568fbc4f1da8756dba8cc0b3afa5647` read from `.git/refs/heads/main` (this planning session has no shell; the tip is pinned by file read, HEAD -> refs/heads/main). Nothing committed by this planner; uncommitted scratch (`audits/pending_patch.diff`, `audits/rejected_patch.diff`), `test-history.txt` rows and branch `dev/604-c6-gates` are untouched. Deliverable is `audits/plan-607.md` only.

## AFFECTED FILES
- `src/kernel/proc/proc_svc.cpp` — `ProcSvc::waitpid` (:304-321): the INV-WAIT-4 publication sequence (`waiting_child_status` :309 → `dequeue_ready` :310 → `state=BLOCKED` :311 → `waiting_child_pid` :312 → SEQ_CST fence :318 → DEBUG fire :319-321) becomes one `arch::IrqGuard` scope; comment block restated with the #607 rationale and the boundedness proof. **No statement is changed, added, removed or reordered** — the diff is the guard line, two braces, and comments (everything else is a 4-space reindent).
- `src/kernel/proc/proc_svc.cpp` (include block, :32-44) — add `#include <kernel/arch/hal/irq_guard.hpp>` (canonical path; the `<kernel/arch/irq_guard.hpp>` shim is deprecated). No other include change.
- `docs/specs/scheduler.md` — §INV-WAIT-3/4/5/6 (:167-212): INV-WAIT-4 gains the atomicity clause, and the "deliberate divergence" paragraph (:199-203, currently "the waitpid publish is lock-free (no guard exists on that path)") is rewritten to the post-#607 truth (guard + post-fence re-verify, with the re-verify scoped to the SMP case it actually repairs).
- `docs/irqguard-ledger.md` — new production KEEP row `P1` for this site (the ledger audits production instantiations only; a *seam*-only site needed no row, a *production* site does). Optional S3 note: the ledger's "20 sites, as of 55fde391" summary is stale against the tree.
- `audits/plan-607.md` — this plan (uncommitted artifact).

Not touched (explicit non-scope): `src/kernel/task/scheduler_lifecycle.cpp` `fire_wait_publish` (the #590 entry guard stays, byte-identical, and now nests), `src/kernel/test/**` (QE-owned: seam hooks, assertions, registry, expected counts), `src/kernel/task/scheduler_queue.cpp` / `ready_queue_manager.cpp` / `task_queue.cpp` / `scheduler_smp.cpp` (cited as the boundedness evidence, not edited — including the RQ hint-miss scan cost, which is pre-existing and gets a follow-up issue, not a fix here), `src/kernel/task/scheduler_registry.cpp` (`set_task_ready` is only reachable on the *post*-window undo leg), `docs/specs/irq-preemption-contract.md` (no new rule: this site conforms to §entry-IF rule 13 instead of violating it).

## INVARIANTS & CONCURRENCY BOUNDARIES

### Premise correction (evidence-first, load-bearing for the whole plan)

The issue text says the window "runs with interrupts ENABLED". That is true for **two of four caller classes** and false for the other two. Measured against the tree:

| Caller class | IF at :310 | Exposed? |
|---|---|---|
| user task via `int $0x80`, default build (`CONFIG_PROC_MGR 0`, `nexios_config.h:1664-1665` (validator documenting default 0; #define at :1771)) | **0** — interrupt gate `0xEE` (`idt.cpp:69-71`; normative: `docs/specs/irq-preemption-contract.md:9`) | no (atomic by interrupt-gate accident - 0xEE is an interrupt gate and clears IF) |
| kernel-task inline caller (`!syscall_is_user_task()`, `proc_svc.cpp:390`) | **1** — plain task context, no trap | **yes** (the class the seam tests exercise; reproduces the observed `1/0/0/0`) |
| user task, `CONFIG_PROC_MGR=1` build — `ask_proc_verdict` (:213) → `IPC::send` blocked wait returns with `arch::sti()` (`send_sync_bounded_err` return-sti legs, e.g. `ipc.cpp:916/:950/:1104/:1147`, as well as `recv`'s `:1473-1478/:1507`) | **1** | **yes — production, the issue's premise is exactly correct here** |
| aarch64 / riscv64 | 0 (DAIF masked on exception entry; `csrci sstatus,2` first trap instruction) | no today; same trap-policy dependence |

Two consequences the implementation must honour:
1. The fix is **not** a no-op: it is the only thing that makes the atomicity hold for the kernel-task and PROC_MGR caller classes, in release, on all three arches.
2. Correctness currently depends on an **undeclared entry-IF assumption**, which `irq-preemption-contract.md:13` explicitly forbids ("no path depends on entry-IF/DAIF/SIE except by declaring it"). #607 removes the dependence; that is the invariant-level justification even where the trap gate already masks.

### INV-PUB-IRQ-1 (atomic publication — the property this change buys)

From `publish_guard` construction to its destruction, no timer tick / ISR / other task can observe any prefix of `status → dequeue → BLOCKED → pid`. Either the whole publication happened, or none of it. Consequence: the wake gate (`state==BLOCKED && waiting_child_pid ∈ {child.id, -1}`, `scheduler_lifecycle.cpp:702-707`) can only be observed in its complete form, so `wake_waiting_parent` can no longer read a dequeue-only prefix and return `NO_WAITER` (`scheduler_lifecycle.cpp:675-677`).

### INV-PUB-IRQ-2 (lock inventory — every lock reachable inside the window; completeness is the audit point)

1. **No lock is taken by the guard itself.** `arch::IrqGuard` writes per-CPU scalars/ring slots and `cli()`; it is not a lock (`irq_guard.hpp:135-189`).
2. `Scheduler::dequeue_ready` (`scheduler_queue.cpp:496-514`) — **lock-free**: `validate_task_ptr` (O(1) magic/range, `scheduler_integrity.cpp:146-172`), `edf_remove` (O(1) intrusive, only if flagged), `rq_task(...).remove` (below), `ready_deferred_ = false`, `invalidate_pending_switch_to` (below).
3. `Scheduler::effective_priority` (called by `dequeue_ready` for the bucket key) — nested `IrqGuard` only (ledger site `S1`); no spinlock.
4. `ReadyQueueManager::remove` (`ready_queue_manager.cpp:121-174`) — **no lock member** (the RQM has no `SpinLock`); `queues_[hint].try_unlink` → `TaskQueue::try_unlink` (`task_queue.cpp:124-185`), O(1) link splice, its own nested `IrqGuard` (`:125`). Hint-miss path: a **bounded scan of `CONFIG_PRIORITY_CEILING+1 = 128` levels** (`:144-150`), each attempt an O(1) `try_unlink`. This is the only loop of consequence — see INV-PUB-IRQ-5 for its cost.
5. `invalidate_pending_switch_to` (`scheduler_queue.cpp:290-300`) → `Scheduler::cancel_pending_switch_cpu` (`scheduler_smp.cpp:39-62`) — 7 atomic stores + one generation bump per targeted CPU, loop bounded by `CONFIG_MAX_CPUS = 8`; **no IPI, no wait, no lock, no `sti`**.
6. `zombie_lock_` (plain leaf `SpinLock`, `scheduler.hpp`) — reachable **only** under the DEBUG seam hook, whose funnel calls `release_zombie`/`try_claim_teardown`; canonical IRQ-off → leaf order, released before return. The hook is UP-gated (`arm_wait_publish` refuses unless `up_cpu_count()==1`, `scheduler_lifecycle.cpp:537-538`), so the timer ISR cannot be holding that leaf on another CPU while we spin.
7. **`scheduler_lock_` is unreachable inside the window — stronger than the seam guard's argument.** The window contains no `set_task_ready` (that is on the post-window undo leg, :348) and no `SchedulerLockGuard` in any callee, so the "direct-enqueue under the global lock" branch (`scheduler_registry.cpp:577-607`) is not even textually present on this path.
8. No `sti` anywhere in the window or its callees: verified by grepping `scheduler_queue.cpp`, `scheduler_smp.cpp`, `task_queue.cpp`, `ready_queue_manager.cpp`, `scheduler_lifecycle.cpp`, `scheduler_integrity.cpp`, plus `sporadic_server.cpp` (reached via `effective_priority` -> `SporadicServer::current_priority()` - audited clean here: const accessor, no sti/lock/log - and named so future edits there get re-audited against this window) — zero occurrences of `sti`.

### INV-PUB-IRQ-3 (nesting safety / guard topology — the answer to "does #590 stay?")

Final topology, exactly:

- **L1 (new, production, both builds)** — `ProcSvc::waitpid`, guard first in the scope, spanning `status store → dequeue → BLOCKED → pid → fence` and, in DEBUG, the fire.
- **L2 (#590, existing, DEBUG-only, unchanged)** — `fire_wait_publish` entry guard, spanning exchange + hook body (`scheduler_lifecycle.cpp:567-571`).

Composition proof: L2 is always constructed inside L1 (single fire site), so the depth sequence is `0→1→2→1→0`. `IrqGuard` saves `irq_was_ = interrupts_enabled()` at construction and restores only that bit: L1 saves the caller's IF (true for kernel-task / PROC_MGR callers, false for a trap-gated user caller — both correct), L2 saves `false` and therefore **cannot emit a stray `sti` mid-window** (the `audit-task-sync-v0.4.2.md:95` L-3 caveat is the reason this must be stated, not assumed). The owner token is set on the 0→1 transition (= the publishing TCB) and is untouched by the nested take, so no `irq_guard_depth_own()`-keyed gate changes behaviour inside the window (and nothing in the window consults one: no park is attempted).

**L2 stays.** Three reasons, all to be recorded in the code comment: (a) it is the seam's own contract — any future fire site inherits IRQ discipline without depending on its caller's scope; (b) it costs zero release bytes (inside the existing `#if defined(CONFIG_DEBUG)` block, `wait_publish_seam.hpp:56-77` untouched); (c) deleting it would couple seam correctness to a caller-side scope that a later refactor can silently widen. No merge, no move: moving L1 into `fire_wait_publish` would make the fix DEBUG-only and fix nothing in release.

### INV-PUB-IRQ-4 (boundedness of the extended window)

No dynamic allocation, no IPC, no VFS, no `Logger` write, no `dmesg_push` and no `reschedule()`/`switch_to_task()`/`park` on the success path. Loops: exactly two, both fixed-bound (128 ready levels, 8 CPUs). Cold/warn legs inside the window: `ReadyQueueManager::remove`'s forced-clear `Logger::error` (only when `in_ready_queue_` is stale-set, one-shot per boot, `ready_queue_manager.cpp:157-173`), `validate_task_ptr`'s corruption log (only on corruption), `sched_runq_trace` (DEBUG ring stores only; compiled to empty inline in release, `scheduler_debug_internal.hpp:89-95`). No user-memory access (the two-cert block :294-303 and `safe_copy_to_user` are outside the scope by construction). Wall bound: see INV-PUB-IRQ-5.

### INV-PUB-IRQ-5 (release identity, added instructions, masked-window quantification)

- The guard is **production code in both builds** (this is a production liveness defect, not a seam): there is **no debug/release behaviour divergence** — the opposite of the #590 seam fix, and it must be stated that way in the audit hand-off.
- Added release cost (x86_64): `IrqGuard` ctor ≈ 12-14 instructions (cpu index, depth load/inc, owner store on 0→1, 4 evidence-ring stores, `cli`), dtor ≈ 10-12 (depth dec, owner clear, 4 ring stores, conditional `sti`) ⇒ **≈ 25 instructions, once per waitpid *block*** (not per scan iteration).
- **Masked-window delta for the trap-gated user path (default build): 0 ns.** IF is already 0 there, so `irq_was_=false` and the dtor does not `sti`; the only change is bookkeeping.
- **Masked-window delta for IF=1 callers (kernel-task; `CONFIG_PROC_MGR=1`)** = the publication work, dominated by `ReadyQueueManager::remove`'s hint-miss scan: a *current* task has `rq_priority_ == 0` and `in_ready_queue_ == false` (both cleared by `TaskQueue::pop_front`, `task_queue.cpp:97-109`), so the hint attempt on level 0 misses and the bounded 128-level scan runs ⇒ **129 O(1) `try_unlink` attempts, each with its own nested IrqGuard (≈24 stores)** ⇒ ≈ 3-5 k cycles ⇒ **≈ 1.5-2.5 µs release @2 GHz**; DEBUG adds one `sched_runq_trace` call per failed attempt ⇒ **tens of µs**, i.e. the same order as the #590 *armed*-hook window that was accepted (`design-review-590-seam-guard-code-r1.md:24`).
- Ratio to the tick: 1 ms (`CONFIG_TICK_HZ 1000`, `nexios_config.h:71`) ⇒ ≈ 0.15-0.25 % release, ≤3 % debug. This is 10-25× the seam guard's accepted 10¹-10² ns **but it is not new work**: the identical work already runs on this exact path today, unmasked, and the same `dequeue_ready` already executes inside `terminate_err`'s IrqGuard (ledger `S6`) and inside `IPC::block_sender`'s publish guard (`ipc.cpp:1595-1637`). The change moves an already-accepted work set inside the mask; it does not invent a critical section.
- Timing-test argument: no test measures tick latency or tick phase; a deferred tick is delivered immediately after the `sti` (level/edge latch, no tick loss), and for the default user path the delta is literally zero. The kernel-task test harness is the only in-suite class affected, and it is the class the change is meant to make atomic.
- Optional empirical check (dev, must be reverted verbatim + grep-verified before any gate, LEARNINGS #46/#608 precedent): one-shot rdtsc delta around the scope logged once in `process_waitpid` to confirm the ≤ tens-of-µs bound.

### INV-PUB-IRQ-6 (wake gate preserved — INV-WAIT-4 order and fence discipline unchanged)
- The `:321->:334` stretch (fence at :318 to re-verify entry) is deliberately UNMASKED: it (plus the cross-CPU path) is the positive reason the post-fence re-verify is live code rather than dead-but-present - a tick landing there is exactly what the re-verify repairs, and stating this pre-empts the dead-code challenge.

Statement order, fences and the wake-side code are untouched. Case analysis:
- **Same-CPU terminator (tick/ISR):** cannot run inside the window (IF=0 ⇒ no local tick), so it runs after the guard with the publication complete — `waited == child.id` (or `-1`) ⇒ match ⇒ `PARENT_COLLECTS` ⇒ wake; the consumed publication is detected by the unchanged post-fence check (:334-336).
- **Reentrant/self terminator inside the window:** impossible — the window calls only queue surgery, stores, a fence and (DEBUG) the seam hook; no `terminate_err`/`terminate_err_from_trap`/`kill`/`sys_exit` is reachable from it.
- **Other CPU (SMP):** the mask is per-CPU and claims nothing here. The pre-existing SEQ_CST fence pair (publisher :318 ↔ `wake_waiting_parent` :664) plus the bounded post-fence re-verify (:334-349, `kWaitReverifyRetryMax = 4`, :193) remains the repair, unchanged and still *outside* the guard. The mask neither weakens nor replaces it.
- **Observability (LEARNINGS #559/(4)):** the BLOCKED window stays observable after the guard drops (it persists until the wake consumes it); what becomes unobservable is exactly the torn pre-publication prefix, which no consumer is entitled to see (the shell's read-only diagnostic included).
- **pid-last still implies BLOCKED**, and the fence still pairs the two sides, because no store moved.

### INV-PUB-IRQ-7 (scope fence — the S1 shape to avoid)

The scope must end **immediately after the DEBUG fire** (release: immediately after the fence). It must **not** extend over the INV-WAIT-6 re-verify (:334-389), the `collect_zombie_child` leg, the `dmesg_push_sev` legs, `Scheduler::reschedule()` (:397) or the `sti/hlt/cli` wait loop (:398-404): the re-verify exists to observe a *concurrent* wake (parking, locking, logging and `reschedule()` under a live IrqGuard are forbidden — `reschedule()` arms a deferred switch that the tick must be able to apply; LEARNINGS "never reschedule under the guard"). The single `}` placement is the whole difference between the fix and a permanent hang.

### INV-PUB-IRQ-8 (accounting / release-identity plumbing)

No `track_*` add/remove, no PMM/MemPool/TCB/VFS/FD delta on the changed lines (ResourceTracker neutral by construction). No new TU (no `tools/meson/gen.mk` change), no new symbol, no new config knob, no new lock rank (`docs/specs/irq-preemption-contract.md` order table unchanged — we take no lock).

### Retrieval (audit check-7 — queries, results, dispositions)

| Query | Result | Disposition |
|---|---|---|
| `graphify query_graph "sys_waitpid publication dequeue_ready state BLOCKED waiting_child_pid fire_wait_publish wake_waiting_parent"` | nodes `sys_waitpid` (`syscall.hpp:277`), `dequeue_ready` (`scheduler.hpp:695`), `wake_waiting_parent` (`scheduler.hpp:581`), plus `docs/specs/zombie-list.md` §7 caveats; a large `statemachine/*` cluster also matched | **used**: confirms the graph models the *symbols* but has no edge for the publish/wake handshake, so the handshake stays comment/spec-authoritative and the tree read is mandatory. **ignored**: the `statemachine` cluster (different domain) |
| `graphify shortest_path(sys_waitpid → wake_waiting_parent)` | 5 hops via `Syscall::handle → panic() ← deadline_miss_handler → wake_waiting_parent`, all INFERRED | **used as negative evidence**: no direct call edge — the publisher never calls the wake path, so the discipline cannot be inherited and must be installed at the publish site |
| `graphify shortest_path(dequeue_ready → wake_waiting_parent)` | 2 hops via `terminate_err_from_trap` | **used**: names the funnel that already runs `dequeue_ready`+`wake` under its own IrqGuard — the discipline reference for INV-PUB-IRQ-2/5 |
| `graphify get_neighbors("waiting_child_pid")` | no node | **used (negative)**: the field is not graph-modelled at all ⇒ the file read (`proc_svc.cpp`/`scheduler_lifecycle.cpp`) is authoritative, no disposition shortcut available |
| `graphify query_graph "IrqGuard nested guard depth reentrancy stray sti restore interrupt state"` | `IrqGuard` (`irq_guard.hpp`), `prompts/LESSONS.md` "2026-06-21 Nested Timer Interrupt Corrupts Scheduler State", `docs/specs/scheduler.md` reentrancy rule | **used**: the nesting/stray-`sti` precedent behind INV-PUB-IRQ-3 and the nested-ISR-corruption lesson that motivates the mask |
| `obsidian_search_vault "waitpid"` (15 hits) | `audits/design-review-590-r2.md:50` ("narrower sub-window real"), `audits/design-review-588-r1.md:44` (fault handler wakes waitpid parents from IRQ-masked context), `audits/design-review-590-code-r1.md:10`, `audits/design-review-590-seam-guard-{r1,code-r1}` | **used**: the S3 (`audits/plan-590-seam-guard.md:108`) that scoped this S2, plus the precedent that a wake from IRQ-masked context is normal |
| `obsidian_search_vault "IrqGuard"` scoped `audits` | `audit-task-sync-v0.4.2.md:95` (L-3: restore-if-enabled / stray-`sti`), `deep-analysis-h2-ssdeadline-v0.3.9.md:344`, `design-review-557-r2.md:17` (guard-at-entry precedes every publish step) | **used**: L-3 became INV-PUB-IRQ-3's explicit claim; #557's publish-guard shape is the sibling pattern |
| `obsidian_search_vault "stray sti"` / `"waiting_child_pid"` / `"publish" + "seam"` phrases | 0 hits | **ignored (empty)**; fell back to in-tree normative sources `docs/specs/irq-preemption-contract.md:9-13`, `docs/specs/scheduler.md:167-212`, `docs/irqguard-ledger.md:1-53` |
| `obsidian_search_vault "publisher" ` scoped `audits` | `plan-590-seam-guard.md:108` (the exact S3 text), `design-review-590-seam-guard-r1.md:32` ("publisher-side window — out-of-scope + separate S2 issue CORRECT") | **used**: this issue's charter, verbatim |

## STEP-BY-STEP CHANGES

1. `src/kernel/proc/proc_svc.cpp` include block — insert `#include <kernel/arch/hal/irq_guard.hpp>` immediately after `#include <kernel/proc/proc_svc.hpp>` (self-header first, then `kernel/arch/...`; `SortIncludes` is off in `.clang-format`, so the position is manual and must stay). No other include edit; do **not** rely on a transitive include, and do not use the deprecated `kernel/arch/irq_guard.hpp` shim.
2. `src/kernel/proc/proc_svc.cpp` :304-321 — replace the INV-WAIT-4 comment + five statements + fence + DEBUG fire with the scoped form below. Constraints: guard is the **first** statement of the block (before the status store, hence before `dequeue_ready`); every existing statement and comment keeps identical text, only reindented by 4; the `#if defined(CONFIG_DEBUG)` stays at column 0 wrapping only the fire; the closing brace goes immediately after `#endif`. Target shape (comment trimmed to what the auditor needs; everything ≤90 cols):

```cpp
        // Issue #590 INV-WAIT-4: publish order is status → dequeue →
        // BLOCKED → pid, pid LAST.  The wake gate is state==BLOCKED +
        // pid, and the fence below pairs them — a terminator that
        // observes the pid also observes BLOCKED.
        //
        // Issue #607: the whole publication is ONE IRQ-atomic unit.
        // It used to inherit whatever IF the caller held — 0 for an
        // int $0x80 user trap (interrupt gate 0xEE), but 1 for a
        // kernel-task caller (:390 leg) and for a CONFIG_PROC_MGR=1
        // build (ask_proc_verdict's blocking IPC returns with sti,
        // ipc.cpp:1475/1507).  A tick landing mid-sequence there
        // dequeues the parent before the pid is published, the
        // terminator's wake_waiting_parent reads waited != child.id,
        // returns NO_WAITER, and the parent blocks with no waker.
        // Closing it also removes the undeclared dependence on
        // trap-entry IF that irq-preemption-contract.md:13 forbids.
        // Boundedness: no lock (dequeue_ready is lock-free); fixed-
        // bound loops only (128 ready levels, 8 CPU arms); no
        // alloc/log/IPC and no reschedule/park in this scope.  The
        // #590 seam guard inside fire_wait_publish nests (depth 2)
        // and cannot emit a stray sti (irq_was_=false there).
        const uint64_t child_id = child->id;
        {
            arch::IrqGuard publish_guard{};
            cur->waiting_child_status = status_ptr;
            Scheduler::dequeue_ready(*cur);
            cur->state = TaskState::BLOCKED;
            cur->waiting_child_pid = target_pid;
            // Issue #590 INV-WAIT-3: pair the terminator-side fence.
            // Both fences are required — release/acquire is not
            // sufficient here (store buffering would let both sides
            // miss on weak models).  Placed before the kernel-caller
            // return so the kernel-caller path is fenced as well.
            __atomic_thread_fence(__ATOMIC_SEQ_CST);
#if defined(CONFIG_DEBUG)
            kernel::test::fire_wait_publish(*cur);
#endif
        }
```

3. `src/kernel/proc/proc_svc.cpp` — verification of statement identity (self-check before handing the diff to the auditor): `git diff -w -- src/kernel/proc/proc_svc.cpp` must show only the include, the guard line, two braces and comment text as additions; any moved/changed statement is a defect. `grep -n "arch::IrqGuard" src/kernel/proc/proc_svc.cpp` must return exactly one hit.
4. `src/kernel/proc/proc_svc.cpp` :334-405 — **no change**. Explicitly do not move the closing brace past :321; do not add a guard to :294-303 (page-table walks) or to the post-fence legs (see INV-PUB-IRQ-7).
5. `src/kernel/task/scheduler_lifecycle.cpp` — **no change**. `fire_wait_publish` keeps its entry guard; its boundedness comment (`:561-566`) is still accurate. Do not add, remove, or re-word anything (a re-word would make the #590 audit evidence stale).
6. `docs/specs/scheduler.md` :167-212 — INV-WAIT-4 bullet: append "the whole sequence is one `arch::IrqGuard` unit (issue #607) — the publication is atomic for every caller class, independent of the entry-IF policy". Rewrite :199-203 to: the publish runs under an IrqGuard in both builds; the after-fence re-verify is retained because it repairs the *cross-CPU* miss (a per-CPU mask cannot), and it must stay outside the guard (no lock/park/log/reschedule under a live IrqGuard). Delete the "no guard exists on that path" sentence.
7. `docs/irqguard-ledger.md` — append a KEEP/class-A row `P1`: site `proc/proc_svc.cpp` `ProcSvc::waitpid` publication block; critical section `status → dequeue_ready → BLOCKED → pid → SEQ_CST fence`; concurrent accessor: timer ISR terminator (`on_tick` watchdog KILL, deadline KILL) and any concurrent task; primitive: IrqGuard; justification: the wake gate must never observe a partial publication; no lock is taken or needed; bounded (see INV-PUB-IRQ-5). Add one line noting the ledger summary is stale (20 sites @ `55fde391`) and that a separate sweep should re-enumerate production sites.
8. Style/compliance while editing: 4 spaces, braces on the same line, ≤80 cols code / ≤90 comments, no new `using`, no single-character names, value-initialised guard (`{}`), no `const_cast`, no dynamic allocation. Run `make check-style` (`Errors: 0`; warnings non-blocking) and clang-format the touched files.
9. Commit shape (after audit APPROVED): one commit `fix(waitpid): fence the publication window under arch::IrqGuard (#607)` containing `proc_svc.cpp` + the two docs; **no test files on main**; reference the audit report path and the gate counts in the commit message body and as an issue comment.
10. Cross-arch: no arch-gated code is introduced (`arch::IrqGuard` is HAL-shared, `irq_guard.hpp:135-189`); all three arches must still build and the off-arch `process_waitpid` legs must run (step gates). Nothing in this change is x86-only.

## TEST STRATEGY

- **Dev lands NO tests** (test sanctity: `src/kernel/test/**` is read-only for the developer; new tests land on `testbed` only). Dev's own evidence is built-in: the statement-identity diff (step 3), the class gates below, and the seam counter parity `g_hook_entered == g_hook_marked == g_hook_woke == g_hook_released` in the two arming tests (no new `1/0/0/0` or `1/1/0/0`).
- **QE owns, on `testbed`:**
  - **Mechanism pin (feasible on TCG, deterministic)** — extend `seam_observe_only` (`test_waitpid.cpp:164-169`) to also record `arch::interrupts_enabled()` and `arch::irq_guard_depth_own()` at the seam, and assert in `waitpid_publish_order_preserved` (`test_waitpid.cpp:1385-1388`) that IF was 0 **and the depth was ≥ 2**. IF alone does not discriminate (L2 already gives IF=0 at the seam); depth ≥ 2 proves the *publish* scope was live at the fire site, i.e. #607 landed. Requires the sanctity ritual (doc-block + implementation updated together, one sentence on why: #607 pins the publish-side guard). The TU already includes `<kernel/arch/irq_guard.hpp>`.
  - **Named test (optional, same property, self-documenting)** — `waitpid_publish_window_masked`: kernel-task caller (IF=1 by construction) arms a hook that asserts IF==0, depth ≥ 2, publisher BLOCKED + unqueued + pid set, then completes normally. Run this pin once against the pre-fix tree and observe it FAIL (depth==1) as the control leg - discriminating power demonstrated, not asserted. (EDF members transit through depth 3 via edf_remove; >=2 already accommodates this.)
  - **Honest limit to record in the test docs:** a "tick lands between :310 and :312" *effect* test is **not constructible** — the fix removes the interleaving, and TCG offers no way to inject a tick inside a masked window. The mechanism pin above is the substitute; the previous wedge class stays covered by the existing armed-seam tests. An SMP effect test would need an implemented `smp_waitpid` (currently a stub, `test_smp_waitpid.cpp:47-49`, `test_expected_counts.hpp:129`) **and** would not be evidence for this fix anyway (a per-CPU mask does not close a cross-CPU interleaving) — file the SMP supplement as its own testbed item, do not claim it here.
  - **Must still pass (regression floor):** `waitpid_missed_wake_consumed_collects` (`test_waitpid.cpp:506` arm), `waitpid_released_corpse_collects` (`...:714` miss leg — the leg that exercises this very window), `waitpid_reverify_wildcard_collects` (`...:1005`), `waitpid_retained_corpse_survives_drain` (`...:1151`), `waitpid_publish_order_preserved` (`...:1321`), `waitpid_reverify_never_fights_the_wake`, `waitpid_zombie_claim_is_atomic`, `waitpid_kill_self_wakes_parent`, `waitpid_zombie_over_new_child`, `waitpid_two_children_sequential_reap`; release leg `waitpid_cr3_switch_on_status_write`.
- **Gates (developer runs, per class, in order; every run appends exactly one measured `test-history.txt` row — append-only):**
  1. Builds: `tools/meson/build.sh --arch x86_64`, `--arch aarch64`, `--arch riscv64` (parity is mandatory); plus one `--build release` build.
  2. `python3 tools/meson/qemu_test.py --arch x86_64 --build debug --class process_waitpid` — expect **11/11**, and run it **three times on the identical binary** (flake bar, LEARNINGS #383/#542).
  3. Adjacent, x86_64 debug, in order: `process_lifecycle`, `scheduler_core`, `core_sched`, `task_lifecycle`, `ipc_core`, `ipc_blocking`, `smp_waitpid` (stub class: green trivially — do **not** cite as SMP evidence).
  4. Off-arch legs (`process_waitpid` carries the default `arch_scope|any`, `tools/test-policy.tsv:22`): `--arch aarch64 --class process_waitpid`, `--arch riscv64 --class process_waitpid` (debug).
  5. Release leg: `--arch x86_64 --build release --class process_waitpid` — expect **1/1**.
  6. `python3 tools/meson/qemu_test.py --arch x86_64 --build debug --class selftest` (CI gate) last.
  7. `make check-style` ⇒ `Errors: 0`.
  8. **Existing failures must be accounted for, never dismissed:** current `test-history.txt` ends with `2026-11-24 09:17:00 arch_aarch64 PASSED: 37 FAILED: 1` and an `all TIMEOUT` row at `09:13:14`. The developer identifies the failing `arch_aarch64` test and either fixes it or references the open issue by number; "pre-existing" is not a disposition (AGENTS.md). A zero-S-line TIMEOUT on identical-binary rerun green is a TCG stall — record `STATUS`, check host load first.
- **Not required for this diff:** `all` / `run_all_classes.sh` (single call site, no new path); escalate to the full suite only if any adjacent class shows a seam or scheduler interaction.

## SIL 3 / SAFETY RISKS

- **[S1] Scope creep past the fence** — if the closing brace lands after :334 (or after `reschedule()`), the post-fence re-verify, the bound-exhausted `dmesg` leg, the undo leg's `set_task_ready`, `Scheduler::reschedule()` or the `sti/hlt/cli` loop would run under a live IrqGuard: a deferred switch that the tick can never apply and an `hlt` that cannot be woken = permanent hang. — **Mitigation:** INV-PUB-IRQ-7 states the exact brace position; step 3's `git diff -w` check plus gates 2/6 would catch it; the comment names the forbidden extension explicitly.
- **[S1] Guard placed after the dequeue (or inside the `if (hook)` body)** — the window stays open at :310-:311 and the wedge survives; this is the "false fix" shape (fix looks present, defect unchanged). — **Mitigation:** guard is the first statement of the block, before the status store; `grep -n "arch::IrqGuard"` = exactly one hit at that line; the QE depth ≥ 2 pin fails if the guard is missing or misplaced inside L2.
- **[S1] Stray `sti` inside the window** — a nested guard that saves `irq_was_=true` would re-enable interrupts mid-publication and restore the very interleaving being closed. — **Mitigation:** the nesting analysis (INV-PUB-IRQ-3) shows L2 saves `false`; and no callee in the window calls `sti` (grep evidence, INV-PUB-IRQ-2 item 8).
- **[S2] A fault inside the window abandons the guard** (no dtor ⇒ per-CPU depth stays 1). — **Mitigation:** not a new mode — `terminate_err` already runs `deliver_wait_status` (fault-armed) under its own guard, the dtor is saturating by design (`irq_guard.hpp:154-179`), and IF is re-established from the resumed context. Disclose it; QE reruns confirm no depth residue (residue surfaces as a hang in the following class).
- **[S2] WCET growth on the blocking waitpid path** (≈1.5-2.5 µs release / tens of µs debug, dominated by the pre-existing RQ hint-miss scan). — **Mitigation:** quantified in INV-PUB-IRQ-5; the identical work already runs unmasked on the same path and inside `terminate_err`'s guard, so no new cost class; no test measures it. A cheap RQ fast path is **NOT VIABLE** here: an early-out on `!in_ready_queue_` is forbidden by INV-RQ-1/2 ("flags are hints, links are authoritative", `task_queue.cpp:126-135`), and the sole-element case needs the level scan. File a follow-up issue for the RQ scan cost itself; do not fold it into this diff.
- **[S2] SMP mis-reading of the fix** — an IRQ mask is per-CPU; a reviewer or a future test could believe the cross-CPU miss is closed. — **Mitigation:** the code comment, the spec paragraph (step 6) and INV-PUB-IRQ-6 case C all state that the fence pair + bounded post-fence re-verify remain the SMP repair; gates must not cite the stub `smp_waitpid` as evidence.
- **[S3] Doc drift** — `docs/specs/scheduler.md:199-203` currently asserts the publish is lock-free with "no guard"; leaving it makes the next audit read the code as the violation. — **Mitigation:** step 6 rewrites it in the same commit.
- **[S3] Ledger omission** — `docs/irqguard-ledger.md` audits production sites, so this one needs a row (unlike the DEBUG-only seam). — **Mitigation:** step 7 adds `P1`; the stale 20-site summary is noted, and a re-enumeration sweep is recommended as its own issue (not scope).
- **[S3] Out-of-scope observation, file-don't-fix:** the kernel-caller leg returns −1 at :390 *after* publishing BLOCKED+pid (:311-312), i.e. a kernel-task caller that is never woken returns to its caller marked BLOCKED with a live publication. Pre-existing, unrelated to the mask, and touching it would change `INV-PROC-1` statement identity. Record it, file a follow-up, do not touch it in this cycle.
- **[S3] Out-of-scope observation, file-don't-fix:** `sched_runq_trace(RqmRemoveHintMiss)` fires on every `dequeue_ready` of a dispatched task in DEBUG (129 records per call) — a diagnostic flood that also inflates the debug masked window. Pre-existing; follow-up.

## OPEN QUESTIONS
- none.
