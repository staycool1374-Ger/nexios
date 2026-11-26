/// @file sync/spinlock_hook.cpp
/// @brief Weak default of the contended-spin decision hook (issue #546-B).
///        Returns Retry with no effect: status-quo bounded spinning.
///        The strong override in sync/park_lock.cpp replaces this at link
///        time wherever the scheduler TU is linked in.
#include <kernel/sync/spinlock_hook.hpp>

namespace kernel {
namespace sync {

__attribute__((weak)) SpinContendAction
spin_contended_hook(SpinLock &lock) noexcept {
    (void)lock;
    return SpinContendAction::Retry;
}

} // namespace sync
} // namespace kernel
