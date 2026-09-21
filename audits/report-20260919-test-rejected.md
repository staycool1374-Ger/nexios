# AUDIT REPORT 2026-09-19T23:02:18Z
PATCH: audits/pending_patch.diff
FILES: src/kernel/syscall/syscall.hpp, src/kernel/syscall/syscall_handlers_fs.cpp, src/kernel/syscall/syscall_handlers_posix_time.cpp, src/kernel/syscall/syscall_helpers.hpp, src/kernel/task/scheduler.cpp, src/kernel/task/scheduler.hpp, src/kernel/task/task.cpp, src/kernel/task/task.hpp, src/kernel/test/resource_tracker.cpp, src/kernel/test/resource_tracker.hpp, src/kernel/test/test_expected_counts.hpp, src/kernel/test/test_isolate.cpp, src/kernel/test/test_posix_time.cpp, src/kernel/test/test_registry.cpp, src/kernel/test/test_weak_stubs.cpp, src/kernel/time/posix_time.cpp, src/kernel/time/posix_time.hpp, src/libc/errno.h, src/libc/syscall.h, src/libc/time.c, src/libc/time.h

## FINDINGS
- [S2] src/kernel/time/posix_time.cpp:474 — sleep_arm never stores task.sleep_expiry_ns, so the sys_nanosleep EINTR remainder path always reports zero
  WHY: sleep_arm computes expiry_ns locally and arms the wheel with it but never writes task.sleep_expiry_ns, while sys_nanosleep reads cur->sleep_expiry_ns (always 0) to fill the remainder.
- [S2] src/kernel/syscall/syscall_handlers_posix_time.cpp:51 — AbiItimerspec lays out it_value before it_interval, but src/libc/time.h struct itimerspec (POSIX order) lays out it_interval first, swapping value/interval for real users
  WHY: The kernel copies the user buffer positionally into it_value/it_interval by field order, so a libc itimerspec is interpreted with value and interval exchanged.
- [S2] src/kernel/time/posix_time.cpp:56 — timespec_to_ns rejects tv_nsec == 999999999, which is a valid timespec value
  WHY: The range check uses nsec >= 999999999 instead of >= 1000000000, so the maximum valid nanosecond value fails with EINVAL.
- [S3] src/kernel/time/posix_time.cpp:454 — timerfd_slot_free treats generation 0 as a hard mismatch, so the sys_timerfd_create rollback leaks the just-allocated slot
  WHY: Generation 0 is the invalid sentinel that bump_gen never yields, yet the create-path rollback calls slot_free with gen 0, which can never match and silently skips the free.

## PATCH
audits/rejected_patch.diff was written and verified with git apply --check; it stores sleep_expiry_ns on arm, reorders the ABI itimerspec structs to POSIX interval-first, fixes the tv_nsec bound (code + test), and accepts owner-validated gen-0 as a free wildcard.

DECISION: REJECTED
