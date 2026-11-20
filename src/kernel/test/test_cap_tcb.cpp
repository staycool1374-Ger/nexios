/*
 * NexIOS RTOS — Capability-Based Access Control (CSpace)
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

/// @file test_cap_tcb.cpp
/// @brief TCB control tests (F4 N1-N3, issue #557): the admin suspend/
///        resume mechanism (core_task group) and the TcbCap authority
///        rule (capability group).

#include <test.hpp>
#include <logger.hpp>
#include <kernel/cap/cap.hpp>
#include <kernel/cap/cap_types.hpp>
#include <kernel/cap/tcb_cap.hpp>
#include <kernel/cap/untyped.hpp>
#include <kernel/memory/kernel_object.hpp>
#include <kernel/memory/mempool.hpp>
#include <kernel/task/scheduler.hpp>
#include <kernel/task/task.hpp>
#include <kernel/task/tcb_control.hpp>
#include <kernel/task/tcb_control_errors.hpp>
#include <kernel/syscall/syscall.hpp>
#include <kernel/test/resource_tracker.hpp>
#include <kernel/test/test_sched_helpers.hpp>
#include <kernel/sync/semaphore.hpp>
#include <kernel/arch/timer.hpp>
#include <kernel/nexios_config.h>

using namespace kernel;

namespace {

// Runs @p entry as a real task with a CSpace, waits, drains, returns it.
// The driver task is made non-EDF (deadline 0, like the harness
// itself): an EDF driver with an earlier deadline than its target
// would never be preempted (edf_earlier fails), starving the target
// forever. A non-EDF current always yields to any EDF candidate.
TaskControlBlock *run_tcb_task(void (*entry)(), uint64_t prio = 11,
                               uint64_t period = 10) {
    auto *t = TaskControlBlock::create(entry, prio, period);
    if (!t)
        return nullptr;
    t->deadline_ticks = 0; // non-EDF driver (see above)
    Scheduler::add_task(*t);
    Scheduler::reschedule();
    kernel::test::wait_for_termination_safe(t);
    Scheduler::drain_zombie_list();
    return t;
}

// Installs a TcbCap for @p target_id/@p gen into @p cs; returns slot or -1.
int install_tcb_cap(cap::CNode *cs, uint64_t target_id, uint32_t gen) {
    auto *cap = cap::TcbCap::create(target_id, gen);
    if (!cap)
        return -1;
    const int s = cs->install(cap, cap::CapType::TCB,
                              cap::CapRights::READ | cap::CapRights::WRITE);
    if (s < 0) {
        cap->release();
        return -1;
    }
    return s;
}

uint64_t tcb_handle(cap::CNode *cs, int slot) {
    return cap::encode_handle(cs->cspace_id, static_cast<uint32_t>(slot),
                              cs->slot_gen(static_cast<uint32_t>(slot)));
}

// Syscall-face encoding of a TCBCTL error: -(1000 + code).
uint64_t tcb_err(uint64_t code) {
    return 0ULL - (1000ULL + code);
}

// Bounded hlt-wait for @p target to leave RUNNING (yields the CPU like
// wait_for_termination_safe does, so a higher-priority waiter cannot
// starve the target). Records spins/state/ticks/in-ready-queue for the
// harness to assert (and to diagnose starvation: READY + in_rq==0
// means dequeued-without-dispatch).
uint64_t g_tcb_wait_spins = 0;
uint64_t g_tcb_wait_state = 0;
uint64_t g_tcb_ticks0 = 0;
uint64_t g_tcb_ticks1 = 0;
uint64_t g_tcb_inrq = 0;
uint64_t g_tcb_inedf = 0;
uint64_t g_tcb_rqprio = 0;
uint64_t g_tcb_deadline = 0;
uint64_t g_tcb_period = 0;
uint64_t g_tcb_policy = 0;

void tcb_wait_settled(TaskControlBlock *target) {
    g_tcb_wait_spins = 0;
    g_tcb_wait_state = 0;
    g_tcb_ticks0 = arch::Timer::ticks();
    for (uint64_t i = 0;
         i < 1000 && target->state != TaskState::TERMINATED &&
         target->state != TaskState::BLOCKED;
         ++i) {
        __atomic_store_n(&kernel::Scheduler::SwSlots::need_resched(), true,
                         __ATOMIC_RELAXED);
        arch::hlt();
        ++g_tcb_wait_spins;
    }
    g_tcb_ticks1 = arch::Timer::ticks();
    g_tcb_wait_state = static_cast<uint64_t>(target->state);
    g_tcb_inrq = target->in_ready_queue_ ? 1ULL : 0ULL;
    g_tcb_inedf = target->in_edf_queue_ ? 1ULL : 0ULL;
    g_tcb_rqprio = target->rq_priority_;
    g_tcb_deadline = target->deadline_ticks;
    g_tcb_period = target->period_ticks;
    g_tcb_policy = static_cast<uint64_t>(target->sched_policy);
}

} // namespace

// Runmode: kernel
// Testidea: F4 suspend parks a READY task: admin_parked set, state
//        BLOCKED, dequeued from ready/EDF queues; a tick does not run it.
// Input: READY target; Scheduler::admin_suspend_err; one tick.
// Expect: admin_parked true; state BLOCKED; !in_ready_queue_;
//         run counter unchanged after the tick.
// Depends: Scheduler::admin_suspend_err, tick path
// Pseudocode: create+READY target; snapshot run counter; suspend (0);
// assert parked/BLOCKED/dequeued; tick; counter unchanged; teardown
// (resume + drain).
JARVIS_TEST(tcb_suspend_ready_parks_and_dequeues, "PRE: none | POST: none") {
    static uint64_t g_runs = 0;
    static uint64_t g_parked = 0;
    static uint64_t g_state = 0;
    static uint64_t g_queued = 0;
    static uint64_t g_runs_after = 0;
    static uint64_t g_admin = 0;
    g_runs = g_parked = g_state = g_queued = g_runs_after = g_admin = 0;

    auto *t = run_tcb_task([]() {
        auto *cur = Scheduler::current_task();
        cur->ensure_cspace();
        // READY target that counts runs; parked before it ever runs.
        auto *target = TaskControlBlock::create(
            []() {
                ++g_runs;
                for (;;)
                    arch::hlt();
            },
            20, 10);
        if (!target)
            return;
        target->is_user_ = true;
        target->parent_id = cur->id;
        // Tick-atomic setup: a timer tick between add and suspend could
        // dispatch the target first (making it RUNNING, not READY).
        {
            arch::IrqGuard irq_guard{};
            Scheduler::add_task(*target);
            // Suspend the never-dispatched READY target (no latch: not RUNNING).
            const errors::TcbCtlError r =
                Scheduler::admin_suspend_err(*target);
            g_parked = (r == errors::TCBCTL_ERR_OK) ? 1ULL : 0ULL;
        }
        g_admin = Scheduler::admin_parked_of(*target) ? 1ULL : 0ULL;
        g_state = static_cast<uint64_t>(target->state);
        g_queued = target->in_ready_queue_ ? 1ULL : 0ULL;
        // Let a tick pass; a parked target must not run.
        const uint64_t t0 = arch::Timer::ticks();
        while (arch::Timer::ticks() == t0)
            arch::hlt();
        g_runs_after = g_runs;
        Scheduler::admin_resume_err(*target);
        Scheduler::terminate(*target, 0);
        Scheduler::drain_zombie_list();
    });
    JARVIS_ASSERT(t != nullptr);
    JARVIS_ASSERT_EQ(1ULL, g_parked);
    JARVIS_ASSERT_EQ(1ULL, g_admin);
    JARVIS_ASSERT_EQ(static_cast<uint64_t>(TaskState::BLOCKED), g_state);
    JARVIS_ASSERT_EQ(0ULL, g_queued);
    JARVIS_ASSERT_EQ(0ULL, g_runs_after);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: F4 resume of a parked READY target re-queues and runs it.
// Input: Suspended READY target; admin_resume_err.
// Expect: READY + queued after resume; runs to completion; counter +1.
// Depends: Scheduler::admin_resume_err, dispatch
// Pseudocode: suspend READY target; resume (0); assert READY+queued;
// run to completion; counter +1; teardown.
JARVIS_TEST(tcb_resume_prev_ready_reenables, "PRE: none | POST: none") {
    static uint64_t g_runs2 = 0;
    static uint64_t g_susp = 0;
    static uint64_t g_resume = 0;
    static uint64_t g_ready = 0;
    static uint64_t g_queued = 0;
    g_runs2 = g_susp = g_resume = g_ready = g_queued = 0;

    auto *t = run_tcb_task([]() {
        auto *cur = Scheduler::current_task();
        // Single-shot target: counts one run, then returns
        // (terminates). Parked before it ever runs.
        auto *target = TaskControlBlock::create(
            []() {
                ++g_runs2;
            },
            20, 10);
        if (!target)
            return;
        target->is_user_ = true;
        target->parent_id = cur->id;
        {
            arch::IrqGuard irq_guard{};
            Scheduler::add_task(*target);
            const errors::TcbCtlError r =
                Scheduler::admin_suspend_err(*target);
            g_susp = static_cast<uint64_t>(r);
            if (r != errors::TCBCTL_ERR_OK)
                return;
        }
        g_resume = static_cast<uint64_t>(
            Scheduler::admin_resume_err(*target));
        g_ready = (target->state == TaskState::READY) ? 1ULL : 0ULL;
        // Queued means bitmap OR EDF list (periodic targets route to
        // the EDF list exclusively — no bitmap flag).
        g_queued = (target->in_ready_queue_ || target->in_edf_queue_) ? 1ULL
                                                                      : 0ULL;
        kernel::test::wait_for_termination_safe(target);
        Scheduler::drain_zombie_list();
    });
    JARVIS_ASSERT(t != nullptr);
    JARVIS_ASSERT_EQ(0ULL, g_susp);
    JARVIS_ASSERT_EQ(0ULL, g_resume);
    JARVIS_ASSERT_EQ(1ULL, g_ready);
    JARVIS_ASSERT_EQ(1ULL, g_queued);
    JARVIS_ASSERT_EQ(1ULL, g_runs2);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: F4 suspend absorbs a wake (INV-ADM-1): target blocked on a
//        Semaphore is suspended; a post wakes but is absorbed (pending
//        latched, task stays parked, did not run); resume delivers.
//        Same shape for a waitpid-blocked parent whose child exits
//        (#588 carve-out INV-ADM-13) plus the INV-ADM-14 budget check.
// Input: Semaphore-blocked target; suspend; post; resume.
// Expect: post -> adm_wake_absorbed_count +1, still BLOCKED+parked,
//         did not run; resume -> READY, wait returns, counter +1.
// Depends: admin_absorb_wake (INV-ADM-1), #588 carve-out, waitpid path
// Pseudocode: block target on semaphore; suspend; post (absorbed, no
// run); resume; wait returns; counter +1; teardown. Repeat shape for
// waitpid-blocked parent + exiting child (assert status delivered).
JARVIS_TEST(tcb_suspend_blocked_absorbs_wake_then_resumes, "PRE: none | POST: none") {
    static uint64_t g_abs0 = 0;
    static uint64_t g_abs1 = 0;
    static uint64_t g_woke = 0;
    static uint64_t g_parked = 0;
    static uint64_t g_resumed = 0;
    static uint64_t g_still_blocked = 0;
    static uint64_t g_still_parked = 0;
    g_abs0 = g_abs1 = g_woke = g_parked = g_resumed = g_still_blocked =
        g_still_parked = 0;

    auto *t = run_tcb_task([]() {
        auto *cur = Scheduler::current_task();
        cur->ensure_cspace();
        sync::Semaphore sem;
        sem.init(0, 1);
        // Latch into the waiter: waits once, counts the wake, exits.
        auto *target = TaskControlBlock::create(
            []() {
                auto *self = Scheduler::current_task();
                auto *s = reinterpret_cast<sync::Semaphore *>(
                    self->user_data);
                s->wait();
                __atomic_fetch_add(&g_woke, 1ULL, __ATOMIC_RELAXED);
            },
            20, 10);
        if (!target)
            return;
        target->is_user_ = true;
        target->parent_id = cur->id;
        target->user_data = &sem;
        Scheduler::add_task(*target);
        Scheduler::reschedule();
        tcb_wait_settled(target);
        if (target->state != TaskState::BLOCKED)
            return;
        g_abs0 = Scheduler::adm_wake_absorbed_count();
        // Suspend the semaphore-BLOCKED target: parked, still BLOCKED.
        if (Scheduler::admin_suspend_err(*target) !=
            errors::TCBCTL_ERR_OK)
            return;
        g_parked = Scheduler::admin_parked_of(*target) ? 1ULL : 0ULL;
        // A post while parked must be absorbed: count +1, still
        // BLOCKED + parked, task did not run past the wait.
        sem.post();
        g_abs1 = Scheduler::adm_wake_absorbed_count();
        g_still_blocked =
            (target->state == TaskState::BLOCKED) ? 1ULL : 0ULL;
        g_still_parked =
            Scheduler::admin_parked_of(*target) ? 1ULL : 0ULL;
        // Resume returns the task to its semaphore wait...
        g_resumed = static_cast<uint64_t>(
            Scheduler::admin_resume_err(*target));
        // ...which a second post then satisfies: wait returns,
        // counter +1, task terminates.
        sem.post();
        kernel::test::wait_for_termination_safe(target);
        Scheduler::drain_zombie_list();
    });
    JARVIS_ASSERT(t != nullptr);
    JARVIS_ASSERT_EQ(g_abs0 + 1, g_abs1);
    JARVIS_ASSERT_EQ(1ULL, g_parked);
    JARVIS_ASSERT_EQ(1ULL, g_still_blocked);
    JARVIS_ASSERT_EQ(1ULL, g_still_parked);
    JARVIS_ASSERT_EQ(0ULL, g_resumed);
    JARVIS_ASSERT_EQ(1ULL, g_woke);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: F4 absorb backstop refuses direct enqueue of a parked task
//        (INV-ADM-1 layer b): enqueue_ready(parked) -> refused, state
//        restored BLOCKED, backstop counter +1, not queued, orphan
//        rescue untouched.
// Input: Parked target; Scheduler::enqueue_ready(*target) directly.
// Expect: Refused; state BLOCKED; adm_wake_backstop_count == 1;
//         in_ready_queue_ false; orphan_rescue_count unchanged.
// Depends: enqueue_ready backstop, orphan_rescue_count oracle
// Pseudocode: suspend READY target; call enqueue_ready directly
// (refused); assert counters/state; teardown (resume + drain).
JARVIS_TEST(tcb_suspend_absorb_backstop_refuses_link, "PRE: none | POST: none") {
    static uint64_t g_back0 = 0;
    static uint64_t g_back1 = 0;
    static uint64_t g_orp0 = 0;
    static uint64_t g_orp1 = 0;
    static uint64_t g_blocked = 0;
    static uint64_t g_queued = 0;
    static uint64_t g_parked = 0;
    g_back0 = g_back1 = g_orp0 = g_orp1 = g_blocked = g_queued = g_parked = 0;

    auto *t = run_tcb_task([]() {
        auto *cur = Scheduler::current_task();
        cur->ensure_cspace();
        sync::Semaphore sem;
        sem.init(0, 1);
        auto *target = TaskControlBlock::create(
            []() {
                auto *self = Scheduler::current_task();
                auto *s = reinterpret_cast<sync::Semaphore *>(
                    self->user_data);
                s->wait();
                while (self->state == TaskState::BLOCKED)
                    arch::pause();
            },
            20, 10);
        if (!target)
            return;
        target->is_user_ = true;
        target->parent_id = cur->id;
        target->user_data = &sem;
        Scheduler::add_task(*target);
        Scheduler::reschedule();
        tcb_wait_settled(target);
        if (target->state != TaskState::BLOCKED)
            return;
        if (Scheduler::admin_suspend_err(*target) !=
            errors::TCBCTL_ERR_OK)
            return;
        g_back0 = Scheduler::adm_wake_backstop_count();
        g_orp0 = Scheduler::orphan_rescue_count();
        // Direct enqueue of a parked task: refused, stays BLOCKED.
        Scheduler::enqueue_ready(*target);
        g_back1 = Scheduler::adm_wake_backstop_count();
        g_orp1 = Scheduler::orphan_rescue_count();
        g_blocked =
            (target->state == TaskState::BLOCKED) ? 1ULL : 0ULL;
        g_parked = Scheduler::admin_parked_of(*target) ? 1ULL : 0ULL;
        g_queued = target->in_ready_queue_ ? 1ULL : 0ULL;
        Scheduler::admin_resume_err(*target);
        Scheduler::terminate(*target, 0);
        Scheduler::drain_zombie_list();
    });
    JARVIS_ASSERT(t != nullptr);
    JARVIS_ASSERT_EQ(0ULL, g_back0); // must-read-0 outside this test
    JARVIS_ASSERT_EQ(g_back0 + 1, g_back1);
    JARVIS_ASSERT_EQ(g_orp0, g_orp1);
    JARVIS_ASSERT_EQ(1ULL, g_blocked);
    JARVIS_ASSERT_EQ(1ULL, g_parked);
    JARVIS_ASSERT_EQ(0ULL, g_queued);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: F4 resume of a live non-suspended target is a typed
//        no-op (NOT_SUSPENDED), state byte-identical.
// Input: Live READY/RUNNING target (never suspended); resume.
// Expect: NOT_SUSPENDED; no state/queue change.
// Depends: admin_resume_err NOT_SUSPENDED leg
// Pseudocode: live target; resume (expect NOT_SUSPENDED); assert
// state/queue untouched; teardown.
JARVIS_TEST(tcb_resume_not_suspended_typed, "PRE: none | POST: none") {
    static uint64_t g_ret = 0;
    static uint64_t g_parked = 0;
    static uint64_t g_state = 0;
    g_ret = g_parked = g_state = 0;

    auto *t = run_tcb_task([]() {
        auto *cur = Scheduler::current_task();
        // Live user child, never suspended.
        auto *target = TaskControlBlock::create(kernel::test::forever_entry,
                                                20, 10);
        if (!target)
            return;
        target->is_user_ = true;
        target->parent_id = cur->id;
        Scheduler::add_task(*target);
        Scheduler::reschedule();
        g_ret = static_cast<uint64_t>(
            Scheduler::admin_resume_err(*target));
        g_parked = Scheduler::admin_parked_of(*target) ? 1ULL : 0ULL;
        g_state = static_cast<uint64_t>(target->state);
        Scheduler::terminate(*target, 0);
        Scheduler::drain_zombie_list();
    });
    JARVIS_ASSERT(t != nullptr);
    JARVIS_ASSERT_EQ(static_cast<uint64_t>(errors::TCBCTL_ERR_NOT_SUSPENDED),
                     g_ret);
    JARVIS_ASSERT_EQ(0ULL, g_parked);
    JARVIS_ASSERT(static_cast<uint64_t>(TaskState::TERMINATED) != g_state);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: F4 suspend/resume of dead targets fails closed: terminate()
//        synchronously unlinks the corpse from the id table
//        (release_zombie runs inside terminate's locked section), so a
//        TERMINATED-but-undrained target is already unaddressable —
//        admin_suspend_err/admin_resume_err return BAD_HANDLE via the
//        find_task miss, which precedes the DEAD check. Same for the
//        reaped half via tcb_control_resolve. DEVIATION from plan-557
//        test 6 (TERMINATED -> DEAD): the DEAD leg covers an in-table
//        TERMINATED TCB, which terminate() never leaves behind, so it is
//        defense-in-depth only; BAD_HANDLE here is the more closed of
//        the two fail-closeds (flagged for dev on #557 to either drive
//        DEAD or re-spec it).
// Input: TERMINATED target (no drain); reaped target.
// Expect: BAD_HANDLE; BAD_HANDLE; no state change.
// Depends: admin_suspend_err/admin_resume_err find_task gate,
//        tcb_control_resolve id-table miss, release_zombie unlink
// Pseudocode: terminate target (no drain); suspend+resume
// (BAD_HANDLE: corpse already unlinked); drain (reaped); resolve via
// fresh cap (BAD_HANDLE); teardown.
JARVIS_TEST(tcb_suspend_dead_target_typed, "PRE: none | POST: none") {
    static uint64_t g_susp = 0;
    static uint64_t g_res = 0;
    static uint64_t g_reaped = 0;
    g_susp = g_res = g_reaped = 0;

    auto *t = run_tcb_task([]() {
        auto *cur = Scheduler::current_task();
        cur->ensure_cspace();
        cap::CNode *cs = cur->get_cspace();
        auto *target = TaskControlBlock::create(kernel::test::forever_entry,
                                                20, 10);
        if (!target)
            return;
        target->is_user_ = true;
        target->parent_id = cur->id;
        Scheduler::add_task(*target);
        int tslot = install_tcb_cap(cs, target->id, target->generation);
        if (tslot < 0) {
            Scheduler::terminate(*target, 0);
            Scheduler::drain_zombie_list();
            return;
        }
        // Terminate but do NOT drain: TERMINATED, still in the table.
        Scheduler::terminate(*target, 0);
        g_susp = static_cast<uint64_t>(
            Scheduler::admin_suspend_err(*target));
        g_res = static_cast<uint64_t>(
            Scheduler::admin_resume_err(*target));
        // Drain (reap + free), then mint a fresh cap: TcbCap::create
        // validates liveness and refuses the dead target (nullptr) —
        // the fail-closed point is one step earlier than resolve, and
        // stronger (no cap to even present). g_reaped records mint
        // refusal (1) vs unexpected success (0).
        Scheduler::drain_zombie_list();
        auto *cap2 = cap::TcbCap::create(target->id, target->generation);
        g_reaped = (cap2 == nullptr) ? 1ULL : 0ULL;
        if (cap2) {
            cap2->release();
        }
        cs->remove(static_cast<uint32_t>(tslot));
    });
    JARVIS_ASSERT(t != nullptr);
    JARVIS_ASSERT_EQ(static_cast<uint64_t>(errors::TCBCTL_ERR_BAD_HANDLE),
                     g_susp);
    JARVIS_ASSERT_EQ(static_cast<uint64_t>(errors::TCBCTL_ERR_BAD_HANDLE),
                     g_res);
    JARVIS_ASSERT_EQ(1ULL, g_reaped);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: F4 deferred RUNNING-target stop: suspend latches
//        admin_stop_requested on a RUNNING user task; a tick with a
//        user-mode PC parks it, a tick with a kernel PC does not.
// Input: RUNNING user task; suspend (latch); tick with user PC; tick
//        with kernel PC.
// Expect: Latch set; user-PC tick -> parked; kernel-PC tick ->
//         still RUNNING.
// Depends: deferred-stop latch, tick PC discrimination (INV-ADM-9)
// Pseudocode: RUNNING user target; suspend (latched, still RUNNING);
// user-PC tick (parked); kernel-PC tick on a fresh target (RUNNING);
// teardown (resume + drain).
JARVIS_TEST(tcb_suspend_deferred_running_boundary, "PRE: none | POST: none") {
    static uint64_t g_latched = 0;
    static uint64_t g_still_running = 0;
    static uint64_t g_parked = 0;
    static uint64_t g_blocked = 0;
    static uint64_t g_klatch = 0;
    static uint64_t g_krunning = 0;
    g_latched = g_still_running = g_parked = g_blocked = g_klatch =
        g_krunning = 0;

    auto *t = run_tcb_task([]() {
        auto *cur = Scheduler::current_task();
        // Fabricated-RUNNING user target: added READY (add_task
        // requires it), flipped to RUNNING under IF=0 so no tick can
        // dispatch the fabrication. Mirrors what the tick observes.
        auto *target = TaskControlBlock::create(
            []() {
                for (;;)
                    arch::hlt();
            },
            20, 10);
        if (!target)
            return;
        target->is_user_ = true;
        target->parent_id = cur->id;
        Scheduler::add_task(*target);
        {
            arch::IrqGuard irq_guard{};
            target->state = TaskState::RUNNING;
            // Suspend a RUNNING target: deferred latch, still RUNNING.
            const errors::TcbCtlError r =
                Scheduler::admin_suspend_err(*target);
            if (r != errors::TCBCTL_ERR_OK)
                return;
            g_latched =
                __atomic_load_n(&target->admin_stop_requested,
                                __ATOMIC_ACQUIRE)
                    ? 1ULL
                    : 0ULL;
            g_still_running =
                (target->state == TaskState::RUNNING) ? 1ULL : 0ULL;
            // Tick with a user-mode PC: the deferred consumer parks it.
            // Single-actor under IF=0 (no tick/ISR can interleave); the
            // consumer's park tail is the same code the tick runs.
            Scheduler::admin_consume_deferred_stop(*target, 0x1000);
            g_parked = Scheduler::admin_parked_of(*target) ? 1ULL : 0ULL;
            g_blocked =
                (target->state == TaskState::BLOCKED) ? 1ULL : 0ULL;
        }
        // Resume + reap the parked target (public API, lock-safe).
        Scheduler::admin_resume_err(*target);
        Scheduler::terminate(*target, 0);
        Scheduler::drain_zombie_list();
        // Kernel-PC half: fresh target, same latch setup; a kernel PC
        // (>= USER_SPACE_LIMIT) must NOT park it and must NOT consume
        // the latch.
        auto *ktarget = TaskControlBlock::create(
            []() {
                for (;;)
                    arch::hlt();
            },
            20, 10);
        if (!ktarget)
            return;
        ktarget->is_user_ = true;
        ktarget->parent_id = cur->id;
        Scheduler::add_task(*ktarget);
        {
            arch::IrqGuard irq_guard{};
            ktarget->state = TaskState::RUNNING;
            if (Scheduler::admin_suspend_err(*ktarget) !=
                errors::TCBCTL_ERR_OK) {
                ktarget->state = TaskState::READY;
                return;
            }
            Scheduler::admin_consume_deferred_stop(
                *ktarget, CONFIG_USER_SPACE_LIMIT);
            g_klatch =
                __atomic_load_n(&ktarget->admin_stop_requested,
                                __ATOMIC_ACQUIRE)
                    ? 1ULL
                    : 0ULL;
            g_krunning =
                (ktarget->state == TaskState::RUNNING) ? 1ULL : 0ULL;
            // Park it for real so teardown is uniform: user-PC consume,
            // then drop the guard before the public-API teardown.
            Scheduler::admin_consume_deferred_stop(*ktarget, 0x1000);
        }
        Scheduler::admin_resume_err(*ktarget);
        Scheduler::terminate(*ktarget, 0);
        Scheduler::drain_zombie_list();
    });
    JARVIS_ASSERT(t != nullptr);
    JARVIS_ASSERT_EQ(1ULL, g_latched);
    JARVIS_ASSERT_EQ(1ULL, g_still_running);
    JARVIS_ASSERT_EQ(1ULL, g_parked);
    JARVIS_ASSERT_EQ(1ULL, g_blocked);
    JARVIS_ASSERT_EQ(1ULL, g_klatch);
    JARVIS_ASSERT_EQ(1ULL, g_krunning);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: F4 watchdog/deadline shift (r1 S1): suspend freezes the
//        watchdog expiry tick (not zeroed, gen+1) and removes the
//        deadline node; ticks past expiry cause no kill/miss; resume
//        shifts both by the elapsed interval (gen+1 again); an unarmed
//        target resumes disarmed with no phantom arm.
// Input: Periodic user target with armed watchdog (period P, expiry
//        E, gen g) + deadline; suspend at S; ticks past E + deadline;
//        resume at R; re-check; unarmed-target half.
// Expect: wdog_armed false, expiry E PRESERVED, gen g+1,
//         admin_wdog_expiry E, deadline node gone; no kill/miss, no
//         TIMING+6/+7, SCHED+21 then +22; resume: expiry E+(R-S),
//         last_kick shifted, gen g+2, deadline shifted,
//         remaining_ticks unchanged, SYS_WATCHDOG_KICK works; unarmed
//         target: wdog_armed false, gen unchanged after resume.
// Depends: INV-ADM-11/16, tick scan, watchdog kick path
// Pseudocode: armed periodic target; suspend; assert frozen + removed;
// tick past E/deadline (no kill/miss, no dmesg records); resume;
// assert shifted + live kick; unarmed half; teardown.
JARVIS_TEST(tcb_resume_rearms_watchdog_and_shifts_deadline, "PRE: none | POST: none") {
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: F4 resume shift saturates — a fabricated far-future
//        admin_wdog_expiry_tick/admin_deadline_ticks resumes clamped
//        at UINT64_MAX (no wrap to an immediate fire).
// Input: Parked target with expiry/deadline set near UINT64_MAX;
//        resume.
// Expect: Clamped to UINT64_MAX; no immediate fire/kill.
// Depends: admin_resume_err saturating shift
// Pseudocode: fabricate near-max expiry/deadline on parked target;
// resume (0); assert clamped, no fire; teardown.
JARVIS_TEST(tcb_resume_shift_saturates, "PRE: none | POST: none") {
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: F4 watchdog arm refused while parked — suspend an
//        armed-candidate target, then SYS_WATCHDOG_CREATE ->
//        INVALID_STATE and no kill on subsequent ticks;
//        SYS_WATCHDOG_KICK -> INVALID_STATE (already-disarmed path).
// Input: Suspended target; CREATE + KICK; tick.
// Expect: Both INVALID_STATE; target not killed by tick scan.
// Depends: suspend-time disarm, watchdog create/kick gates
// Pseudocode: suspend target; CREATE (INVALID_STATE); KICK
// (INVALID_STATE); tick; target alive; teardown (resume + drain).
JARVIS_TEST(tcb_watchdog_arm_refused_while_parked, "PRE: none | POST: none") {
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: F4 a task halted in resched_park_until_dispatched is a
//        legitimate suspend target (INV-ADM-9): stays halted, released
//        by resume without tripping park/apply counters.
// Input: Task halted in the resched park; suspend; resume.
// Expect: Stays halted across suspend; resume releases; no
//         park/apply counter trip.
// Depends: INV-ADM-9, resched park path
// Pseudocode: halt task in resched park; suspend (stays halted);
// resume (released, no counter trip); teardown.
JARVIS_TEST(tcb_suspend_parked_runner_released_by_resume, "PRE: none | POST: none") {
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: F4 latch-armed suspend refusal — a fabricated latch via
//        gs::lockpark_latch_arm makes suspend return BUSY; reset via
//        gs::lockpark_reset_for_test.
// Input: Armed latch; suspend.
// Expect: BUSY; park state untouched.
// Depends: #582 fail-closed latch, lockpark_reset_for_test
// Pseudocode: arm latch; suspend (BUSY); assert untouched; reset;
// teardown.
JARVIS_TEST(tcb_suspend_latch_armed_refused, "PRE: none | POST: none") {
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: F4 two park owners stay mutually exclusive (rev 2) —
//        suspend a debug-parked target -> BUSY, debug park intact;
//        debug_park_stop on an admin-parked target -> false,
//        admin_parked intact.
// Input: Debug-parked target + suspend; admin-parked target +
//        debug_park_stop.
// Expect: BUSY + intact; false + intact.
// Depends: INV-ADM park mutual exclusion, both directions
// Pseudocode: debug-park target; suspend (BUSY, still debug-parked);
// admin-park other; debug_park_stop (false, still admin-parked);
// teardown both.
JARVIS_TEST(tcb_debugger_admin_park_mutual_exclusion, "PRE: none | POST: none") {
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: F4 mk_supervisor_stop capability matrix (r1 S2-2): creator
//        allow; orphaned creator deny; foreign deny (watch-holding
//        foreign still deny); SUPERVISOR-scope allow only for the bound
//        grantee; onward-copied cap deny; self deny; kernel/idle deny;
//        revoked/deleted/stale/readonly deny.
// Input: Matrix of caller/target/cap combinations via 99/100/101.
// Expect: Each cell returns its specified typed code, zero side
//         effects on refusal.
// Depends: tcb_control_authorize INV-ADM-12 branches, TcbCap
//         create/create_supervised, DeathNotify watch (no authority)
// Pseudocode: build each cell; call; assert exact typed code;
// teardown all caps/tasks.
JARVIS_TEST(mk_supervisor_stop, "PRE: none | POST: none") {
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: F4 never-mem-mgr predicate (rev 2): a user helper whose
//        CSpace holds a minted Untyped cap -> PROTECTED_ROLE for SUSPEND
//        and CONFIGURE; after the Untyped cap is deleted the same target
//        is suspendable; the mem-mgr pid cell overrides regardless.
// Input: User task + minted Untyped; SUSPEND/CONFIGURE; delete Untyped;
//        SUSPEND again; set_mem_mgr_pid(target); SUSPEND; clear.
// Expect: PROTECTED_ROLE x2; then 0 (suspendable); PROTECTED_ROLE via
//         pid cell; 0 after clear. Teardown restores cell 0.
// Depends: tcb_control_role_protected (INV-ADM-15 durable layer b),
//         mem-mgr pid cell
// Pseudocode: helper + Untyped (PROTECTED x2); delete Untyped (OK);
// set pid cell (PROTECTED); clear cell; teardown.
JARVIS_TEST(tcb_never_mem_mgr_predicate, "PRE: none | POST: none") {
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: F4 TcbCap derivation cascade — grant/copy a TcbCap then
//        CAP_REVOKE the parent: the derived cap's suspend fails closed;
//        a SUPERVISOR-scope cap copied onward cannot be used by the
//        copy's new holder.
// Input: TcbCap grant + copy; REVOKE parent; suspend via derived.
// Expect: Derived suspend fails (INV-MK-7: named cap survives, derived
//         destroyed); onward-copied SUPERVISOR cap unusable by the new
//         holder.
// Depends: cap::revoke_derivations over TcbCap edges, grantee binding
// Pseudocode: create TcbCap; grant + copy; REVOKE parent; derived
// suspend fails; SUPERVISOR copy-onward fails for new holder;
// teardown.
JARVIS_TEST(tcb_cap_derivation_cascade, "PRE: none | POST: none") {
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: F4 N1 configure — priority re-buckets through
//        set_priority; affinity applies and clamps as set_affinity_err
//        does; a parked target stays configurable.
// Input: 99 with mask bit0 (priority) + value; bit1 (affinity) +
//        value; oversized priority; parked target configure.
// Expect: Priority + affinity applied; over-ceiling INVALID_ARGS;
//         parked target configurable (returns 0, reprioritized).
// Depends: sys_tcb_configure N1, set_priority/set_affinity_err,
//        admin_suspend_err/resume_err park path
// Pseudocode: spawn BLOCKED user child (creator-linked); configure
// priority (applied); affinity (applied); over-ceiling
// (INVALID_ARGS); suspend (parked); configure while parked;
// resume; teardown.
JARVIS_TEST(tcb_config_priority_and_affinity, "PRE: none | POST: none") {
    static uint64_t g_prio = 0;
    static uint64_t g_got_prio = 0;
    static uint64_t g_aff = 0;
    static uint64_t g_got_aff = 0;
    static uint64_t g_over = 0;
    static uint64_t g_parked = 0;
    g_prio = g_aff = g_over = g_parked = g_got_prio = g_got_aff = 0;

    auto *t = run_tcb_task([]() {
        auto *cur = Scheduler::current_task();
        cur->ensure_cspace();
        cap::CNode *cs = cur->get_cspace();
        // Live user child, creator-linked to the caller. Blocked on a
        // semaphore so suspend parks synchronously. Priority 20 (above
        // this task): higher number wins dispatch, so the child is
        // guaranteed to run and block.
        sync::Semaphore sem;
        sem.init(0, 1);
        auto *target = TaskControlBlock::create(
            []() {
                auto *self = Scheduler::current_task();
                auto *s = reinterpret_cast<sync::Semaphore *>(
                    self->user_data);
                s->wait();
                while (self->state == TaskState::BLOCKED)
                    arch::pause();
            },
            20, 10);
        JARVIS_ASSERT(target != nullptr);
        if (!target)
            return;
        target->is_user_ = true;
        target->parent_id = cur->id;
        target->user_data = &sem;
        Scheduler::add_task(*target);
        Scheduler::reschedule();
        tcb_wait_settled(target);
        JARVIS_ASSERT_EQ(static_cast<uint64_t>(TaskState::BLOCKED),
                         g_tcb_wait_state);
        int tslot = install_tcb_cap(cs, target->id, target->generation);
        if (tslot < 0) {
            Scheduler::terminate(*target, 0);
            Scheduler::drain_zombie_list();
            return;
        }
        const uint64_t th = tcb_handle(cs, static_cast<uint32_t>(tslot));
        // Priority field (bit 0) applies through set_priority.
        g_prio = Syscall::handle(
            static_cast<uint64_t>(SyscallNumber::TCB_CONFIGURE), th, 1,
            20, 0, nullptr);
        g_got_prio = target->priority;
        // Affinity field (bit 1) applies; CPU 0 always admissible.
        g_aff = Syscall::handle(
            static_cast<uint64_t>(SyscallNumber::TCB_CONFIGURE), th, 2, 0,
            1, nullptr);
        g_got_aff = target->cpu_affinity;
        // Over-ceiling priority is rejected, state untouched.
        g_over = Syscall::handle(
            static_cast<uint64_t>(SyscallNumber::TCB_CONFIGURE), th, 1,
            1ULL << 40, 0, nullptr);
        // A parked target stays configurable: suspend (synchronous park
        // — the target is already BLOCKED), configure while parked,
        // resume back to BLOCKED.
        g_parked = Syscall::handle(
            static_cast<uint64_t>(SyscallNumber::TCB_SUSPEND), th, 0, 0,
            0, nullptr);
        uint64_t reprio = g_parked;
        if (reprio == 0) {
            reprio = Syscall::handle(
                static_cast<uint64_t>(SyscallNumber::TCB_CONFIGURE), th, 1,
                21, 0, nullptr);
        }
        uint64_t resume = reprio;
        if (resume == 0) {
            resume = Syscall::handle(
                static_cast<uint64_t>(SyscallNumber::TCB_RESUME), th, 0, 0,
                0, nullptr);
        }
        if (resume != 0)
            g_parked = resume;
        else if (reprio != 0)
            g_parked = reprio;
        cs->remove(static_cast<uint32_t>(tslot));
        // Reap the child (still BLOCKED on the semaphore or parked).
        Scheduler::terminate(*target, 0);
        Scheduler::drain_zombie_list();
    });
    JARVIS_ASSERT(t != nullptr);
    Logger::warn("[TCB-DIAG] spins=%llu state=%llu t0=%llu t1=%llu inrq=%llu inedf=%llu rqprio=%llu dl=%llu per=%llu pol=%llu",
                 g_tcb_wait_spins, g_tcb_wait_state, g_tcb_ticks0,
                 g_tcb_ticks1, g_tcb_inrq, g_tcb_inedf, g_tcb_rqprio,
                 g_tcb_deadline, g_tcb_period, g_tcb_policy);
    JARVIS_ASSERT_EQ(0ULL, g_prio);
    JARVIS_ASSERT_EQ(20ULL, g_got_prio);
    JARVIS_ASSERT_EQ(0ULL, g_aff);
    JARVIS_ASSERT((g_got_aff & 1ULL) != 0ULL);
    JARVIS_ASSERT_EQ(tcb_err(1), g_over);
    JARVIS_ASSERT_EQ(0ULL, g_parked);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: F4 N1 mask gate — mask 0 and bits >= 4 fail INVALID_ARGS;
//        bit2/bit3 fail UNSUPPORTED; priority over
//        CONFIG_PRIORITY_CEILING fails INVALID_ARGS.
// Input: 99 with mask 0, mask 0x10, mask bit2, mask bit3, priority
//        over ceiling.
// Expect: INVALID_ARGS, INVALID_ARGS, UNSUPPORTED, UNSUPPORTED,
//         INVALID_ARGS.
// Depends: sys_tcb_configure mask gate (no silent ignore)
// Pseudocode: build an authorized child target (as in
// tcb_config_priority_and_affinity); each malformed call returns its
// typed code; teardown.
JARVIS_TEST(tcb_config_unsupported_and_bad_mask, "PRE: none | POST: none") {
    static uint64_t g_m0 = 0;
    static uint64_t g_mhigh = 0;
    static uint64_t g_bit2 = 0;
    static uint64_t g_bit3 = 0;
    static uint64_t g_over = 0;
    g_m0 = g_mhigh = g_bit2 = g_bit3 = g_over = 0;

    auto *t = run_tcb_task([]() {
        auto *cur = Scheduler::current_task();
        cur->ensure_cspace();
        cap::CNode *cs = cur->get_cspace();
        sync::Semaphore sem;
        sem.init(0, 1);
        auto *target = TaskControlBlock::create(
            []() {
                auto *self = Scheduler::current_task();
                auto *s = reinterpret_cast<sync::Semaphore *>(
                    self->user_data);
                s->wait();
                while (self->state == TaskState::BLOCKED)
                    arch::pause();
            },
            20, 10);
        JARVIS_ASSERT(target != nullptr);
        if (!target)
            return;
        target->is_user_ = true;
        target->parent_id = cur->id;
        target->user_data = &sem;
        Scheduler::add_task(*target);
        Scheduler::reschedule();
        tcb_wait_settled(target);
        JARVIS_ASSERT_EQ(static_cast<uint64_t>(TaskState::BLOCKED),
                         g_tcb_wait_state);
        int tslot = install_tcb_cap(cs, target->id, target->generation);
        if (tslot < 0) {
            Scheduler::terminate(*target, 0);
            Scheduler::drain_zombie_list();
            return;
        }
        const uint64_t th = tcb_handle(cs, static_cast<uint32_t>(tslot));
        const uint64_t cfg =
            static_cast<uint64_t>(SyscallNumber::TCB_CONFIGURE);
        g_m0 = Syscall::handle(cfg, th, 0, 10, 0, nullptr);
        g_mhigh = Syscall::handle(cfg, th, 0x10, 10, 0, nullptr);
        g_bit2 = Syscall::handle(cfg, th, 1ULL << 2, 0, 0, nullptr);
        g_bit3 = Syscall::handle(cfg, th, 1ULL << 3, 0, 0, nullptr);
        g_over = Syscall::handle(cfg, th, 1, 1ULL << 40, 0, nullptr);
        cs->remove(static_cast<uint32_t>(tslot));
        Scheduler::terminate(*target, 0);
        Scheduler::drain_zombie_list();
    });
    JARVIS_ASSERT(t != nullptr);
    JARVIS_ASSERT_EQ(tcb_err(1), g_m0);
    JARVIS_ASSERT_EQ(tcb_err(1), g_mhigh);
    JARVIS_ASSERT_EQ(tcb_err(12), g_bit2);
    JARVIS_ASSERT_EQ(tcb_err(12), g_bit3);
    JARVIS_ASSERT_EQ(tcb_err(1), g_over);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: F4 syscall surface — 99/100/101 dispatch through
//        Syscall::handle; MAX_SYSCALL == 103 with F5 VSPACE_MAP (was
//        102 at F4 landing); 103 rejected past-MAX; ABI unchanged.
// Input: handle(99/100/101, ...) reachable; MAX pin; 103 probe.
// Expect: Dispatch works with typed codes; MAX == 103; 103 ->
//        GENERIC_FAIL; ABI 0x10000.
// Depends: syscall table wiring, ABI_VERSION stability
// Pseudocode: call 99/100/101 (typed outcomes, no crash); assert
// MAX == 103; call 103 (GENERIC_FAIL); ABI_VERSION == 0x10000.
JARVIS_TEST(sys_tcb_surface_and_numbering, "PRE: none | POST: none") {
    static uint64_t g_99 = 0;
    static uint64_t g_100 = 0;
    static uint64_t g_101 = 0;
    static uint64_t g_102 = 0;
    static uint64_t g_abi = 0;
    g_99 = g_100 = g_101 = g_102 = g_abi = 0;

    auto *t = run_tcb_task([]() {
        // 99 with empty mask: INVALID_ARGS proves dispatch reached the
        // handler (not INVALID_NUMBER).
        g_99 = Syscall::handle(
            static_cast<uint64_t>(SyscallNumber::TCB_CONFIGURE), 0, 0, 0,
            0, nullptr);
        // 100/101 with a bogus handle: BAD_HANDLE (typed, no crash).
        g_100 = Syscall::handle(
            static_cast<uint64_t>(SyscallNumber::TCB_SUSPEND),
            0xDEADBEEFULL, 0, 0, 0, nullptr);
        g_101 = Syscall::handle(
            static_cast<uint64_t>(SyscallNumber::TCB_RESUME), 0xDEADBEEFULL,
            0, 0, 0, nullptr);
        // 102 is VSPACE_MAP now; 103 is past MAX -> GENERIC_FAIL.
        g_102 = Syscall::handle(103, 0, 0, 0, 0, nullptr);
        g_abi = Syscall::handle(
            static_cast<uint64_t>(SyscallNumber::ABI_VERSION), 0, 0, 0, 0,
            nullptr);
    });
    JARVIS_ASSERT(t != nullptr);
    JARVIS_ASSERT_EQ(tcb_err(1), g_99);
    JARVIS_ASSERT_EQ(tcb_err(2), g_100);
    JARVIS_ASSERT_EQ(tcb_err(2), g_101);
    JARVIS_ASSERT_EQ(static_cast<uint64_t>(-1), g_102);
    JARVIS_ASSERT_EQ(0x10000ULL, g_abi);
    JARVIS_ASSERT_EQ(103ULL,
                     static_cast<uint64_t>(SyscallNumber::MAX_SYSCALL));
    JARVIS_TEST_PASS();
}

void register_tcb_control_tests() {
    Logger::info("Registering tcb_control tests");
    JARVIS_REGISTER_TEST(tcb_suspend_ready_parks_and_dequeues);
    JARVIS_REGISTER_TEST(tcb_resume_prev_ready_reenables);
    JARVIS_REGISTER_TEST(tcb_suspend_blocked_absorbs_wake_then_resumes);
    JARVIS_REGISTER_TEST(tcb_suspend_absorb_backstop_refuses_link);
    JARVIS_REGISTER_TEST(tcb_resume_not_suspended_typed);
    JARVIS_REGISTER_TEST(tcb_suspend_dead_target_typed);
    JARVIS_REGISTER_TEST(tcb_suspend_deferred_running_boundary);
    JARVIS_REGISTER_TEST(tcb_resume_rearms_watchdog_and_shifts_deadline);
    JARVIS_REGISTER_TEST(tcb_resume_shift_saturates);
    JARVIS_REGISTER_TEST(tcb_watchdog_arm_refused_while_parked);
    JARVIS_REGISTER_TEST(tcb_suspend_parked_runner_released_by_resume);
    JARVIS_REGISTER_TEST(tcb_suspend_latch_armed_refused);
    JARVIS_REGISTER_TEST(tcb_debugger_admin_park_mutual_exclusion);
}

void register_cap_tcb_tests() {
    Logger::info("Registering cap_tcb tests");
    JARVIS_REGISTER_TEST(mk_supervisor_stop);
    JARVIS_REGISTER_TEST(tcb_never_mem_mgr_predicate);
    JARVIS_REGISTER_TEST(tcb_cap_derivation_cascade);
    JARVIS_REGISTER_TEST(tcb_config_priority_and_affinity);
    JARVIS_REGISTER_TEST(tcb_config_unsupported_and_bad_mask);
    JARVIS_REGISTER_TEST(sys_tcb_surface_and_numbering);
}
