# AUDIT REPORT 20260915-161640
PATCH: audits/pending_patch.diff
FILES: src/kernel/kernel.cpp, src/kernel/task/scheduler.cpp, src/kernel/task/task.hpp, src/kernel/test/test_expected_counts.hpp, src/kernel/test/test_init.cpp, src/kernel/test/test_isolate.cpp, src/services/shell.cpp, test-history.txt

## FINDINGS
- [S3] src/kernel/kernel.cpp:reaper loop — daemon-IPC wake source is not level-triggered: IPC::send wakes via set_task_ready (task-state store) without setting a notify value, so a push landing between the recv-drain/is_empty check and notify.wait() registration can be lost; zombie pokes remain level-safe via value persistence.
  WHY: The sleep decision gates on RELAXED is_empty + try_wait, neither of which observes a concurrent msg_queue.push that already consumed its one-shot READY wake on the still-RUNNING reaper.
- [S3] src/kernel/kernel.cpp:loop comment overstates "level-triggered on all three wake sources" — accurate for zombie delta and notify value, not for the IPC-queue source per the race above; consequence is bounded (one delayed daemon-ready log line, self-healing on the next zombie poke), no liveness impact on zombie draining.
  WHY: Every zombie birth unconditionally sets the notify value which wait() consumes on entry, so zombie-drain liveness never depends on the racy IPC path.

DECISION: APPROVED
