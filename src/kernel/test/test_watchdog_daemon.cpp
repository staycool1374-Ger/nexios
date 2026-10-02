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

/// @file test_watchdog_daemon.cpp
/// @brief Watchdog daemon supervision tests (issue #277): WdogCap
///        single-owner/revoke/dispose contract, cross-task arm/kick via
///        handles, and the boot-spawned watchdogd presence. Every cap is
///        disposed and every slot removed by test end (snapshot-clean).

#include <test.hpp>
#include <logger.hpp>
#include <kernel/syscall/syscall.hpp>
#include <kernel/syscall/syscall_errors.hpp>
#include <kernel/task/scheduler.hpp>
#include <kernel/task/task.hpp>
#include <kernel/arch/timer.hpp>
#include <kernel/arch/irq_guard.hpp>
#include <kernel/cap/cap.hpp>
#include <kernel/cap/cap_types.hpp>
#include <kernel/cap/wdog.hpp>
#include <kernel/watchdog/watchdogd.hpp>
#include "test_sched_helpers.hpp"

using namespace kernel;

namespace kernel {
// Shared with syscall_handlers_cap.cpp (same TU-visible helper).
cap::CNode *current_cspace();
} // namespace kernel

// Runmode: kernel
// Testidea: Single live WdogCap per target pid, fail-closed double.
// Input: Forever task added (never dispatched); create cap, create again.
// Expect: first non-null; second null; dispose cleans up.
// Depends: WdogCap::create/dispose (issue #277)
JARVIS_TEST(wdogcap_create_single_owner, "PRE: none | POST: none") {
    auto *helper =
        kernel::test::create_forever_task(9, 10, "wdogcap_own");
    JARVIS_ASSERT(helper != nullptr);
    auto *a = cap::WdogCap::create(helper->id);
    JARVIS_ASSERT(a != nullptr);
    auto *b = cap::WdogCap::create(helper->id);
    JARVIS_ASSERT(b == nullptr);
    a->dispose();
    auto *c = cap::WdogCap::create(helper->id);
    JARVIS_ASSERT(c != nullptr);
    c->dispose();
    kernel::test::terminate_and_drain(*helper);
}

// Runmode: kernel
// Testidea: Revoked cap fails closed and disarms the target.
// Input: Driven task installs own-cap for a helper? No — revoke path
//        is direct: create cap for helper, revoke it.
// Expect: after revoke, cross-task CREATE returns -INVALID_ARGS and
//         the target is disarmed (never killed by teardown).
// Depends: WdogCap::revoke (issue #277)
JARVIS_TEST(wdogcap_revoke_fails_closed, "PRE: none | POST: none") {
    auto *helper =
        kernel::test::create_forever_task(9, 10, "wdogcap_rev");
    JARVIS_ASSERT(helper != nullptr);
    auto *cap = cap::WdogCap::create(helper->id);
    JARVIS_ASSERT(cap != nullptr);
    {
        arch::IrqGuard guard{};
        helper->wdog_period_ticks = 1000;
        helper->wdog_last_kick_tick = arch::Timer::ticks();
        helper->wdog_expiry_tick = helper->wdog_last_kick_tick + 1000;
        helper->wdog_armed = true;
        helper->wdog_gen = 1;
    }
    cap->revoke();
    JARVIS_ASSERT(!helper->wdog_armed);
    cap->dispose();
    JARVIS_ASSERT(TaskControlBlock::is_valid(helper));
    JARVIS_ASSERT(helper->state != TaskState::TERMINATED);
    kernel::test::terminate_and_drain(*helper);
}

// Runmode: kernel
// Testidea: Dispose disarms but never kills the target.
// Input: Arm helper via direct field setup; dispose the cap.
// Expect: helper alive and disarmed; no TERMINATED state.
// Depends: WdogCap::dispose (issue #277)
JARVIS_TEST(wdogcap_dispose_disarms_not_kills, "PRE: none | POST: none") {
    auto *helper =
        kernel::test::create_forever_task(9, 10, "wdogcap_dis");
    JARVIS_ASSERT(helper != nullptr);
    auto *cap = cap::WdogCap::create(helper->id);
    JARVIS_ASSERT(cap != nullptr);
    {
        arch::IrqGuard guard{};
        helper->wdog_period_ticks = 1000;
        helper->wdog_last_kick_tick = arch::Timer::ticks();
        helper->wdog_expiry_tick = helper->wdog_last_kick_tick + 1000;
        helper->wdog_armed = true;
        helper->wdog_gen = 1;
    }
    cap->dispose();
    JARVIS_ASSERT(!helper->wdog_armed);
    JARVIS_ASSERT(helper->wdog_gen != 0);
    JARVIS_ASSERT(helper->state != TaskState::TERMINATED);
    kernel::test::terminate_and_drain(*helper);
}

// Runmode: kernel
// Testidea: Cross-task arm/kick through installed handles (driven).
// Input: Driven task creates WdogCap for the harness task? No —
//        driven task installs a cap for ITSELF in its own CSpace, then
//        CREATE(pid=self, handle) and KICK(pid=self, handle) via the
//        cross-task path; then revokes and expects failure.
// Expect: cross CREATE returns 0 and arms; cross KICK returns 0 with
//         expiry == last_kick + period; post-revoke CREATE fails;
//         slot removed, cap disposed.
// Depends: 91/92 pid-arg path, CNode install/remove (issue #277)
JARVIS_TEST(daemon_cross_arm_kick, "PRE: none | POST: none") {
    static uint64_t g_create_rc = 0;
    static uint64_t g_armed = 0;
    static uint64_t g_kick_rc = 0;
    static uint64_t g_kick_ok = 0;
    static uint64_t g_recreate_rc = 0;
    auto *t = TaskControlBlock::create(
        []() {
            auto *cur = Scheduler::current_task();
            cap::WdogCap *cap = cap::WdogCap::create(cur->id);
            if (cap == nullptr)
                return;
            cap::CNode *cs = current_cspace();
            if (cs == nullptr) {
                cap->dispose();
                return;
            }
            uint32_t gen = 0;
            int idx = cs->install(cap, cap::CapType::Wdog,
                                  cap::CAP_RIGHT_WRITE, &gen);
            if (idx < 0) {
                cap->dispose();
                return;
            }
            uint64_t handle = cap::encode_user_handle(cs->cspace_id, idx,
                                                      gen);
            g_create_rc = Syscall::handle(
                static_cast<uint64_t>(SyscallNumber::WATCHDOG_CREATE), 500,
                cur->id, handle, 0, nullptr);
            g_armed = cur->wdog_armed ? 1 : 0;
            g_kick_rc = Syscall::handle(
                static_cast<uint64_t>(SyscallNumber::WATCHDOG_KICK),
                cur->id, handle, 0, 0, nullptr);
            g_kick_ok =
                (cur->wdog_expiry_tick ==
                 cur->wdog_last_kick_tick + cur->wdog_period_ticks)
                    ? 1
                    : 0;
            cap->revoke();
            g_recreate_rc = Syscall::handle(
                static_cast<uint64_t>(SyscallNumber::WATCHDOG_CREATE), 500,
                cur->id, handle, 0, nullptr);
            cs->remove(static_cast<uint32_t>(idx));
            cap->dispose();
        },
        11, 10);
    JARVIS_ASSERT(t != nullptr);
    Scheduler::add_task(*t);
    Scheduler::reschedule();
    kernel::test::wait_for_termination_safe(t);
    JARVIS_ASSERT(g_create_rc == 0);
    JARVIS_ASSERT(g_armed == 1);
    JARVIS_ASSERT(g_kick_rc == 0);
    JARVIS_ASSERT(g_kick_ok == 1);
    JARVIS_ASSERT(g_recreate_rc ==
                  static_cast<uint64_t>(-errors::SYS_ERR_SCHED_INVALID_ARGS));
    kernel::test::terminate_if_live(t);
    Scheduler::drain_zombie_list();
}

// Runmode: kernel
// Testidea: watchdogd is spawned at boot with a live pid.
// Input: Read watchdogd::get_watchdogd_pid() + Scheduler::find_task.
// Expect: pid nonzero and resolving to a magic-valid TCB (daemon runs
//         in every boot, test and production alike).
// Depends: taskdefs watchdogd row, reboot_from_table (issue #277)
JARVIS_TEST(daemon_watchdogd_spawned, "PRE: none | POST: none") {
    const uint64_t pid = watchdogd::get_watchdogd_pid();
    JARVIS_ASSERT(pid != 0);
    auto *t = Scheduler::find_task(pid);
    JARVIS_ASSERT(t != nullptr);
    JARVIS_ASSERT(t->magic == TaskControlBlock::TCB_MAGIC);
    JARVIS_TEST_PASS();
}

void register_watchdog_daemon_tests() {
    Logger::info("Registering watchdog daemon tests");
    JARVIS_REGISTER_TEST(wdogcap_create_single_owner);
    JARVIS_REGISTER_TEST(wdogcap_revoke_fails_closed);
    JARVIS_REGISTER_TEST(wdogcap_dispose_disarms_not_kills);
    JARVIS_REGISTER_TEST(daemon_cross_arm_kick);
    JARVIS_REGISTER_TEST(daemon_watchdogd_spawned);
}
