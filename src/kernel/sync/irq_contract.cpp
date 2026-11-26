/// @file sync/irq_contract.cpp
/// @brief IRQ/preemption contract enforcement (issue #546-A).
///        Atomics only: no locks, no allocation, no scheduler services,
///        no serial writes on any check path. Evidence is deferred to
///        the rate-limited WARN latch.
#include <kernel/sync/irq_contract.hpp>
#include <kernel/arch/hal/irq_guard.hpp>
#include <kernel/arch/hal/timer.hpp>
#include <kernel/arch/io.hpp>
#include <kernel/log/dmesg.hpp>
#include <kernel/sync/spinlock_order.hpp>
#include <kernel/task/task.hpp>
#include <kernel/kernel.hpp>

namespace kernel {
namespace irq_contract {

namespace {

// Violation counters, one per class (index = uint8_t(ViolClass)).
// Index 0 unused.
uint64_t s_violations[7] = {};

// One-shot WARN latch per class: dmesg emitted at most once per boot.
uint64_t s_warned_latch[7] = {};

// IRQ-off lateness high-water (raw counter cycles) + late-tick count +
// last tick raw_counter sample.
uint64_t s_tick_late_max_cycles = 0;
uint64_t s_tick_late_count = 0;
uint64_t s_tick_last_cycles = 0;

// Deferred halt latch (certification builds, policy == 2).
uint64_t s_halt_latched = 0;
const char *s_halt_site = nullptr;

// DeclaredWaitScope per-CPU slot (A.3): row id + entry triple + live.
const char *s_declared_row[CONFIG_MAX_CPUS] = {};
uint64_t s_declared_depth[CONFIG_MAX_CPUS] = {};
uint64_t s_declared_owner[CONFIG_MAX_CPUS] = {};
uint64_t s_declared_entry_if[CONFIG_MAX_CPUS] = {};
uint64_t s_declared_live[CONFIG_MAX_CPUS] = {};

// Late-tick factor: delta > kLateTickFactor × expected period counts as
// an IRQ-off window (TU-local; no new config key per A.6).
constexpr uint64_t kLateTickFactor = 2;

void count(ViolClass cls) noexcept {
    __atomic_fetch_add(&s_violations[static_cast<uint64_t>(cls)], 1,
                       __ATOMIC_RELAXED);
}

// Rate-limited evidence: one dmesg WARN per class per boot, then silent
// counting. Deferred so check paths never touch serial directly.
void warn_once(ViolClass cls, const char *site, uint64_t id) noexcept {
    const uint64_t idx = static_cast<uint64_t>(cls);
    if (__atomic_exchange_n(&s_warned_latch[idx], 1, __ATOMIC_RELAXED) != 0) {
        return;
    }
    log::dmesg_push_sev(log::ErrorSubsystem::SCHED,
                        log::kDmesgBase_SCHED + 25, log::LogSeverity::WARN,
                        site, static_cast<uintptr_t>(id));
}

void enforce(ViolClass cls, const char *site, uint64_t id) noexcept {
    count(cls);
#if CONFIG_IRQ_CONTRACT_ASSERT == 1
    warn_once(cls, site, id);
#elif CONFIG_IRQ_CONTRACT_ASSERT == 2
    warn_once(cls, site, id);
    // C6 never halts (A.4): exempt before latching.
    if (cls != ViolClass::C6) {
        request_halt(site);
    }
#endif
}

} // namespace

void check_wait_ctx(const char *site, bool self_owned,
                    bool declared) noexcept {
#if CONFIG_IRQ_CONTRACT_ASSERT == 0
    (void)site;
    (void)self_owned;
    (void)declared;
    return;
#else
    const ExecCtx ctx = classify();
    if (ctx == ExecCtx::Isr || ctx == ExecCtx::Boot) {
        enforce(ViolClass::C1, site, 0);
        return;
    }
    if (!self_owned && !declared) {
        enforce(ViolClass::C2, site, 0);
    }
#endif
}

void check_block_store(const char *site, const TaskControlBlock *target,
                       bool self) noexcept {
#if CONFIG_IRQ_CONTRACT_ASSERT == 0
    (void)site;
    (void)target;
    (void)self;
    return;
#else
    (void)self;
    const ExecCtx ctx = classify();
    // Attributed id: the target when valid, else 0 (never trust a wild
    // pointer for evidence; the actor context is still recorded by class).
    uint64_t target_id = 0;
    if (target != nullptr &&
        target->magic == TaskControlBlock::TCB_MAGIC) {
        target_id = target->id;
    }
    // SELF/FOREIGN split (INV-BLK-1/INV-BLK-2): a task blocking itself
    // must be in a wait-capable context (ISR/Boot self-block is C1);
    // a FOREIGN store never asserts the blocker's IF.
    if (self) {
        if (ctx == ExecCtx::Isr || ctx == ExecCtx::Boot) {
            // Known tension: the pager-fault path blocks the faulting
            // task inside the #PF ISR by paper-§4.8 design and trips C1;
            // the A.5 matrix decides exemption-row vs redesign.
            enforce(ViolClass::C1, site, target_id);
            return;
        }
    } else {
        // FOREIGN: assert the enforceable half of "victim holds no
        // spinlock" — the actor must hold none while moving another task
        // to BLOCKED. (A true victim-side check needs per-task lock
        // tracking, which does not exist.)
        if (sync::spinlock_held_by_current_count() != 0) {
            enforce(ViolClass::C2, site, target_id);
            return;
        }
        return;
    }
    // SELF store in task/trap context: no spinlock held across the
    // transition (holder-never-blocks).
    if (sync::spinlock_held_by_current_count() != 0) {
        enforce(ViolClass::C2, site, target_id);
        return;
    }
#endif
}

void syscall_exit_epilogue(const char *tag) noexcept {
#if CONFIG_IRQ_CONTRACT_ASSERT == 0
    (void)tag;
    return;
#else
    // Exit must leave no live guard and no held spinlock behind.
    if (arch::irq_guard_depth_own() != 0) {
        enforce(ViolClass::C2, tag, 0);
        return;
    }
    if (!sync::spinlock_contract_check_clean(tag)) {
        enforce(ViolClass::C2, tag, 0);
        return;
    }
#endif
}

void note_tick_arrival() noexcept {
#if CONFIG_IRQ_CONTRACT_ASSERT == 0
    return;
#else
    const uint64_t now = arch::Timer::raw_counter();
    const uint64_t freq = arch::Timer::raw_counter_freq_hz();
    const uint64_t prev =
        __atomic_exchange_n(&s_tick_last_cycles, now, __ATOMIC_RELAXED);
    if (prev == 0 || freq == 0) {
        return;
    }
    const uint64_t expected = freq / CONFIG_TICK_HZ;
    const uint64_t delta = now - prev;
    if (expected != 0 && delta > kLateTickFactor * expected) {
        __atomic_fetch_add(&s_tick_late_count, 1, __ATOMIC_RELAXED);
        uint64_t cur = __atomic_load_n(&s_tick_late_max_cycles,
                                       __ATOMIC_RELAXED);
        while (delta > cur) {
            if (__atomic_compare_exchange_n(&s_tick_late_max_cycles, &cur,
                                            delta, true, __ATOMIC_RELAXED,
                                            __ATOMIC_RELAXED)) {
                break;
            }
        }
    }
#endif
}

void request_halt(const char *site) noexcept {
    __atomic_exchange_n(&s_halt_latched, 1, __ATOMIC_RELAXED);
    s_halt_site = site;
}

void flush_halt() noexcept {
#if CONFIG_IRQ_CONTRACT_ASSERT == 2
    if (__atomic_load_n(&s_halt_latched, __ATOMIC_RELAXED) != 0) {
        panic("[IRQ-CONTRACT] halt latched");
    }
#else
    return;
#endif
}

uint64_t violation_count(ViolClass cls) noexcept {
    return __atomic_load_n(&s_violations[static_cast<uint64_t>(cls)],
                           __ATOMIC_RELAXED);
}

uint64_t tick_late_max_cycles() noexcept {
    return __atomic_load_n(&s_tick_late_max_cycles, __ATOMIC_RELAXED);
}

uint64_t tick_late_count() noexcept {
    return __atomic_load_n(&s_tick_late_count, __ATOMIC_RELAXED);
}

bool declared_wait_enter(const char *row) noexcept {
    const uint64_t cpu = arch::cpu_index() % CONFIG_MAX_CPUS;
    if (__atomic_load_n(&s_declared_live[cpu], __ATOMIC_RELAXED) != 0) {
        return false;
    }
    s_declared_row[cpu] = row;
    s_declared_depth[cpu] = arch::irq_guard_depth_own();
    s_declared_owner[cpu] = arch::irq_guard_owner_own();
    s_declared_entry_if[cpu] = arch::interrupts_enabled() ? 1 : 0;
    __atomic_store_n(&s_declared_live[cpu], 1, __ATOMIC_RELAXED);
    return true;
}

bool declared_wait_exit_ok(const char *row) noexcept {
    const uint64_t cpu = arch::cpu_index() % CONFIG_MAX_CPUS;
    if (__atomic_load_n(&s_declared_live[cpu], __ATOMIC_RELAXED) == 0) {
        return false;
    }
    const bool ok =
        s_declared_row[cpu] == row &&
        s_declared_depth[cpu] == arch::irq_guard_depth_own() &&
        s_declared_owner[cpu] == arch::irq_guard_owner_own() &&
        s_declared_entry_if[cpu] ==
            (arch::interrupts_enabled() ? 1 : 0);
    __atomic_store_n(&s_declared_live[cpu], 0, __ATOMIC_RELAXED);
    return ok;
}

bool declared_wait_live() noexcept {
    const uint64_t cpu = arch::cpu_index() % CONFIG_MAX_CPUS;
    return __atomic_load_n(&s_declared_live[cpu], __ATOMIC_RELAXED) != 0;
}

void reset_for_test() noexcept {
    for (uint64_t i = 0; i < 7; ++i) {
        s_violations[i] = 0;
        s_warned_latch[i] = 0;
    }
    s_tick_late_max_cycles = 0;
    s_tick_late_count = 0;
    s_tick_last_cycles = 0;
    s_halt_latched = 0;
    s_halt_site = nullptr;
    for (uint64_t cpu = 0; cpu < CONFIG_MAX_CPUS; ++cpu) {
        s_declared_row[cpu] = nullptr;
        s_declared_depth[cpu] = 0;
        s_declared_owner[cpu] = 0;
        s_declared_entry_if[cpu] = 0;
        s_declared_live[cpu] = 0;
    }
}

DeclaredWaitScope::DeclaredWaitScope(const char *row) noexcept
    : row_(row) {
    if (!declared_wait_enter(row)) {
        enforce(ViolClass::C3, row, 0);
    }
}

DeclaredWaitScope::~DeclaredWaitScope() noexcept {
    if (!declared_wait_exit_ok(row_)) {
        enforce(ViolClass::C3, row_, 0);
    }
}

} // namespace irq_contract
} // namespace kernel
