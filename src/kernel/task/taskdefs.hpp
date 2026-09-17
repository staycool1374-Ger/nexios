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

/// @file taskdefs.hpp
/// @brief Boot-time task definition table and reboot-from-table facility.

#pragma once

#include <types.hpp>
#include <kernel/task/sporadic_server.hpp>

namespace kernel {
namespace task {

/// @brief Classifies how a task is created from a TaskDef entry.
enum class TaskType : uint8_t {
    KERNEL,          ///< kernel-space function,  TaskControlBlock::create()
    USER_ELF,        ///< user-space ELF from initrd, elf::load()
    SPORADIC_SERVER, ///< user-space ELF + SporadicServer + daemon registration
};

/// @brief Single entry in the boot-time task-definition table.
/// Entries are validated at compile time against nexios_config.h bounds
/// and at boot time for ELF-file existence.  Set enabled = false to skip.
struct TaskDef {
    const char *name; ///< for logging / debug (max CONFIG_TASK_NAME_LEN)
    TaskType type;
    bool enabled;

    // KERNEL type
    void (*kernel_entry)();

    // USER_ELF / SPORADIC_SERVER
    const char *elf_path;

    uint64_t priority;
    uint64_t period_ticks;
    uint64_t wcet_ticks; ///< 0 = implicit 100% load (pessimistic), >0 = real
                         ///< estimate

    // SporadicServer params (only for SPORADIC_SERVER)
    uint64_t ss_budget;
    uint64_t ss_period;
    uint64_t ss_bg_prio;

    // Daemon registration (only for SPORADIC_SERVER)
    const char *daemon_name;
    void (*set_pid_fn)(uint64_t);
    uint64_t (*get_pid_fn)();

    uint64_t ss_budget_granularity; ///< for SPORADIC_SERVER (ticks per budget
                                    ///< unit, default 1)

    size_t user_stack_size; ///< for USER_ELF (0 = default 32_KiB)

    bool is_shell; ///< call Scheduler::set_shell_task after creation

    ServerMode ss_mode = ServerMode::SPORADIC; ///< server discipline
                                              ///< (issue #22; trailing so
                                              ///< existing rows keep binding;
                                              ///< default = SPORADIC)
};

/// @brief Kills all tasks (except idle), spawns the system from g_task_defs[],
///        then enters the idle loop.  Never returns.
void reboot_from_table() __attribute__((noreturn));

/// @brief Read-only task-table accessors (issue #24): the verify class
///        inspects budgets/modes without spawning.  No mutation, no alloc.
size_t taskdefs_count() noexcept;
const TaskDef *taskdefs_at(size_t i) noexcept; // nullptr when out of range
/// @brief Single-entry server-params rule (issue #24): the same predicate
///        the compile-time table check enforces, callable at runtime for
///        negative tests (e.g. BACKGROUND with non-idle bg priority).
bool taskdefs_server_params_valid(const TaskDef &d) noexcept;
/// @brief Whole-table validation at runtime (mirrors the static_assert).
bool taskdefs_valid() noexcept;

} // namespace task
} // namespace kernel
