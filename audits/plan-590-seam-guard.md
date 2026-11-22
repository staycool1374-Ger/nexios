# PLAN: #590 follow-up — run the wait-publish seam hook under arch::IrqGuard (funnel-faithful IRQ discipline)

Base verified: branch `main`, tip `0eb94f82dc400b6edeb5a462609d6de6d372f1d3` (`.git/refs/heads/main`). Deliverable is doc-only: the planner commits nothing.

## AFFECTED FILES
- `src/kernel/task/scheduler_lifecycle.cpp` — `fire_wait_publish()` (:549-554): take `arch::IrqGuard` **before** the single-shot exchange so the installed hook runs to completion with IRQs masked; add a rationale comment naming both funnel sites. No include change (`<kernel/arch/hal/irq_guard.hpp>` already at :40).
- `src/kernel/test/wait_publish_seam.hpp` — **optional, comment-only** one-line contract sentence in the §Contract paragraph (:10-14) stating the hook runs under `arch::IrqGuard`. QE-owned file: only with QE-lane confirmation (see OPEN QUESTIONS). No code change; the `#else` release no-ops (:56-77) stay byte-identical.
- `audits/plan-590-seam-guard.md` — this plan (uncommitted artifact).

Not touched (explicit non-scope): `src/kernel/proc/proc_svc.cpp` (:304-321 publication window stays as-is, including the DEBUG-gated fire at :319-321), `src/kernel/test/test_waitpid.cpp` (test-owned, read-only), `src/kernel/task/scheduler_terminate.cpp` (both funnels are the reference, not the edit target), `tools/meson/gen.mk` (no new TU), `docs/irqguard-ledger.md` (no new *production* IrqGuard site — see S3).

## INVARIANTS & CONCURRENCY BOUNDARIES

**INV-SEAM-IRQ-1 (atomicity).** From `fire_wait_publish` entry to hook return, no timer tick/ISR can observe or interrupt the sequence `dequeue_ready → state=TERMINATED/exit_code → wake_waiting_parent → release_zombie` (`test_waitpid.cpp:126-140`). The whole sequence becomes one IRQ-atomic unit — exactly the discipline of the real funnels: `terminate_err` (`scheduler_terminate.cpp:96` guard; body :134-152 wake, :165 release) and `terminate_err_from_trap` (:254 guard; body :267-293).

**INV-SEAM-IRQ-2 (lock order: IRQ-off strictly before every take).** Complete inventory of locks reachable under the guard:
1. `zombie_lock_` (plain `sync::SpinLock`, `scheduler.hpp:1742`) — taken in `release_zombie` (`scheduler_lifecycle.cpp:102-111`), leaf, released inside that scope. Canonical IRQ-off→leaf order; never leaf→global.
2. `scheduler_lock_` via `SchedulerLockGuard` — reachable **only** through `set_task_ready`'s non-UP direct-enqueue branch (`scheduler_registry.cpp:584-585`, preceded by `ENSURE(scheduler_lock_depth_own() == 0)`), which is unreachable here: `arm_wait_publish` refuses unless `Scheduler::up_cpu_count() == 1` (`scheduler_lifecycle.cpp:537-538`) and the seam's kernel-task parents clamp to CPU0 (`scheduler_queue.cpp:469-476`). If ever reached, the order is IRQ-off → global lock — identical to `terminate_err` (:96 then :116).
3. No other lock: `TaskQueue`/`ReadyQueueManager` mutations are single lock-free `IrqGuard` units (`task_queue.hpp:15-17`; `ready_queue_manager.cpp:39`, :193-200), the id table is lock-free bounded probing (`scheduler_id_table.cpp:100-131`), and `deadline_list_`/`all_tasks_` unlinks are membership-guarded plain list surgery.

**INV-SEAM-IRQ-3 (nesting safety).** `IrqGuard` is per-CPU depth-counted, saves the IF bit at construction and restores only that bit on destruction (saturating dtor; `irq_guard.hpp:135-189`, ctor :138, dtor :162-179). The fire call site has depth 0 (no `IrqGuard` anywhere in `proc_svc.cpp`), so the seam takes 0→1→0 and re-enables exactly what it disabled. A hypothetical future call site already under a guard saves `irq_was_ = false` → **no stray `sti`** (the audit-L-3 caveat recorded in `audits/audit-task-sync-v0.4.2.md:95`), and the nested take is a pure depth increment.

**INV-SEAM-IRQ-4 (boundedness — no loops / blocking / logging / allocation on the guarded path).**
- No allocation (no `MemPool`, no heap, no VMM), no IPC send, no VFS, no `Logger` on the success path, no `reschedule()`/`switch_to_task()`/park entry.
- All loops bounded: id-table probe ≤ `ID_TABLE_SIZE` (80) probes (`scheduler_id_table.cpp:103`, :123); ready/EDF ops are O(1) intrusive-list units; `find_task`/`wait_collect_spared` are single bounded probes.
- The only logging legs are pre-existing **cold refusal** legs of exactly the kind the real funnel already executes inside its own `IrqGuard`: `terminate_err` warns under its guard (:104-111), `enqueue_ready`'s admin-backstop warn (`scheduler_queue.cpp:459-462`), and `deliver_wait_status`'s fault leg (`scheduler_lifecycle.cpp:617-627`) is lock-free dmesg explicitly documented "safe from … IRQ-masked fault context" (:586-588).
- Worst-case guarded wall time: tens of microseconds (one list unlink set, one enqueue, one leaf lock, one 8-byte user store). Bounded, no I/O, no waiting.

**INV-SEAM-IRQ-5 (release identity — structural, not conventional).** The guard is written inside the existing `#if defined(CONFIG_DEBUG)` block (`scheduler_lifecycle.cpp:523` … `:575`), so it compiles out entirely in release; the header's release branch (`wait_publish_seam.hpp:56-77`) is untouched and the call site stays DEBUG-gated (`proc_svc.cpp:319-321`). Release: zero bytes, zero instructions, zero behaviour delta. The debug/release divergence is limited to IRQ masking for a bounded window inside a debug-only test seam — no control-flow, failure-policy or state-transition change (CODING_STYLE §6).

**INV-SEAM-IRQ-6 (seam semantics unchanged).** `__atomic_exchange_n` remains the sole consumer of the slot; "first fire after arm consumes the hook" and the `WaitPublishSeamScope` teardown discipline (:566-572) are unchanged; arm-time gating (UP-only, test-active-only, first-armer-wins) is unchanged.

**INV-SEAM-IRQ-7 (accounting).** No `track_*` add/remove, no PMM/MemPool/TCB/VFS/FD delta: the changed lines allocate and free nothing (ResourceTracker neutral by construction).

### Retrieval (audit check 7 — artifacts + dispositions)
| Query | Result | Disposition |
|---|---|---|
| `graphify query_graph("how does fire_wait_publish seam hook differ from the terminate_err_from_trap funnel in IRQ discipline and locks taken")` | nodes: `fire_wait_publish` (`scheduler_lifecycle.cpp:549`), `seam_full_funnel` (`test_waitpid.cpp:126`), `wake_waiting_parent` (`scheduler.hpp:906`), `dequeue_ready` (:1045), `release_zombie` (:1239), `IrqGuard` (`irq_guard.hpp:135`), `terminate_err_from_trap` (`scheduler.hpp:823`), `docs/irqguard-ledger.md`, `audits/report-20261117-m1-death-funnel.md` | **used**: confirms one fire site, the hook's call graph, and the funnel pair to cite as the fidelity reference |
| `graphify shortest_path(fire_wait_publish → terminate_err_from_trap)` | 5 hops: `fire_wait_publish → TaskControlBlock ← wait_collect_spared → find_task ← kill → terminate_err_from_trap` | **used as negative evidence**: no direct call edge between seam and funnel, so the IRQ discipline must be added *inside* the seam (it is not inherited) |
| `graphify get_neighbors(fire_wait_publish)` | only a `TaskControlBlock` reference + TU containment; no production caller | **used**: scope is the DEBUG seam only; no production call path to re-audit |
| `graphify query_graph("which locks does wake_waiting_parent take … safe under arch::IrqGuard")` | `zombie_lock_` (`scheduler.hpp:1742`), `IrqGuard`/`irq_was_`, `"IrqGuard Ledger"`, `ScopeGuard` | **used**: bounds the lock inventory in INV-SEAM-IRQ-2 |
| `obsidian_search_vault("IrqGuard")` | `audits/audit-task-sync-v0.4.2.md:95` (L-3: restore-if-enabled / stray-`sti` caveat), `audits/deep-analysis-h2-ssdeadline-v0.3.9.md:344` | **used**: the nesting / stray-`sti` caution became INV-SEAM-IRQ-3 |
| `obsidian_search_vault("waitpid")` | `audits/design-review-564-*` (sys_waitpid handler homes; #590 publication / revive-retry notes) | **used**: no spec-level contract forbids serialising the seam |
| `obsidian_search_vault("wait-publish seam #590" / "waitpid INV-WAIT" / "IrqGuard ledger" / "zombie-list spec release_zombie")` | 0 matches (vault carries `audits/*`; specs live in-tree) | **ignored (empty)**; fell back to `docs/irqguard-ledger.md:3-53` and `audits/plan-590.md:24,371-376` (the seam's approved contract) |
| `audits/plan-590.md:24,371-376` (prior approved seam plan) | seam contract: CONFIG_DEBUG-gated header under `src/kernel/test/`, single-shot exchange, RAII scope, "no production caller arms it" | **used**: the new guard must not alter any of these; it is an internal discipline change only |

## STEP-BY-STEP CHANGES

1. **`src/kernel/task/scheduler_lifecycle.cpp` — `fire_wait_publish` (:549-554): move the exchange behind the guard.** Replace the body so the guard is constructed first, then the exchange, then the call. Keep the existing signature, `noexcept` and surrounding `#if defined(CONFIG_DEBUG)` block intact. Target code:
```
void fire_wait_publish(TaskControlBlock &publisher) noexcept {
    // Issue #590 follow-up: the seam must run the hook under the same IRQ
    // discipline as the real terminate funnels — arch::IrqGuard spans
    // dequeue -> TERMINATED -> wake -> release in
    // Scheduler::terminate_err_from_trap (scheduler_terminate.cpp:254 with
    // the parent wake at :293) and in Scheduler::terminate_err (:96,
    // wake :152, release :165).  Without it a timer tick lands mid-hook
    // and parks the BLOCKED, dequeued publisher with the wake unfinished
    // and no other waker (observed partial signatures 1/0/0/0 and
    // 1/1/0/0).  The guard is taken BEFORE the exchange on purpose: the
    // exchange->cli sub-window would leave the identical wedge, only
    // narrower.
    arch::IrqGuard irq_guard{};
    auto *hook = __atomic_exchange_n(&s_wait_publish_hook, nullptr,
                                     __ATOMIC_ACQ_REL);
    if (hook != nullptr)
        hook(publisher);
}
```
2. **Do not change** anything else in that block: `s_wait_publish_hook` storage (:528), `arm_wait_publish` (:532-547 — UP + test-active gating), `WaitPublishSeamScope` ctor/dtor (:556-572), the closing `#endif` (:575). The exchange's `__ATOMIC_ACQ_REL` exchange-to-null semantics stay exactly as they are (they are the single-shot guarantee).
3. **`src/kernel/task/scheduler_lifecycle.cpp`** — no include edit: `<kernel/arch/hal/irq_guard.hpp>` is already included (:40) and `arch::IrqGuard` is already instantiated in this TU (:121, :474).
4. **`src/kernel/proc/proc_svc.cpp:319-321`** — leave unchanged. No guard at the call site: the guarantee lives in the seam (so any future call site inherits it) and the publisher's own publication order/fences (:304-318) stay untouched.
5. **`src/kernel/test/wait_publish_seam.hpp` — optional, comment-only** (only if QE confirms; see OPEN QUESTIONS): append one sentence to the §Contract paragraph (:10-14), e.g. "the hook runs under `arch::IrqGuard` (fire entry → hook return), mirroring the terminate funnel's IRQ discipline". No code, no `#else` change.
6. **Style compliance** while editing: 4 spaces, braces on the same line, ≤80 columns for code / ≤90 for comments, no new `using`, no single-character names (CODING_STYLE §10.6) — the snippet in step 1 already conforms.
7. **Nothing else.** No refactor of the hook bodies (test-owned), no wake/release reordering, no change to `release_zombie` / `wake_waiting_parent` / `set_task_ready`.

## TEST STRATEGY

- **New tests: none.** This is a dev-lane production change and `src/kernel/test/**` is read-only for the developer (test sanctity). The seam hooks, the arming scenarios and any new assertions belong to QE on `testbed`.
- **QE-owned verification (the acceptance evidence):**
  - The two partial-signature scenarios must progress to PASS — they are the only tests whose hook actually enters (`seam_full_funnel`, `test_waitpid.cpp:126-140`):
    - `waitpid_missed_wake_consumed_collects` (`test_waitpid.cpp:363`, arms at :394-395)
    - `waitpid_reverify_wildcard_collects` (`:625`, arms at :654-655)
  - Must stay green (regression floor): `waitpid_released_corpse_collects` (`:479`, miss leg `seam_funnel_no_wake` :143-152), `waitpid_retained_corpse_survives_drain` (`:549`, `seam_noop`), `waitpid_publish_order_preserved` (`:786`, read-only `seam_observe_only` — asserts BLOCKED + unqueued + pid at the seam point; the guard must not change what the hook observes), `waitpid_reverify_never_fights_the_wake` (`:702`), `waitpid_zombie_claim_is_atomic` (`:856`), `waitpid_kill_self_wakes_parent` (`:915`), `waitpid_zombie_over_new_child` (`:181`), `waitpid_two_children_sequential_reap` (`:230`).
  - Full class: `process_waitpid` 11/11 in debug (10 `JARVIS_REGISTER_TEST` + 1 `JARVIS_REGISTER_RELEASE_TEST`, `test_waitpid.cpp:959-972`; class id at `test_registry.cpp:815`) and 1/1 in release (`waitpid_cr3_switch_on_status_write`).
  - Expected seam progress counters after the fix: `g_hook_entered == g_hook_marked == g_hook_woke == g_hook_released` — no more 1/0/0/0 or 1/1/0/0.
- **Gates (developer runs; QE repeats on `testbed`):**
  - `python3 tools/meson/qemu_test.py --arch x86_64 --build debug --class process_waitpid`
  - waitpid-adjacent classes, x86_64 debug: `process_lifecycle`, `process_signals`, `core_sched`, `core_task`, `ipc_core`, `ipc_blocking`
  - off-arch legs (`process_waitpid` carries `default|*|arch_scope|any`, `tools/test-policy.tsv:22`, so all three arches must run it): `--arch aarch64 --class process_waitpid` and `--arch riscv64 --class process_waitpid` (debug); release leg `--arch x86_64 --build release --class process_waitpid`
  - three-arch builds: `tools/meson/build.sh build/meson-x86_64`, `…/meson-aarch64`, `…/meson-riscv64` (the guard is arch-neutral: `arch::IrqGuard` is HAL-shared, `irq_guard.hpp:135-189`)
  - style: `make check-style` → `Errors: 0` (warnings are non-blocking)
  - one `test-history.txt` row per class run: `<ts> <class> PASSED: <n> FAILED: <n> TIME: <measured>` — append-only, measured times only
  - release-identity spot check for the auditor: with `CONFIG_DEBUG` off, `fire_wait_publish` is the header inline no-op (`wait_publish_seam.hpp:62`) — no new release symbol, no new release byte
- **Not required:** full-suite `all` runs for this diff (single debug-only seam call site); QE may add one if the class results show any seam interaction.

## SIL 3 / SAFETY RISKS

- **[S1] A hook-only guard scope (guard inside `if (hook != nullptr)`) leaves the `exchange → cli()` sub-window** — a tick there captures the hook into a local, nulls the slot, and parks the publisher BLOCKED+dequeued with nothing runnable to wake it: the same permanent wedge, only rarer (an unreproducible-once-in-N flake). — **Mitigation:** step 1 takes the guard before the exchange, so the property is "no tick from fire entry", and the comment records why the narrower scope is rejected.
- **[S1] Debug/release behavioural divergence (CODING_STYLE §6)** — an unconditional `cli/sti` now sits in every *unarmed* debug `waitpid` block. — **Mitigation:** the code is inside the existing `#if defined(CONFIG_DEBUG)` seam block (release: zero), it changes no control flow / failure policy / state transition, the masked window is bounded (tens of microseconds, no I/O, no wait), and the release header + call site are untouched. State this explicitly in the audit hand-off.
- **[S2] A misbehaving hook (blocking, allocating, logging, rescheduling) would turn the guard into a latency hazard** — **Mitigation:** the enumerated callee/lock proof (INV-SEAM-IRQ-2/4); the seam contract already fixes the hook shape (single-shot, test-owned, no production caller — `wait_publish_seam.hpp:10-14`); optional one-line contract note (step 5).
- **[S2] A faulting hook abandons the `IrqGuard` (no dtor ⇒ per-CPU depth stays 1)** — **Mitigation:** not a new mode: the real funnel already runs `deliver_wait_status` (fault-armed) inside `terminate_err`'s guard, the dtor is saturating by design (`irq_guard.hpp:154-179`), and the fault-kill path re-establishes IF from the resumed context. No code change; QE reruns confirm no depth residue (residue would surface as a hang in the following class).
- **[S3] IrqGuard-ledger omission** — `docs/irqguard-ledger.md:3-53` audits *production* sites (20 KEEP). This change adds a DEBUG-only seam site, so no ledger row is correct — **but say so** in the audit comment so the omission is not read as a missed ledger entry (class-A justification: "test seam, UP-only, no ISR accessor, release-compiled-out").
- **[S3] Fidelity nuance (informational):** the hook order is wake → release (`test_waitpid.cpp:136-138`), matching `terminate_err` (:152 then :165) but not the trap funnel's INV-593-2 release-before-wake order (:287 then :293). Under IRQ-off on UP the parent cannot run between the two, so there is no new exposure; aligning the hook order is QE's call (test file read-only here).
- **[S3] Out-of-scope observation — file a follow-up issue, do NOT fix here:** the *publisher* side (`proc_svc.cpp:304-321`) still runs with IRQs enabled. A tick landing between `dequeue_ready(*cur)` (:310) and `waiting_child_pid = target_pid` (:312) parks the parent before publication; a later terminator's `wake_waiting_parent` then reads `waited != child.id` and returns `NO_WAITER` — the same permanent wedge. The seam guard cannot cover that window (it starts later), and it is the one window the observed signatures 1/0/0/0 and 1/1/0/0 do not explain (which is why it was never observed here). Recommend an S2 issue with the publisher-side guard decision attached.

## OPEN QUESTIONS
- QE lane only: do you want the one-line contract note in `wait_publish_seam.hpp` (QE-owned, step 5)? Default if unanswered: **no** — the rationale comment lives at `fire_wait_publish` in the dev-owned `.cpp` and the header stays untouched.
