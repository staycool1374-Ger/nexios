# AUDIT REPORT 2026-08-25T17-01-35Z
PATCH: audits/fix_delta.diff
FILES: src/kernel/arch/x86_64/hal/iopb.cpp, src/kernel/elf/elf.cpp, src/kernel/task/task.cpp, src/kernel/syscall/syscall_handlers_mmio.cpp, src/kernel/test/test_cap_mmio.cpp

## FINDINGS
- [S3] src/kernel/task/task.cpp:900 — IOPB slot initialized after memset in create(): defensive explicit init after C++ default-initialization zeroing
- [S3] src/kernel/task/task.cpp:1130 — IOPB slot initialized after memset in create_user(): defensive explicit init after C++ default-initialization zeroing
- [S3] src/kernel/task/task.cpp:1310 — IOPB slot initialized after memset in clone(): defensive explicit init after C++ default-initialization zeroing
- [S3] src/kernel/elf/elf.cpp:492 — IOPB slot initialized after memset in finalize_loaded_task(): defensive explicit init after C default-initialization zeroing
- [S3] src/kernel/arch/x86_64/hal/iopb.cpp:55-60 — iopb_claim defense-in-depth: stale non-NONE slot now checked against g_iopb_claimed bit before returning "already holds slot"; falls through to claim fresh pool entry
- [S3] src/kernel/arch/x86_64/hal/iopb.cpp:89-91 — iopb_grant_range now rejects unclaimed slots (slot NONE OR bit not set)
- [S3] src/kernel/arch/x86_64/hal/iopb.cpp:115-117 — iopb_switch_to valid_slot logic: slot loadable only if claimed; unclaimed slots trigger default-deny mask
- [S3] src/kernel/arch/x86_64/hal/iopb.cpp:143-145 — iopb_port_allowed now rejects unclaimed slots
- [S3] src/kernel/syscall/syscall_handlers_mmio.cpp:87 — arch::iopb_switch_to(*t) added after grant_range for immediate grant effectiveness; no lock nesting (each function acquires/releases g_iopb_lock independently, sequentially)
- [S3] src/kernel/test/test_cap_mmio.cpp:120-124 — test mmio_cap_install_lookup_revoke: harness root CNode created before ResourceTracker baseline capture to avoid leak counting
- [S3] src/kernel/test/test_cap_mmio.cpp:316-322 — test ioport_grant_dispatch_happy: explicit user-task fixture (is_user_=true) with termination wait and zombie drain

## PATCH
audits/rejected_patch.diff was NOT written; all findings are S3-only (defensive initializations, consistency hardening). No corrective changes required.

## DECISION: APPROVED
