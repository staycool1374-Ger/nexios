# AUDIT REPORT 2026-11-17T23:00:00Z (M3 re-audit r2)
PATCH: unstaged `git diff -- src/kernel/ipc/ipc.cpp` vs audited tree (staged M3 delta, base 95e02b07f) + prior report audits/report-20261117-m3-direct-switch-token.md
FILES: src/kernel/ipc/ipc.cpp

## SCOPE
Re-audit of the single S2 from the prior M3 audit (per-task-lifetime saturation of
`ipc_direct_attempts` vs documented per-wait budget). M1 delta already APPROVED and
untouched. The unstaged delta is 11 insertions, 0 deletions: three
`cur->ipc_direct_attempts = 0` resets + comments at the three wait entries
(send_sync_err ~890, send_sync_bounded_err ~1173, reply_recv_err ~1405).
No other file differs vs the audited tree.

## FINDINGS
- [S3] docs/specs/ipc.md:78 — names counter `ipc_reply_dropped_count` that exists nowhere in code (carried over, unchanged by this delta)
  WHY: phantom counter misleads instrumentation; docs-only, no code defect.
- [S3] src/kernel/ipc/ipc.cpp:deliver_with_reply_rule — plain read of dest.generation under only the queue lock (carried over, untouched by this delta)
  WHY: benign (mismatch resolves to Pushed untokened, waiter retries via tick recheck) and matches the pre-existing dest-gone pattern.
- [S3] docs/specs/death-notify.md — DeathRecord inserts dead_gen mid-struct (carried over, untouched by this delta)
  WHY: intentional per same-patch spec update; out-of-tree supervisor ABI note only.

## VERIFIED (S2 closure + no regression)
- S2 CLOSED: all three `try_direct_switch` call sites (ipc.cpp:1110, :1315, :1467 — the only callers in-tree) sit in loops capped by `ipc_direct_attempts < kDirectMaxAttempts` (3 guards / 3 increments, the only mutations besides init + the 3 resets). Each reset sits after `cur` null-resolution (send_sync_err:881, bounded:1159, reply_recv:1354) and before its `for (;;)` with no lock/IRQ guard in scope — plain u8 store to own (RUNNING) TCB, single-writer/single-reader, no lock-order or ISR interaction. No mid-wait reset path exists; per-wait budget (3 fresh attempts per call, incl. each REPLY_RECV server-loop iteration) now matches the task.hpp:1022 "per wait" contract. No overflow: uint8_t field, max value 3 per wait, reset on every entry (init zeroing at task.cpp:416 retained for first wait).
- No regression: delta is purely additive (no deletion, no token/ticket/sidecar/sweep/gate/H2/arch/pool8 line touched). All prior APPROVED findings stand as recorded in the prior report.
- Evidence (developer-provided, auditor ran no builds/tests per role rules): test-history.txt rows ipc 96/96, servers 66/66, core_sched 127/127 plus one 0/0 TIMEOUT under load with green 16s rerun (recorded flake pattern); style Errors 0; x86 debug build green. Prior full gate set carried forward.
- CHECK-7: graphify.query_graph returned 14 stale nodes (no M3 symbols, graph lags); obsidian vault search 0 hits (greenfield). Proceeded on code + in-repo specs.

DECISION: APPROVED
