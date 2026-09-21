# AUDIT REPORT 2026-09-19T10-04-17Z
PATCH: audits/pending_patch.diff
FILES: src/kernel/syscall/syscall.hpp, src/kernel/syscall/syscall_handlers_misc.cpp, src/libc/syscall.h

## FINDINGS
- [S3] src/kernel/syscall/syscall_handlers_misc.cpp:71 — hardcoded (1<<16)|0 duplicates NEXIOS_ABI_* macros instead of including src/libc/syscall.h
  WHY: Kernel TUs lack -I src/libc so the header is unincludeable, and the staged syscall_abi drift test (ABI_VERSION == macros) is an acceptable binding anti-drift guard for this frozen v1.0 constant, documented by the in-code drift-note comment.

DECISION: APPROVED