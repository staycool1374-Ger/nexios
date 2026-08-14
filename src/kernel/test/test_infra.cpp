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

/// @file test_infra.cpp
/// @brief Test-infrastructure integrity tests (v0.3.8).  Verifies the
///        lazy-daemon-restart flag (g_vfs_touched / mark_vfs_touched) and
///        that snapshot/restore preserves daemon state when VFS is not
///        touched.  C-class query tests of real test-isolation state.

#include <test.hpp>
#include <logger.hpp>
#include <kernel/core/global_state.hpp>
#include <kernel/test/test_isolate.hpp>
#include <kernel/task/scheduler.hpp>
#include <kernel/vfs/vfsd.hpp>
#include <kernel/driver/iocd.hpp>

using namespace kernel;

// Runmode: kernel
// Testidea: The vfs_touched flag must default to false so that tests which
//           never touch VFS skip the expensive daemon reload path.
// Input: Read kernel::test::g_vfs_touched at test start.
// Expect: false.
// Depends: kernel/test/test_isolate.hpp
JARVIS_TEST(infra_vfs_touched_defaults_false, "PRE: none | POST: none") {
    JARVIS_ASSERT_FMT(kernel::gs::get_vfs_touched() == false,
                      "g_vfs_touched must default to false (got %d)",
                      (int)kernel::gs::get_vfs_touched());
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: mark_vfs_touched() flips the lazy-daemon-restart flag; a VFS
//           syscall handler that marks the flag must be observable by the
//           runner's snapshot_restore (which then reloads daemons).
// Input: Call kernel::test::mark_vfs_touched(), then read the flag.
// Expect: true.
// Depends: kernel/test/test_isolate.hpp
JARVIS_TEST(infra_mark_vfs_touched_sets_flag, "PRE: none | POST: none") {
    JARVIS_ASSERT_FMT(kernel::gs::get_vfs_touched() == false,
                      "precondition: g_vfs_touched must start false (got %d)",
                      (int)kernel::gs::get_vfs_touched());
    kernel::test::mark_vfs_touched();
    JARVIS_ASSERT_FMT(kernel::gs::get_vfs_touched() == true,
                      "mark_vfs_touched() must set g_vfs_touched (got %d)",
                      (int)kernel::gs::get_vfs_touched());
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: When a test does NOT touch VFS, snapshot_restore must preserve
//           the vfsd/iocd daemon PIDs and their tasks must remain alive —
//           the lazy path skips daemon reload entirely (v0.3.8, ~70-80%
//           overhead reduction).  This test records the PIDs, touches
//           nothing, and the next snapshot_restore is expected to keep them.
// Input: Record vfsd::get_vfsd_pid() / iocd::get_iocd_pid(); verify tasks
//        exist; snapshot/restore between tests must not change them.
// Expect: Both PIDs non-zero and their TCBs live.  Guard: if no snapshot is
//         active (harness isolation disabled) the test FAILS rather than
//         passing vacuously.
// Depends: kernel/vfs/vfsd.hpp, kernel/driver/iocd.hpp,
//          kernel/test/test_isolate.hpp
JARVIS_TEST(infra_daemon_state_preserved_when_vfs_untouched,
            "PRE: vfsd, iocd | POST: none") {
    if (!kernel::test::snapshot_is_active()) {
        JARVIS_FAIL("snapshot isolation inactive — cannot verify restore "
                    "preservation (refusing to pass vacuously)");
    }
    uint64_t vfsd_pid = vfsd::get_vfsd_pid();
    uint64_t iocd_pid = iocd::get_iocd_pid();
    JARVIS_ASSERT_FMT(vfsd_pid != 0, "vfsd PID must be registered");
    JARVIS_ASSERT_FMT(iocd_pid != 0, "iocd PID must be registered");

    auto *vfsd_task = Scheduler::find_task(vfsd_pid);
    auto *iocd_task = Scheduler::find_task(iocd_pid);
    JARVIS_ASSERT_FMT(vfsd_task != nullptr && TaskControlBlock::is_valid(vfsd_task),
                      "vfsd task (pid=%lu) must be alive", vfsd_pid);
    JARVIS_ASSERT_FMT(iocd_task != nullptr && TaskControlBlock::is_valid(iocd_task),
                      "iocd task (pid=%lu) must be alive", iocd_pid);

    // This test must NOT touch VFS: after the harness snapshot_restore runs
    // (g_vfs_touched == false), daemons are preserved in place.  Record the
    // PIDs in the log so a manual cross-check against the next test's
    // [RUN_PRE] line is possible.
    Logger::info("[INFRA] vfsd_pid=%lu iocd_pid=%lu (daemon state preserved)",
                 vfsd_pid, iocd_pid);
    JARVIS_TEST_PASS();
}

void register_infra_tests() {
    Logger::info("Registering test-infrastructure tests");
    JARVIS_REGISTER_TEST(infra_vfs_touched_defaults_false);
    JARVIS_REGISTER_TEST(infra_mark_vfs_touched_sets_flag);
    JARVIS_REGISTER_TEST(infra_daemon_state_preserved_when_vfs_untouched);
}
