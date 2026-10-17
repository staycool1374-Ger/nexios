#pragma once

/// @file scheduler_admission_internal.hpp
/// @brief Shared cross-TU declarations for scheduler_admission.cpp (issue
///        #323).  Pure move: no logic changes.
///        Exposes the Liu-Leyland LUB table and the admission numerator
///        helper, which the legacy warn-only path in scheduler.cpp
///        (Scheduler::add_task) still consumes directly.

#include <kernel/task/scheduler.hpp>

namespace kernel::sched_admission {

// Liu-Leyland Rate-Monotonic LUB bounds (scaled by 1000000)
inline constexpr uint64_t LIU_LEYLAND_MAX_TASKS = 20;
inline constexpr uint32_t LIU_LEYLAND_BOUNDS[LIU_LEYLAND_MAX_TASKS + 1] = {
    0,      1000000, 828427, 779763, 756828, 743491, 734772,
    728626, 724061,  720537, 717734, 715451, 713557, 711958,
    710592, 709409,  708378, 707472, 706669, 705952, 705298};
inline constexpr uint32_t LIU_LEYLAND_LIMIT = 693147;

/// @brief Frozen admission numerator (issues #20/#22): explicit WCET, else
///        the attached server's max budget C, else remaining_ticks.
uint64_t server_budget_for_admission(const TaskControlBlock &t) noexcept;

}  // namespace kernel::sched_admission
