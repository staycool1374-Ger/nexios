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
// M2 follow-up (#545 FIRE): bounded denied-spin block escapes + foreign
// depths admitted to the park (INV-L4).  Diagnostic only.
uint64_t g_denied_escape_count = 0;
uint64_t g_foreign_depth_count = 0;

void note_denied(ParkDenyReason reason) noexcept {
    __atomic_fetch_add(&g_spin_fallback_count, 1, __ATOMIC_RELAXED);
    g_park_denied_reason = static_cast<uint64_t>(reason);
}

} // namespace

// Note: the own-scope token is arch::irq_guard_owner_token() (current
// task, unconditionally — see irq_guard.hpp for why it must not depend on
// isr_nesting/IF: the token is recorded at take time and compared at check
// time, and IF always flips in between).  No local copy is kept: take and
// check must use the identical source or the comparison fabricates
// foreign owners (observed on rv64).

static ParkDenyReason deny_reason() noexcept {
    if (CONFIG_LOCK_PARK_ENABLE == 0)
        return ParkDenyReason::Disabled;
    // IsrContext arm (M2 §6.2 + #545 FIRE): a hardware ISR must never park
    // (no task to deschedule).  Two precision carve-outs, both fail-closed:
    // (a) genuine syscall-trap level (in_syscall_trap_level): the trap runs
    // on the calling task's stack with a task to deschedule, and the park
    // path's own sti/hlt owns the IRQ state exactly like sys_receive's
    // wait loop — without this, every syscall would deny (all traps bump
    // isr_nesting) and the FIRE shape could never park; (b) stale nesting
    // with interrupts enabled: isr_nesting is observably wrong then
    // (rv64 suite baseline), and IF==1 proves we are not inside a hardware
    // ISR (interrupt gates clear IF; our ISRs never re-enable), so the
    // count must not deny.  IF==0 keeps the deny (fail-closed direction,
    // covering the QE manual-nesting test which holds IF=0 via IrqGuard).
    if (kernel::isr_nesting_own() != 0 && !arch::in_syscall_trap_level() &&
        !arch::interrupts_enabled())
        return ParkDenyReason::IsrContext;
    if (arch::irq_guard_depth_own() != 0) {
        // INV-L4 (#545 FIRE, convicted H1a/V1): a depth owned by a foreign
        // context is diagnostic-only, never a denial when allowed — the
        // syscall trap path holds no IrqGuard, so any depth seen there is
        // foreign/leaked.  Owner 0 (unknown/boot) and the own token stay
        // denied (fail-closed).
        const uint64_t owner = arch::irq_guard_owner_own();
        if (owner != 0 && owner != arch::irq_guard_owner_token())
            return ParkDenyReason::ForeignGuard;
        return ParkDenyReason::IrqGuarded;
    }
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

bool lock_park_allowed(const SpinLock &lock) noexcept {
    (void)lock;
    const ParkDenyReason reason = deny_reason();
    if (reason == ParkDenyReason::None)
        return true;
#if CONFIG_LOCK_PARK_FOREIGN_OWNER_ALLOW == 1
    if (reason == ParkDenyReason::ForeignGuard)
        return true;
#endif
    return false;
}

/// @brief Bounded, loud denied-spin block: pause-spins up to
///        CONFIG_LOCK_DENIED_SPIN_ROUNDS with watchdog checkpoints, then
///        returns false so the caller re-diagnoses and re-arms.  Never
///        spins silently forever — the TSC watchdog FIRES on the overrun.
static bool denied_spin_block(SpinLock &lock, const char *where) noexcept {
    for (uint64_t i = 0; i < CONFIG_LOCK_DENIED_SPIN_ROUNDS; ++i) {
        arch::pause();
        if ((i & 255U) == 255U)
            tick_watchdog::checkpoint(where);
        if (lock.try_lock())
            return true;
    }
    return false;
}

/// @brief First-escape diagnostic: one ERROR per lock site so the soak
///        log convicts the shape without serial spam.  Includes the lock
///        holder PC so a stale (abandoned) hold is distinguishable from a
///        live slow holder without GDB: a holder address that never
///        corresponds to a live critical section across repeated escapes
///        is the follow-up target (#545).
static void note_escape(const SpinLock &lock, const char *site,
                        const char *where, ParkDenyReason reason) noexcept {
    const uint64_t escapes =
        __atomic_fetch_add(&g_denied_escape_count, 1, __ATOMIC_RELAXED) + 1;
#if defined(CONFIG_DEBUG)
    if (escapes == 1) {
        uint64_t holder_task = 0;
        uint64_t holder_rank = 0;
        const bool holder_known = sync::spinlock_find_holder(
            &lock, &holder_task, &holder_rank);
        kernel::Logger::error(
            "[PARK] denied-spin escape lock=%lx site=%s where=%s if=%d "
            "irqd=%lu owner=%lx reason=%lu holder=%lx holder_task=%lu "
            "holder_rank=%lu holder_known=%d",
            reinterpret_cast<uint64_t>(&lock), site, where,
            arch::interrupts_enabled() ? 1 : 0, arch::irq_guard_depth_own(),
            arch::irq_guard_owner_own(), static_cast<uint64_t>(reason),
            reinterpret_cast<uint64_t>(lock.holder()), holder_task,
            holder_rank, holder_known ? 1 : 0);
    }
#else
    (void)lock;
    (void)site;
    (void)where;
    (void)escapes;
    (void)reason;
#endif
}

ParkLockGuard::ParkLockGuard(SpinLock &lock, const char *site) noexcept
    : lock_(lock) {
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
        // Status-quo denial: bounded loud spin (never silent).  Each block
        // is bounded; escapes re-diagnose and re-arm so the TSC watchdog
        // FIRES on a true wedge instead of freezing silently.  A
        // ForeignGuard reason here is a predicate/deny TOCTOU (the owner
        // changed between the two same-thread reads): fall through to the
        // park below instead of spinning on a foreign depth.
        const ParkDenyReason reason = deny_reason();
        if (reason == ParkDenyReason::ForeignGuard) {
            __atomic_fetch_add(&g_foreign_depth_count, 1, __ATOMIC_RELAXED);
        } else {
            note_denied(reason);
            for (;;) {
#if CONFIG_LOCK_DENIED_SPIN_STI_ALLOW == 1
                // Last resort (default OFF): sti window heals a genuine
                // own-guard denial shape.  Never taken for ISR context,
                // held spinlocks, or interrupts-already-on.
                if (reason != ParkDenyReason::IsrContext &&
                    spinlock_held_by_current_count() == 0 &&
                    !arch::interrupts_enabled()) {
                    arch::sti();
                    const bool got =
                        denied_spin_block(lock_, "lock_spin_denied_sti");
                    arch::cli();
                    if (got) {
                        owned_ = true;
                        return;
                    }
                    note_escape(lock_, site, "lock_spin_denied", reason);
                    continue;
                }
#endif
                if (denied_spin_block(lock_, "lock_spin_denied")) {
                    owned_ = true;
                    return;
                }
                note_escape(lock_, site, "lock_spin_denied", reason);
            }
        }
    }
    __atomic_fetch_add(&g_park_count, 1, __ATOMIC_RELAXED);
    if (arch::irq_guard_depth_own() != 0) {
        // Admitted on a foreign-owned depth (INV-L4): the depth check in
        // the predicate already proved foreign (allowed + depth live).
        __atomic_fetch_add(&g_foreign_depth_count, 1, __ATOMIC_RELAXED);
    }
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
    // real deny code for diagnosis).  Bounded loud spin (same discipline
    // as the denied path): never silent, watchdog FIRES on a true wedge.
    for (;;) {
        if (denied_spin_block(lock_, "lock_spin_exhausted")) {
            owned_ = true;
            return;
        }
        note_escape(lock_, site, "lock_spin_exhausted", deny_reason());
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

uint64_t lock_denied_escape_count() noexcept {
    return __atomic_load_n(&g_denied_escape_count, __ATOMIC_RELAXED);
}

uint64_t lock_foreign_depth_count() noexcept {
    return __atomic_load_n(&g_foreign_depth_count, __ATOMIC_RELAXED);
}

void park_lock_reset_for_test() noexcept {
    g_park_count = 0;
    g_spin_fallback_count = 0;
    g_park_exhausted_count = 0;
    g_park_denied_reason = static_cast<uint64_t>(ParkDenyReason::None);
    g_denied_escape_count = 0;
    g_foreign_depth_count = 0;
}

} // namespace sync
} // namespace kernel
