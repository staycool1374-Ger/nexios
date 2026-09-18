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

/// @file test_daemon_restart_crash.cpp
/// @brief Demonstrates the daemon restart crash after reload_daemon_tasks.
///
/// When restart_stale_daemons() is called after reload_daemon_tasks(),
/// the newly created daemon tasks crash on their first context switch
/// (no startup message printed).  This is a latent bug in the test
/// isolation teardown sequence: reload_daemon_tasks() leaves the kernel
/// in a state (corrupted page tables or MemPool) that makes userspace
/// ELF execution unsafe.
///
/// The crash is observable in the serial output:
///   [DAEMON] 'vfsd' restarted (PID=4, restart #1)   ← created
///   Scheduler: task '' (ID=4) terminated              ← crashes
///   [DAEMON] 'vfsd' died (PID=4), restart_count=1     ← notify_death in
///   cleanup
///
/// The dmesg_push_base(0xDA01) in notify_death persists the event
/// across reboot_from_table() because the dmesg buffer is never cleared.

#include <test.hpp>
#include <logger.hpp>
#include <kernel/test/test_isolate.hpp>
#include <kernel/daemon/daemon_mgr.hpp>
#include <kernel/task/scheduler.hpp>
#include <kernel/task/task.hpp>
#include <kernel/vfs/vfsd.hpp>
#include <kernel/driver/iocd.hpp>

using namespace kernel;

// Runmode: kernel
// Testidea: Restart daemons after reload to expose the post-cleanup crash
// Input: reload_daemon_tasks then restart_stale_daemons
// Expect: New daemon tasks die silently on first context switch
// Depends: kernel::daemon, kernel::Scheduler, kernel::test::reload_daemon_tasks
//
// GATED OUT (ROADMAP v0.3.13 T0-7): this test asserted only that the
// restarted daemons are PRESENT in the scheduler while its own docstring
// documents they crash on first dispatch — codifying a latent kernel
// daemon-restart bug as PASS.  Fixing the bug (teardown-state corruption in
// reload_daemon_tasks / daemon_mgr) is a kernel change tracked separately.
// Re-enable only together with that fix AND an execution-evidence assertion
// (e.g. the restarted vfsd/iocd reach their startup message).
#if 0
JARVIS_TEST(daemon_restart_after_cleanup_crash, "PRE: vfsd,iocd") {
    // Kill all tasks including daemons
    test::reload_daemon_tasks();

    // Recreate daemon tasks from initrd — these crash at first run
    daemon::restart_stale_daemons();

    // After restart_stale_daemons, the daemon tasks exist in the
    // scheduler but will crash when the timer ISR picks them.
    // Check they are at least present in the daemon entries.
    for (uint64_t i = 0; i < daemon::MAX_DAEMONS; ++i) {
        const auto &entry = daemon::get_entry(i);
        if (entry.pid == 0)
            continue;
        auto *t = Scheduler::find_task(entry.pid);
        JARVIS_ASSERT(t != nullptr);
    }

    JARVIS_TEST_PASS();
}
#endif

// Runmode: kernel
// Testidea: The daemon lifecycle entry points fail closed on unknown names
//           — a typo'd supervisor command must neither kill nor resurrect
//           anything and must leave both daemon PIDs and their tasks intact.
//           (Covers ensure_running/terminate/reset_restart_count, issue #135.
//           A full kill-and-resurrect cycle is NOT attempted here: a
//           resurrected daemon that dispatches hits the known teardown-state
//           crash documented above, and killing a real daemon desynchronises
//           the snapshot-restored entry PIDs from the live task set.)
// Input: ensure_running/terminate/reset_restart_count on "no-such-daemon"
//        and on "".
// Expect: Both daemon PIDs unchanged, both tasks still live.
// Depends: kernel::daemon lifecycle, vfsd/iocd pid getters
JARVIS_TEST(daemon_unknown_name_rejected, "PRE: vfsd, iocd | POST: none") {
    const uint64_t vfsd_before = vfsd::get_vfsd_pid();
    const uint64_t iocd_before = iocd::get_iocd_pid();
    JARVIS_ASSERT(vfsd_before != 0);
    JARVIS_ASSERT(iocd_before != 0);

    daemon::ensure_running("no-such-daemon");
    daemon::terminate("no-such-daemon");
    daemon::reset_restart_count("no-such-daemon");
    daemon::ensure_running("");
    daemon::terminate("");
    daemon::reset_restart_count("");

    JARVIS_ASSERT_EQ(vfsd_before, vfsd::get_vfsd_pid());
    JARVIS_ASSERT_EQ(iocd_before, iocd::get_iocd_pid());
    JARVIS_ASSERT(Scheduler::find_task(vfsd_before) != nullptr);
    JARVIS_ASSERT(Scheduler::find_task(iocd_before) != nullptr);
    JARVIS_TEST_PASS();
}

void register_daemon_restart_crash_tests() {
    Logger::info("Registering daemon restart crash tests");
#if 0
    JARVIS_REGISTER_TEST(daemon_restart_after_cleanup_crash);
#endif
    JARVIS_REGISTER_TEST(daemon_unknown_name_rejected);
}
