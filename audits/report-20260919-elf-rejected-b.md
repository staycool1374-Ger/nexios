# AUDIT REPORT 2026-09-19T18:35:35Z
PATCH: audits/pending_plan-77.md v3 (plan review — no code diff yet)
FILES: src/services/shell.cpp, src/kernel/elf/elf.cpp, src/kernel/elf/elf_loader.hpp, src/kernel/elf/elf_loader.cpp, src/kernel/task/scheduler.hpp, src/kernel/task/scheduler.cpp, src/kernel/kernel.cpp, src/kernel/test/test_libc_verify.cpp, docs/specs/syscall-abi-picolibc.md

## FINDINGS
- [S2] src/kernel/kernel.cpp:218 — plan migrates `/etc/rc` onto the background ElfLoader but never moves `ElfLoader::ensure_task()` (:301, after rc :158-230 + daemon wait) before the rc runner, so rc `request_load` queues VALIDATING with no loader task to service it and the boot-time sync-wait strands init.
  WHY: An activation path whose servicing task does not exist at request time deadlocks the boot sequence it was migrated to serve.
- [S3] src/kernel/elf/elf_loader.cpp:246 — plan step 3 mandates `destroy_completed_tcb` for denied never-added TCBs yet never states that `reset()` (:246 `cleanup()`+`delete`) must be reconciled to the same destructor.
  WHY: Leaving `reset()` on the unsafe pair preserves the exact single-ownership violation the v1 audit blocked.
- [S3] src/kernel/elf/elf_loader.cpp:111 — plan step 3 orders "new load frees retained FIRST" without stating before-vs-after validation of the new request, so a bad new path could destroy a good retained image.
  WHY: Free-before-validate on a fallible request turns a rejected load into data loss of the retained image.
- [S3] src/services/shell.cpp:2048 — plan asserts shell/rc paths are loader-migratable but never addresses `initrd::find` vs loader `vfs::resolve`/`syscall_path_open` (elf_loader.cpp:115,313) path-resolution parity for initrd-resident images.
  WHY: A migration across different name-resolution domains needs an explicit parity statement or rc/shell loads fail FILE_NOT_FOUND post-converge.
- [S3] audits/pending_plan-77.md:30 — step numbering incoherent (two steps numbered "5"), SIL S1 cites "step 6" for the admission call (step 6 is the struck memory budget; admission is step 2), test strategy cites "step 3" for argv (argv is step 4).
  WHY: Stale step cross-references make the plan unimplementable as written and misdirect the S1 closure claim.
- [S3] audits/pending_plan-77.md:7 — AFFECTED FILES still carries undecided v2 language ("EITHER direct `elf::load` (stay) OR migrate … (decide §3.1)") and "+ memory budget" after converge was decided and the budget gate struck.
  WHY: A decided plan retaining decide-later scope language re-opens the closed questions at implementation time.

DECISION: REJECTED
