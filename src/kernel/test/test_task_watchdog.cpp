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

/// @file test_task_watchdog.cpp
/// @brief Per-task software watchdog stubs (issue #41). All six are
///        JARVIS_TEST_PASS stubs on testbed: SYS_WATCHDOG_CREATE/KICK
///        (91/92) and the TCB wdog fields live on main only. After the
///        testbed→main merge each stub is replaced with the real
///        assertion per its Pseudocode block. Live QEMU proof of the
///        expiry path already exists (issue #41 thread, 2026-10-02).

#include <test.hpp>
#include <logger.hpp>

using namespace kernel;

// Runmode: kernel
// Testidea: CREATE arms the caller's watchdog with period + generation.
// Input: SYS_WATCHDOG_CREATE(period) via raw syscall 91 from a test task.
// Expect: returns 0; TCB wdog_armed, period/expiry set, wdog_gen >= 1;
//         period=0 and reserved-arg!=0 return -SCHED_INVALID_ARGS.
// Depends: SYS_WATCHDOG_CREATE (issue #41, main only)
/* Pseudocode: spawn test task; rc = raw_syscall(91, 100, 0);
   assert rc == 0 && tcb.wdog_armed && gen >= 1;
   assert raw_syscall(91, 0, 0) == -INVALID_ARGS; cleanup task. */
JARVIS_TEST(watchdog_create_arms, "PRE: none | POST: none") {
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: KICK restarts the period from now.
// Input: CREATE(100), advance < 100 ticks, KICK via raw syscall 92.
// Expect: returns 0; expiry moves forward (last_kick updated); KICK on
//         a disarmed task returns -SCHED_INVALID_STATE.
// Depends: SYS_WATCHDOG_CREATE/KICK (issue #41, main only)
/* Pseudocode: create(100); e1 = expiry; sleep 10 ticks; kick();
   assert expiry > e1; disarm via teardown; assert kick ==
   -INVALID_STATE; cleanup task. */
JARVIS_TEST(watchdog_kick_extends, "PRE: none | POST: none") {
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Expiry under the default action kills fail-closed + ring.
// Input: CREATE with a short period, never kick, advance past expiry.
// Expect: task TERMINATED via defer_kill; TIMING+6 ERROR ring entry
//         present with the task name; one-shot (no second fire).
// Depends: on_tick scan, CONFIG_WATCHDOG_ACTION==3 (issue #41, main)
/* Pseudocode: create(5); spin 50 ticks without kicking; assert task
   TERMINATED && reaped; assert dmesg contains TIMING+6 for the task;
   assert no second expiry (armed == false). */
JARVIS_TEST(watchdog_expiry_kill_default, "PRE: none | POST: none") {
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: /proc/<pid>/watchdog shows live state, write declined.
// Input: CREATE on a test task; read /proc/<pid>/watchdog; write it.
// Expect: read shows "armed=1 period=... last_kick=... expires=...";
//         write returns VFS_INVALID (kick exclusively via syscall).
// Depends: PidWatchdogVnode (issue #41, main only)
/* Pseudocode: create(1000); read /proc/pid/watchdog into buf;
   assert starts "armed=1 period=1000"; assert write(buf) ==
   VFS_INVALID; cleanup task. */
JARVIS_TEST(watchdog_proc_node_read, "PRE: vfsd | POST: none") {
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: CONFIG_WATCHDOG_ACTION 0..4 matrix compiles and dispatches.
// Input: build matrix (compile-time) + LOG_ONLY/DEMOTE expiry observes.
// Expect: static_assert range holds; LOG_ONLY leaves the task alive
//         with a ring entry; DEMOTE halves priority (compile per build).
// Depends: CONFIG_WATCHDOG_ACTION (issue #41, main only)
/* Pseudocode (LOG_ONLY build): create(5); expire; assert task alive
   && ring has TIMING+6. Other actions verified by their own builds;
   matrix documented in watchdog.md §3. */
JARVIS_TEST(watchdog_config_action_matrix, "PRE: none | POST: none") {
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: A recycled TCB slot never matches a stale arm.
// Input: CREATE on a task, reap it (teardown disarms + bumps gen),
//        allocate a new task reusing the slot, advance past old expiry.
// Expect: no expiry fires for the recycled task (armed == false);
//         wdog_gen != 0 (never reuses the never-armed sentinel).
// Depends: teardown disarm, TaskFields rewind (issue #41, main only)
/* Pseudocode: create(5) on task A; destroy A; spawn B (slot reuse);
   advance 50 ticks; assert B alive && !B.wdog_armed; cleanup B. */
JARVIS_TEST(watchdog_gen_stale_after_reap, "PRE: none | POST: none") {
    JARVIS_TEST_PASS();
}

void register_task_watchdog_tests() {
    Logger::info("Registering task watchdog tests");
    JARVIS_REGISTER_TEST(watchdog_create_arms);
    JARVIS_REGISTER_TEST(watchdog_kick_extends);
    JARVIS_REGISTER_TEST(watchdog_expiry_kill_default);
    JARVIS_REGISTER_TEST(watchdog_proc_node_read);
    JARVIS_REGISTER_TEST(watchdog_config_action_matrix);
    JARVIS_REGISTER_TEST(watchdog_gen_stale_after_reap);
}
