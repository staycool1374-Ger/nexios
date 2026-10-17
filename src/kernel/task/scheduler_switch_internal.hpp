#pragma once

/// @file scheduler_switch_internal.hpp
/// @brief Shared cross-TU declarations for the deferred-switch engine
///        (issue #323).  Pure move support: no logic change.
///        `switch_to_task` + `charge_exec` are defined once in
///        scheduler.cpp (until the dedicated switch strand moves them);
///        staying callers (reschedule/switch paths) and the extracted
///        scheduler_terminate.cpp TU resolve them through these
///        declarations, replacing the former file-static forward decl.

#include <kernel/task/scheduler.hpp>

namespace kernel {

// Deferred-switch engine: single definitions live in scheduler.cpp (they
// move to scheduler_switch.cpp in the dedicated switch strand).  Declared
// here at kernel scope — matching the definitions' linkage — so staying
// callers (reschedule/switch paths) and the extracted
// scheduler_terminate.cpp TU resolve the same symbols.
void switch_to_task(TaskControlBlock *current, TaskControlBlock &next,
                    sync::SpinLock *held_lock);
void charge_exec(TaskControlBlock &t) noexcept;

namespace sched_switch {
// Convention anchor for the future switch-engine home; owns no symbols
// yet.  scheduler_terminate.cpp consumes the engine through the
// kernel-scope declarations above.
}  // namespace sched_switch

}  // namespace kernel
