# AUDIT REPORT 2026-11-17-581-generation-fix
PATCH: audits/pending_patch.diff
FILES: docs/specs/ipc.md, src/kernel/ipc/ipc.cpp

## FINDINGS
- [S3] src/kernel/ipc/ipc.cpp:582 — find_task-then-load window on sender->generation under the dest queue lock
  WHY: id_table_find returns a raw TCB* whose generation is then atomic-loaded with no lifetime hold; a concurrent reap/recycle of the sender between the two steps reads a freed or foreign generation field — the failure direction is fail-safe (mismatch or null falls through to legacy untokened push with the ticket left live, never a forged stamp), IDs are monotonic/never reused so ABA match on a distinct task is impossible, and the pattern matches the in-tree precedent (note_delivered_request ipc.cpp:760, REPLY_RECV validation ipc.cpp:1382, death_notify/pager_registry find_task+deref sites); no action required.

## CHECKS (all verified against the diff + targeted reads)
- Identity is sender-side: dest.generation read is gone; match requires find_task(m.sender_id)->generation == reply_wait_dest_gen (ipc.cpp:569-585). cur_id != 0 guard added so anonymous (unstamped, id 0) traffic can never match a ticket.
- Fallthrough shape preserved: non-matching id, null sender, generation mismatch, or ticket-changed (old != seen) all fall to the shared tail push_locked(m, 0) → Pushed; Full only when the queue is genuinely full (ipc.cpp:598-600). No message drop introduced. Ticket stays live on mismatch for sweep-stamp recovery.
- Consume ordering unchanged and after both proofs: id match → generation match → exchange(ticket,0) → old==seen recheck → stamp+push (ipc.cpp:571-594).
- No new lock edge: id_table_find is a bounded pure-array probe (scheduler_id_table.cpp:114-131, no locks, no alloc, no park) called under the already-held ParkLockGuard; want_gen load is scoped inside the id-match so ordinary (non-ticket/id-mismatch) traffic pays no extra atomic.
- Validation side untouched and correct: REPLY_RECV still checks caller (requester) generation vs cur->last_caller_gen plus live ticket (ipc.cpp:1382-1385) — requester-vs-requester, consistent with the fixed deliver side.
- Scope clean: no test files, no arch files, no CONFIG gates, CONFIG_LOCK_DENIED_STI_ALLOW untouched. Docs clause (ipc.md) matches the code: sender-TCB identity, untokened fallback on unresolvable sender, issue #581 reference.
- Check-7: retrieval artifacts reported on-thread (graphify query 11 nodes, mostly noise, no function coverage — file text authoritative; vault search 0 hits, no prior art; live-GDB evidence + code reading on issue #581). Accepted as filed; auditor has no independent issue-thread read path in this session.
- Check-11: test-history.txt corroborates the claimed runs (2026-11-17 rows: ipc 96/96 x2, ipc_core 27/27, ipc_blocking 4/4, ipc_extended 14/14, ipc_lock_free 3/3, cap_ipc 6/6, plus 96/95-count rows matching the aarch64/riscv64 claims). Vacuity acknowledged: no in-tree test asserts PushedReply, so green runs are regression-only; QE unpark of test_ipc_reply.cpp 28/29/30/33 is the acceptance gate (not demanded of the developer).

DECISION: APPROVED
