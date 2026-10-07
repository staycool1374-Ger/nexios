# AUDIT REPORT PRE-186
PATCH: (none — pre-implementation plan review)
FILES: src/kernel/debug/debug_stop.cpp, src/kernel/debug/debug_stop.hpp, tests/test_debug_syscall.cpp, tests/test_expected_counts.hpp, userspace/debugd_proto.h, userspace/debugd.c, tools/debugd/tests/test_cproto.c
## FINDINGS
- [S3] snap_budget_of helper (debug_stop.cpp): budget must be read with an atomic load (__atomic_load_n ACQUIRE, matching tgt.debugger_id style at debug_stop.cpp:273), never a plain member read.
  WHY: Budget is written by the scheduler tick path and read in ISR-fault/tick-park context, so a non-atomic cross-context read risks tearing and lacks ordering.
- [S3] debugd.c stop-reply extension (4 call sites): the added key:val pairs must use bounded snprintf checked against sizeof(g_reply) (ceiling verified: g_reply[2*sizeof(g_mem_buf)+64], debugd.c:119, with existing bound check at :184).
  WHY: The +≤48-char growth claim holds only if every new formatting site enforces the ceiling, otherwise RSP framing breaks.
- [S3] GDB interop (debugd_proto.h reply pairs): live stock-GDB session in the test strategy must confirm unknown-key tolerance for the new budget pair.
  WHY: A stock GDB that rejects unknown stop-reply keys would turn a display-only extension into a session-breaking change.
DECISION: APPROVED