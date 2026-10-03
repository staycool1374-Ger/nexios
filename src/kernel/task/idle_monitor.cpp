/*
 * NexIOS RTOS — Development Roadmap / Kernel Core
 * Copyright (C) 2026 Arnold Hasshold
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

/// @file idle_monitor.cpp
/// @brief Idle-task safety monitors, foundation slice (issues #43/#282).
///        M1-M6 contract (docs/specs/idle_monitor.md §1): BSP-idle only,
///        chunk-budgeted, noexcept, no allocation, no new locks, skip
///        invalid, test seams, never kills.

#include <kernel/task/idle_monitor.hpp>
#include <kernel/task/all_tasks_registry.hpp>
#include <kernel/task/scheduler.hpp>
#include <kernel/task/task.hpp>
#include <kernel/nexios_config.h>
#include <kernel/arch/hal/timer.hpp>
#include <kernel/log/dmesg.hpp>
#include <kernel/log/dmesg_catalog.hpp>

namespace kernel {

namespace {

/// @brief M4 guard: safe to inspect (address range + TCB magic, the
///        same two checks as AllTasksRegistry's safe_tcb, which is
///        TU-local there) and not idle, not running anywhere.
bool scan_candidate(const TaskControlBlock *t) noexcept {
    if (!t)
        return false;
    if (reinterpret_cast<uint64_t>(t) < 0xFFFF800000000000ULL)
        return false;
    if (!TaskControlBlock::is_valid(t))
        return false;
    if (Scheduler::is_idle_task(t))
        return false;
    if (Scheduler::is_current_on_any_cpu(t))
        return false;
    return true;
}

/// @brief Advance a chunked scan over the registry from @p cursor.
/// @tparam Visit Callable (TaskControlBlock*) -> void, pure reads +
///         flag sets only (M3/M6).
/// @return Progress; cursor wraps to head at the end (wrapped=true).
template <typename Visit>
IdleScanProgress scan_chunk(IdleScanCursor &cursor, uint64_t budget,
                            Visit visit) noexcept {
    IdleScanProgress progress = {};
    const AllTasksRegistry &registry = Scheduler::all_tasks();
    TaskControlBlock *pos =
        cursor.pos != nullptr ? cursor.pos : registry.first_ptr();
    uint64_t remaining = budget;
    while (pos != nullptr && remaining > 0) {
        TaskControlBlock *current = pos;
        pos = registry.next_ptr(pos);
        if (pos == nullptr)
            progress.wrapped = true;
        if (!scan_candidate(current)) {
            ++progress.skipped;
            continue;
        }
        visit(current);
        ++progress.scanned;
        --remaining;
    }
    cursor.pos = pos;
    return progress;
}

} // namespace

IdleScanProgress idle_scan_stack(IdleScanCursor &cursor,
                                 uint64_t budget) noexcept {
    const auto verify = [](TaskControlBlock *t) noexcept {
        uint8_t bad_segment = 0;
        uint64_t bad_va = 0;
        const bool user_ok = canary_verify_user_segments(t, bad_segment,
                                                          bad_va);
        const bool kstack_ok = canary_verify_kernel_stack(t);
        idle_publish_stack_low_water(t->kstack_low_water_,
                                     t->kernel_stack_top,
                                     t->stack_low_water_bytes);
        if (!user_ok || !kstack_ok) {
            // Existing escalation path only (spec §3 E1): production
            // fail-stops here exactly as the guard-page #PF path does.
            // The scanner itself kills/reaps/reprioritizes nothing (M6).
#if CONFIG_STACK_OVERFLOW_HOOK
            stack_overflow_hook(t);
#endif
        }
    };
    return scan_chunk(cursor, budget, verify);
}

IdleScanProgress idle_scan_stall(IdleScanCursor &cursor, uint64_t budget,
                                 uint64_t now_tick) noexcept {
    const auto flag = [now_tick](TaskControlBlock *t) noexcept {
        if (t->last_progress_tick == 0)
            return;
        if (now_tick < t->last_progress_tick)
            return;
        if (now_tick - t->last_progress_tick >
            static_cast<uint64_t>(
                CONFIG_IDLE_MONITOR_STALL_THRESHOLD_TICKS))
            t->stuck_suspected = true;
    };
    return scan_chunk(cursor, budget, flag);
}

IdleScanProgress idle_aggregate_util(IdleScanCursor &cursor,
                                     uint64_t budget) noexcept {
    const auto aggregate = [](TaskControlBlock *t) noexcept {
        t->util_per_mille = idle_util_for(t->exec_period_ns,
                                          t->period_ticks);
        // block_per_mille / preempt_per_mille intentionally untouched:
        // no such sinks exist (see header). deadline_meets is owned by
        // the period-reload sites, never written here.
    };
    return scan_chunk(cursor, budget, aggregate);
}

uint32_t idle_util_for(uint64_t exec_ns, uint64_t period_ticks) noexcept {
    constexpr uint64_t kNsPerTick = 1000000ULL;
    constexpr uint64_t kPerMilleMax = 1000;
    if (period_ticks == 0 ||
        period_ticks == TaskControlBlock::NO_PERIOD)
        return 0;
    const uint64_t period_ns = period_ticks * kNsPerTick;
    if (period_ns == 0)
        return 0;
    uint64_t util = kPerMilleMax;
    if (exec_ns <= UINT64_MAX / kPerMilleMax)
        util = (exec_ns * kPerMilleMax) / period_ns;
    if (util > kPerMilleMax)
        util = kPerMilleMax;
    return static_cast<uint32_t>(util);
}

void idle_note_period_reload(TaskControlBlock &t) noexcept {
    if (!t.deadline_missed && t.deadline_meets < UINT32_MAX)
        ++t.deadline_meets;
}

bool idle_note_exec_sample(TaskControlBlock &t) noexcept {
    if (t.exec_period_ns <= t.wcet_observed_ns)
        return false;
    const uint64_t prev_observed = t.wcet_observed_ns;
    t.wcet_observed_ns = t.exec_period_ns;
    constexpr uint64_t kNsPerTick = 1000000ULL;
    const uint64_t bound_ns = (t.wcet_ticks > UINT64_MAX / kNsPerTick)
                                  ? UINT64_MAX
                                  : t.wcet_ticks * kNsPerTick;
    if (t.wcet_ticks > 0 && prev_observed <= bound_ns &&
        t.wcet_observed_ns > bound_ns) {
        log::dmesg_push_sev(log::ErrorSubsystem::TIMING,
                            log::kDmesgBase_TIMING + 5,
                            log::LogSeverity::WARN, t.name,
                            t.wcet_observed_ns - bound_ns);
        return true;
    }
    return false;
}

IdleScanProgress idle_scan_mem(IdleScanCursor &cursor,
                                 uint64_t budget) noexcept {
    const auto check = [](TaskControlBlock *t) noexcept {
        const uint64_t outstanding = (t->mem_alloc_ops_ >= t->mem_free_ops_)
                                         ? t->mem_alloc_ops_ - t->mem_free_ops_
                                         : 0;
        const uint64_t min_ops = static_cast<uint64_t>(
            CONFIG_IDLE_MONITOR_MEM_MIN_OPS);
        if (outstanding > t->mem_prev_outstanding_ + min_ops &&
            t->memory_used_pages_ == t->mem_prev_used_) {
            if (t->mem_leak_streak_ < UINT32_MAX)
                ++t->mem_leak_streak_;
        } else {
            t->mem_leak_streak_ = 0;
        }
        t->mem_prev_outstanding_ = outstanding;
        t->mem_prev_used_ = t->memory_used_pages_;
        if (t->mem_leak_streak_ ==
            static_cast<uint32_t>(CONFIG_IDLE_MONITOR_MEM_PASSES)) {
            // Report only (M6): no kill, no reap, no handler — one WARN
            // per streak, auto re-armed when the streak breaks.
            log::dmesg_push_sev(log::ErrorSubsystem::PMM,
                                log::kDmesgBase_PMM + 1,
                                log::LogSeverity::WARN, t->name,
                                outstanding);
        }
    };
    return scan_chunk(cursor, budget, check);
}

void idle_publish_stack_low_water(uint64_t low_water_rsp, uint64_t stack_top,
                                  uint32_t &dst) noexcept {
    if (low_water_rsp == 0 || stack_top <= low_water_rsp) {
        dst = 0;
        return;
    }
    const uint64_t bytes = stack_top - low_water_rsp;
    dst = (bytes > static_cast<uint64_t>(UINT32_MAX))
              ? UINT32_MAX
              : static_cast<uint32_t>(bytes);
}

void idle_monitor_slice() noexcept {
#if !CONFIG_IDLE_MONITOR_ENABLED
    return;
#else
    static IdleScanCursor stack_cursor = {};
    static IdleScanCursor stall_cursor = {};
    static IdleScanCursor util_cursor = {};
    static IdleScanCursor mem_cursor = {};
    constexpr uint64_t kChunk =
        static_cast<uint64_t>(CONFIG_IDLE_MONITOR_CHUNK);
#if CONFIG_IDLE_MONITOR_STACK_CHECK
    (void)idle_scan_stack(stack_cursor, kChunk);
#endif
#if CONFIG_IDLE_MONITOR_STALL
    (void)idle_scan_stall(stall_cursor, kChunk, arch::Timer::ticks());
#endif
#if CONFIG_IDLE_MONITOR_UTIL
    (void)idle_aggregate_util(util_cursor, kChunk);
#endif
#if CONFIG_IDLE_MONITOR_MEM
    (void)idle_scan_mem(mem_cursor, kChunk);
#endif
#endif
}

} // namespace kernel
