# AUDIT REPORT 2026-09-17T16-00-00Z
PATCH: audits/pending_patch.diff
FILES: docs/specs/deadline.md, src/kernel/kernel.cpp, src/kernel/nexios_config.h, src/kernel/task/admission_selftest.cpp, src/kernel/task/admission_selftest.hpp, src/kernel/task/taskdefs.cpp, src/kernel/task/taskdefs.hpp, src/kernel/test/test_expected_counts.hpp, src/kernel/test/test_registry.cpp, src/kernel/test/test_sched_admission_verify.cpp, src/kernel/test/test_weak_stubs.cpp, src/lib/test.hpp

## FINDINGS
- [S3] src/kernel/task/admission_selftest.cpp:149 + src/kernel/test/test_sched_admission_verify.cpp:310 — admission_check_cpu_locked called without holding scheduler_lock_ despite the "caller must hold" contract (scheduler.hpp)
  WHY: Safe by construction here (boot runs with interrupts still disabled — no sti in kernel.cpp before the hook — single BSP over a quiescent table through an allocation-free read-only helper; the test asserts only table-state-independent outcomes, WCET_INVALID/exempt-OK), but the exemption is not stated at either call site.
- [S3] src/kernel/task/admission_selftest.cpp:153 — Logger::error %u formatters fed uint64_t args (err/util/bound)
  WHY: Matches existing tree precedent (scheduler.cpp set_affinity warn path) and values fit in practice, but a 64-bit format specifier would remove the varargs width mismatch.
- [S3] check-7 — retrieval artifacts stated on issue #24 thread, not independently verified (no gh access from auditor)
  WHY: Per task instruction, unverifiable retrieval is noted rather than failed; static checks 1-6 pass with no S1/S2.

DECISION: APPROVED