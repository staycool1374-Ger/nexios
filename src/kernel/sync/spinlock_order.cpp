/// @file spinlock_order.cpp
/// @brief Lock-rank table + per-CPU held-lock bookkeeping (M2, #546).
/// The only TU that may resolve lock owners (needs Scheduler/task types).

#include <kernel/sync/spinlock_order.hpp>
#include <kernel/arch/cpu_context.hpp>
#include <kernel/arch/hal/irq_guard.hpp>
#include <kernel/task/scheduler.hpp>
#include <kernel/task/task.hpp>
#include <lib/logger.hpp>

namespace kernel {
namespace sync {
namespace {

// Always-on per-CPU take depth (both builds).  See header.
uint64_t g_held_depth[CONFIG_MAX_CPUS] = {};

#if defined(CONFIG_DEBUG)
// Per-CPU held-lock stack (own slot touched only; ISR-serialized on UP).
struct CpuHeldLocks {
    HeldLock entries[kMaxHeldLocks] = {};
    uint64_t depth = 0;
    uint64_t overflow = 0;
    uint64_t unpaired = 0;
};

CpuHeldLocks g_held[CONFIG_MAX_CPUS] = {};
// Report-once per (outer,inner) rank pair + event counters (diagnostic;
// monotonic, atomics for ISR/task shared increments).
uint64_t g_violations[4][4] = {};
bool g_violation_reported[4][4] = {};

inline CpuHeldLocks &own_held() noexcept {
    return g_held[arch::cpu_index() % CONFIG_MAX_CPUS];
}

inline uint64_t resolve_owner_id() noexcept {
    if (kernel::isr_nesting_own() != 0)
        return 0; // ISR context owns as 0 (never a live task id).
    auto *cur = Scheduler::current_task();
    // Live task ids start at 1 (PID 1 = init); 0 means "no task context"
    // only when cur is null (early boot) — a taken lock then attributes
    // to 0, which the contract check treats as owned, never clean.
    return cur != nullptr ? cur->id : 0;
}
#endif // CONFIG_DEBUG

} // namespace

uint64_t spinlock_held_depth_own() noexcept {
    return g_held_depth[arch::cpu_index() % CONFIG_MAX_CPUS];
}

void spinlock_held_bump() noexcept {
    ++g_held_depth[arch::cpu_index() % CONFIG_MAX_CPUS];
}

void spinlock_held_drop() noexcept {
    uint64_t &slot = g_held_depth[arch::cpu_index() % CONFIG_MAX_CPUS];
    if (slot != 0)
        --slot; // Unpaired release is a no-op (never negative).
}

#if defined(CONFIG_DEBUG)

void spinlock_note_acquire(const void *addr, uint8_t rank) noexcept {
    CpuHeldLocks &held = own_held();
    const uint64_t owner = resolve_owner_id();
    // Report-only order check over the locks already held (outer → inner
    // must be strictly increasing rank numbers: Sched(1) → Queue(2) →
    // Leaf(3); Unranked locks are invisible).  Runs BEFORE the push so a
    // take never flags itself; on overflow the full stack is scanned.
    if (rank != static_cast<uint8_t>(LockRank::Unranked)) {
        for (uint64_t i = 0; i < held.depth; ++i) {
            const uint8_t outer = held.entries[i].rank;
            if (outer == static_cast<uint8_t>(LockRank::Unranked) ||
                outer < rank)
                continue;
            // outer >= inner with both ranked: order violation.
            __atomic_fetch_add(&g_violations[outer][rank], 1, __ATOMIC_RELAXED);
            if (!g_violation_reported[outer][rank]) {
                g_violation_reported[outer][rank] = true;
                kernel::Logger::warn(
                    "lockorder: rank inversion outer=%u inner=%u (report-only)",
                    static_cast<unsigned>(outer), static_cast<unsigned>(rank));
            }
        }
    }
    if (held.depth < kMaxHeldLocks) {
        held.entries[held.depth].addr = addr;
        held.entries[held.depth].owner_id = owner;
        held.entries[held.depth].rank = rank;
        ++held.depth;
    } else {
        __atomic_fetch_add(&held.overflow, 1, __ATOMIC_RELAXED);
    }
}

void spinlock_note_release(const void *addr) noexcept {
    CpuHeldLocks &held = own_held();
    // Pop by address from the top (LIFO order not required by callers).
    for (uint64_t i = held.depth; i > 0; --i) {
        if (held.entries[i - 1].addr == addr) {
            for (uint64_t j = i - 1; j + 1 < held.depth; ++j)
                held.entries[j] = held.entries[j + 1];
            --held.depth;
            held.entries[held.depth] = HeldLock{};
            return;
        }
    }
    // Unpaired release (e.g. init-time unlock of a never-locked lock):
    // silent no-op + one-shot note.  Depth counter untouched (see header).
    static bool noted_once = false;
    if (!noted_once) {
        noted_once = true;
        kernel::Logger::warn("lockorder: unpaired release (init-time unlock?)");
    }
    __atomic_fetch_add(&held.unpaired, 1, __ATOMIC_RELAXED);
}

uint64_t spinlock_held_by_current_count() noexcept {
    const CpuHeldLocks &held = own_held();
    const uint64_t owner = resolve_owner_id();
    uint64_t count = 0;
    for (uint64_t i = 0; i < held.depth; ++i) {
        if (held.entries[i].owner_id == owner)
            ++count;
    }
    return count;
}

bool spinlock_contract_check_clean(const char *where) noexcept {
    (void)where;
    return spinlock_held_by_current_count() == 0;
}

void spinlock_order_reset_for_test() noexcept {
    arch::IrqGuard irq_guard{};
    for (uint64_t c = 0; c < CONFIG_MAX_CPUS; ++c) {
        g_held[c] = CpuHeldLocks{};
        g_held_depth[c] = 0;
    }
    for (uint64_t o = 0; o < 4; ++o) {
        for (uint64_t i = 0; i < 4; ++i) {
            g_violations[o][i] = 0;
            g_violation_reported[o][i] = false;
        }
    }
}

uint64_t spinlock_violation_count(uint8_t outer_rank,
                                  uint8_t inner_rank) noexcept {
    if (outer_rank >= 4 || inner_rank >= 4)
        return 0;
    return __atomic_load_n(&g_violations[outer_rank][inner_rank],
                           __ATOMIC_RELAXED);
}

uint64_t spinlock_overflow_count() noexcept {
    uint64_t total = 0;
    for (uint64_t c = 0; c < CONFIG_MAX_CPUS; ++c) {
        total += __atomic_load_n(&g_held[c].overflow, __ATOMIC_RELAXED);
    }
    return total;
}

uint64_t spinlock_unpaired_release_count() noexcept {
    uint64_t total = 0;
    for (uint64_t c = 0; c < CONFIG_MAX_CPUS; ++c) {
        total += __atomic_load_n(&g_held[c].unpaired, __ATOMIC_RELAXED);
    }
    return total;
}

#endif // CONFIG_DEBUG

} // namespace sync
} // namespace kernel
