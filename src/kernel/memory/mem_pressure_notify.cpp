/// @file kernel/memory/mem_pressure_notify.cpp
/// @brief R-MEM supervisor observation seam (issue #575, Increment 2).
#include <kernel/memory/mem_pressure_notify.hpp>
#include <kernel/memory/mem_pressure.hpp>
#include <kernel/memory/pmm.hpp>
#include <kernel/task/task.hpp>
#include <kernel/task/scheduler.hpp>
#include <kernel/sync/spinlock.hpp>
#include <kernel/sync/spinlock_guard.hpp>
#include <kernel/arch/hal/timer.hpp>

namespace kernel {
namespace mem_pressure {

namespace {
// Single supervisor slot. Guarded by a Leaf lock for registration and
// takeover only — service_pending() resolves via lock-free find_task
// and never takes this lock (INV-2: the poke path holds no locks).
sync::SpinLock g_supervisor_lock_{sync::LockRank::Leaf};
uint64_t g_supervisor_id = 0;
uint32_t g_supervisor_gen = 0;
bool g_supervisor_used = false;
// Post-freeze waterline baseline (INV-6): first-tick free count after
// freeze; 0 = not captured yet.
uint64_t g_waterline_baseline = 0;
uint64_t g_observed_tick = 0;

bool task_live_supervisor(const TaskControlBlock *t) {
    if (t == nullptr) {
        return false;
    }
    return t->magic == TaskControlBlock::TCB_MAGIC &&
           t->state != TaskState::TERMINATED &&
           t->state != TaskState::REAPED;
}
} // namespace

bool register_supervisor(TaskControlBlock &caller) noexcept {
    SpinLockGuard<sync::SpinLock> guard(g_supervisor_lock_);
    if (g_supervisor_used) {
        TaskControlBlock *occupant = Scheduler::find_task(g_supervisor_id);
        // Idempotent re-registration by the current occupant.
        if (occupant != nullptr && occupant->id == caller.id &&
            occupant->generation == g_supervisor_gen &&
            task_live_supervisor(occupant)) {
            return true;
        }
        // A different live occupant holds the slot: fail closed.
        if (occupant != nullptr &&
            occupant->generation == g_supervisor_gen &&
            task_live_supervisor(occupant)) {
            return false;
        }
        // Occupant dead or recycled: fall through to takeover.
    }
    __atomic_store_n(&g_supervisor_id, caller.id, __ATOMIC_RELAXED);
    __atomic_store_n(&g_supervisor_gen, caller.generation,
                     __ATOMIC_RELAXED);
    __atomic_store_n(&g_supervisor_used, true, __ATOMIC_RELAXED);
    return true;
}

void service_pending() noexcept {
    // (a) Post-freeze baseline capture (once).
    if (PMM::is_frozen() &&
        __atomic_load_n(&g_waterline_baseline, __ATOMIC_RELAXED) == 0) {
        __atomic_store_n(&g_waterline_baseline, PMM::free_memory(),
                         __ATOMIC_RELAXED);
    } else if (__atomic_load_n(&g_waterline_baseline, __ATOMIC_RELAXED) >
               0) {
        // (b) Waterline evaluation (no division, no overflow: both
        // operands are page counts, threshold_pct() <= 100).
        uint64_t baseline =
            __atomic_load_n(&g_waterline_baseline, __ATOMIC_RELAXED);
        uint64_t free_now = PMM::free_memory();
        uint64_t pct = threshold_pct();
        if (free_now * 100 <= baseline * (100 - pct)) {
            latch(MemPressureReason::PMM_PRESSURE);
        }
    }
    // (c) Pending Notify poke. No locks: resolve via lock-free find_task
    // and re-validate id + generation + liveness (mirrors
    // DeathNotify::on_task_death). take_pending() first would drop the
    // record when nobody resolves, so resolve-then-take.
    TaskControlBlock *sup = nullptr;
    {
        // Snapshot the slot without holding the lock across the poke.
        // The slot is only mutated under the Leaf lock above; a torn
        // read here resolves to no-supervisor (fail-closed) because the
        // generation re-check fails.
        uint64_t sup_id =
            __atomic_load_n(&g_supervisor_id, __ATOMIC_RELAXED);
        uint32_t sup_gen =
            __atomic_load_n(&g_supervisor_gen, __ATOMIC_RELAXED);
        bool used = __atomic_load_n(&g_supervisor_used, __ATOMIC_RELAXED);
        if (used) {
            TaskControlBlock *cand = Scheduler::find_task(sup_id);
            if (cand != nullptr && cand->generation == sup_gen &&
                task_live_supervisor(cand)) {
                sup = cand;
            }
        }
    }
    if (sup == nullptr) {
        return;
    }
    if (!take_pending()) {
        return;
    }
    sup->notify.notify(pulse_for(reason()));
    __atomic_store_n(&g_observed_tick, arch::Timer::ticks(),
                     __ATOMIC_RELAXED);
}

PressureRecord record() noexcept {
    PressureRecord out{};
    out.reason = static_cast<uint64_t>(reason());
    out.count = count();
    out.latch_tick = latch_tick();
    out.observed_tick =
        __atomic_load_n(&g_observed_tick, __ATOMIC_RELAXED);
    return out;
}

uint64_t observed_tick() noexcept {
    return __atomic_load_n(&g_observed_tick, __ATOMIC_RELAXED);
}

} // namespace mem_pressure
} // namespace kernel
