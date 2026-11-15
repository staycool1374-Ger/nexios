/// @file park_lock.cpp
/// @brief Bounded spin → timed park acquisition (M2, issues #545/#546).

#include <kernel/sync/park_lock.hpp>
#include <kernel/sync/spinlock.hpp>
#include <kernel/sync/spinlock_order.hpp>
#include <kernel/arch/hal/irq_guard.hpp>
#include <kernel/arch/cpu_context.hpp>
#include <kernel/arch/io.hpp>
#include <kernel/arch/hal/timer.hpp>
#include <kernel/task/scheduler.hpp>
#include <kernel/task/task.hpp>
#include <kernel/time/posix_time.hpp>
#include <kernel/task/tick_watchdog.hpp>
#if defined(CONFIG_DEBUG)
#include <lib/logger.hpp>
#endif

namespace kernel {
namespace sync {
namespace {

uint64_t g_park_count = 0;
uint64_t g_spin_fallback_count = 0;
uint64_t g_park_exhausted_count = 0;
uint64_t g_park_denied_reason =
    static_cast<uint64_t>(ParkDenyReason::None);

void note_denied(ParkDenyReason reason) noexcept {
    __atomic_fetch_add(&g_spin_fallback_count, 1, __ATOMIC_RELAXED);
    g_park_denied_reason = static_cast<uint64_t>(reason);
}

} // namespace

bool lock_park_allowed(const SpinLock &lock) noexcept {
    (void)lock;
    if (CONFIG_LOCK_PARK_ENABLE == 0)
        return false;
    // ISR context never parks (no task to deschedule; try_lock only).
    if (kernel::isr_nesting_own() != 0)
        return false;
    // An IrqGuard scope is live: sti/hlt would widen its IRQ-off window.
    if (arch::irq_guard_depth_own() != 0)
        return false;
    // Never nest a park inside a held spinlock (cross-lock deadlock).
    if (spinlock_held_by_current_count() != 0)
        return false;
    auto *task = Scheduler::current_task();
    if (task == nullptr || task->magic != TaskControlBlock::TCB_MAGIC)
        return false;
    if (Scheduler::is_idle_task(task))
        return false;
    if (task->state != TaskState::RUNNING)
        return false;
    if (CONFIG_TICK_HZ == 0)
        return false; // No tick source: park_ns divisor + wheel both dead.
    return true;
}

static ParkDenyReason deny_reason() noexcept {
    if (CONFIG_LOCK_PARK_ENABLE == 0)
        return ParkDenyReason::Disabled;
    if (kernel::isr_nesting_own() != 0)
        return ParkDenyReason::IsrContext;
    if (arch::irq_guard_depth_own() != 0)
        return ParkDenyReason::IrqGuarded;
    if (spinlock_held_by_current_count() != 0)
        return ParkDenyReason::LockHeld;
    auto *task = Scheduler::current_task();
    if (task == nullptr || task->magic != TaskControlBlock::TCB_MAGIC)
        return ParkDenyReason::NotTask;
    if (Scheduler::is_idle_task(task))
        return ParkDenyReason::IdleTask;
    if (task->state != TaskState::RUNNING)
        return ParkDenyReason::NotRunning;
    if (CONFIG_TICK_HZ == 0)
        return ParkDenyReason::NoTicks;
    return ParkDenyReason::None;
}

ParkLockGuard::ParkLockGuard(SpinLock &lock, const char *site) noexcept
    : lock_(lock) {
    (void)site; // Reserved for the DEBUG one-shot park line.
    if (lock_.try_lock()) {
        owned_ = true;
        return;
    }
    // Bounded pause-spin (fast path unchanged: uncontended try_lock).
    for (uint64_t i = 0; i < CONFIG_LOCK_SPIN_ROUNDS; ++i) {
        arch::pause();
        if ((i & 255U) == 255U)
            tick_watchdog::checkpoint("lock_spin");
        if (lock_.try_lock()) {
            owned_ = true;
            return;
        }
    }
    if (!lock_park_allowed(lock_)) {
        // Status quo: pure spin (never worse than today).  INV-L1 denial
        // recorded; checkpoint cadence keeps the tick watchdog informed.
        note_denied(deny_reason());
        for (;;) {
            arch::pause();
            tick_watchdog::checkpoint("lock_spin_denied");
            if (lock_.try_lock()) {
                owned_ = true;
                return;
            }
        }
    }
    __atomic_fetch_add(&g_park_count, 1, __ATOMIC_RELAXED);
    auto *task = Scheduler::current_task();
    // ns per park cycle, saturating (grace ~ms fits u64 trivially).
    uint64_t park_ns = static_cast<uint64_t>(CONFIG_LOCK_PARK_TICKS);
    park_ns *= 1000000000ULL;
    park_ns /= static_cast<uint64_t>(CONFIG_TICK_HZ);
    for (uint64_t attempt = 0;
         attempt < CONFIG_LOCK_PARK_MAX_ATTEMPTS; ++attempt) {
        if (!time::PosixTime::sleep_arm(*task, park_ns)) {
            note_denied(ParkDenyReason::ArmFailed);
            break; // Arm failed: fall through to pure spin below.
        }
        {
            arch::IrqGuard irq_guard{};
            Scheduler::dequeue_ready(*task);
            task->state = TaskState::BLOCKED;
        }
        Scheduler::reschedule();
        // sys_receive shape (unconditional sti/hlt/cli): trap entry owns
        // the IRQ state (SIE=0, no locks held), so sti is safe here —
        // and mandatory, since IF=0 sleeps through the wheel timeout.
        for (;;) {
            arch::sti();
            arch::hlt();
            arch::cli();
            if (task->state != TaskState::BLOCKED)
                break;
        }
        time::PosixTime::sleep_cancel(*task);
        // INV-L2 resume: the tick scan READY+enqueued us; resuming on-CPU
        // without a dispatch would leave RUNNING+queued, so unlink first
        // (dequeue_ready is a safe no-op when not queued), then RUNNING.
        Scheduler::dequeue_ready(*task);
        task->state = TaskState::RUNNING;
        tick_watchdog::checkpoint("lock_park_resume");
        if (lock_.try_lock()) {
            owned_ = true;
            return;
        }
        // Still contended: next attempt re-arms (fresh wheel timeout).
    }
    __atomic_fetch_add(&g_park_exhausted_count, 1, __ATOMIC_RELAXED);
    __atomic_fetch_add(&g_spin_fallback_count, 1, __ATOMIC_RELAXED);
    // Reason untouched: exhaustion is not a denial (keeps the last
    // real deny code for diagnosis).
    for (;;) {
        arch::pause();
        tick_watchdog::checkpoint("lock_spin_exhausted");
        if (lock_.try_lock()) {
            owned_ = true;
            return;
        }
    }
}

ParkLockGuard::~ParkLockGuard() noexcept {
    if (owned_) {
        owned_ = false;
        lock_.unlock();
    }
}

uint64_t lock_park_count() noexcept {
    return __atomic_load_n(&g_park_count, __ATOMIC_RELAXED);
}

uint64_t lock_spin_fallback_count() noexcept {
    return __atomic_load_n(&g_spin_fallback_count, __ATOMIC_RELAXED);
}

uint64_t lock_park_exhausted_count() noexcept {
    return __atomic_load_n(&g_park_exhausted_count, __ATOMIC_RELAXED);
}

uint64_t lock_park_denied_reason() noexcept {
    return g_park_denied_reason;
}

void park_lock_reset_for_test() noexcept {
    g_park_count = 0;
    g_spin_fallback_count = 0;
    g_park_exhausted_count = 0;
    g_park_denied_reason = static_cast<uint64_t>(ParkDenyReason::None);
}

} // namespace sync
} // namespace kernel
