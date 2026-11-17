/// @file spinlock_order.cpp
/// @brief Lock-rank table + per-CPU held-lock bookkeeping (M2, #546).
/// The only TU that may resolve lock owners (needs Scheduler/task types).

#include <kernel/sync/spinlock_order.hpp>
#include <kernel/arch/cpu_context.hpp>
#include <kernel/arch/io.hpp>
#include <kernel/arch/hal/timer.hpp>
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

// Wedge hunt (#545): last-take / last-release record per lock.  Answers
// "who took this lock last, and did a release run after" without trusting
// the order stack (unreliable for one contended queue lock) and without
// depending on global event recency.  32 hash slots, each holding the LAST
// take and the LAST release for its address; collisions reset the slot
// (addr-tagged, so a foreign record is never mistaken for this lock's).
// Lock-free: writers only touch their hashed slot.  DEBUG-only, static
// storage, no alloc.
struct LockLastEvent {
    uint64_t addr = 0;
    uint64_t take_ord = 0;
    uint64_t take_owner = 0;
    uint64_t take_tick = 0;
    uint8_t take_rank = 0;
    uint8_t take_cpu = 0;
    uint8_t take_if = 0;
    uint64_t rel_ord = 0;
    uint64_t rel_owner = 0;
    uint64_t rel_tick = 0;
    uint8_t rel_rank = 0;
    uint8_t rel_cpu = 0;
    uint8_t rel_if = 0;
};

inline constexpr uint64_t kLockLastSlots = 512;

LockLastEvent g_lock_last[kLockLastSlots] = {};
uint64_t g_lock_last_ord = 0;

inline LockLastEvent &lock_last_slot(const void *addr) noexcept {
    // Spread by bits 8..16: TCB-pool stride aligns the low bits, so a
    // >>4 hash collides systematically for per-task queue locks.
    return g_lock_last[(reinterpret_cast<uint64_t>(addr) >> 8) %
                       kLockLastSlots];
}

inline void lock_last_take(const void *addr, uint64_t owner,
                           uint8_t rank) noexcept {
    LockLastEvent &slot = lock_last_slot(addr);
    const uint64_t addr_val = reinterpret_cast<uint64_t>(addr);
    if (slot.addr != addr_val) {
        slot = LockLastEvent{};
        slot.addr = addr_val;
    }
    slot.take_ord =
        __atomic_fetch_add(&g_lock_last_ord, 1, __ATOMIC_RELAXED) + 1;
    slot.take_owner = owner;
    slot.take_rank = rank;
    slot.take_tick = arch::Timer::ticks();
    slot.take_cpu =
        static_cast<uint8_t>(arch::cpu_index() % CONFIG_MAX_CPUS);
    slot.take_if = arch::interrupts_enabled() ? 1 : 0;
}

inline void lock_last_release(const void *addr, uint64_t owner,
                              uint8_t rank) noexcept {
    LockLastEvent &slot = lock_last_slot(addr);
    const uint64_t addr_val = reinterpret_cast<uint64_t>(addr);
    if (slot.addr != addr_val) {
        slot = LockLastEvent{};
        slot.addr = addr_val;
    }
    slot.rel_ord =
        __atomic_fetch_add(&g_lock_last_ord, 1, __ATOMIC_RELAXED) + 1;
    slot.rel_owner = owner;
    slot.rel_rank = rank;
    slot.rel_tick = arch::Timer::ticks();
    slot.rel_cpu =
        static_cast<uint8_t>(arch::cpu_index() % CONFIG_MAX_CPUS);
    slot.rel_if = arch::interrupts_enabled() ? 1 : 0;
}

inline CpuHeldLocks &own_held() noexcept {
    return g_held[arch::cpu_index() % CONFIG_MAX_CPUS];
}

inline uint64_t resolve_owner_id() noexcept {
    // Owner is always the current task (0 when there is none, e.g. early
    // boot).  Deliberately independent of isr_nesting/IF: the id is
    // recorded at take time and matched at check time, and IF always flips
    // in between (that is what a guard does), so any IF- or
    // nesting-dependent rule mismatches itself across the pair.
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
        held.entries[held.depth].site = nullptr;
        held.entries[held.depth].cpu =
            static_cast<uint8_t>(arch::cpu_index() % CONFIG_MAX_CPUS);
        held.entries[held.depth].take_if =
            arch::interrupts_enabled() ? 1 : 0;
        ++held.depth;
    } else {
        __atomic_fetch_add(&held.overflow, 1, __ATOMIC_RELAXED);
    }
    lock_last_take(addr, owner, rank);
}

void spinlock_note_release(const void *addr) noexcept {
    CpuHeldLocks &held = own_held();
    // Pop by address from the top (LIFO order not required by callers).
    for (uint64_t i = held.depth; i > 0; --i) {
        if (held.entries[i - 1].addr == addr) {
            const uint8_t rank = held.entries[i - 1].rank;
            for (uint64_t j = i - 1; j + 1 < held.depth; ++j)
                held.entries[j] = held.entries[j + 1];
            --held.depth;
            held.entries[held.depth] = HeldLock{};
            lock_last_release(addr, resolve_owner_id(), rank);
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
    lock_last_release(addr, resolve_owner_id(), 0xFF);
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

bool spinlock_find_holder(const void *addr, uint64_t *owner_out,
                          uint64_t *rank_out) noexcept {
    const CpuHeldLocks &held = own_held();
    for (uint64_t i = held.depth; i > 0; --i) {
        if (held.entries[i - 1].addr == addr) {
            if (owner_out)
                *owner_out = held.entries[i - 1].owner_id;
            if (rank_out)
                *rank_out = held.entries[i - 1].rank;
            return true;
        }
    }
    if (owner_out)
        *owner_out = 0;
    if (rank_out)
        *rank_out = 0;
    return false;
}

void spinlock_annotate_site(const void *addr, const char *site) noexcept {
    CpuHeldLocks &held = own_held();
    for (uint64_t i = held.depth; i > 0; --i) {
        if (held.entries[i - 1].addr == addr) {
            held.entries[i - 1].site = site;
            return;
        }
    }
}

uint64_t spinlock_dump_held(uint64_t *addr_out, const char **site_out,
                            uint64_t *owner_out, uint8_t *rank_out,
                            uint8_t *cpu_out, uint8_t *take_if_out,
                            size_t max) noexcept {
    const CpuHeldLocks &held = own_held();
    const uint64_t n = held.depth < max ? held.depth : max;
    for (uint64_t i = 0; i < n; ++i) {
        if (addr_out)
            addr_out[i] =
                reinterpret_cast<uint64_t>(held.entries[i].addr);
        if (site_out)
            site_out[i] = held.entries[i].site;
        if (owner_out)
            owner_out[i] = held.entries[i].owner_id;
        if (rank_out)
            rank_out[i] = held.entries[i].rank;
        if (cpu_out)
            cpu_out[i] = held.entries[i].cpu;
        if (take_if_out)
            take_if_out[i] = held.entries[i].take_if;
    }
    return n;
}

/// @brief Copy the last take and the last release recorded for @p addr
///        (all slots scanned; addr-tag disambiguates collisions).
///        take_*_out describe the most recent take (take_ord==0 = none
///        recorded); rel_*_out the most recent release.  If take_ord >
///        rel_ord, a take is outstanding with no later release — the lock
///        is held by take_owner and nobody unlocked it since.  Any
///        out-pointer may be null.  Returns true when any slot holds this
///        address at all.  evictor_out (may be null) receives the hashed
///        slot's current occupant, or 0 when the slot is empty or still
///        holds this address — distinguishes "evicted by a colliding
///        lock" from "never taken".
bool spinlock_lock_last(const void *addr, uint64_t *take_ord_out,
                        uint64_t *take_owner_out, uint8_t *take_rank_out,
                        uint8_t *take_cpu_out, uint8_t *take_if_out,
                        uint64_t *take_tick_out,
                        uint64_t *rel_ord_out, uint64_t *rel_owner_out,
                        uint8_t *rel_rank_out, uint8_t *rel_cpu_out,
                        uint8_t *rel_if_out, uint64_t *rel_tick_out,
                        uint64_t *evictor_out) noexcept {
    const uint64_t want = reinterpret_cast<uint64_t>(addr);
    // Scan all slots: the hashed slot may have been claimed by a
    // colliding address after this lock's take.  The addr tag
    // disambiguates; on a miss, evictor_out reports the hashed slot's
    // current occupant (distinguishes "evicted" from "never taken").
    uint64_t evictor = 0;
    {
        const LockLastEvent &hashed =
            g_lock_last[(want >> 8) % kLockLastSlots];
        evictor = __atomic_load_n(&hashed.addr, __ATOMIC_ACQUIRE);
    }
    if (evictor_out)
        *evictor_out = (evictor == want) ? 0 : evictor;
    for (uint64_t s = 0; s < kLockLastSlots; ++s) {
        const LockLastEvent &slot = g_lock_last[s];
        if (__atomic_load_n(&slot.addr, __ATOMIC_ACQUIRE) != want)
            continue;
        if (take_ord_out)
            *take_ord_out =
                __atomic_load_n(&slot.take_ord, __ATOMIC_RELAXED);
        if (take_owner_out)
            *take_owner_out =
                __atomic_load_n(&slot.take_owner, __ATOMIC_RELAXED);
        if (take_rank_out)
            *take_rank_out =
                __atomic_load_n(&slot.take_rank, __ATOMIC_RELAXED);
        if (take_cpu_out)
            *take_cpu_out =
                __atomic_load_n(&slot.take_cpu, __ATOMIC_RELAXED);
        if (take_if_out)
            *take_if_out =
                __atomic_load_n(&slot.take_if, __ATOMIC_RELAXED);
        if (take_tick_out)
            *take_tick_out =
                __atomic_load_n(&slot.take_tick, __ATOMIC_RELAXED);
        if (rel_ord_out)
            *rel_ord_out =
                __atomic_load_n(&slot.rel_ord, __ATOMIC_RELAXED);
        if (rel_owner_out)
            *rel_owner_out =
                __atomic_load_n(&slot.rel_owner, __ATOMIC_RELAXED);
        if (rel_rank_out)
            *rel_rank_out =
                __atomic_load_n(&slot.rel_rank, __ATOMIC_RELAXED);
        if (rel_cpu_out)
            *rel_cpu_out =
                __atomic_load_n(&slot.rel_cpu, __ATOMIC_RELAXED);
        if (rel_if_out)
            *rel_if_out =
                __atomic_load_n(&slot.rel_if, __ATOMIC_RELAXED);
        if (rel_tick_out)
            *rel_tick_out =
                __atomic_load_n(&slot.rel_tick, __ATOMIC_RELAXED);
        return true;
    }
    return false;
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
    for (uint64_t i = 0; i < kLockLastSlots; ++i)
        g_lock_last[i] = LockLastEvent{};
    g_lock_last_ord = 0;
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
