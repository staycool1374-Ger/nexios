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

/// @file test_cleanup.cpp
/// @brief Test cleanup and teardown tests.

#include <kernel/test/test_cleanup.hpp>
#include <kernel/test/test_isolate.hpp>
#include <kernel/test/resource_tracker.hpp>
#include <kernel/task/scheduler.hpp>
#include <kernel/task/task.hpp>
#include <kernel/driver/driver.hpp>
#include <kernel/log/dmesg.hpp>
#include <kernel/daemon/daemon_mgr.hpp>
#include <kernel/irq_thread.hpp>
#include <kernel/arch/hal/irq_guard.hpp>

namespace kernel::test {

void test_cleanup_all() {
    arch::IrqGuard guard;

    // ---- 1. Terminate and reap all test-created tasks ----
    // reload_daemon_tasks kills daemon tasks + user tasks, reaps orphans,
    // and restarts fresh daemon processes from initrd.
    reload_daemon_tasks();
    daemon::restart_stale_daemons();

    // ---- 2. Clean up any remaining non-idle, non-current tasks ----
    // Spare init (harness), the shell, and threaded-IRQ handler tasks so the
    // interactive system survives an in-shell `selftest` run.
    auto *current = Scheduler::current_task();
    auto *idle = Scheduler::get_idle_task();
    auto *harness = Scheduler::get_harness_task();
    auto *shell = Scheduler::get_shell_task();
    for (uint64_t i = 1; i < Scheduler::task_count(); ++i) {
        auto *t = Scheduler::task_at(i);
        if (!t || t == idle || t == current || t == harness || t == shell ||
            IrqThread::is_irq_thread_task(t))
            continue;
        t->state = TaskState::TERMINATED;
        t->exit_code = 0;
    }
    Scheduler::reap_orphans();

    // ---- 3. Unload all drivers ----
    DriverRegistry::unload_all();

    // ---- 4. Clear dmesg ----
    kernel::log::DmesgService::instance().clear();

    // ---- 5. Reset ResourceTracker counters to zero ----
    ResourceCounters zero = {};
    ResourceTracker::instance().restore(zero);
}

} // namespace kernel::test
