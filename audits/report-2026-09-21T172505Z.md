# AUDIT REPORT 2026-09-21T172505Z
PATCH: audits/pending_patch.diff
FILES: src/kernel/ipc/ipc.cpp, src/kernel/syscall/syscall_handlers_ipc.cpp, src/kernel/task/task.hpp, src/kernel/test/test_ipc_extended.cpp, src/kernel/test/test_expected_counts.hpp

## FINDINGS
- [S2] src/kernel/syscall/syscall_handlers_ipc.cpp:237-240 — sys_recv_fast loop-top oversized exit returns without clearing blocked_in_recv on iterations >=2, contradicting the patch's own task.hpp contract ("cleared on every loop exit (ok / timeout / oversized)")
  WHY: flag set in iter 1 stays true across the while re-check, so a concurrent oversized-only push (SMP peer or tick-preempted sender in the resume-to-loop-top window) exits via this path leaking stale-true; that task later BLOCKED in a non-IPC wait (waitpid) then passes the new arrival gate on any message and resumes with stale wait state — the exact filed #208 defect recurring through the fix's own marker.
- [S3] src/kernel/syscall/syscall_handlers_ipc.cpp:81-84,246-249 — blocked_in_recv is stored AFTER state=BLOCKED, so a send completing between the two stores observes BLOCKED-with-no-channel and (unlike old code) does not wake a genuine recv waiter
  WHY: strictly narrower wake coverage than the pre-patch code in a 2-store window (shared post-resume recheck machinery bounds the damage to the pre-existing check-then-block gap, so hardening only); publishing the flag before the state store closes the delta window at zero cost.

## PATCH
audits/rejected_patch.diff was written: adds the missing blocked_in_recv=false on the sys_recv_fast loop-top oversized exit (S2 fix) plus flag-before-state store ordering in sys_receive/sys_recv_fast (S3 hardening).

DECISION: REJECTED
