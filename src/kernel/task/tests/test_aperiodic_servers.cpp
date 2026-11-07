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

/// @file test_aperiodic_servers.cpp
/// @brief Deferrable + background server-mode tests (issue #22, v0.4.8).
///
/// DRIVEN: server state machines are driven through REAL init/activation/
/// consume/completion/replenish calls on live objects; scheduler placement
/// is observed through REAL add_task_err() outcomes and queue flags.
/// Admitted tasks block on a gate semaphore if dispatched (issue #20
/// dispatch-race lesson: empty entries self-terminate mid-setup).

#include <test.hpp>
#include <logger.hpp>
#include <kernel/task/sporadic_server.hpp>
#include <kernel/task/scheduler.hpp>
#include <kernel/task/task.hpp>
#include <kernel/task/taskdefs.hpp>
#include <kernel/arch/irq_guard.hpp>
#include <kernel/sync/semaphore.hpp>
#include <kernel/nexios_config.h>
#include "test_sched_helpers.hpp"

using namespace kernel;
using task::ServerMode;
using task::SporadicServer;

namespace {

// Blocks on the gate carried via user_data if ever dispatched.
void srv_gated_entry() {
    auto *self = Scheduler::current_task();
    auto *gate = reinterpret_cast<sync::Semaphore *>(self->user_data);
    gate->wait();
}

TaskControlBlock *make_srv_task(uint64_t wcet, uint64_t period,
                                sync::Semaphore *gate) {
    auto *t = TaskControlBlock::create(srv_gated_entry, 11, period);
    if (t == nullptr)
        return nullptr;
    t->wcet_ticks = wcet;
    t->user_data = gate;
    return t;
}

void destroy_denied_srv(TaskControlBlock *t) {
    if (t == nullptr)
        return;
    JARVIS_ASSERT(Scheduler::find_task(t->id) == nullptr);
    TaskControlBlock::destroy(t);
}

} // namespace

// Runmode: kernel
// Testidea: DEFERRABLE preserves budget across idle (no per-completion ring).
// Input: DS(C=3,T=10); activate@0; consume 1; complete; reactivate.
// Expect: remaining==2 and pending==0 after completion; still 2 after
// reactivation (vs SS, which would schedule a replenishment).
// Depends: SporadicServer::on_completion mode branch (issue #22)
JARVIS_TEST(ds_preserves_budget_across_idle, "PRE: none | POST: none") {
    SporadicServer ds;
    ds.init(3, 10, 0, 1, ServerMode::DEFERRABLE);
    JARVIS_ASSERT(ds.mode() == ServerMode::DEFERRABLE);
    ds.on_activation(0);
    JARVIS_ASSERT(ds.consume(0));
    ds.on_completion(1);
    JARVIS_ASSERT_EQ(2ULL, ds.remaining_budget());
    JARVIS_ASSERT_EQ(0ULL, ds.pending_count());
    ds.on_activation(2);
    JARVIS_ASSERT_EQ(2ULL, ds.remaining_budget());
    JARVIS_ASSERT_EQ(0ULL, ds.pending_count());
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: DEFERRABLE restores to full C at the period boundary.
// Input: DS(C=3,T=10); activate@0; consume 3 → EXHAUSTED; top-up at t=10.
// Expect: remaining==3, state != EXHAUSTED, ring still empty.
// Depends: SporadicServer::process_replenishments top-up (issue #22)
JARVIS_TEST(ds_periodic_topup, "PRE: none | POST: none") {
    SporadicServer ds;
    ds.init(3, 10, 0, 1, ServerMode::DEFERRABLE);
    ds.on_activation(0);
    JARVIS_ASSERT(ds.consume(0));
    JARVIS_ASSERT(ds.consume(1));
    JARVIS_ASSERT(!ds.consume(2));
    JARVIS_ASSERT(ds.state() == SporadicServer::EXHAUSTED);
    ds.process_replenishments(10);
    JARVIS_ASSERT_EQ(3ULL, ds.remaining_budget());
    JARVIS_ASSERT(ds.state() != SporadicServer::EXHAUSTED);
    JARVIS_ASSERT_EQ(0ULL, ds.pending_count());
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: pin the documented DS/SS preservation trade-off end to end.
// Input: C=4; bursts 2+2 back-to-back (no period elapse); then run the
// period boundary at t=10 for both.
// Expect: both serve all 4 ticks and demote (EXHAUSTED/bg); DS carries
// zero ring debt and restores to full C at the boundary, while SS carries
// 2 replenishment entries and restores only consumed amounts (rem 2).
// Depends: DS vs SS completion/exhaustion/top-up paths (issue #22)
JARVIS_TEST(ds_back_to_back_bursts, "PRE: none | POST: none") {
    SporadicServer ds;
    ds.init(4, 10, 0, 1, ServerMode::DEFERRABLE);
    ds.set_base_priority(20);
    SporadicServer ss;
    ss.init(4, 10, 0);
    ss.set_base_priority(20);
    for (uint64_t burst = 0; burst < 2; ++burst) {
        uint64_t now = burst * 2;
        ds.on_activation(now);
        ds.consume(now);
        ds.consume(now + 1);
        ds.on_completion(now + 1);
        ss.on_activation(now);
        ss.consume(now);
        ss.consume(now + 1);
        ss.on_completion(now + 1);
    }
    JARVIS_ASSERT_EQ(0ULL, ds.remaining_budget());
    JARVIS_ASSERT_EQ(0ULL, ds.pending_count());
    JARVIS_ASSERT(ds.state() == SporadicServer::EXHAUSTED);
    JARVIS_ASSERT(ss.state() == SporadicServer::EXHAUSTED);
    JARVIS_ASSERT_EQ(0ULL, ss.current_priority());
    JARVIS_ASSERT_EQ(2ULL, ss.pending_count());
    ds.process_replenishments(10);
    ss.process_replenishments(10);
    JARVIS_ASSERT_EQ(4ULL, ds.remaining_budget());
    JARVIS_ASSERT_EQ(0ULL, ds.pending_count());
    JARVIS_ASSERT_EQ(2ULL, ss.remaining_budget());
    JARVIS_ASSERT_EQ(1ULL, ss.pending_count());
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: BACKGROUND never promotes, full budget or not.
// Input: BG(C=2,T=10,bg=0,base=20); drain fully; top-up.
// Expect: current_priority()==0 throughout.
// Depends: SporadicServer::current_priority BG rule (issue #22)
JARVIS_TEST(bg_never_promotes, "PRE: none | POST: none") {
    SporadicServer bg;
    bg.init(2, 10, 0, 1, ServerMode::BACKGROUND);
    bg.set_base_priority(20);
    JARVIS_ASSERT(bg.is_background());
    JARVIS_ASSERT_EQ(0ULL, bg.current_priority());
    bg.on_activation(0);
    JARVIS_ASSERT(bg.consume(0));
    JARVIS_ASSERT_EQ(0ULL, bg.current_priority());
    JARVIS_ASSERT(!bg.consume(1));
    JARVIS_ASSERT_EQ(0ULL, bg.current_priority());
    bg.process_replenishments(10);
    JARVIS_ASSERT_EQ(2ULL, bg.remaining_budget());
    JARVIS_ASSERT_EQ(0ULL, bg.current_priority());
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: a non-exempt BACKGROUND task stays out of EDF dispatch while an
// EDF-eligible RT task takes the deadline list (idle-time only, enforced
// in edf_eligible — not just by daemon exempt flags).
// Input: BG(p100, C=60) + RT(p100, wcet=10), both admitted, IRQs masked.
// Expect: BG in bitmap (!in_edf_queue_), RT in EDF list.
// Depends: edf_eligible BACKGROUND exclusion (issue #22)
JARVIS_TEST(bg_no_edf_preempt, "PRE: none | POST: none") {
    sync::Semaphore gate;
    gate.init(0, 1);
    auto *bg = make_srv_task(0, 100, &gate);
    JARVIS_ASSERT(bg != nullptr);
    bg->init_sporadic_server(60, 100, 0, 1, ServerMode::BACKGROUND);
    auto *rt = make_srv_task(10, 100, &gate);
    JARVIS_ASSERT(rt != nullptr);
    {
        arch::IrqGuard irq;
        JARVIS_ASSERT(Scheduler::add_task_err(*bg) ==
                      errors::SCHED_ERR_OK);
        JARVIS_ASSERT(Scheduler::add_task_err(*rt) ==
                      errors::SCHED_ERR_OK);
        JARVIS_ASSERT(!bg->in_edf_queue_);
        JARVIS_ASSERT(rt->in_edf_queue_);
    }
    gate.post();
    gate.post();
    kernel::test::terminate_and_drain(*bg);
    kernel::test::terminate_and_drain(*rt);
    Scheduler::drain_zombie_list();
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: admission counts every server mode's C identically; explicit
// WCET still overrides the server budget.
// Input: SS(C=60) admitted; DS(C=60) would total 120>82 → denied;
// server task with wcet=5 counts 5 (60+5=65<=82 → admitted).
// Expect: DENIED / OK as above; denied TCB absent from tables.
// Depends: server_budget_for_admission helper (issues #20/#22)
JARVIS_TEST(admission_counts_server_budgets, "PRE: none | POST: none") {
    sync::Semaphore gate;
    gate.init(0, 1);
    auto *ss = make_srv_task(0, 100, &gate);
    JARVIS_ASSERT(ss != nullptr);
    ss->init_sporadic_server(60, 100, 0);
    JARVIS_ASSERT(ss->get_sporadic_server()->mode() ==
                  ServerMode::SPORADIC);
    JARVIS_ASSERT(Scheduler::add_task_err(*ss) == errors::SCHED_ERR_OK);
    auto *ds = make_srv_task(0, 100, &gate);
    JARVIS_ASSERT(ds != nullptr);
    ds->init_sporadic_server(60, 100, 0, 1, ServerMode::DEFERRABLE);
    JARVIS_ASSERT(Scheduler::add_task_err(*ds) ==
                  errors::SCHED_ERR_ADMISSION_DENIED);
    destroy_denied_srv(ds);
    auto *ov = make_srv_task(5, 100, &gate);
    JARVIS_ASSERT(ov != nullptr);
    ov->init_sporadic_server(60, 100, 0);
    JARVIS_ASSERT(Scheduler::add_task_err(*ov) == errors::SCHED_ERR_OK);
    gate.post();
    gate.post();
    kernel::test::terminate_and_drain(*ss);
    kernel::test::terminate_and_drain(*ov);
    Scheduler::drain_zombie_list();
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: 4-arg init and TaskDef defaults stay SPORADIC (compat).
// Input: 4-arg init(C,T,bg); default-constructed TaskDef.
// Expect: mode()==SPORADIC; TaskDef::ss_mode==SPORADIC.
// Depends: ServerMode default args (issue #22)
JARVIS_TEST(mode_compat_default, "PRE: none | POST: none") {
    SporadicServer ss;
    ss.init(5, 100, 2);
    JARVIS_ASSERT(ss.mode() == ServerMode::SPORADIC);
    JARVIS_ASSERT(!ss.is_deferrable());
    JARVIS_ASSERT(!ss.is_background());
    task::TaskDef def{};
    JARVIS_ASSERT(def.ss_mode == ServerMode::SPORADIC);
    JARVIS_TEST_PASS();
}

void register_aperiodic_servers_tests() {
    Logger::info("Registering aperiodic_servers tests");
    JARVIS_REGISTER_TEST(ds_preserves_budget_across_idle);
    JARVIS_REGISTER_TEST(ds_periodic_topup);
    JARVIS_REGISTER_TEST(ds_back_to_back_bursts);
    JARVIS_REGISTER_TEST(bg_never_promotes);
    JARVIS_REGISTER_TEST(bg_no_edf_preempt);
    JARVIS_REGISTER_TEST(admission_counts_server_budgets);
    JARVIS_REGISTER_TEST(mode_compat_default);
}
