# PLAN: #597 — gate the waitpid a0/RAX frame override on TRAP-FRAME parents (`is_user_`), never on a live kernel stack

Issue #597 (bug, `kernel:process` / `kernel:task`, `severity:S1`, sil3-relevant).
Lane: **developer**, branch `main` (must be confirmed with
`git branch --show-current` before any edit), base `63e1c2f95` (F11 Stage C +
testbed merge of the new QE waitpid tests; merge commit local, gates red).
Production change ⇒ full pipeline (planner → developer → Phase 3 auditor).
Test files (`src/kernel/test/**`, `test_expected_counts.hpp`, harness tables) are
**QE-owned (`testbed`)**: this plan writes no test and edits no test file.

**Fix shape (decided): keep the Issue #221 override for parents that resume
through a real trap frame (`is_user_ == true`), skip it for kernel-task parents
(`is_user_ == false`), whose waitpid return path is a direct C++ return and
whose `task_stack_ptr()` is the LIVE kernel stack.** One predicate
(`p->is_user_`), applied at the three copy-paste sites of the same hunk. No new
lock, no new config knob, no new syscall, no new arch branch, no behaviour
change on the user leg.

---

## RETRIEVAL (audit check 7 — artefacts + dispositions)

| Query | Result | Disposition |
|---|---|---|
| `graphify query_graph("wake_waiting_parent task_stack_ptr trap frame RAX override sys_waitpid", bfs 3)` | `wake_waiting_parent` (scheduler.hpp:868), `sys_waitpid` (syscall.hpp:298), `rax` (task.hpp:273 + hal/context.hpp:39), plus #546 tick-watchdog noise (community 372) | **used**: confirmed the wake is declared in scheduler.hpp and the syscall side is a thin delegation; the watcher nodes were **ignored** (unrelated community, no edge into the wake path) |
| `graphify query_graph("deliver_wait_status scheduler_lifecycle frame[0] child pid write parent kernel stack", dfs 3)` | `deliver_wait_status`/`scheduler_lifecycle.cpp` (community 202), `scheduler_lock_depth_own`, `drain_zombie_list`, #513 trap-owner nodes; 523 nodes, over-budget | **used** to place delivery + wake in one TU; **ignored** the breadth (DFS too wide) and re-read the file directly |
| `graphify shortest_path("wake_waiting_parent" → "task_stack_ptr")` | **no node** — `task_stack_ptr` is an inline helper in `scheduler_integrity.hpp`, absent from the graph | **graph lag**: the exact question the fix turns on (what pointer means) was answered from the **files** (`scheduler_integrity.hpp:41-53`, per-arch `ArchContext`), per the staleness doctrine — never skipped, just superseded |
| `obsidian_search_vault("waitpid")` | `audits/design-review-588-code-r1.md:19` ("no waitpid/wake section exists ⇒ header doc-comments are the record"), `design-review-590-code-r1.md:10` (INV-WAIT-4 publish order + INV-WAIT-3 fence pair), `design-review-593-*` (trap funnel) | **used**: invariant records go into `scheduler.hpp` doc-comments; `docs/specs/scheduler.md` is deliberately not touched (no section to amend) |
| `obsidian_search_vault("INV-WAIT")` | `design-review-588-r1.md:18` (INV-WAIT-2), `design-review-590-code-r1.md:10,24` (INV-WAIT-3/4/6) | **used**: the gate is placed *after* the ready/latch/publish steps so INV-WAIT-3/4/5/6 and the INV-WAIT-2 pre-cert contract are untouched |

---

## CLAIM VERIFICATION IN-TREE (the pre-conditions this plan builds on)

| Claim | Tree truth re-read this session |
|---|---|
| A kernel-task parent **never blocks in a trap** for waitpid | `ProcSvc::waitpid` publishes status→dequeue→BLOCKED→pid (`proc_svc.cpp:304-318`) and then returns `SYS_ERR_GENERIC_FAIL` **immediately** for a non-user caller (`proc_svc.cpp:390-391`); the in-trap `reschedule()` + `sti/hlt` loop (`proc_svc.cpp:397-404`) is user-only. A kernel caller's waitpid return is therefore a **direct C++ return** (`Syscall::handle` → caller), not a frame pop. **Claim CONFIRMED.** |
| Kernel callers already observe the pid **without** the frame patch | the wake clears the publication (`scheduler_lifecycle.cpp:707` `p->waiting_child_pid = 0`) and delivers the real exit code through the armed helper (`:667-668` → `deliver_wait_status`, which writes the parent's true status slot); every kernel-task caller keys on that state, never on a return slot: `test_waitpid.cpp:55-66` (`self->waiting_child_pid == 0`), `test_scheduler.cpp:43-55` (`state` leaves BLOCKED), `test_task_lifecycle.cpp:56`, `test_preemption.cpp:51`, `test_syscall_fuzz.cpp:250`. There is **no production kernel-task waitpid caller** (grep `SyscallNumber::WAITPID` = tests only). **Claim CONFIRMED — the kernel leg loses nothing when the override is skipped.** |
| A user parent resumes through a real trap frame | x86 syscall = `int 0x80` ISR: `regs[0] = syscall_handler(...)` (`kernel.cpp:2762-2766`) then iretq through that frame; aarch64 publishes `switch_away_from_terminating` for the BLOCKED caller in the syscall path (`kernel.cpp:2901-2916`) and `syscall_entry.S` applies it at its epilogue. So `frame[0]` (x86/aarch64) / `frame[9]` (riscv, `OFF_A0==72`) is a genuine return-value slot for `is_user_ == true`. **#221 intent is preserved by keeping the write on this leg — byte-identical.** |
| `is_user_` is the right discriminator | `task.hpp:669-674`: "v0.4.0 MP-1: every task — kernel AND user — now owns a private PML4, so `page_table_ != 0` is no longer a valid user-task discriminator. **This flag is the authoritative one.**" Public member (public section opens at `task.hpp:563`; cross-TU precedent `syscall_handlers_fs.cpp:45`). Written once at birth (`task.cpp:396` ctor / `create_user` / `clone` / `spawn_thread`) ⇒ a plain read is race-free. |
| The existing `if (task_stack_ptr(p))` guard does **not** protect kernel parents | `task_stack_ptr` = `context.rsp` / `context.sp_el0` / `context.sp` (`scheduler_integrity.hpp:41-53`), and **every** kernel-task birth path stores a real stack pointer there: `task.cpp:1534` (create), `:1754` (create_user), `:2285` (clone), `:2762` (spawn_thread); the switch-out overwrite then makes it the **live** stack. ⇒ the pointer is non-zero for kernel parents ⇒ the write happens. |
| …and therefore **aa64/rv64 are NOT safe by construction** (issue text says "pointer reads 0") | contradicted in-tree: `create()` sets `context.sp_el0 = stack` (`task.cpp:1551`) and `context.sp = stack` (`:1566`) for kernel tasks, both non-zero. The write therefore already executes on those arches too and lands at `sp[0]`/`sp[9]` of a **live** kernel stack — it simply happens to hit spilled locals rather than a return address, which is why no fault was observed. This is why the fix must be an **arch-neutral** predicate, not a null-pointer check, and why the same predicate must go to the twins (below). |
| THREE sites, not one | `scheduler_lifecycle.cpp:690-705` (wake_waiting_parent), `syscall_handlers_misc.cpp:204-217` (sys_exit inline wake, `TASK_STACK_PTR`), `syscall_handlers_thread.cpp:246-255` (sys_thread_exit inline wake, `THREAD_JOIN_STACK_PTR`). Identical copy-paste hunk, identical defect class. |

---

## AFFECTED FILES

- `src/kernel/task/scheduler_lifecycle.cpp` — `Scheduler::wake_waiting_parent`: add the `p->is_user_` conjunct to the frame-patch gate (:690) and rewrite/extend the Issue #221 comment into the #597 record (INV-597-1). Nothing else in the block moves.
- `src/kernel/syscall/syscall_handlers_misc.cpp` — `Syscall::sys_exit` inline wake: same predicate on its patch branch (:204), one-line comment cross-referencing #597 + the lifecycle site.
- `src/kernel/syscall/syscall_handlers_thread.cpp` — `Syscall::sys_thread_exit` inline wake: same predicate on its `if (frame)` guard (:249), one-line comment cross-reference.
- `src/kernel/task/scheduler.hpp` — doc-comments only: `wake_waiting_parent` (:837-868) gains INV-597-1 ("the a0/RAX override is written only for a `is_user_` parent; a kernel parent has no trap frame — its `task_stack_ptr` is its live stack"); zero code change.
- **Not touched (deliberate):** `docs/specs/scheduler.md` (no waitpid/wake section exists — #588 audit is the record), all `src/kernel/test/**` + `test_expected_counts.hpp` (QE-owned), `nexios_config.h` (no knob), `gen.mk` (no new TU), `scheduler_integrity.*` (no accessor change), `ProcSvc::waitpid` (publication/in-trap loop untouched).

---

## INVARIANTS & CONCURRENCY BOUNDARIES

- **INV-597-1 (new, the whole fix):** the a0/RAX override is written **iff** the
  woken parent resumes through a trap frame — `p->is_user_ == true` (x86/aarch64:
  `frame[0]`, riscv64: `frame[9]`). A `is_user_ == false` parent is never written:
  its `task_stack_ptr(p)` is a **live** kernel stack (or the harness live RSP,
  `scheduler_snapshot_fields.cpp:111`), and no word there is a return-value slot.
- **INV-221-1 (preserved):** the user-waitpid path is byte-identical — `is_user_`
  is true for exactly the parents #221 targeted, so the emitted instruction
  stream for that leg does not change (only one `test`/branch is added ahead of it).
- **INV-597-2 (writer identity):** `is_user_` is write-once at birth, before the
  TCB is published to any registry/queue ⇒ a plain (non-atomic) read in the wake
  path is race-free, consistent with the existing cross-TU reads
  (`syscall_handlers_fs.cpp:45`, `syscall_handlers_debug.cpp:120`). No new atomic
  is introduced (CODING_STYLE §11.6 is satisfied by the single-writer-before-publish
  contract).
- **INV-WAIT-2/3/4/5/6 unchanged:** the gate sits **after** `deliver_wait_status`,
  the `wait_collect_pending_` latch, `dequeue_ready`/`set_task_ready` and before
  the `waiting_child_pid = 0` clear — no reorder, no added fence, no added lock.
  Publish/consume precedence (`proc_svc.cpp:322-336`) is untouched.
- **Concurrency boundary:** the edited region runs in IRQ-masked / possibly
  ISR-masked-wake context (`scheduler_terminate.cpp:152`, `:293`,
  `scheduler_watchdog.cpp:320`, `:478`, and inside `sys_exit`/`sys_thread_exit`).
  The change adds **no** lock, **no** allocation, **no** logging, **no** IRQ-window
  widening — it removes a store from a path that is the only one currently
  mutating foreign memory there.
- **Dead-state discipline (§12.1):** the predicate reads `is_user_` on a parent
  already resolved by `find_task()`/`task_at()` and matched on a live
  publication; no new pointer is stored, no generation tag is needed.
- **Debug/release parity (CODING_STYLE §6):** pure predicate, no `#ifdef` added —
  identical control flow in both builds.
- **ResourceTracker:** no resource is allocated/released by this change ⇒ no
  new counter, no `test_isolate` change.

---

## STEP-BY-STEP CHANGES

1. **`src/kernel/task/scheduler_lifecycle.cpp` (site 1, primary).** In
   `Scheduler::wake_waiting_parent`, in the `if (p->state == TaskState::BLOCKED)`
   block, change the guard
   `if (task_stack_ptr(p)) {` → `if (p->is_user_ && task_stack_ptr(p)) {`.
   Keep `dequeue_ready`/`set_task_ready` order, the `frame` reinterprets and the
   `#if defined(CONFIG_ARCH_RISCV64) frame[9] / #else frame[0]` split exactly as
   they are. Rewrite the comment above the guard to state: (a) the override is
   only valid for a parent that resumes through a trap frame; (b) a kernel-task
   parent returns directly from `ProcSvc::waitpid` (`proc_svc.cpp:390-391`) and
   observes the result via the cleared publication + delivered status, so it
   needs no patch; (c) its `task_stack_ptr` is the **live** kernel stack, and the
   store there is what planted `child.id` on a live return address (x86 GDB:
   `ctx_rsp` 200 B inside the live region, slot held `0x403c7e`, fault fetch
   `pc = 0xa`); (d) `is_user_` is the authoritative discriminator
   (`task.hpp:669-674`); (e) INV-597-1 + "issue #597". Comment lines ≤ 90 chars.
2. **`src/kernel/syscall/syscall_handlers_misc.cpp` (site 2, `sys_exit`).** In the
   inline wake loop, change `if (TASK_STACK_PTR(p)) {` → `if (p->is_user_ &&
   TASK_STACK_PTR(p)) {`; leave the `frame`/`#if riscv64` body untouched; replace
   the "Issue #221" comment prefix with one line naming #597 + the same
   trap-frame rule, pointing at `wake_waiting_parent` for the full rationale.
3. **`src/kernel/syscall/syscall_handlers_thread.cpp` (site 3,
   `sys_thread_exit`).** Same hunk: change `if (frame) {` → `if (p->is_user_ &&
   frame) {` (the `reinterpret_cast` of `THREAD_JOIN_STACK_PTR(p)` stays, so the
   null path is preserved), plus the same one-line #597 comment.
4. **`src/kernel/task/scheduler.hpp` (documentation only).** In the
   `wake_waiting_parent` doc-comment, add one paragraph: "Issue #597 /
   INV-597-1: the a0/RAX override is written only for a `is_user_` parent (it
   resumes through a trap frame). A kernel-task parent never does — its waitpid
   return is a direct C++ return and its `task_stack_ptr` is its live kernel
   stack — so the store is skipped there (all arches)." No code change; do not
   touch `docs/specs/scheduler.md`.
5. **Do not expand scope.** No refactor of the three duplicated loops, no
   helper extraction, no `task_stack_ptr`-validity API, no new invariant-enforcing
   ENSURE/ASSERT on a reachable path (CODING_STYLE §5.1), no counter/dmesg code.
   The three one-line predicates are the whole production diff (≈ 4–6 changed
   lines + comments).
6. **Verify (in this order, all before handing the diff to the auditor).**
   `git branch --show-current` == `main`; `git diff --stat` shows only the four
   files above and **no** `src/kernel/test/**`;
   `tools/meson/build.sh --arch x86_64`, `--arch aarch64`, `--arch riscv64` (three
   green builds — cross-arch parity is mandatory);
   `make check-style` ⇒ **Errors: 0**; then the class runs of TEST STRATEGY with
   one `test-history.txt` row appended per invocation (measured TIME only).

---

## TEST STRATEGY

**Developer lands no test** (test sanctity: `src/kernel/test/**` and
`test_expected_counts.hpp` are QE-owned on `testbed`). The plan hands QE the
sketch below; dev proves the fix with the existing classes.

**New tests (QE sketch — stub-first, names are proposals):**

- `process_waitpid/waitpid_kernel_parent_sys_exit_no_clobber` — kernel-task
  parent (prio 20) blocks in the real waitpid publication; a kernel-task child
  exits via `Syscall::handle(SYS_EXIT)` (site 2). Asserts: parent's status slot
  = 42, parent leaves BLOCKED with `waiting_child_pid == 0`, child absent from
  `all_tasks_`, **and the 8-byte word at the parent's `task_stack_ptr` captured
  before the child exits is unchanged after the wake** (deterministic
  direct proof of INV-597-1), suite still scheduling (no #PF).
- `process_waitpid/waitpid_kernel_parent_thread_exit_no_clobber` — same shape
  through `SYS_THREAD_EXIT` (site 3) with a joinable thread child.
- `process_waitpid/waitpid_user_parent_pid_still_delivered` — EL0 parent (the
  `test_fpu_context_user` SYS_WAITPID shape) asserts the #221 contract is
  intact: return value / a0 slot == child id (not −1), per arch offset
  (x86/aarch64 `frame[0]`, riscv64 `frame[9]`).
- Count row: `process_waitpid` 11 → 14 in `test_expected_counts.hpp`
  (`src/kernel/test/test_expected_counts.hpp:87`) — QE-owned row, QE bump.

**Existing classes to run (rationale in brackets):**

- `process_waitpid` (x86 debug, 11) — the 8 new QE tests are the regression that
  reproduces the crash window (`seam_full_funnel` fires `wake_waiting_parent`
  inside the publication window with a kernel-task parent,
  `test_waitpid.cpp:126-140`); currently TIMEOUT. **Primary gate.**
- `scheduler_core` (39) — hosts `scheduler_waitpid_wakes_parent`
  (`test_scheduler.cpp:306`, kernel-task parent) and the #513 slot-safety /
  #545–#578 park classes that share the wake path.
- `scheduler_preemption` (13) — kernel-task parents blocking in the real WAITPID
  under preemption (`test_preemption.cpp:44-59`).
- `process_lifecycle` (16, **all three arches**) — kernel-task waitpid child
  block leg (`test_task_lifecycle.cpp:56`) + child-table/clone users; this is the
  cross-arch kernel-parent evidence for aa64/rv64, where the same store was
  previously executed (latent corruption).
- `task_fpu` (8, **aarch64 + riscv64 only**) — real EL0 `SYS_WAITPID` parent
  (`test_fpu_context_user.cpp:349-500`): the cross-arch proof that the
  `is_user_` leg still delivers the pid (#221 intact off-x86).
- `proc_elf` — fork/exec/waitpid production shape and reap linkage (#221/#588
  callers), x86 debug.
- `safe` (157) — curated debug smoke incl. scheduler/process rows.
- Release leg: `process_waitpid` is not in the release subset by policy
  (`tools/test-policy.tsv` decides); run the release `selftest`/`safe` row for
  the build-type parity check anyway.

**Command shape (only these entry points):**
`python3 tools/meson/qemu_test.py --arch x86_64 --build debug --class process_waitpid`
(and one invocation per class/arch above), `bash scripts/run_all_classes.sh <arch>
<build>` only for a release gate. **One `test-history.txt` row per invocation**,
measured TIME, `STATUS:` on any abnormal termination.

---

## SIL 3 / SAFETY RISKS

- [S1] **Fixing only `wake_waiting_parent` leaves the S1 class live.** `sys_exit`
  and `sys_thread_exit` carry the byte-identical hunk (`syscall_handlers_misc.cpp:204-217`,
  `syscall_handlers_thread.cpp:246-255`) and are reachable whenever a kernel-task
  child exits through the syscall instead of `Scheduler::terminate` — same
  live-stack store, same #PF shape. Mitigation: **all three sites carry the
  predicate in this change** (steps 1–3); an audit finding of "twins untouched"
  is a REJECT.
- [S1] **Wrong discriminator.** `page_table_ != 0` is invalid since MP-1
  (`task.hpp:669-674`: every task owns a PML4) and would re-open the bug.
  Mitigation: `is_user_` only, quoted with its in-tree authority in the comment.
- [S1] **Over-broad removal.** Deleting the override outright (alternative (c))
  discards #221's user-leg guarantee and needs new return plumbing — rejected;
  the plan keeps the user leg byte-identical and only removes the *kernel* store.
- [S2] **Latent kernel task with a genuine trap frame.** If a future kernel-mode
  caller ever enters waitpid through `int 0x80`/`vectors.S`, gating on
  `is_user_` would drop a legitimate override. Today unreachable:
  `proc_svc.cpp:390-391` returns before any in-trap wait for non-user callers and
  no in-tree kernel caller issues a trap syscall. Mitigation: the comment states
  the precondition ("a kernel parent's waitpid return is a direct C++ return"),
  so the next change that breaks it trips over a documented contract.
- [S2] **Cross-arch side effect must be stated, not assumed.** The gate *changes*
  the aa64/rv64 kernel-parent behaviour (the write previously did execute —
  `task.cpp:1551/1566` — into a live stack). That is the correction, but the
  auditor needs the evidence: `process_lifecycle` (16) on aa64/rv64 and
  `task_fpu` (8) on aa64/rv64 are mandatory rows; no user-leg instruction change
  (`is_user_ == true` ⇒ unchanged).
- [S2] **Slot-verification alternative rejected, and the reason recorded.** Proving
  that `task_stack_ptr(p)` points at a frame base would require a per-arch frame
  derivation plus a kernel-stack-range walk on an IRQ-masked wake path (no locks
  allowed there) — more code and more risk than one predicate, with no cheap
  witness word available. Recorded in the comment so it is not re-litigated.
- [S3] **Invariant erosion on the wake path.** The patch is placed after
  `deliver_wait_status`/latch/`set_task_ready` and before the pid clear — no
  reorder, no fence change, no lock; INV-WAIT-3/4/5/6 remain as audited in #590.
  Failure direction is skip-the-store (strictly safer than today).
- [S3] **Style/robustness hygiene:** comment ≤ 90 chars, no magic numbers, no
  single-char names beyond the allowlist (`p`), no `#ifdef` divergence between
  debug and release.

---

## OPEN QUESTIONS

- none
