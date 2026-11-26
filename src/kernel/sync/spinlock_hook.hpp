/// @file sync/spinlock_hook.hpp
/// @brief Contended-spin decision hook (issue #546-B). Types only.
///        Include closure is exactly <types.hpp> (verified by grep gate);
///        in particular this header must never include task/scheduler.hpp,
///        task/task.hpp, or sync/park_lock.hpp (Q1 compile-time
///        independence of sync/ from task/).
#pragma once

#include <types.hpp>

namespace kernel {
namespace sync {

class SpinLock;

/// @brief Outcome of one contended-spin decision.
enum class SpinContendAction : uint8_t {
    Retry = 0,   ///< lock not held; re-enter the bounded spin (nothing armed)
    Acquired = 1 ///< lock held, holder published, order note done, entry-IF
                 ///< restored; caller does no further bookkeeping
};

/// @brief Hook invoked exactly once per exhausted bounded-spin round.
///        Weak default (spinlock_hook.cpp) returns Retry with no effect.
///        Strong override lives in the scheduler-facing TU
///        (sync/park_lock.cpp) and may park iff lock_park_allowed().
///        Called with the target lock not held by this context.
SpinContendAction spin_contended_hook(SpinLock &lock) noexcept;

} // namespace sync
} // namespace kernel
