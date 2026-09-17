#pragma once

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

/// @file scheduler_errors.hpp
/// @brief Scheduler error codes and string lookup.

#pragma once

#include <types.hpp>
#include <assert.hpp>

namespace kernel::errors {

#define SCHEDULER_ERROR_CODES                                                  \
    X(OK, 0, "OK")                                                             \
    X(TABLE_FULL, 1, "Task table full (MAX_TASKS reached)")                    \
    X(NOT_FOUND, 2, "Task not found")                                          \
    X(DUPLICATE_ID, 3, "Task ID already in use")                               \
    X(INVALID_STATE, 4, "Task in invalid state for operation")                 \
    X(NO_CURRENT, 5, "No current task context")                                \
    X(NO_IDLE, 6, "Idle task not initialized")                                 \
    X(PREEMPT_DISABLED, 7, "Preemption is disabled")                           \
    X(ZOMBIE, 8, "Task is a zombie (freed but not reaped)")                    \
    X(INVALID_MAGIC, 9, "Task control block magic invalid")                    \
    X(NO_SHELL, 10, "Shell task not set")                                      \
    X(INVALID_ARGS, 11, "Invalid arguments")                                   \
    X(ADMISSION_DENIED, 12, "Admission denied: Liu-Leyland bound exceeded")    \
    X(WCET_INVALID, 13, "Invalid WCET: exceeds period or period untracked")

/// @brief Error codes returned by scheduler operations.
// NOLINTNEXTLINE(performance-enum-size)
enum SchedulerError : uint64_t {
#define X(name, num, msg) SCHED_ERR_##name = (num),
    SCHEDULER_ERROR_CODES
#undef X
};

/// @brief Return a human-readable string for a scheduler error code.
template <> inline const char *error_string(SchedulerError e) {
    switch (e) {
#define X(name, num, msg)                                                      \
    case SCHED_ERR_##name:                                                     \
        return msg;
        SCHEDULER_ERROR_CODES
#undef X
    }
    return "Unknown scheduler error";
}

} // namespace kernel::errors