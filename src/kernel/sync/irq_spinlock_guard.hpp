#pragma once

#include <kernel/arch/hal/irq_guard.hpp>
#include <kernel/sync/spinlock.hpp>

namespace kernel {
namespace sync {

/// @brief RAII guard that combines IRQ disable + SpinLock acquisition.
///
/// Disables interrupts (prevents ISR preemption on UP), then acquires the
/// SpinLock (ensures SMP safety).  On destruction, releases the lock and
/// restores the interrupt state that was active on entry.
///
/// Provides temporary unlock/lock for OOM handler callbacks that must
/// release the lock before calling the handler and re-acquire for retry.
class [[nodiscard]] IrqSpinLockGuard {
  public:
    explicit IrqSpinLockGuard(SpinLock &lock) noexcept
        : lock_(lock), irq_was_(arch::interrupts_enabled()), held_(true) {
        arch::cli();
        // Issue #546-B: IRQ-combined takes never reach the contended hook
        // (a park inside a bare-cli atomic region would sti mid-section).
        lock_.lock_irq_atomic();
    }

    ~IrqSpinLockGuard() noexcept {
        if (held_)
            do_unlock();
    }

    /// @brief Temporarily release the lock and re-enable IRQs.
    /// Used by OOM handler paths: release before calling handler,
    /// re-acquire before retry.
    void unlock() noexcept {
        if (held_) {
            do_unlock();
            held_ = false;
        }
    }

    /// @brief Re-acquire lock and disable IRQs after a temporary unlock().
    void lock() noexcept {
        if (!held_) {
            irq_was_ = arch::interrupts_enabled();
            arch::cli();
            lock_.lock_irq_atomic();
            held_ = true;
        }
    }

  private:
    void do_unlock() noexcept {
        lock_.unlock();
        if (irq_was_)
            arch::sti();
    }

    SpinLock &lock_;
    bool irq_was_;
    bool held_;
};

} // namespace sync
} // namespace kernel
