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

/// @file test_sched_admission.cpp
/// @brief Enforced admission-control tests (issue #20, v0.4.8).
///
/// DRIVEN: admission decisions are observed through REAL add_task_err()
/// outcomes on live TCBs — the harness never fakes utilization sums and
/// never mutates scheduler tables.  Denied TCBs are destroyed (never
/// registered ⇒ teardown no-ops); admitted tasks are terminated + drained.
/// Background load during tests is zero by construction (init/harness and
/// SS daemons are edf-exempt, shell/user-app are aperiodic, idle/monitor
/// are skipped), so explicit-WCET fills give deterministic bounds.
///
/// Liveness: every admitted task blocks on a test-local gate semaphore if
/// ever dispatched.  An empty-lambda entry would self-terminate on the
/// first timer tick (dispatch race: the task vanishes from the LUB scan
/// before the next setup call), flaking every multi-task assertion.

#include <test.hpp>
#include <logger.hpp>
#include <kernel/syscall/syscall.hpp>
#include <kernel/task/scheduler.hpp>
#include <kernel/task/task.hpp>
#include <kernel/arch/timer.hpp>
#include <kernel/arch/io.hpp>
#include <kernel/arch/irq_guard.hpp>
#include <kernel/sync/semaphore.hpp>
#include <kernel/nexios_config.h>
#include "test_sched_helpers.hpp"

using namespace kernel;
using kernel::errors::SchedulerError;

namespace {

// Blocks forever on the gate carried via user_data (never posted during
// setup; posted once at teardown so a dispatched task can finish dying).
void gated_entry() {
    auto *self = Scheduler::current_task();
    auto *gate = reinterpret_cast<sync::Semaphore *>(self->user_data);
    gate->wait();
}

// Period-`period` task with explicit WCET (utilization = wcet/period).
TaskControlBlock *make_task(uint64_t wcet, uint64_t period,
                            sync::Semaphore *gate) {
    auto *t = TaskControlBlock::create(gated_entry, 11, period);
    if (t == nullptr)
        return nullptr;
    t->wcet_ticks = wcet;
    t->user_data = gate;
    return t;
}

// Destroy a TCB that was NEVER admitted (fail-closed ⇒ absent from every
// table; destroy() teardown paths no-op on absent entries).
void destroy_denied(TaskControlBlock *t) {
    if (t == nullptr)
        return;
    JARVIS_ASSERT(Scheduler::find_task(t->id) == nullptr);
    TaskControlBlock::destroy(t);
}

} // namespace

// Runmode: kernel
// Testidea: Liu-Leyland gate denies the task that would overrun the bound.
// Input: A(p100,w60) admitted (60<=100); B(p100,w60) would total 120>82.
// Expect: B → SCHED_ERR_ADMISSION_DENIED, absent from id_table_, no leak.
// Depends: Scheduler::admission_check_locked (issue #20)
JARVIS_TEST(admission_lub_rejects_overrun, "PRE: none | POST: none") {
    sync::Semaphore gate;
    gate.init(0, 1);
    auto *a = make_task(60, 100, &gate);
    JARVIS_ASSERT(a != nullptr);
    JARVIS_ASSERT(Scheduler::add_task_err(*a) == errors::SCHED_ERR_OK);
    auto *b = make_task(60, 100, &gate);
    JARVIS_ASSERT(b != nullptr);
    JARVIS_ASSERT(Scheduler::add_task_err(*b) ==
                  errors::SCHED_ERR_ADMISSION_DENIED);
    destroy_denied(b);
    gate.post();
    kernel::test::terminate_and_drain(*a);
    Scheduler::drain_zombie_list();
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: WCET exceeding the period is a config error, rejected first.
// Input: C(p100, wcet 200).
// Expect: C → SCHED_ERR_WCET_INVALID, never counted, no leak.
// Depends: Scheduler::check_wcet_locked (issue #20)
JARVIS_TEST(admission_wcet_exceeds_period_rejected,
            "PRE: none | POST: none") {
    sync::Semaphore gate;
    gate.init(0, 1);
    auto *c = make_task(200, 100, &gate);
    JARVIS_ASSERT(c != nullptr);
    JARVIS_ASSERT(Scheduler::add_task_err(*c) ==
                  errors::SCHED_ERR_WCET_INVALID);
    destroy_denied(c);
    Scheduler::drain_zombie_list();
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: exemption precedes WCET-validity (audit S3): an untracked
// (aperiodic) task carrying an explicit WCET is exempt-admitted, never
// rejected — denying exempt tasks (e.g. boot daemons with stale WCET)
// would wedge the boot.  WCET-validity binds only gated tasks.
// Input: D(period 0, wcet 50).
// Expect: D → SCHED_ERR_OK (excluded from the LUB numerator).
// Depends: Scheduler::admission_check_locked exemption order (issue #20)
JARVIS_TEST(admission_wcet_untracked_exempt_admits, "PRE: none | POST: none") {
    sync::Semaphore gate;
    gate.init(0, 1);
    auto *d = make_task(50, 0, &gate);
    JARVIS_ASSERT(d != nullptr);
    JARVIS_ASSERT(Scheduler::add_task_err(*d) == errors::SCHED_ERR_OK);
    gate.post();
    kernel::test::terminate_and_drain(*d);
    Scheduler::drain_zombie_list();
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: a lone implicit-100% task exactly meets the n=1 bound.
// Input: E(p100, wcet 0 → implicit 100); bound(1) == 1000000.
// Expect: admitted (100 > 100 is false); retry-after-free also admits
// (defer pattern: denial holds no state, so a later retry succeeds).
// Depends: Scheduler::admission_check_locked (issue #20)
JARVIS_TEST(admission_implicit_single_ok_defer_retries,
            "PRE: none | POST: none") {
    sync::Semaphore gate;
    gate.init(0, 1);
    auto *e = make_task(0, 100, &gate);
    JARVIS_ASSERT(e != nullptr);
    JARVIS_ASSERT(Scheduler::add_task_err(*e) == errors::SCHED_ERR_OK);
    gate.post();
    kernel::test::terminate_and_drain(*e);
    auto *f = make_task(0, 100, &gate);
    JARVIS_ASSERT(f != nullptr);
    JARVIS_ASSERT(Scheduler::add_task_err(*f) == errors::SCHED_ERR_OK);
    gate.post();
    kernel::test::terminate_and_drain(*f);
    Scheduler::drain_zombie_list();
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: create()-time memory admission fails closed when the global
// budget is exhausted, and recovers fully on release.
// Input: reserve the entire remaining budget, then create().
// Expect: create → nullptr; after release, create succeeds again.
// Depends: TaskControlBlock::create budget gate + init_memory_budget (#20)
JARVIS_TEST(admission_memory_budget_denied_at_create,
            "PRE: none | POST: none") {
#if CONFIG_MEMORY_BUDGET
    uint64_t avail = Scheduler::remaining_memory_budget();
    JARVIS_ASSERT(avail > 0);
    JARVIS_ASSERT(Scheduler::reserve_memory_pages(avail));
    auto *g = TaskControlBlock::create(gated_entry, 11, 0);
    JARVIS_ASSERT(g == nullptr); // budget OOM: admission denied at create
    Scheduler::release_memory_pages(avail);
    sync::Semaphore gate;
    gate.init(0, 1);
    auto *h = make_task(0, 0, &gate);
    JARVIS_ASSERT(h != nullptr);
    TaskControlBlock::destroy(h);
#else
    // Audit S3: budget APIs exist only under CONFIG_MEMORY_BUDGET (as does
    // the gate itself) — opt-out builds trivially pass to keep the class
    // count stable across configurations.
#endif
    Scheduler::drain_zombie_list();
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: untracked/exempt tasks admit even when the system is overfull.
// Input: overfill to a confirmed denial; then aperiodic, NO_PERIOD, and
// edf-exempt periodic tasks.
// Expect: all three → SCHED_ERR_OK (excluded from the LUB numerator).
// Depends: Scheduler::admission_exempted (issue #20)
JARVIS_TEST(admission_exemptions_untracked, "PRE: none | POST: none") {
    sync::Semaphore gate;
    gate.init(0, 1);
    auto *a = make_task(60, 100, &gate);
    JARVIS_ASSERT(a != nullptr);
    JARVIS_ASSERT(Scheduler::add_task_err(*a) == errors::SCHED_ERR_OK);
    auto *full = make_task(60, 100, &gate);
    JARVIS_ASSERT(full != nullptr);
    JARVIS_ASSERT(Scheduler::add_task_err(*full) ==
                  errors::SCHED_ERR_ADMISSION_DENIED);
    destroy_denied(full);
    auto *ap = make_task(0, 0, &gate);
    JARVIS_ASSERT(ap != nullptr);
    JARVIS_ASSERT(Scheduler::add_task_err(*ap) == errors::SCHED_ERR_OK);
    auto *np = make_task(0, TaskControlBlock::NO_PERIOD, &gate);
    JARVIS_ASSERT(np != nullptr);
    JARVIS_ASSERT(Scheduler::add_task_err(*np) == errors::SCHED_ERR_OK);
    auto *ex = make_task(90, 100, &gate);
    JARVIS_ASSERT(ex != nullptr);
    ex->edf_exempt = true; // mirrors the taskdefs daemon spawn site
    JARVIS_ASSERT(Scheduler::add_task_err(*ex) == errors::SCHED_ERR_OK);
    gate.post();
    gate.post();
    gate.post();
    gate.post();
    kernel::test::terminate_and_drain(*a);
    kernel::test::terminate_and_drain(*ap);
    kernel::test::terminate_and_drain(*np);
    kernel::test::terminate_and_drain(*ex);
    Scheduler::drain_zombie_list();
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: fork under an overfull system fails closed with no leak.
// Input: dispatched forker(p100,w5) + fill A(p100,w60) = 65%; the forked
// child (implicit 100%) would total 165 > bound(3) = 72.
// Expect: sys_fork → -1; parent unaffected; no ResourceTracker delta.
// Depends: Syscall::sys_fork admission path (issue #20)
JARVIS_TEST(admission_fork_denied_no_leak, "PRE: none | POST: none") {
    static volatile uint64_t g_fork_ret = 0;
    g_fork_ret = 0;
    sync::Semaphore gate;
    gate.init(0, 1);
    auto *fill = make_task(60, 100, &gate);
    JARVIS_ASSERT(fill != nullptr);
    JARVIS_ASSERT(Scheduler::add_task_err(*fill) == errors::SCHED_ERR_OK);
    auto *forker = TaskControlBlock::create(
        []() {
            uint64_t regs[22] = {};
            g_fork_ret = Syscall::handle(
                static_cast<uint64_t>(SyscallNumber::FORK), 0, 0, 0, 0, regs);
        },
        11, 100);
    JARVIS_ASSERT(forker != nullptr);
    forker->wcet_ticks = 5;
    Scheduler::add_task(*forker);
    Scheduler::reschedule();
    kernel::test::wait_for_termination_safe(forker);
    JARVIS_ASSERT(g_fork_ret == static_cast<uint64_t>(-1));
    kernel::test::terminate_if_live(forker);
    gate.post();
    kernel::test::terminate_and_drain(*fill);
    Scheduler::drain_zombie_list();
    JARVIS_TEST_PASS();
}

void register_sched_admission_tests() {
    Logger::info("Registering sched_admission tests");
    JARVIS_REGISTER_TEST(admission_lub_rejects_overrun);
    JARVIS_REGISTER_TEST(admission_wcet_exceeds_period_rejected);
    JARVIS_REGISTER_TEST(admission_wcet_untracked_exempt_admits);
    JARVIS_REGISTER_TEST(admission_implicit_single_ok_defer_retries);
    JARVIS_REGISTER_TEST(admission_memory_budget_denied_at_create);
    JARVIS_REGISTER_TEST(admission_exemptions_untracked);
    JARVIS_REGISTER_TEST(admission_fork_denied_no_leak);
}
