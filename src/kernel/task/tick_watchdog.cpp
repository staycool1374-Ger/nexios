/// @file tick_watchdog.cpp
/// @brief TSC-domain tick watchdog (M2, issue #546 §6.2 item 6).

#include <kernel/task/tick_watchdog.hpp>
#include <kernel/arch/hal/timer.hpp>
#include <kernel/arch/hal/irq_guard.hpp>
#include <kernel/sync/irq_contract.hpp> // issue #546-C grace auto-derive
#include <kernel/arch/cpu_context.hpp>
#include <kernel/arch/io.hpp>
#include <kernel/task/scheduler.hpp>
#include <kernel/task/task.hpp>
#include <kernel/sync/park_lock.hpp>
#include <kernel/log/dmesg.hpp>
#include <kernel/log/dmesg_catalog.hpp>
#include <lib/logger.hpp>
#include <lib/assert.hpp>

namespace kernel {
namespace tick_watchdog {
namespace {

bool g_usable = false; // raw_counter_freq_hz() != 0 observed at least once
uint64_t g_grace_cycles = 0;
uint64_t g_grace_source = 0; // 0 = floor default, 1 = measured auto-derive
uint64_t g_last_counter[CONFIG_MAX_CPUS] = {};
uint64_t g_last_ticks[CONFIG_MAX_CPUS] = {};
const char *g_fire_where = nullptr;
bool g_fired = false;
uint64_t g_fire_count = 0;
uint64_t g_overrun_cycles = 0;
bool g_enabled_cached = false;

inline void try_enable() noexcept {
    if (g_enabled_cached)
        return;
    if (CONFIG_TICK_WATCHDOG_ENABLE == 0)
        return;
    const uint64_t freq = arch::Timer::raw_counter_freq_hz();
    if (freq == 0 || CONFIG_TICK_HZ == 0) {
        return; // Fail closed until the counter is calibrated.
    }
    // Issue #546-C: grace = max(floor, 10 x measured lateness), in raw
    // counter cycles (the checkpoint compares cycles, not ticks).
    // Monotone (never shrinks). The measured input is Increment A's
    // tick_late_max_cycles high-water, converted with the raw-counter
    // frequency; until any measurement exists the floor stands.
    // Saturating multiply-then-divide keeps full precision.
    const uint64_t floor_ticks =
        static_cast<uint64_t>(CONFIG_TICK_WATCHDOG_GRACE_TICKS) != 0
            ? static_cast<uint64_t>(CONFIG_TICK_WATCHDOG_GRACE_TICKS)
            : 1;
    const uint64_t per_tick =
        freq / static_cast<uint64_t>(CONFIG_TICK_HZ);
    uint64_t candidate_ticks = floor_ticks;
    uint64_t source = 0;
    if (per_tick != 0) {
        const uint64_t measured =
            kernel::irq_contract::tick_late_max_cycles() / per_tick;
        uint64_t auto_gate = measured;
        if (auto_gate > ~0ULL / 10U) {
            auto_gate = ~0ULL / 10U;
        }
        auto_gate *= 10U;
        if (auto_gate > candidate_ticks) {
            candidate_ticks = auto_gate;
            source = 1;
        }
    }
    uint64_t candidate_cycles = candidate_ticks;
    if (candidate_cycles > ~0ULL / freq) {
        candidate_cycles = ~0ULL / freq;
    }
    candidate_cycles *= freq;
    candidate_cycles /= static_cast<uint64_t>(CONFIG_TICK_HZ);
    if (candidate_cycles > g_grace_cycles) {
        g_grace_cycles = candidate_cycles;
        g_grace_source = source;
    }
    if (g_grace_cycles == 0) {
        g_grace_cycles = 1;
    }
    g_enabled_cached = true;
    g_usable = true;
}

inline void fire(const char *where, uint64_t elapsed) noexcept {
    g_fired = true;
    ++g_fire_count;
    g_overrun_cycles = elapsed;
    g_fire_where = where;
    const uint64_t cpu = arch::cpu_index() % CONFIG_MAX_CPUS;
    const uint64_t last_ticks =
        __atomic_load_n(&g_last_ticks[cpu], __ATOMIC_RELAXED);
    const auto *cur = Scheduler::current_task();
    const uint64_t cur_id = cur != nullptr ? cur->id : 0;
    const uint64_t cur_state =
        cur != nullptr ? static_cast<uint64_t>(cur->state) : 99ULL;
    kernel::Logger::error(
        "[TICKWD] FIRE where=%s overrun_cyc=%lu ticks=%lu cur=%lu st=%lu "
        "if=%d irqd=%lu isr=%lu deny=%lu park=%lu denied=%lu foreign=%lu "
        "rearm=%lu",
        where, elapsed, last_ticks, cur_id, cur_state,
        arch::interrupts_enabled() ? 1 : 0, arch::irq_guard_depth_own(),
        kernel::isr_nesting_own(), sync::lock_park_denied_reason(),
        sync::lock_park_count(), sync::lock_denied_escape_count(),
        sync::lock_foreign_depth_count(), sync::lock_park_rearm_count());
    log::dmesg_push_sev(log::ErrorSubsystem::TIMING,
                        log::kDmesgBase_TIMING + 8, log::LogSeverity::ERROR,
                        "tick_watchdog", cur_id);
#if CONFIG_TICK_WATCHDOG_ACTION == 2
    panic("[TICKWD] tick stalled");
#endif
}

} // namespace

void init() noexcept {
    try_enable();
}

void note_tick() noexcept {
    try_enable(); // Late calibration enables on first measurable tick.
    if (!g_usable) {
        return;
    }
    // Issue #546-C: UP-only snapshot. An AP tick path must not move the
    // BSP snapshot (undocumented SMP semantics); fail closed instead.
    const uint64_t cpu = arch::cpu_index() % CONFIG_MAX_CPUS;
    if (arch::cpu_index() != 0) {
        return;
    }
    __atomic_store_n(&g_last_counter[cpu], arch::Timer::raw_counter(),
                     __ATOMIC_RELAXED);
    __atomic_store_n(&g_last_ticks[cpu], arch::Timer::ticks(),
                     __ATOMIC_RELAXED);
}

void checkpoint(const char *where) noexcept {
    if (!g_usable || Scheduler::is_test_active()) {
        return;
    }
    const uint64_t cpu = arch::cpu_index() % CONFIG_MAX_CPUS;
    const uint64_t last_counter =
        __atomic_load_n(&g_last_counter[cpu], __ATOMIC_RELAXED);
    const uint64_t last_ticks =
        __atomic_load_n(&g_last_ticks[cpu], __ATOMIC_RELAXED);
    if (last_counter == 0) {
        return; // No tick observed yet — nothing to compare against.
    }
    const uint64_t now = arch::Timer::raw_counter();
    const uint64_t ticks_now = arch::Timer::ticks();
    if (ticks_now != last_ticks) {
        return; // Tick advanced since the last note — alive.
    }
    const uint64_t elapsed =
        now >= last_counter ? now - last_counter : 0;
    if (elapsed > g_grace_cycles && !g_fired) {
        fire(where, elapsed);
    }
}

void idle_checkpoint(const char *where) noexcept {
    (void)where;
    // Resync only, never fire: idle-current implies no schedulable work
    // is stranded (any READY task would have been dispatched instead).
    if (!g_usable || Scheduler::is_test_active()) {
        return;
    }
    const uint64_t cpu = arch::cpu_index() % CONFIG_MAX_CPUS;
    __atomic_store_n(&g_last_counter[cpu], arch::Timer::raw_counter(),
                     __ATOMIC_RELAXED);
    __atomic_store_n(&g_last_ticks[cpu], arch::Timer::ticks(),
                     __ATOMIC_RELAXED);
}

bool fired() noexcept {
    return g_fired;
}

uint64_t fire_count() noexcept {
    return g_fire_count;
}

uint64_t last_evidence() noexcept {
    return g_overrun_cycles;
}

TickWatchdogRecord read_tick_watchdog() noexcept {
    TickWatchdogRecord out{};
    out.grace_cycles = g_grace_cycles;
    out.grace_source = g_grace_source;
    out.enabled = g_usable;
    out.fired = g_fired;
    out.fire_count = g_fire_count;
    out.overrun_cycles = g_overrun_cycles;
    out.where_tag = g_fire_where;
    const uint64_t now_ticks = arch::Timer::ticks();
    uint64_t worst = 0;
    for (uint64_t cpu = 0; cpu < CONFIG_MAX_CPUS; ++cpu) {
        const uint64_t snap =
            __atomic_load_n(&g_last_ticks[cpu], __ATOMIC_RELAXED);
        if (snap != 0 && now_ticks >= snap) {
            const uint64_t stale = now_ticks - snap;
            if (stale > worst) {
                worst = stale;
            }
        }
    }
    out.per_cpu_last_stale = worst;
    return out;
}

void reset_for_test() noexcept {
    for (uint64_t cpu = 0; cpu < CONFIG_MAX_CPUS; ++cpu) {
        g_last_counter[cpu] = 0;
        g_last_ticks[cpu] = 0;
    }
    g_fired = false;
    g_fire_count = 0;
    g_overrun_cycles = 0;
    g_fire_where = nullptr;
}

void note_tick_override() noexcept {
    // A deliberate tick rewind (test seam) is not a stall: resync the
    // snapshot so the next checkpoint compares against fresh values.
    if (!g_usable) {
        return;
    }
    const uint64_t cpu = arch::cpu_index() % CONFIG_MAX_CPUS;
    __atomic_store_n(&g_last_counter[cpu], arch::Timer::raw_counter(),
                     __ATOMIC_RELAXED);
    __atomic_store_n(&g_last_ticks[cpu], arch::Timer::ticks(),
                     __ATOMIC_RELAXED);
}

} // namespace tick_watchdog
} // namespace kernel
