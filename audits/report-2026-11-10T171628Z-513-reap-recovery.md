# SIL 3 Audit Report — Issue #513 M1 writer-table gap (reap-recovery republish)

- Date (UTC): 2026-11-10T171628Z
- Scope: `audits/pending_patch.diff` vs worktree at `/Users/arnold/jarvis` (branch `main`)
- Patch under review:
  - `src/kernel/task/scheduler_lifecycle.cpp:698` — adds `republish_entry_kstack(7)` after the raw `set_current_ptr` in the `Scheduler::reap_orphans` corrupt-current recovery fallback.
  - `src/kernel/task/scheduler.hpp` (SwSlots writer table, ~line 1063) — doc update listing `reap_current_recovery (scheduler_lifecycle.cpp:698, ...)` as a covered no-arm/no-switch publisher.
- Method: static read-only review. Read `pending_patch.diff`, `scheduler_lifecycle.cpp:640-767`, `scheduler.hpp:1020-1145` + `485-660` + `1137-1145`, `scheduler_switch.cpp:60-111/148-205/207-343/700-733`, `scheduler_registry.cpp:490-546/650-706`, `elf.cpp:807-886` (exec site 5). No edits, no builds, no test runs by the auditor; gate numbers below are as reported in the task request.

## Verification

1. Writer-table completeness — CONFIRMED. Raw `set_current_ptr` writers enumerated: `set_current` (switch.cpp:276, covered site 2), `set_current_task` (:169, site 1), `set_current_index` (:152, site 6), `cancel_pending_switch` (:106 + smp/tick cpu-variant paths route through the same republish), `restore_preempted_current` (:342, site 4), `exec_into_current` (elf.cpp:870, site 5), apply path `on_context_switch -> set_current_task -> republish` (switch.cpp:718-720 comment + integrity.cpp:365 call site), init/remove/unregister/idle-recreate sites (registry.cpp:501, :665, :693; lifecycle.cpp:693, :759) all target kernel-idle (`idle_task_`/`own_idle()`/`new_idle`), for which `republish_entry_kstack` is specified no-op. The reap-recovery site (lifecycle.cpp:698) was the sole non-idle raw write without a republish; the patch closes exactly that gap with unique site tag 7 (no collision with sites 1-6).
2. `republish_entry_kstack` semantics — SAFE for this call site. Publishes `(load_kstack_base, load_kstack_top)` only when `current` is magic-valid AND `is_user_` AND `kernel_stack != nullptr` AND `kernel_stack_top != 0`, else leaves the slot untouched (switch.cpp:182-205); therefore a kernel-idle recovery is a no-op and a user-task recovery publishes a valid pair. No live-RSP check and no zeroing by design (stale-slot policing is at trap time via `validate_trap_frame`), so the call cannot strand a live user task on slot==0.
3. Lock/IRQ safety — NO NEW HAZARD. `republish_entry_kstack` takes only `arch::IrqGuard`, never `scheduler_lock_`, is `noexcept`, performs two release stores; callable from the reap path (which may hold `scheduler_lock_`) without lock inversion or allocation. Matches all six pre-existing call sites' context.
4. Arch parity — PRESERVED. The added call is arch-neutral (SwSlots atomics); the rv64/aa64 arm-does-not-publish rationale (switch.cpp:712-720) and the x86_64 arm-publishes exception (switch.cpp:721-733 + hpp:1077-1080) are untouched and the doc edit restates them verbatim.
5. Style/static — CLEAN by inspection. Added lines are short, commented with issue + writer-table reference, `// site: reap-recovery` tag convention matches sites 1-6; doc comment stays inside the existing `SwSlots` writer-table block with correct `file:line` reference (`scheduler_lifecycle.cpp:698` matches the patched worktree). No `new`/`delete`/`malloc`/`free`, no global mutation outside the designated SwSlots channel, no control-flow change (straight-line insert inside the existing `magic != TCB_MAGIC` guard; null `first_ptr()` degrades to the function's documented no-op via the `c &&` guard).
6. Gates (as reported, not re-executed by this read-only audit): style Errors 0; x86_64 debug drivers 111/111; riscv64 66/66; aarch64 66/66.

## FINDINGS

- None. No [S1]/[S2]/[S3] defects found in the patch.

## Verdict

DECISION: APPROVED
