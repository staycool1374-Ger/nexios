# AUDIT REPORT PRE-256
PATCH: (none — pre-implementation plan review)
FILES: sync_errors.hpp, sync.hpp, mutex.hpp, semaphore.hpp, queue.hpp, notify.hpp, eventgroup.hpp, spsc_ring.hpp, mutex.cpp, semaphore.cpp, queue.cpp, notify.cpp, eventgroup.cpp
## FINDINGS
- [S3] step-3:predicate-sites — ~30 predicate application sites are not line-enumerated and guard-position vs in-scan use is undistinguished, while semaphore.cpp:104-109 and queue.cpp:106-110,139-143 use select-then-guard (purge stale, pick max-priority including dead, guard only best yet always unlink) rather than per-candidate conjunction.
  WHY: Adding an awakeable-skip inside those best-selection loops would unlink/wake a different waiter than today in the teardown-asymmetry window, a functional wake-order change smuggled as refactor.
- [S3] step-3:predicate-home — shared header home is unspecified and notify.hpp/eventgroup.hpp deliberately forward-declare TCB instead of including task.hpp, so a predicate reading task->state/generation needs an explicit include/placement/signature decision.
  WHY: The wrong home forces a new task.hpp include edge into lightweight headers or risks ODR/linkage defects if the definition is not inline, a coupling hazard the post-audit must verify.
- [S3] step-4:select_best_waiter — the three mutex scans were verified textually identical in selection criteria (mutex.cpp:120-131,525-537,592-603), but the plan must pin the helper as a pure index selector with a lock-held precondition.
  WHY: Any unlink, restore_priority, pop_ceiling, transfer, or set_task_ready moved inside the helper would shift the §11.1-adjacent ownership-transfer boundary that is load-bearing.
- [S3] step-5:eventgroup-rename — members are bits_/wanted_bits so param `bits` shadows nothing and the stated unshadowing rationale is inaccurate, making the decl+defs+call-args rename cosmetic churn.
  WHY: Same-typed positional-argument edits invite arg-swap typos no compiler check will catch, so the post-audit must confirm the rename touched no logic.
- [S3] step-6:unchecked-add — the infallibility WHY is true only if each outer capacity check and its add execute under one continuously held guard (queue.cpp:279-282,432-435; eventgroup.cpp:220-224; mutex.cpp:343-349,384-391; semaphore.cpp:247-253), and queue add variants lack dedup.
  WHY: Any guard drop between check and add reopens the TOCTOU window the new comment claims is closed, leaving a false safety justification in the code.
- [S3] step-2:comment-collapse — collapsing four H-7 notes to one canonical WHY at Queue::send and dropping audit-ID tags removes per-site rationale and traceability.
  WHY: A guard without its UAF/ready-queue-corruption justification at the exact check site invites future weakening, so each stripped site must reference the canonical note.
DECISION: APPROVED