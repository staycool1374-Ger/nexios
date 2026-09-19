# AUDIT REPORT 2026-09-19T18:26:04Z
PATCH: audits/pending_plan-77.md (plan review — no code diff yet)
FILES: src/services/shell.cpp, src/kernel/elf/elf.cpp, src/kernel/elf/elf_loader.hpp, src/kernel/elf/elf_loader.cpp, src/kernel/task/scheduler.hpp, src/kernel/task/scheduler.cpp, src/kernel/kernel.cpp, src/kernel/test/test_libc_verify.cpp, docs/specs/syscall-abi-picolibc.md

## FINDINGS
- [S2] src/kernel/task/scheduler.hpp:151 — plan names `admission_check_locked` as the shell/rc activation call, but its contract requires the caller to already hold `scheduler_lock_`, which shell context does not.
  WHY: Invoking a lock-held helper without the lock races the task tables, and taking the lock in shell to satisfy it creates TOCTOU plus §11.1 lock-across-reschedule hazard — the sole legal entry is lock-taking `add_task_err` (scheduler.cpp:4683).
- [S2] src/kernel/kernel.cpp:218 — `/etc/rc` runner is a second direct `elf::load` + `add_task` activation site, but the plan scopes admission/denial work to shell UX and lists only failure-policy/arg-syntax for kernel.cpp.
  WHY: Fixing only `cmd_runelf` leaves a boot-time path that activates tasks through the warn-only legacy `add_task` (scheduler.hpp:128-130) with no admission, defeating the plan's own S1 closure.
- [S3] src/kernel/task/scheduler.cpp:4683 — plan's step 6 mandates a "memory budget" gate that does not exist in the scheduler (admission is Liu-Leyland + WCET only).
  WHY: A step referencing a nonexistent check is not implementable as written — the plan must either define the new gate or drop it.
- [S3] src/kernel/elf/elf_loader.cpp:183 — plan's denial policy (retain-for-retry vs release) omits the destructor choice for a denied `take_completed` TCB (`destroy_completed_tcb` vs `cleanup()`+`delete`, cf. `reset()` :246 using the latter on a never-added TCB its own comment calls unsafe).
  WHY: Per CODING_STYLE §12.4 single-ownership, a denied-but-never-added TCB freed by the wrong destructor leaks tracker counts or double-unregisters, so the blocking decision must name the destructor pair.
- [S3] src/kernel/elf/elf.cpp:107 — plan correctly pins DYNAMIC/INTERP rejection as S3, confirmed load-bearing since `validate_segment` returns true for non-PT_LOAD and `validate_header` checks no `e_type`.
  WHY: Without an explicit `ET_EXEC`-only gate, static-only loader silently accepts dynamic images that then die at rip 0, misattributed to the #75 fault.
- [S3] src/services/shell.cpp:2048 — plan does not pin concurrent direct-`load` vs background-`ElfLoader` (`ALREADY_LOADING`) interaction while converge-vs-dual remains open.
  WHY: Until §3.1 converges, a direct load racing an in-flight background load needs a defined serialization/test pin or the dual path reintroduces unowned-handoff risk.

DECISION: REJECTED
