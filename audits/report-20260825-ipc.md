# AUDIT REPORT 2026-08-25T12:00:00Z
PATCH: audits/pending_patch.diff
FILES: src/kernel/ipc/buffer_pool.cpp, src/kernel/ipc/ipc.cpp, src/kernel/syscall/syscall_handlers_ipc.cpp, src/kernel/cap/endpoint.cpp, src/kernel/cap/endpoint.hpp

## FINDINGS
- [S3] buffer_pool.cpp:clear_pte_in_pml4 — TLB flush unconditionally fires when active PML4 matches; safe because INVLPG on non-present entry is no-op, and foreign PML4 staleness is handled at switch time
- [S3] buffer_pool.cpp:map() owner check (H-2) — reject unless entries[idx].owner_task == task.id; prevents silent cross-task take-over
- [S3] buffer_pool.cpp:map() VA validation (M-6) — page-aligned, within USER_SPACE_LIMIT, no existing PTE; prevents kUserYieldStubVa PTE-overwrite class and double-map corruption
- [S3] cap/endpoint.hpp:disposed_ volatile flag (H-3) — published under q.lock_ before sender wake; re-checked by send_via_cap at entry and after blocked-wake
- [S3] cap/endpoint.cpp:dispose() drains blocked senders under q.lock_ with REAPED+TERMINATED filter; undoes PI boost under IrqGuard + move_priority
- [S3] ipc.cpp:send_via_cap self-send guard (M-2) — task sending to its own bound endpoint returns false; prevents forever-block on undrainable full queue
- [S3] ipc.cpp:MessageQueue::~MessageQueue PI undo (M-4) — owner returns to base_priority under IrqGuard + move_priority when no blocked senders remain
- [S3] syscall_handlers_ipc.cpp:sys_receive dequeue_ready (M-5) — Scheduler::dequeue_ready(*cur) before state=BLOCKED+reschedule; enforces INV-2/WEDGE invariant

## PATCH
audits/rejected_patch.diff was not written; all changes pass static audit verification.

DECISION: APPROVED
