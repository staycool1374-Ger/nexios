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

/// @file test_sched_admission_verify.cpp
/// @brief End-to-end admission verification journeys (issue #24, v0.4.8).
///
/// Covers ONLY what sched_admission / deadline_* / smp_sched do not:
/// taskdefs-table budgets, deny→reap→retry lifecycle, cross-mode budget
/// parity, the BACKGROUND table guard, per-CPU epsilon admission, exact
/// budget round-trips, and runtime parity with the boot self-test.
/// DRIVEN through REAL add_task_err() outcomes and helper outputs on
/// gated-entry TCBs (issue #20 dispatch-race lesson applies throughout).

#include <test.hpp>
#include <logger.hpp>
#include <string.hpp>
#include <kernel/task/scheduler.hpp>
#include <kernel/task/task.hpp>
#include <kernel/task/taskdefs.hpp>
#include <kernel/arch/irq_guard.hpp>
#include <kernel/sync/semaphore.hpp>
#include <kernel/nexios_config.h>
#include "test_sched_helpers.hpp"

using namespace kernel;
using task::ServerMode;

namespace {

// Blocks on the gate carried via user_data if ever dispatched.
void verify_gated_entry() {
    auto *self = Scheduler::current_task();
    auto *gate = reinterpret_cast<sync::Semaphore *>(self->user_data);
    gate->wait();
}

TaskControlBlock *verify_make_task(uint64_t wcet, uint64_t period,
                                   sync::Semaphore *gate) {
    auto *t = TaskControlBlock::create(verify_gated_entry, 11, period);
    if (t == nullptr)
        return nullptr;
    t->wcet_ticks = wcet;
    t->user_data = gate;
    return t;
}

void verify_destroy_denied(TaskControlBlock *t) {
    if (t == nullptr)
        return;
    JARVIS_ASSERT(Scheduler::find_task(t->id) == nullptr);
    TaskControlBlock::destroy(t);
}

// Synthetic probe candidate (mirrors admission_selftest.cpp): stack-only,
// never added to any table.
void verify_make_probe(TaskControlBlock &cand, uint64_t wcet,
                       uint64_t period, uint64_t deadline, bool exempt) {
    // Same memset discipline as TaskControlBlock::create (task.cpp);
    // the probe is never added to any table and makes no virtual calls.
    memset(&cand, 0, sizeof(cand));
    cand.magic = TaskControlBlock::TCB_MAGIC;
    cand.id = ~0ULL;
    cand.state = TaskState::READY;
    cand.priority = 11;
    cand.base_priority = 11;
    cand.cpu_affinity = 1;
    cand.period_ticks = period;
    cand.deadline_ticks = deadline;
    cand.wcet_ticks = wcet;
    cand.remaining_ticks = period;
    cand.sched_policy = SchedPolicy::AUTO;
    cand.edf_exempt = exempt;
}

bool taskdefs_name_eq(const char *a, const char *b) {
    if (a == nullptr || b == nullptr)
        return a == b;
    for (; *a != 0 && *b != 0; ++a, ++b) {
        if (*a != *b)
            return false;
    }
    return *a == *b;
}

} // namespace

// Runmode: kernel
// Testidea: every enabled SPORADIC_SERVER table row satisfies the server
// params rule (budget<=period, daemon wiring), with vfsd/iocd spot values.
// Input: read-only taskdefs accessor walk.
// Expect: all enabled SS rows valid; vfsd (2,10) + iocd (3,10) present;
// whole-table taskdefs_valid() true.
// Depends: task::taskdefs_at/taskdefs_server_params_valid (issue #24)
JARVIS_TEST(admission_verify_taskdefs_server_budgets,
            "PRE: none | POST: none") {
    bool saw_vfsd = false;
    bool saw_iocd = false;
    for (size_t i = 0; i < task::taskdefs_count(); ++i) {
        const task::TaskDef *d = task::taskdefs_at(i);
        JARVIS_ASSERT(d != nullptr);
        if (d->type != task::TaskType::SPORADIC_SERVER || !d->enabled)
            continue;
        JARVIS_ASSERT(task::taskdefs_server_params_valid(*d));
        JARVIS_ASSERT(d->ss_budget <= d->ss_period);
        if (taskdefs_name_eq(d->daemon_name, "vfsd")) {
            saw_vfsd = true;
            JARVIS_ASSERT_EQ(2ULL, d->ss_budget);
            JARVIS_ASSERT_EQ(10ULL, d->ss_period);
        }
        if (taskdefs_name_eq(d->daemon_name, "iocd")) {
            saw_iocd = true;
            JARVIS_ASSERT_EQ(3ULL, d->ss_budget);
            JARVIS_ASSERT_EQ(10ULL, d->ss_period);
        }
    }
    JARVIS_ASSERT(saw_vfsd);
    JARVIS_ASSERT(saw_iocd);
    JARVIS_ASSERT(task::taskdefs_valid());
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: denial holds no state — after the holder is reaped, the same
// shape admits (defer-then-retry lifecycle).
// Input: fill CPU0 to confirmed DENIED; terminate+drain holder; retry.
// Expect: retry returns SCHED_ERR_OK; teardown restores baseline.
// Depends: add_task_err fail-closed contract (issues #20/#23)
JARVIS_TEST(admission_verify_defer_retry_after_reap,
            "PRE: none | POST: none") {
    sync::Semaphore gate;
    gate.init(0, 1);
    auto *holder = verify_make_task(60, 100, &gate);
    JARVIS_ASSERT(holder != nullptr);
    auto *extra = verify_make_task(60, 100, &gate);
    JARVIS_ASSERT(extra != nullptr);
    {
        arch::IrqGuard irq;
        JARVIS_ASSERT(Scheduler::add_task_err(*holder) ==
                      errors::SCHED_ERR_OK);
        JARVIS_ASSERT(Scheduler::add_task_err(*extra) ==
                      errors::SCHED_ERR_ADMISSION_DENIED);
    }
    verify_destroy_denied(extra);
    gate.post();
    kernel::test::terminate_and_drain(*holder);
    auto *retry = verify_make_task(60, 100, &gate);
    JARVIS_ASSERT(retry != nullptr);
    {
        arch::IrqGuard irq;
        JARVIS_ASSERT(Scheduler::add_task_err(*retry) ==
                      errors::SCHED_ERR_OK);
    }
    gate.post();
    kernel::test::terminate_and_drain(*retry);
    Scheduler::drain_zombie_list();
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: SPORADIC/DEFERRABLE/BACKGROUND with identical C contribute
// identically to the admission numerator (mode-agnostic math).
// Input: three unregistered server TCBs (C=60), probe helper per mode.
// Expect: all OK with EQUAL util outputs (bg cancels out of the delta).
// Depends: server_budget_for_admission (issues #20/#22)
JARVIS_TEST(admission_verify_cross_mode_budget_parity,
            "PRE: none | POST: none") {
    sync::Semaphore gate;
    gate.init(0, 1);
    auto *ss = verify_make_task(0, 100, &gate);
    auto *ds = verify_make_task(0, 100, &gate);
    auto *bs = verify_make_task(0, 100, &gate);
    JARVIS_ASSERT(ss != nullptr);
    JARVIS_ASSERT(ds != nullptr);
    JARVIS_ASSERT(bs != nullptr);
    ss->init_sporadic_server(60, 100, 0);
    ds->init_sporadic_server(60, 100, 0, 1, ServerMode::DEFERRABLE);
    bs->init_sporadic_server(60, 100, 0, 1, ServerMode::BACKGROUND);
    uint64_t u_ss = 0;
    uint64_t u_ds = 0;
    uint64_t u_bs = 0;
    {
        arch::IrqGuard irq;
        JARVIS_ASSERT(Scheduler::admission_check_cpu_locked(
                          *ss, 0, &u_ss, nullptr) == errors::SCHED_ERR_OK);
        JARVIS_ASSERT(Scheduler::admission_check_cpu_locked(
                          *ds, 0, &u_ds, nullptr) == errors::SCHED_ERR_OK);
        JARVIS_ASSERT(Scheduler::admission_check_cpu_locked(
                          *bs, 0, &u_bs, nullptr) == errors::SCHED_ERR_OK);
    }
    JARVIS_ASSERT(u_ss == u_ds);
    JARVIS_ASSERT(u_ds == u_bs);
    TaskControlBlock::destroy(ss);
    TaskControlBlock::destroy(ds);
    TaskControlBlock::destroy(bs);
    Scheduler::drain_zombie_list();
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: the BACKGROUND table guard rejects non-idle bg priorities.
// Input: vfsd row copy mutated to BACKGROUND + bg 5; original row.
// Expect: mutated copy invalid, original valid (rule from issue #22,
// callable at runtime).
// Depends: taskdefs_server_params_valid (issues #22/#24)
JARVIS_TEST(admission_verify_background_table_guard,
            "PRE: none | POST: none") {
    const task::TaskDef *vfsd = nullptr;
    for (size_t i = 0; i < task::taskdefs_count(); ++i) {
        const task::TaskDef *d = task::taskdefs_at(i);
        if (d != nullptr && d->type == task::TaskType::SPORADIC_SERVER &&
            d->enabled && taskdefs_name_eq(d->daemon_name, "vfsd")) {
            vfsd = d;
            break;
        }
    }
    JARVIS_ASSERT(vfsd != nullptr);
    JARVIS_ASSERT(task::taskdefs_server_params_valid(*vfsd));
    task::TaskDef bad = *vfsd;
    bad.ss_mode = ServerMode::BACKGROUND;
    bad.ss_bg_prio = 5;
    JARVIS_ASSERT(!task::taskdefs_server_params_valid(bad));
    task::TaskDef good_bg = *vfsd;
    good_bg.ss_mode = ServerMode::BACKGROUND;
    good_bg.ss_bg_prio = 0;
    JARVIS_ASSERT(task::taskdefs_server_params_valid(good_bg));
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: every populated partition admits an epsilon task (per-CPU
// boot invariant, live at runtime).
// Input: epsilon (wcet 1, period 100) targeted per up-CPU.
// Expect: SCHED_ERR_OK on each CPU (single-CPU: CPU0 only).
// Depends: admission_check_cpu_locked partition key (issue #23)
JARVIS_TEST(admission_verify_percpu_epsilon, "PRE: none | POST: none") {
    sync::Semaphore gate;
    gate.init(0, 1);
    uint64_t up = Scheduler::up_cpu_count();
    if (up > CONFIG_MAX_CPUS)
        up = CONFIG_MAX_CPUS;
    if (up < 1)
        up = 1;
    for (uint64_t cpu = 0; cpu < up; ++cpu) {
        auto *e = verify_make_task(1, 100, &gate);
        JARVIS_ASSERT(e != nullptr);
        Scheduler::set_affinity(*e, static_cast<uint64_t>(1ULL) << cpu);
        errors::SchedulerError r;
        {
            arch::IrqGuard irq;
            r = Scheduler::add_task_err(*e);
        }
        JARVIS_ASSERT(r == errors::SCHED_ERR_OK);
        gate.post();
        kernel::test::terminate_and_drain(*e);
    }
    Scheduler::drain_zombie_list();
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: budget exhaustion is exact — failed reserves never mutate,
// reserve/release round-trips restore exactly.
// Input: reserve(remaining+1) → false + unchanged; reserve(avail) →
// release(avail) → identical counter.
// Expect: all three hold (opt-out builds trivially pass).
// Depends: global budget accounting (issue #20)
JARVIS_TEST(admission_verify_budget_exhaustion_roundtrip,
            "PRE: none | POST: none") {
#if CONFIG_MEMORY_BUDGET
    uint64_t avail = Scheduler::remaining_memory_budget();
    JARVIS_ASSERT(avail > 0);
    JARVIS_ASSERT(!Scheduler::reserve_memory_pages(avail + 1));
    JARVIS_ASSERT_EQ(avail, Scheduler::remaining_memory_budget());
    JARVIS_ASSERT(Scheduler::reserve_memory_pages(avail));
    Scheduler::release_memory_pages(avail);
    JARVIS_ASSERT_EQ(avail, Scheduler::remaining_memory_budget());
#else
    // Opt-out builds compile the gate out (sched_admission precedent).
#endif
    Scheduler::drain_zombie_list();
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: runtime parity with the boot self-test — the same four
// synthetic probes give the same outcomes live.
// Audit note: the probed outcomes below (WCET_INVALID from candidate
// fields only; exempt-OK from candidate flags only) are independent of
// live table state by construction, so the caller-holds-lock contract
// of admission_check_cpu_locked is safely waived here (same waiver as
// the boot self-test; the budget fail-proof reads a counter no ISR
// path mutates).
// Input: stack-TCB probes (wcet-invalid, exempt-stale) + budget
// fail-proof + CONFIG range (admit-math exact part is boot-only:
// live background load makes exact utils non-pinned).
// Expect: WCET_INVALID / OK / fail-proof / in-range, matching boot.
// Depends: admission helpers + boot self-test contract (issue #24)
JARVIS_TEST(admission_verify_boot_parity, "PRE: none | POST: none") {
    TaskControlBlock bad{};
    verify_make_probe(bad, 200, 100, 1000, false);
    JARVIS_ASSERT(Scheduler::admission_check_cpu_locked(bad, 0, nullptr,
                                                        nullptr) ==
                  errors::SCHED_ERR_WCET_INVALID);
    TaskControlBlock stale{};
    verify_make_probe(stale, 50, 0, 0, true);
    JARVIS_ASSERT(Scheduler::admission_check_cpu_locked(stale, 0, nullptr,
                                                        nullptr) ==
                  errors::SCHED_ERR_OK);
#if CONFIG_MEMORY_BUDGET
    uint64_t avail = Scheduler::remaining_memory_budget();
    JARVIS_ASSERT(avail > 0);
    JARVIS_ASSERT(!Scheduler::reserve_memory_pages(avail + 1));
    JARVIS_ASSERT_EQ(avail, Scheduler::remaining_memory_budget());
#endif
    JARVIS_ASSERT(CONFIG_DEADLINE_ACTION <= 4);
    JARVIS_TEST_PASS();
}

void register_sched_admission_verify_tests() {
    Logger::info("Registering sched_admission_verify tests");
    JARVIS_REGISTER_TEST(admission_verify_taskdefs_server_budgets);
    JARVIS_REGISTER_TEST(admission_verify_defer_retry_after_reap);
    JARVIS_REGISTER_TEST(admission_verify_cross_mode_budget_parity);
    JARVIS_REGISTER_TEST(admission_verify_background_table_guard);
    JARVIS_REGISTER_TEST(admission_verify_percpu_epsilon);
    JARVIS_REGISTER_TEST(admission_verify_budget_exhaustion_roundtrip);
    JARVIS_REGISTER_TEST(admission_verify_boot_parity);
}
