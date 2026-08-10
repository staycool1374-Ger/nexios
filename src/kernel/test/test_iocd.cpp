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

/// @file test_iocd.cpp
/// @brief I/O completion detection tests.

#include <test.hpp>
#include <logger.hpp>
#include <kernel/task/task.hpp>
#include <kernel/task/scheduler.hpp>
#include <kernel/ipc/ipc.hpp>
#include <kernel/driver/driver.hpp>
#include <kernel/driver/iocd.hpp>
#include <kernel/vfs/vfsd.hpp>
#include <kernel/daemon/daemon_mgr.hpp>

using namespace kernel;

// Runmode: kernel
// Testidea: IOCD boots and registers with the kernel
// Input: None (boot sequence)
// Expect: iocd::get_iocd_pid() returns a non-zero PID and the task exists
// Depends: kernel/driver/iocd
JARVIS_TEST(iocd_boots_and_registers, "PRE: vfsd, iocd | POST: iocd") {
    uint64_t pid = iocd::get_iocd_pid();
    Logger::info("[TEST:iocd_boots_and_registers] pid=%u vfsd_pid=%u", pid,
                 vfsd::get_vfsd_pid());
    JARVIS_ASSERT(pid != 0);
    uint64_t vfsd_pid = vfsd::get_vfsd_pid();
    JARVIS_ASSERT(vfsd_pid != 0);
    auto *vfsd_task = Scheduler::find_task(vfsd_pid);
    JARVIS_ASSERT(vfsd_task != nullptr);
    auto *task = Scheduler::find_task(pid);
    JARVIS_ASSERT(task != nullptr);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: STUB - Keyboard IRQ is converted to an IPC event
// Input: None (stub test)
// Expect: Passes (stub)
// Depends: kernel/task, kernel/ipc, kernel/driver
JARVIS_TEST(iocd_keyboard_irq_to_event, "PRE: none | POST: iocd") {
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: STUB - Serial IRQ is converted to an IPC event
// Input: None (stub test)
// Expect: Passes (stub)
// Depends: kernel/task, kernel/ipc, kernel/driver
JARVIS_TEST(iocd_serial_irq_to_event, "PRE: none | POST: iocd") {
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: STUB - MMIO region unmapped when task exits
// Input: None (stub test)
// Expect: Passes (stub)
// Depends: kernel/task, kernel/ipc, kernel/driver
JARVIS_TEST(iocd_mmio_unmap_on_exit, "PRE: none | POST: iocd") {
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: STUB - Unauthorized MMIO map attempt is rejected
// Input: None (stub test)
// Expect: Passes (stub)
// Depends: kernel/task, kernel/ipc, kernel/driver
JARVIS_TEST(iocd_unauthorized_mmio_rejected, "PRE: none | POST: iocd") {
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: STUB - Multiple device handlers registered with IOCD
// Input: None (stub test)
// Expect: Passes (stub)
// Depends: kernel/task, kernel/ipc, kernel/driver
JARVIS_TEST(iocd_multiple_device_handlers, "PRE: none | POST: iocd") {
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: STUB - IRQ affinity routing works correctly
// Input: None (stub test)
// Expect: Passes (stub)
// Depends: kernel/task, kernel/ipc, kernel/driver
JARVIS_TEST(iocd_irq_affinity, "PRE: none | POST: iocd") {
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: IOCD restarts after crash (TERMINATED → cleanup → respawn)
// Input: Kill iocd task (set TERMINATED), reap, then restart_stale_daemons
// Expect: New iocd PID is non-zero and different from original; new task exists
// Depends: kernel/task, kernel/ipc, kernel/iocd, kernel/daemon
// DISABLED: deliberately kills daemon manually
#if 0
JARVIS_TEST(iocd_crash_restarts) {
    uint64_t old_pid = iocd::get_iocd_pid();
    JARVIS_ASSERT(old_pid != 0);

    auto* old_task = Scheduler::find_task(old_pid);
    JARVIS_ASSERT(old_task != nullptr);

    old_task->state = TaskState::TERMINATED;
    Scheduler::reap_orphans();
    JARVIS_ASSERT_EQ(0ULL, iocd::get_iocd_pid());

    daemon::restart_stale_daemons();

    uint64_t new_pid = iocd::get_iocd_pid();
    JARVIS_ASSERT(new_pid != 0);
    JARVIS_ASSERT(new_pid != old_pid);

    auto* new_task = Scheduler::find_task(new_pid);
    JARVIS_ASSERT(new_task != nullptr);
    JARVIS_ASSERT(new_task->state == TaskState::READY ||
                  new_task->state == TaskState::RUNNING);

    JARVIS_TEST_PASS();
}
#endif

// Runmode: kernel
// Testidea: Registers all IOCD tests with the test framework
// Input: None
// Expect: All IOCD tests registered
// Depends: test framework
void register_iocd_tests() {
    Logger::info("Registering I/O daemon tests");
    JARVIS_REGISTER_TEST(iocd_boots_and_registers);
    JARVIS_REGISTER_TEST(iocd_keyboard_irq_to_event);
    JARVIS_REGISTER_TEST(iocd_serial_irq_to_event);
    JARVIS_REGISTER_TEST(iocd_mmio_unmap_on_exit);
    JARVIS_REGISTER_TEST(iocd_unauthorized_mmio_rejected);
    JARVIS_REGISTER_TEST(iocd_multiple_device_handlers);
    JARVIS_REGISTER_TEST(iocd_irq_affinity);
    // DISABLED: deliberately kills daemon manually, leaving daemon lifecycle
    // in a non-standard state that snapshot_restore cannot clean up
    // JARVIS_REGISTER_TEST(iocd_crash_restarts);
}
