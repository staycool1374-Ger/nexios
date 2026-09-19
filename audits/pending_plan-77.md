# PLAN: runelf end-to-end user ELF execution (#77) — wire take_completed into run/schedulable + admission + argv + acceptance

Source: issue #77 + docs/specs/elf-loader.md + verified in-tree implementation (2026-09-19).
Status: PLAN REVIEW (no code changed; audits/pending_patch.diff does not exist for #77).

## AFFECTED FILES (decided)
- `src/services/shell.cpp` (`cmd_runelf`) — migrate onto background `ElfLoader` (request + completion handoff); `add_task_err` admission + denial UX; argv deferred with loud error (currently dropped silently).
- `src/kernel/elf/elf.cpp` (`load`, `finalize_loaded_task`, `setup_user_stack`) — handoff assert `tls_base_==0`; `ET_EXEC`-only `e_type` gate; fix the #75 rip-0 death as acceptance gate. No argv plumbing (deferred).
- `src/kernel/elf/elf_loader.{hpp,cpp}` — `take_completed()` handoff contract (priority/period/deadline set by caller); `reset()` reconciled to `destroy_completed_tcb`; validate-before-free order on new requests; existing reset/wait discipline otherwise.
- `src/kernel/task/scheduler.{hpp,cpp}` — `add_task_err` invoked at activation on this path (exists, never called here). No memory-budget gate (struck).
- `src/kernel/kernel.cpp` (`/etc/rc` runner + boot order) — hoist `ensure_task()` before rc; per-line failure policy; `add_task_err` admission; path parity with shell resolution.
- `src/kernel/test/test_libc_verify.cpp` (issue #75, in flight) — acceptance vehicle (real dispatch through the wired path); plus denial/argv-deferral pins; owned by `proc_elf` aggregate.
- `docs/specs/syscall-abi-picolibc.md` §7/§13 — TLS-bootstrap interim rule (unset + thread-local-storage=false) vs #171 crt0; record, no code.

## INVARIANTS & CONCURRENCY BOUNDARIES
- Loader single-owner cleanup (elf-loader.md §5): only the loader task mutates IDLE→DONE/FAILED/CANCELED; shell performs exactly request_load/request_cancel; cancel observed per chunk; `load_generation_` drops stale wakes.
- Handoff ownership: DONE TCB owned by singleton until `take_completed()`; caller sets settled priority/base_priority/period/deadline BEFORE `add_task`; never `add_task` then mutate.
- Admission BEFORE activation via lock-taking `add_task_err` at ALL activation sites (shell + rc), fail-closed with denial code; denial leaves tables/queues/tracker untouched per `add_task_err` contract.
- No lock across reschedule (§11.1): loader yields per chunk with spinlock released, no IrqGuard held (existing contract, unchanged).
- `tls_base_==0` on every handoff (assert, not assume); FS_BASE publish sends 0 (loads nothing); no TLB/PCID action beyond loader-mapped pages.
- Snapshot isolation: loader TCB is baseline-owned; tests call `reset()` first + `wait_loader_idle()` last, never leave a load in flight (elf-loader.md §9).
- BRK red zone MP-2.2: heap bounded by STACK_VADDR, `sys_brk` rejects at/over; program allocations small.
- TCB memset discipline (LEARNINGS #74): any new field set explicitly at all 4 zeroing sites + exec reset + snapshot parity.

## STEP-BY-STEP CHANGES
1. CONVERGE (DECIDED 2026-09-19): shell `runelf` AND the `/etc/rc` runner both migrate onto the background `ElfLoader` + `take_completed` path. Direct `elf::load` gains no new callers and keeps no activation role. Boot order (hard requirement): hoist `ElfLoader::ensure_task()` BEFORE the rc runner in `init_task_main` (currently kernel.cpp:301, after rc :158-230 + daemon wait) — a background-loader rc cannot dispatch before its servicing task exists; rc performs request + sync-wait on completion per load (not fire-and-forget).
2. Activation entry (BOTH sites, converged path): after `take_completed()`, set settled priority/base_priority/period/deadline, then lock-taking `Scheduler::add_task_err` (scheduler.cpp:4683) — never `admission_check_locked` directly, never legacy void `add_task`. Denial returns a `scheduler_errors.hpp` code to shell UX + dmesg (+ boot log for rc), never ENSURE.
3. Denial policy (DECIDED 2026-09-19): RETAIN-FOR-RETRY — a denied image stays singleton-owned; a NEW request first validates its own path, THEN frees the retained image via `destroy_completed_tcb` (never free-before-validate: a bad new path must not destroy a good retained image). `reset()` is reconciled to `destroy_completed_tcb` like every other never-added path — never `cleanup()`+`delete` on never-added (its current :246 form). At most one retained image exists.
4. `elf.cpp`: handoff assert `tls_base_==0`; add `ET_EXEC`-only rejection (`e_type` gate at `validate_header`: DYNAMIC/INTERP fail with a clear shell/dmesg error, never silent rip-0 death). Argv: DEFERRED (DECIDED 2026-09-19) — `runelf` with args returns a loud "arguments not yet supported" error instead of silently dropping them; `setup_user_stack` keeps its null-arg form; full argv plumbing is a follow-up issue with stack-image fuzz tests.
5. Fix #75 rip-0 death as acceptance gate (fault-context dump at `deliver_signal_to_user` located; GDB proved syscalls fire pre-death).
6. `/etc/rc`: per-line failure policy (skip-and-continue with boot log) + `add_task_err` admission with shell-equivalent denial UX; lines carrying args are rejected loudly per the argv deferral. Path parity (hard requirement): the converged loader opens via `syscall_path_open`/vfs while shell `runelf` resolves via `initrd::find` — state ONE resolution domain (vfs, with initrd mounted before first use) or add an explicit mapping step; acceptance pins initrd-resident loads through the converged path (no post-converge FILE_NOT_FOUND regression).
7. Memory budget: STRUCK — no such scheduler gate exists (admission is Liu-Leyland + WCET only); a memory-budget gate is future work, not this issue.
8. Tests: `libc_verify` via wired path (or `runelf_*` class) in `proc_elf` + denial-path pin (TABLE_FULL and ADMISSION_DENIED fail closed, retained image freed by next load per step 3) + argv-deferral pin (args → loud error, never silent drop); full gates; test-history rows.

## TEST STRATEGY
- Acceptance: hosted-C program (printf/malloc/scanf) runs to EXIT(0) with correct markers via the WIRED path (not test-only scaffolding), zero ResourceTracker delta.
- New/changed: `runelf_*` or extended `libc_verify`; `proc_elf` aggregate count bump; neighbours `elf_loader`, `syscall_core`, `shell_interaction`, `process_rlimit`.
- Negative pins: denial path (admission over bound fails closed, image state defined); corrupt/truncated ELF (existing loader taxonomy); cancel mid-load (existing).
- Non-goals as tests: shared-lib resolution, dynamic binaries (rejection pinned, not execution), argv round-trip (deferred per step 4).

## SIL 3 / SAFETY RISKS
- S1 — Unadmitted RT task: activation without Liu-Leyland check breaks schedulability claims. Closed by mandatory `add_task_err` call (step 2) + denial test.
- S1 — Stale-arm strand (LEARNINGS #74: terminate re-arms off dying current): any new arm-then-terminate sequence on this path must self-block, never return-to-trampoline.
- S2 — Zombie/double-terminate on the exit path: apply the test_syscall.cpp:651-655 gate (clean EXIT unqueued vs signal-death queued) wherever the wired path tears down.
- S2 — Orphaned background load: reset-first/wait-idle-last discipline on every path including failure (spec §9).
- S2 — argv stack corruption: deferred (step 4) — no argv reaches `setup_user_stack` until the follow-up issue with stack-image fuzz tests; the deferral pin asserts loud rejection.
- S3 — Static-only silently loads broken dynamics: DYNAMIC/INTERP images must be REJECTED with a clear error (not silently ignored-then-die at rip 0); add validation + message.
- S3 — Serial-loopback/MCR leakage, initrd staging bloat, tmpfs 64 KiB test-staging cap (strip), x86_64-only execution (other archs warn-skip).

## OPEN QUESTIONS
- none (all three decided 2026-09-19: converge / retain-for-retry with new-load-frees / argv deferred-loud). Plan implementable as written once the #75 gate (step 5) clears.
