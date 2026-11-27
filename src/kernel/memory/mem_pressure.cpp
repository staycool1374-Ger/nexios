/// @file kernel/memory/mem_pressure.cpp
/// @brief R-MEM unstable-state warning latch (M4, #575 kernel half).
#include <kernel/memory/mem_pressure.hpp>
#include <kernel/arch/hal/timer.hpp>
#include <kernel/log/dmesg.hpp>
#include <kernel/nexios_config.h>

namespace kernel {
namespace mem_pressure {

namespace {
/// Single-word latch state: 0 = clear, 1 = latched. Plain atomics —
/// no lock, no allocation, callable from any lock context.
uint8_t g_latched = 0;
MemPressureReason g_reason = MemPressureReason::NONE;
uint64_t g_count = 0;
bool g_pending = false;
uint64_t g_latch_tick = 0;
} // namespace

static uint64_t latch_ordinal(MemPressureReason reason) noexcept {
    // Ordinal first so every caller counts even when another wins.
    uint64_t ordinal =
        __atomic_fetch_add(&g_count, 1, __ATOMIC_RELAXED) + 1;
    uint8_t expected = 0;
    // First caller wins the row; everyone counts (sustained-pressure
    // telemetry without serial spam).
    if (!__atomic_compare_exchange_n(&g_latched, &expected, 1, false,
                                     __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {
        return ordinal;
    }
    // Winner-only stores (§11.6 all-or-nothing: every shared index is
    // atomic from every context — the losers never touch these).
    __atomic_store_n(&g_reason, reason, __ATOMIC_RELAXED);
    __atomic_store_n(&g_pending, true, __ATOMIC_RELAXED);
    __atomic_store_n(&g_latch_tick, arch::Timer::ticks(),
                     __ATOMIC_RELAXED);
    // Lock-free emit path only (dmesg_push_sev never takes a lock — same
    // contract the panic path relies on).
    log::dmesg_push_sev(log::ErrorSubsystem::PMM,
                        log::kDmesgBase_PMM + 4, log::LogSeverity::ERROR,
                        "mem: unstable-state warning (exhaustion/pressure)",
                        static_cast<uintptr_t>(reason));
    return ordinal;
}

void latch(MemPressureReason reason) noexcept {
    (void)latch_ordinal(reason);
}

bool latch_notable(MemPressureReason reason) noexcept {
    return (latch_ordinal(reason) % 64) == 0;
}

bool take_pending() noexcept {
    return __atomic_exchange_n(&g_pending, false, __ATOMIC_RELAXED);
}

uint64_t latch_tick() noexcept {
    return __atomic_load_n(&g_latch_tick, __ATOMIC_RELAXED);
}

void clear() noexcept {
    __atomic_store_n(&g_reason, MemPressureReason::NONE, __ATOMIC_RELAXED);
    __atomic_store_n(&g_pending, false, __ATOMIC_RELAXED);
    __atomic_store_n(&g_latched, 0, __ATOMIC_RELAXED);
}

void snapshot_reset() noexcept {
    __atomic_store_n(&g_reason, MemPressureReason::NONE, __ATOMIC_RELAXED);
    __atomic_store_n(&g_pending, false, __ATOMIC_RELAXED);
    __atomic_store_n(&g_latch_tick, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&g_count, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&g_latched, 0, __ATOMIC_RELAXED);
}

bool latched() noexcept {
    return __atomic_load_n(&g_latched, __ATOMIC_RELAXED) != 0;
}

MemPressureReason reason() noexcept {
    return __atomic_load_n(&g_reason, __ATOMIC_RELAXED);
}

uint64_t count() noexcept {
    return __atomic_load_n(&g_count, __ATOMIC_RELAXED);
}

bool pending_notify() noexcept {
    return __atomic_load_n(&g_pending, __ATOMIC_RELAXED);
}

uint8_t threshold_pct() noexcept {
    return CONFIG_MEM_PRESSURE_THRESHOLD_PCT;
}

} // namespace mem_pressure
} // namespace kernel
