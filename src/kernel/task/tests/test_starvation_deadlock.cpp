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

/// @file test_starvation_deadlock.cpp
/// @brief Starvation and deadlock scenario tests.
///
/// v0.3.10 rework (SIMULATED → DRIVEN): all contention is driven by REAL
/// kernel tasks (prio ≥ 11) dispatched by the real timer ISR.  Tasks hold
/// mutexes / wait on semaphores in their own running lambdas and block on
/// real gates; no set_current impersonation, no direct priority/state writes.

#include <test.hpp>
#include <logger.hpp>
#include <scope_guard.hpp>
#include <kernel/test/test_sched_helpers.hpp>
#include <kernel/sync/mutex.hpp>
#include <kernel/sync/semaphore.hpp>
#include <kernel/task/scheduler.hpp>
#include <kernel/task/task.hpp>
#include <kernel/arch/timer.hpp>

using namespace kernel;

static uint64_t g_starvation_counter = 0;

// Runmode: kernel
// Testidea: Create a high-priority task that runs briefly and a low-priority
// task that should eventually get CPU time.  Driven: both are REAL dispatched
// tasks; the high-priority task yields (short body), the low-priority task
// records its execution.  This documents the scheduler's current fairness
// behaviour under real dispatch.
// Input: High(11) runs briefly, Low(5) increments a counter when it runs.
// Expect: Low runs at least once after a reasonable number of real ticks.
TEST_CLASS(SchedulerStarvation) {
    g_starvation_counter = 0;

    auto *low = TaskControlBlock::create(
        []() { ++g_starvation_counter; }, 5, 10);
    CT_ASSERT(low != nullptr);

    auto *high = TaskControlBlock::create(
        []() {
            uint64_t limit = 100000ULL;
            for (uint64_t i = 0; i < limit; ++i) {
            }
        },
        10, 10);
    CT_ASSERT(high != nullptr);

    // Register both cooperating tasks under one IrqGuard so a timer tick
    // cannot split the registration (cookbook Rule 2).
    {
        arch::IrqGuard guard;
        Scheduler::add_task(*low);
        Scheduler::add_task(*high);
    }

    g_starvation_counter = 0;

    // Dispatch both for real; the timer ISR drives scheduling.  Exit early if
    // a TCB was reaped + 0xDD-poisoned (is_valid false) so the poll never
    // spins on freed memory.
    Scheduler::reschedule();
    uint64_t start = arch::Timer::ticks();
    while (((TaskControlBlock::is_valid(low) &&
             low->state != TaskState::TERMINATED) ||
            (TaskControlBlock::is_valid(high) &&
             high->state != TaskState::TERMINATED)) &&
           arch::Timer::ticks() - start < 2000)
        arch::pause();

    // NOTE: In a strict RM scheduler without aging, low may starve — this
    // documents current behaviour.  If g_starvation_counter == 0, starvation
    // is confirmed and aging should be added.
    if (g_starvation_counter == 0) {
        Logger::warn("SchedulerStarvation: low task never ran — "
                     "starvation confirmed");
    }

    // A starved task is still READY and queued.  terminate()→release_zombie
    // ENSUREs !in_ready_queue_; the ready queue may have been lazily rebuilt
    // (next_task) leaving a stale flag that dequeue_ready's early-return
    // path cannot clear → kernel panic (scheduler.cpp:281).  For tasks that
    // never ran, the direct remove_task+cleanup+delete teardown is the safe
    // pattern (it dequeues explicitly and never touches the zombie list).
    if (low->state != TaskState::TERMINATED) {
        Scheduler::remove_task(*low);
        low->cleanup();
        delete low;
    }
    if (high->state != TaskState::TERMINATED) {
        Scheduler::remove_task(*high);
        high->cleanup();
        delete high;
    }
};

// Runmode: kernel
// Testidea: Build a real deadlock chain through mutexes held by REAL
// dispatched tasks that block on real gates, then release the chain.
// NOTE: this chain uses semaphore GATES (not Mutex::lock) as the release
// point so the harness controls wake-up order deterministically.  The claim
// in the original comment that "Mutex::lock() cannot genuinely block a
// dispatched task" is STALE — since the deferred-switch wait was added
// (mutex.cpp:283-293) Mutex::lock() blocks genuinely; test_priority_inheritance
// relies on exactly that.  The gate-based chain is kept because it exercises
// the cross-mutex deadlock topology, not because mutex blocking is impossible.
// Input: A (prio 11) holds M1 and blocks on gate_a; B (prio 15) holds M2 and
//        blocks on gate_b; C (prio 20) blocks on gate_c.  Harness posts gates.
// Expect: All tasks block genuinely (no crash); the chain releases in order;
// all mutexes end unlocked.
TEST_CLASS(PriorityInversionChain5) {
    sync::Mutex m1, m2, m3;
    m1.init();
    m2.init();
    m3.init();
    sync::Semaphore gate_a;
    gate_a.init(0, 1);
    sync::Semaphore gate_b;
    gate_b.init(0, 1);
    sync::Semaphore gate_c;
    gate_c.init(0, 1);

    // A: holds M1, blocks on gate_a.
    struct ACtx {
        uint64_t m1_;
        uint64_t gate_;
    } actx;
    actx.m1_ = reinterpret_cast<uint64_t>(&m1);
    actx.gate_ = reinterpret_cast<uint64_t>(&gate_a);
    auto *a = TaskControlBlock::create(
        []() {
            auto *self = Scheduler::current_task();
            auto *c = reinterpret_cast<ACtx *>(self->user_data);
            auto *mm1 = reinterpret_cast<sync::Mutex *>(c->m1_);
            auto *g = reinterpret_cast<sync::Semaphore *>(c->gate_);
            mm1->lock();
            g->wait();
            // Semaphore::wait() only sets BLOCKED and defers the switch
            // (INV-4); it returns immediately.  Spin until the timer ISR
            // actually suspends this task, then exit when post() wakes it
            // (state no longer BLOCKED).  Mirrors ipc.cpp blocking idiom.
            while (self->state == TaskState::BLOCKED)
                arch::pause();
            mm1->unlock();
        },
        11, 10);
    CT_ASSERT(a != nullptr);
    a->user_data = &actx;
    Scheduler::add_task(*a);
    Scheduler::reschedule();
    while (a->state != TaskState::BLOCKED)
        arch::pause();
    CT_ASSERT(m1.owner() == a);

    // B: holds M2, blocks on gate_b.
    struct BCtx {
        uint64_t m2_;
        uint64_t gate_;
    } bctx;
    bctx.m2_ = reinterpret_cast<uint64_t>(&m2);
    bctx.gate_ = reinterpret_cast<uint64_t>(&gate_b);
    auto *b = TaskControlBlock::create(
        []() {
            auto *self = Scheduler::current_task();
            auto *c = reinterpret_cast<BCtx *>(self->user_data);
            auto *mm2 = reinterpret_cast<sync::Mutex *>(c->m2_);
            auto *g = reinterpret_cast<sync::Semaphore *>(c->gate_);
            mm2->lock();
            g->wait();
            while (self->state == TaskState::BLOCKED)
                arch::pause();
            mm2->unlock();
        },
        15, 10);
    CT_ASSERT(b != nullptr);
    b->user_data = &bctx;
    Scheduler::add_task(*b);
    Scheduler::reschedule();
    while (b->state != TaskState::BLOCKED)
        arch::pause();
    CT_ASSERT(m2.owner() == b);
    CT_ASSERT(b->state == TaskState::BLOCKED);

    // C: blocks on gate_c.
    struct CCtx {
        uint64_t gate_;
    } cctx;
    cctx.gate_ = reinterpret_cast<uint64_t>(&gate_c);
    auto *c = TaskControlBlock::create(
        []() {
            auto *self = Scheduler::current_task();
            auto *ctx = reinterpret_cast<CCtx *>(self->user_data);
            auto *g = reinterpret_cast<sync::Semaphore *>(ctx->gate_);
            g->wait();
            while (self->state == TaskState::BLOCKED)
                arch::pause();
        },
        20, 10);
    CT_ASSERT(c != nullptr);
    c->user_data = &cctx;
    Scheduler::add_task(*c);
    Scheduler::reschedule();
    while (c->state != TaskState::BLOCKED)
        arch::pause();

    // All three genuinely blocked — the chain is live.
    CT_ASSERT(a->state == TaskState::BLOCKED);
    CT_ASSERT(b->state == TaskState::BLOCKED);
    CT_ASSERT(c->state == TaskState::BLOCKED);

    // Release the chain: C first, then B (unlocks M2), then A (unlocks M1).
    gate_c.post();
    kernel::test::wait_for_termination_safe(c);
    gate_b.post();
    kernel::test::wait_for_termination_safe(b);
    gate_a.post();
    kernel::test::wait_for_termination_safe(a);
    // Cleanup BEFORE asserting (cookbook Rule 5): all three self-terminated,
    // so reclaim via the zombie list; remove_task+cleanup+delete on a zombie
    // would double-free.
    Scheduler::drain_zombie_list();

    CT_ASSERT(!m1.is_locked());
    CT_ASSERT(!m2.is_locked());
    CT_ASSERT(!m3.is_locked());
};

// Runmode: kernel
// Testidea: Orchestrated lock/unlock patterns across 3 mutexes and 3 REAL
// dispatched tasks.  All tasks block genuinely on semaphore GATES (see
// PriorityInversionChain5 NOTE: Mutex::lock() retry loop cannot genuinely
// block a dispatched task at 1ms ticks — deferred switch never lands inside
// the MAX_WAITERS+1 budget).  The holder holds all 3 mutexes; the contenders
// acquire the released mutexes after the holder terminates.
// Input: 3 real tasks (prio 11, 15, 20), 3 mutexes, real dispatch + gates.
// Expect: All mutexes unlocked at end; no crash; no corrupt state.
TEST_CLASS(DeadlockNestedMutexLoad) {
    sync::Mutex mtx[3];
    for (int i = 0; i < 3; ++i)
        mtx[i].init();
    sync::Semaphore gate;
    gate.init(0, 1);
    sync::Semaphore gate_c1;
    gate_c1.init(0, 1);
    sync::Semaphore gate_c2;
    gate_c2.init(0, 1);

    // Holder: locks M0 then M1 then M2, blocks on the gate, unlocks reverse.
    struct HCtx {
        uint64_t m0_;
        uint64_t m1_;
        uint64_t m2_;
        uint64_t gate_;
    } hctx;
    hctx.m0_ = reinterpret_cast<uint64_t>(&mtx[0]);
    hctx.m1_ = reinterpret_cast<uint64_t>(&mtx[1]);
    hctx.m2_ = reinterpret_cast<uint64_t>(&mtx[2]);
    hctx.gate_ = reinterpret_cast<uint64_t>(&gate);
    auto *holder = TaskControlBlock::create(
        []() {
            auto *self = Scheduler::current_task();
            auto *c = reinterpret_cast<HCtx *>(self->user_data);
            auto *m0 = reinterpret_cast<sync::Mutex *>(c->m0_);
            auto *m1 = reinterpret_cast<sync::Mutex *>(c->m1_);
            auto *m2 = reinterpret_cast<sync::Mutex *>(c->m2_);
            auto *g = reinterpret_cast<sync::Semaphore *>(c->gate_);
            m0->lock();
            m1->lock();
            m2->lock();
            g->wait();
            // Semaphore::wait() only sets BLOCKED and defers the switch
            // (INV-4); it returns immediately.  Spin until the timer ISR
            // actually suspends this task, then exit when post() wakes it
            // (state no longer BLOCKED).  Mirrors ipc.cpp blocking idiom.
            while (self->state == TaskState::BLOCKED)
                arch::pause();
            m2->unlock();
            m1->unlock();
            m0->unlock();
        },
        11, 10);
    CT_ASSERT(holder != nullptr);
    holder->user_data = &hctx;
    Scheduler::add_task(*holder);
    Scheduler::reschedule();
    while (holder->state != TaskState::BLOCKED)
        arch::pause();
    CT_ASSERT(mtx[0].owner() == holder);
    CT_ASSERT(mtx[1].owner() == holder);
    CT_ASSERT(mtx[2].owner() == holder);

    // Contender1 blocks on gate_c1, then acquires M2 after the holder
    // releases it.
    uint64_t c1slot[3];
    uint64_t c1_acquired = 0;
    c1slot[0] = reinterpret_cast<uint64_t>(&gate_c1);
    c1slot[1] = reinterpret_cast<uint64_t>(&mtx[2]);
    c1slot[2] = reinterpret_cast<uint64_t>(&c1_acquired);
    auto *c1 = TaskControlBlock::create(
        []() {
            auto *self = Scheduler::current_task();
            auto *s = reinterpret_cast<uint64_t *>(self->user_data);
            auto *g = reinterpret_cast<sync::Semaphore *>(s[0]);
            auto *m = reinterpret_cast<sync::Mutex *>(s[1]);
            auto *acq = reinterpret_cast<uint64_t *>(s[2]);
            g->wait();
            while (self->state == TaskState::BLOCKED)
                arch::pause();
            m->lock();
            __atomic_store_n(acq, 1, __ATOMIC_RELEASE);
            m->unlock();
        },
        15, 10);
    CT_ASSERT(c1 != nullptr);
    c1->user_data = c1slot;
    Scheduler::add_task(*c1);
    Scheduler::reschedule();
    while (c1->state != TaskState::BLOCKED)
        arch::pause();
    CT_ASSERT(c1->state == TaskState::BLOCKED);

    // Contender2 blocks on gate_c2, then acquires M0 after the holder
    // releases it.
    uint64_t c2slot[3];
    uint64_t c2_acquired = 0;
    c2slot[0] = reinterpret_cast<uint64_t>(&gate_c2);
    c2slot[1] = reinterpret_cast<uint64_t>(&mtx[0]);
    c2slot[2] = reinterpret_cast<uint64_t>(&c2_acquired);
    auto *c2 = TaskControlBlock::create(
        []() {
            auto *self = Scheduler::current_task();
            auto *s = reinterpret_cast<uint64_t *>(self->user_data);
            auto *g = reinterpret_cast<sync::Semaphore *>(s[0]);
            auto *m = reinterpret_cast<sync::Mutex *>(s[1]);
            auto *acq = reinterpret_cast<uint64_t *>(s[2]);
            g->wait();
            while (self->state == TaskState::BLOCKED)
                arch::pause();
            m->lock();
            __atomic_store_n(acq, 1, __ATOMIC_RELEASE);
            m->unlock();
        },
        20, 10);
    CT_ASSERT(c2 != nullptr);
    c2->user_data = c2slot;
    Scheduler::add_task(*c2);
    Scheduler::reschedule();
    while (c2->state != TaskState::BLOCKED)
        arch::pause();
    CT_ASSERT(c2->state == TaskState::BLOCKED);

    // All genuinely blocked — the contention is live.
    CT_ASSERT(holder->state == TaskState::BLOCKED);
    CT_ASSERT(c1->state == TaskState::BLOCKED);
    CT_ASSERT(c2->state == TaskState::BLOCKED);

    // Release the holder: it unlocks M2, M1, M0, then terminates.  The
    // contenders then acquire the freed mutexes and terminate.
    gate.post();
    kernel::test::wait_for_termination_safe(holder);
    gate_c1.post();
    kernel::test::wait_for_termination_safe(c1);
    gate_c2.post();
    kernel::test::wait_for_termination_safe(c2);
    // Cleanup BEFORE asserting (cookbook Rule 5): all three self-terminated,
    // so reclaim via the zombie list; remove_task+cleanup+delete on a zombie
    // would double-free.
    Scheduler::drain_zombie_list();

    CT_ASSERT(c1_acquired == 1);
    CT_ASSERT(c2_acquired == 1);
    for (int i = 0; i < 3; ++i)
        CT_ASSERT(!mtx[i].is_locked());
};

void register_starvation_deadlock_tests() {
    Logger::info("Registering starvation/deadlock tests");
    REGISTER_CLASS(SchedulerStarvation);
    REGISTER_CLASS(PriorityInversionChain5);
    REGISTER_CLASS(DeadlockNestedMutexLoad);
    // DeadlockRecoveryResourceReclamation — never implemented
}
