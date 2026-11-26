/// @file task/liveness_ledger.cpp
/// @brief Positive liveness guarantee bookkeeping (issue #546-D).
///        Derived ages (resettable) + monotone escalation counters
///        (never rewound except the test seam). Atomics only; no locks,
///        no allocation, no scheduler services, no serial on hot paths.
#include <kernel/task/liveness_ledger.hpp>
#include <kernel/task/task.hpp>
#include <kernel/sync/park_lock.hpp>
#include <kernel/sync/irq_contract.hpp>
#include <kernel/arch/cpu_context.hpp>
#include <kernel/arch/io.hpp>
#include <kernel/log/dmesg.hpp>
#include <kernel/log/dmesg_catalog.hpp>

namespace kernel {
namespace liveness {

namespace {

// 128 id-hashed slots x CONFIG_MAX_CPUS CPUs, 16 B each (16 KB BSS).
LivenessSlot s_slots[CONFIG_MAX_CPUS][128] = {};

// Monotone family (never rewound by snapshot restore).
uint64_t s_wait_ceiling_count = 0;
uint64_t s_relief_fires = 0;
uint64_t s_relief_deferred = 0;
uint64_t s_breaker_fires = 0;

// WARNed task ids are tracked in-slot (rsvd bit 0), so the one-shot
// WARN is exact per task with no auxiliary table.
constexpr uint32_t kWarnedLatchBit = 1U << 0;

// Test seam ceiling (0 = use kWaitCeilingTicks).
uint32_t g_test_ceiling_ticks = 0;

constexpr uint64_t kIdHashMask = 127ULL; // ID_TABLE_SIZE=128 - 1

inline uint64_t slot_index(uint64_t task_id) noexcept {
    return task_id & kIdHashMask;
}

} // namespace

void note_task_tick(uint64_t task_id, uint8_t state, bool in_ready_queue,
                    bool admin_parked, bool sleep_armed,
                    bool wait_covered) noexcept {
#if CONFIG_LIVENESS_ACTION == 0
    (void)task_id;
    (void)state;
    (void)in_ready_queue;
    (void)admin_parked;
    (void)sleep_armed;
    (void)wait_covered;
    return;
#else
    (void)sleep_armed; // Coverage is decided by the caller (wait_covered).
    if (task_id == 0) {
        return;
    }
    const uint64_t cpu = arch::cpu_index() % CONFIG_MAX_CPUS;
    const uint64_t idx = slot_index(task_id);
    LivenessSlot &slot = s_slots[cpu][idx];
    // Owner check: adopt on mismatch (under-counts on collision, never
    // fabricates — colliding live tasks reset each other's ages).
    const uint64_t owner =
        __atomic_load_n(&slot.owner_id, __ATOMIC_RELAXED);
    if (owner != task_id) {
        __atomic_store_n(&slot.owner_id, task_id, __ATOMIC_RELAXED);
        __atomic_store_n(&slot.ready_age, static_cast<uint16_t>(0),
                         __ATOMIC_RELAXED);
        __atomic_store_n(&slot.wait_ticks, static_cast<uint16_t>(0),
                         __ATOMIC_RELAXED);
        return;
    }
    // READY + queued + not admin-parked = denied wait. RUNNING, BLOCKED
    // sleeps, admin parks, terminated/reaped all zero the ages.
    const TaskState task_state = static_cast<TaskState>(state);
    if (task_state == TaskState::READY && in_ready_queue && !admin_parked) {
        const uint16_t age =
            __atomic_load_n(&slot.ready_age, __ATOMIC_RELAXED);
        if (age < 65535) {
            __atomic_store_n(&slot.ready_age,
                             static_cast<uint16_t>(age + 1),
                             __ATOMIC_RELAXED);
        }
        return;
    }
    __atomic_store_n(&slot.ready_age, static_cast<uint16_t>(0),
                     __ATOMIC_RELAXED);
    // Covered BLOCKED waits (D.5 closed list is enforced by the caller
    // passing wait_covered only for peer/channel-released waits).
    // WAITING (sleep/delay) never reaches this call.
    if (task_state == TaskState::BLOCKED && wait_covered) {
        const uint16_t waited =
            __atomic_load_n(&slot.wait_ticks, __ATOMIC_RELAXED);
        if (waited < 65535) {
            __atomic_store_n(&slot.wait_ticks,
                             static_cast<uint16_t>(waited + 1),
                             __ATOMIC_RELAXED);
        }
    } else {
        __atomic_store_n(&slot.wait_ticks, static_cast<uint16_t>(0),
                         __ATOMIC_RELAXED);
    }
#endif
}

void note_relief_fire() noexcept {
    __atomic_fetch_add(&s_relief_fires, 1ULL, __ATOMIC_RELAXED);
}

void note_relief_deferred() noexcept {
    __atomic_fetch_add(&s_relief_deferred, 1ULL, __ATOMIC_RELAXED);
}

void note_breaker_fire() noexcept {
    __atomic_fetch_add(&s_breaker_fires, 1ULL, __ATOMIC_RELAXED);
}

bool note_wait_ceiling(uint64_t task_id) noexcept {
#if CONFIG_LIVENESS_ACTION == 0
    (void)task_id;
    return false;
#else
    const uint64_t cpu = arch::cpu_index() % CONFIG_MAX_CPUS;
    const uint64_t idx = slot_index(task_id);
    LivenessSlot &slot = s_slots[cpu][idx];
    const uint64_t owner =
        __atomic_load_n(&slot.owner_id, __ATOMIC_RELAXED);
    if (owner != task_id) {
        return false;
    }
    const uint32_t ceiling =
        g_test_ceiling_ticks != 0 ? g_test_ceiling_ticks
                                  : static_cast<uint32_t>(kWaitCeilingTicks);
    const uint64_t waited = __atomic_load_n(&slot.wait_ticks, __ATOMIC_RELAXED);
    // First crossing only: later ticks keep counting time but must not
    // re-fire (one WARN per task per boot; the count is monotone).
    if (waited != ceiling) {
        return waited > ceiling;
    }
    __atomic_fetch_add(&s_wait_ceiling_count, 1ULL, __ATOMIC_RELAXED);
#if CONFIG_LIVENESS_ACTION >= 1
    const uint32_t flags =
        __atomic_load_n(&slot.reserved, __ATOMIC_RELAXED);
    if ((flags & kWarnedLatchBit) == 0) {
        __atomic_store_n(&slot.reserved, flags | kWarnedLatchBit,
                         __ATOMIC_RELAXED);
        log::dmesg_push_sev(log::ErrorSubsystem::SCHED,
                            log::kDmesgBase_SCHED + 26,
                            log::LogSeverity::WARN, "liveness wait ceiling",
                            static_cast<uintptr_t>(task_id));
    }
#endif
#if CONFIG_LIVENESS_ACTION >= 2
    kernel::irq_contract::request_halt("liveness:wait_ceiling");
#endif
    return true;
#endif
}

LivenessReadout read_liveness() noexcept {
    LivenessReadout out{};
    out.wait_ceiling_count =
        __atomic_load_n(&s_wait_ceiling_count, __ATOMIC_RELAXED);
    out.relief_fires = __atomic_load_n(&s_relief_fires, __ATOMIC_RELAXED);
    out.relief_deferred =
        __atomic_load_n(&s_relief_deferred, __ATOMIC_RELAXED);
    out.breaker_fires =
        __atomic_load_n(&s_breaker_fires, __ATOMIC_RELAXED);
    uint64_t worst_age = 0;
    uint64_t worst_wait = 0;
    for (uint64_t cpu = 0; cpu < CONFIG_MAX_CPUS; ++cpu) {
        for (uint64_t i = 0; i < 128; ++i) {
            const uint64_t age =
                __atomic_load_n(&s_slots[cpu][i].ready_age, __ATOMIC_RELAXED);
            const uint64_t waited = __atomic_load_n(
                &s_slots[cpu][i].wait_ticks, __ATOMIC_RELAXED);
            if (age > worst_age) {
                worst_age = age;
            }
            if (waited > worst_wait) {
                worst_wait = waited;
            }
        }
    }
    out.max_ready_age = worst_age;
    out.wait_windows_max = worst_wait;
    out.holder_blocked_observed = sync::lock_probe_count();
    return out;
}

void set_wait_ceiling_for_test(uint32_t ticks) noexcept {
    g_test_ceiling_ticks = ticks;
}

void reset_liveness_ledger() noexcept {
    for (uint64_t cpu = 0; cpu < CONFIG_MAX_CPUS; ++cpu) {
        for (uint64_t i = 0; i < 128; ++i) {
            s_slots[cpu][i].owner_id = 0;
            s_slots[cpu][i].ready_age = 0;
            s_slots[cpu][i].wait_ticks = 0;
            s_slots[cpu][i].reserved = 0;
        }
    }
    g_test_ceiling_ticks = 0;
}

void reset_liveness_counters_for_test() noexcept {
    s_wait_ceiling_count = 0;
    s_relief_fires = 0;
    s_relief_deferred = 0;
    s_breaker_fires = 0;
}

} // namespace liveness
} // namespace kernel
