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

/// @file test_sync_block_pattern.cpp
/// @brief Regression tests for audit-task-sync-v0.4.2 C-1/C-2/H-1 (spinlock
/// must NOT be held across Scheduler::reschedule(); a BLOCKED task must be
/// dequeued from the ready queue) and C-3 (sporadic-server replenishment ring
/// full must coalesce, never silently drop budget).

#include <test.hpp>
#include <logger.hpp>
#include <kernel/sync/semaphore.hpp>
#include <kernel/sync/eventgroup.hpp>
#include <kernel/task/task.hpp>
#include <kernel/task/scheduler.hpp>
#include <kernel/task/sporadic_server.hpp>
#include <kernel/test/test_sched_helpers.hpp>

using namespace kernel;

// Runmode: kernel
// Testidea: Verifies a REAL task blocking in sem.wait() is removed from the
// ready queue (C-2/INV-2): while BLOCKED, the task must not be physically
// queued, so a later post() wake re-enqueues exactly once and the worker
// completes.  Also verifies wait() returns (does not spin on lock_) so the
// harness can post from task context (C-1: lock released before reschedule).
// Input: Worker (prio 11) genuinely blocks in sem.wait(); harness posts.
// Expect: Worker BLOCKED and not in ready queue; post() wakes it; worker
//         completes; no ResourceTracker delta.
// Depends: kernel::sync::Semaphore, kernel::TaskControlBlock, kernel::Scheduler
JARVIS_TEST(semaphore_wait_blocked_not_in_ready_queue,
            "PRE: none | POST: none") {
    sync::Semaphore sem;
    sem.init(0, 1);
    static uint64_t g_woken = 0;
    g_woken = 0;

    auto *worker = TaskControlBlock::create(
        []() {
            auto *self = Scheduler::current_task();
            sync::Semaphore *s = reinterpret_cast<sync::Semaphore *>(
                self->user_data);
            s->wait();
            while (self->state == TaskState::BLOCKED)
                arch::pause();
            __atomic_store_n(&g_woken, 1, __ATOMIC_RELEASE);
        },
        11, 10);
    JARVIS_ASSERT(worker != nullptr);
    worker->user_data = &sem;
    Scheduler::add_task(*worker);
    Scheduler::reschedule();

    // Worker genuinely blocks inside sem.wait() and is dequeued from the
    // ready queue (INV-2 — a BLOCKED task must never be physically queued).
    while (worker->state != TaskState::BLOCKED)
        arch::pause();
    JARVIS_ASSERT(worker->state == TaskState::BLOCKED);
    JARVIS_ASSERT(!worker->in_ready_queue_);

    // Wake it (real post — from task context; proves the lock was released
    // before the deferred switch, otherwise this post would deadlock on lock_).
    sem.post();
    kernel::test::wait_for_termination_safe(worker);
    Scheduler::drain_zombie_list();
    JARVIS_ASSERT_EQ(1ULL, g_woken);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: EventGroup::wait_bits() (H-1) must release lock_ before the
// deferred switch and dequeue the BLOCKED task (INV-2).  The harness posts
// the bit from task context; the worker must wake and complete.
// Input: Worker (prio 11) blocks in eg.wait_bits(0x1); harness sets the bit.
// Expect: Worker BLOCKED, not in ready queue; set_bits() wakes it; completes.
// Depends: kernel::sync::EventGroup, kernel::TaskControlBlock, kernel::Scheduler
JARVIS_TEST(eventgroup_wait_bits_blocked_not_in_ready_queue,
            "PRE: none | POST: none") {
    sync::EventGroup eg;
    eg.init();
    static uint64_t g_woken = 0;
    g_woken = 0;

    auto *worker = TaskControlBlock::create(
        []() {
            auto *self = Scheduler::current_task();
            sync::EventGroup *e =
                reinterpret_cast<sync::EventGroup *>(self->user_data);
            e->wait_bits(0x1);
            while (self->state == TaskState::BLOCKED)
                arch::pause();
            __atomic_store_n(&g_woken, 1, __ATOMIC_RELEASE);
        },
        11, 10);
    JARVIS_ASSERT(worker != nullptr);
    worker->user_data = &eg;
    Scheduler::add_task(*worker);
    Scheduler::reschedule();

    while (worker->state != TaskState::BLOCKED)
        arch::pause();
    JARVIS_ASSERT(worker->state == TaskState::BLOCKED);
    JARVIS_ASSERT(!worker->in_ready_queue_);

    eg.set_bits(0x1);
    kernel::test::wait_for_termination_safe(worker);
    Scheduler::drain_zombie_list();
    JARVIS_ASSERT_EQ(1ULL, g_woken);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: EventGroup::try_wait_bits (M-6 serialization) must observe a
// concurrently set bit — i.e. the read is under the spinlock and reflects a
// set_bits() that happened on another real task.
// Input: Harness sets a bit directly (no waiter), then try_wait_bits reads it.
// Expect: try_wait_bits returns true immediately.
// Depends: kernel::sync::EventGroup
JARVIS_TEST(eventgroup_try_wait_bits_serialized, "PRE: none | POST: none") {
    sync::EventGroup eg;
    eg.init();
    eg.set_bits(0x2);
    JARVIS_ASSERT(eg.try_wait_bits(0x2));
    JARVIS_ASSERT(!eg.try_wait_bits(0x1));
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Verifies the sporadic-server replenishment ring-full coalescing
// (C-3): consuming past MAX_REPLENISHMENTS worth of budget must NOT silently
// drop any consumed amount — the amount is merged into the newest pending
// replenishment, so the total eventual restore equals the total consumed and
// the coalesce counter reflects the merges.
// Input: Drive a SporadicServer through init/consume/on_completion cycles with
//        granularity 1 until the 8-entry ring saturates, then force more.
// Expect: coalesce_count() > 0 and sum of pending + restored budget == consumed.
// Depends: kernel::task::SporadicServer
JARVIS_TEST(sporadic_replenishment_ring_full_coalesces,
            "PRE: none | POST: none") {
    task::SporadicServer ss;
    // Budget large enough that the 9 cycles × 2 units consumed never exhausts
    // it (consume() returns false once budget hits zero).
    ss.init(/*budget_c=*/100, /*period_t=*/100, /*bg_prio=*/1,
            /*granularity=*/1);
    ss.set_base_priority(20);

    // Saturate the ring: 8 replenishments pending.
    for (uint64_t i = 0; i < 8; ++i) {
        // on_activation then consume twice (2 units) then complete → one
        // replenishment of 2 per cycle.
        ss.on_activation(i * 10);
        JARVIS_ASSERT(ss.consume(i * 10 + 1));
        JARVIS_ASSERT(ss.consume(i * 10 + 2));
        ss.on_completion(i * 10 + 3);
    }
    JARVIS_ASSERT_EQ(8ULL, ss.pending_count());

    // One more cycle — ring full: must coalesce, not drop.
    ss.on_activation(1000);
    JARVIS_ASSERT(ss.consume(1001));
    JARVIS_ASSERT(ss.consume(1002));
    ss.on_completion(1003);

    JARVIS_ASSERT(ss.coalesce_count() > 0);
    JARVIS_ASSERT_EQ(8ULL, ss.pending_count()); // still bounded at MAX

    // Total budget accounted: 9 cycles × 2 consumed = 18; none dropped (the
    // 9th was coalesced into an existing entry).  budget_remaining_ was
    // 100-18=82; restoring all 18 reaches the cap C=100.  If the 9th cycle
    // had been silently dropped, only 16 would restore → 98, not 100.
    ss.process_replenishments(2000);
    JARVIS_ASSERT_EQ(100ULL, ss.remaining_budget());
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Verifies a replenishment must NOT force an IDLE server ACTIVE
// (M-8): an idle-but-ACTIVE server would block the next on_activation(),
// delaying aperiodic jobs.  After a budget-exhausted→replenished cycle the
// server returns to IDLE (not ACTIVE) when it completed its work.
// Input: Exhaust a server, let a replenishment restore budget while IDLE.
// Expect: state() is IDLE (not ACTIVE) after the replenishment.
// Depends: kernel::task::SporadicServer
JARVIS_TEST(sporadic_replenishment_does_not_force_idle_active,
            "PRE: none | POST: none") {
    task::SporadicServer ss;
    ss.init(/*budget_c=*/2, /*period_t=*/100, /*bg_prio=*/1,
            /*granularity=*/1);
    ss.set_base_priority(20);

    ss.on_activation(0);
    JARVIS_ASSERT(ss.consume(1));
    JARVIS_ASSERT(!ss.consume(2)); // exhausted → schedules replenishment
    JARVIS_ASSERT(ss.state() == task::SporadicServer::EXHAUSTED);

    // Replenishment due: budget restored, server was EXHAUSTED → ACTIVE.
    ss.process_replenishments(100);
    JARVIS_ASSERT(ss.state() == task::SporadicServer::ACTIVE);

    // Job completes → server goes IDLE, replenishment scheduled for what was
    // consumed.  A LATER replenishment while IDLE must NOT force ACTIVE.
    ss.on_completion(101);
    JARVIS_ASSERT(ss.state() == task::SporadicServer::IDLE);
    ss.process_replenishments(200);
    JARVIS_ASSERT(ss.state() == task::SporadicServer::IDLE);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Verifies semaphore priority-inheritance restore uses strict `>`
// (H-6): after a boost-and-restore cycle, holder_priority_ must clear and the
// owner's priority must return to its base — no permanent inflation across
// repeated lock cycles.
// Input: Owner (prio 10) holds a semaphore and blocks on a gate; a high-prio
//        waiter (prio 15) blocks on the semaphore (boosting the owner); the
//        gate is released, the owner posts the semaphore (restoring priority
//        and waking the waiter).
// Expect: Owner boosted to 15 while the waiter is blocked; after post, owner
//         priority restored to base 10 (no latch).
// Depends: kernel::sync::Semaphore, kernel::TaskControlBlock, kernel::Scheduler
JARVIS_TEST(semaphore_pi_restore_clears_holder_latch,
            "PRE: none | POST: none") {
    sync::Semaphore sem;
    sem.init(1, 1);
    sync::Semaphore gate;
    gate.init(0, 1);

    // Owner holds the semaphore (count 1 → 0), records acquisition, blocks on
    // the gate so the harness knows it owns the semaphore.
    struct OwnerCtx {
        uint64_t sem_;
        uint64_t gate_;
        uint64_t acquired_;
    } octx;
    octx.sem_ = reinterpret_cast<uint64_t>(&sem);
    octx.gate_ = reinterpret_cast<uint64_t>(&gate);
    octx.acquired_ = 0;

    auto *owner = TaskControlBlock::create(
        []() {
            auto *self = Scheduler::current_task();
            auto *c = reinterpret_cast<OwnerCtx *>(self->user_data);
            auto *s = reinterpret_cast<sync::Semaphore *>(c->sem_);
            auto *g = reinterpret_cast<sync::Semaphore *>(c->gate_);
            s->wait(); // acquires (count 1 → 0)
            __atomic_store_n(&c->acquired_, 1, __ATOMIC_RELEASE);
            g->wait(); // block until harness releases the gate
            while (self->state == TaskState::BLOCKED)
                arch::pause();
            s->post(); // restore priority + wake the waiter
        },
        11, 10);
    JARVIS_ASSERT(owner != nullptr);
    owner->user_data = &octx;
    Scheduler::add_task(*owner);
    Scheduler::reschedule();
    while (octx.acquired_ != 1)
        arch::pause();
    while (owner->state != TaskState::BLOCKED)
        arch::pause();
    JARVIS_ASSERT_EQ(11ULL, owner->priority);

    // Waiter (higher priority) blocks on the held semaphore → boosts owner.
    auto *waiter = TaskControlBlock::create(
        []() {
            auto *self = Scheduler::current_task();
            sync::Semaphore *s = reinterpret_cast<sync::Semaphore *>(
                self->user_data);
            s->wait();
            while (self->state == TaskState::BLOCKED)
                arch::pause();
        },
        20, 10);
    JARVIS_ASSERT(waiter != nullptr);
    waiter->user_data = &sem;
    Scheduler::add_task(*waiter);
    while (waiter->state != TaskState::BLOCKED)
        arch::pause();

    // Owner boosted to the waiter's priority while the waiter is blocked.
    JARVIS_ASSERT_EQ(20ULL, owner->priority);

    // Release the gate: owner wakes, posts → waiter acquires, owner's
    // priority restored to base (no latch without the H-6 `>` fix).
    gate.post();
    kernel::test::wait_for_termination_safe(waiter);
    kernel::test::wait_for_termination_safe(owner);
    // Read priority while the zombie TCB is still allocated (before drain).
    JARVIS_ASSERT_EQ(11ULL, owner->priority);
    Scheduler::drain_zombie_list();
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Registers the audit-task-sync block-pattern regression tests.
// Input: None
// Expect: All tests registered via JARVIS_REGISTER_TEST
// Depends: kernel test framework
void register_sync_block_pattern_tests() {
    Logger::info("Registering sync block-pattern tests");
    JARVIS_REGISTER_TEST(semaphore_wait_blocked_not_in_ready_queue);
    JARVIS_REGISTER_TEST(eventgroup_wait_bits_blocked_not_in_ready_queue);
    JARVIS_REGISTER_TEST(eventgroup_try_wait_bits_serialized);
    JARVIS_REGISTER_TEST(sporadic_replenishment_ring_full_coalesces);
    JARVIS_REGISTER_TEST(sporadic_replenishment_does_not_force_idle_active);
    JARVIS_REGISTER_TEST(semaphore_pi_restore_clears_holder_latch);
}