/// @file tick_watchdog.cpp
/// @brief TSC-domain tick watchdog (M2, issue #546 §6.2 item 6).

#include <kernel/task/tick_watchdog.hpp>
#include <kernel/arch/hal/timer.hpp>
#include <kernel/arch/hal/irq_guard.hpp>
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
uint64_t g_last_counter = 0;
uint64_t g_last_ticks = 0;
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
    if (freq == 0 || CONFIG_TICK_HZ == 0)
        return; // Fail closed until the counter is calibrated.
    // Saturating multiply-then-divide (grace 500 × GHz-scale fits u64
    // with orders of margin; order chosen to keep full precision).
    uint64_t grace = CONFIG_TICK_WATCHDOG_GRACE_TICKS;
    if (grace > ~0ULL / freq)
        grace = ~0ULL / (freq != 0 ? freq : 1);
    grace *= freq;
    grace /= static_cast<uint64_t>(CONFIG_TICK_HZ);
    if (grace == 0)
        grace = 1;
    g_grace_cycles = grace;
    g_enabled_cached = true;
    g_usable = true;
}

inline void fire(const char *where, uint64_t elapsed) noexcept {
    g_fired = true;
    ++g_fire_count;
    g_overrun_cycles = elapsed;
    const auto *cur = Scheduler::current_task();
    const uint64_t cur_id = cur != nullptr ? cur->id : 0;
    const uint64_t cur_state =
        cur != nullptr ? static_cast<uint64_t>(cur->state) : 99ULL;
    kernel::Logger::error(
        "[TICKWD] FIRE where=%s overrun_cyc=%lu ticks=%lu cur=%lu st=%lu "
        "if=%d irqd=%lu isr=%lu deny=%lu park=%lu denied=%lu foreign=%lu "
        "rearm=%lu",
        where, elapsed, g_last_ticks, cur_id, cur_state,
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
    if (!g_usable)
        return;
    g_last_counter = arch::Timer::raw_counter();
    g_last_ticks = arch::Timer::ticks();
}

void checkpoint(const char *where) noexcept {
    if (!g_usable || Scheduler::is_test_active())
        return;
    if (g_last_counter == 0)
        return; // No tick observed yet — nothing to compare against.
    const uint64_t now = arch::Timer::raw_counter();
    const uint64_t ticks_now = arch::Timer::ticks();
    if (ticks_now != g_last_ticks)
        return; // Tick advanced since the last note — alive.
    const uint64_t elapsed =
        now >= g_last_counter ? now - g_last_counter : 0;
    if (elapsed > g_grace_cycles && !g_fired)
        fire(where, elapsed);
}

void idle_checkpoint(const char *where) noexcept {
    (void)where;
    // Resync only, never fire: idle-current implies no schedulable work
    // is stranded (any READY task would have been dispatched instead).
    if (!g_usable || Scheduler::is_test_active())
        return;
    g_last_counter = arch::Timer::raw_counter();
    g_last_ticks = arch::Timer::ticks();
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

void reset_for_test() noexcept {
    g_last_counter = 0;
    g_last_ticks = 0;
    g_fired = false;
    g_fire_count = 0;
    g_overrun_cycles = 0;
}

void note_tick_override() noexcept {
    // A deliberate tick rewind (test seam) is not a stall: resync the
    // snapshot so the next checkpoint compares against fresh values.
    if (!g_usable)
        return;
    g_last_counter = arch::Timer::raw_counter();
    g_last_ticks = arch::Timer::ticks();
}

} // namespace tick_watchdog
} // namespace kernel
