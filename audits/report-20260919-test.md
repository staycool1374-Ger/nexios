# AUDIT REPORT 2026-09-19T20:20:01Z
PATCH: audits/pending_patch.diff
FILES: mk/rules.mk, src/kernel/elf/elf_loader.cpp, src/kernel/test/test_expected_counts.hpp, src/kernel/test/test_libc_verify.cpp, src/kernel/test/test_registry.cpp, userspace/picolibc/libc_verify.c, userspace/picolibc/libc_verify.c.elf

## FINDINGS
- [S3] src/kernel/test/test_libc_verify.cpp:212 — free(0)/free(1) invoke FdTable::free (vnode_ref_dec) on /dev/tty vnodes shared WITHOUT vnode_ref_inc, contradicting the destroy_completed_tcb no-dec discipline (elf_loader.cpp:219-221); assigned tmpfs vnodes (resolve, no inc) are likewise dec'd by generic teardown (task.cpp:1862).
  WHY: Unowned refcount decrements corrupt shared vnode counters, though observably benign here (tty_close is a no-op, tmpfs close is nullptr, unlink is refcount-independent, tracker fd adds/removes balance 5/5).
- [S3] src/kernel/test/test_libc_verify.cpp:75 — resolve-fail branches skip unlink of already-staged files (image :75-78, stdin :99-105 incl. short-write, stdout :248-254), leaking tmpfs entries on those paths.
  WHY: Every staging failure should leave no residue, though these branches are practically unreachable (create-ok-then-resolve-fail) and only run on already-failing paths under VFS-touched isolation.

DECISION: APPROVED
