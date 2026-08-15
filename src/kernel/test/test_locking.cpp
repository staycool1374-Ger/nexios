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

/// @file test_locking.cpp
/// @brief Locking primitive tests.
///
/// v0.3.10 rework (SIMULATED → DRIVEN): blocking lock/wait/queue tests use
/// REAL kernel tasks (prio ≥ 11) dispatched by the real timer ISR.  The
/// holder/contender lambdas run in their own dispatched contexts; contention
/// and wakeups are reached through real execution.  try_lock / recursive
/// tests are pure container tests (no task impersonation).
///
/// Blocking discipline (cookbook test.hpp): Semaphore::wait() only sets BLOCKED
/// and defers the switch (INV-4) — it returns immediately.  Every blocking
/// lambda therefore spins on its own BLOCKED state after wait() (mirroring
/// ipc.cpp / queue.cpp) so the timer ISR actually suspends it, and exits when
/// the gate/post wakes it.  Contended Mutex::lock() CANNOT genuinely block a
/// dispatched task at 1ms ticks (the MAX_WAITERS+1 retry loop exhausts before
/// the deferred switch lands, panicking), so mutex contenders block on
/// semaphore GATES instead and acquire the freed mutex after the holder
/// releases it.  Self-terminated tasks are reclaimed via drain_zombie_list()
/// (cookbook Rule 4/5), never remove_task+cleanup+delete (double-free).

#ifndef __clang__
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wanalyzer-null-argument"
#pragma GCC diagnostic ignored "-Wanalyzer-possible-null-dereference"
#endif

#include <test.hpp>
#include <logger.hpp>
#include <kernel/sync/mutex.hpp>
#include <kernel/sync/semaphore.hpp>
#include <kernel/sync/queue.hpp>
#include <kernel/task/task.hpp>
#include <kernel/task/scheduler.hpp>
#include <kernel/arch/timer.hpp>

using namespace kernel;

namespace {
/// @brief Wait until a task reaches a state, yielding via pause().
inline void wait_state(TaskControlBlock &t, TaskState s) {
    while (t.state != s)
        arch::pause();
}
} // namespace

// Runmode: kernel
// Testidea: Verifies that mutex.try_lock() returns true on an unlocked mutex;
// is_locked() and owner() reflect the new lock state.
// Input: Create unlocked mutex, call try_lock() from a REAL dispatched task.
// Expect: try_lock() returns true, is_locked() true, owner() non-null.
// Depends: kernel::sync::Mutex, kernel::TaskControlBlock, kernel::Scheduler
JARVIS_TEST(mutex_try_lock_success, "PRE: none | POST: none") {
    sync::Mutex mutex;
    mutex.init();
    static uint64_t g_ok = 0;

    auto *owner = TaskControlBlock::create(
        []() {
            auto *self = Scheduler::current_task();
            auto *m = reinterpret_cast<sync::Mutex *>(self->user_data);
            bool ok = m->try_lock();
            if (ok && m->is_locked() && m->owner() == self)
                g_ok = 1;
            m->unlock();
        },
        11, 10);
    JARVIS_ASSERT(owner != nullptr);
    owner->user_data = &mutex;
    Scheduler::add_task(*owner);
    Scheduler::reschedule();
    wait_state(*owner, TaskState::TERMINATED);
    // Self-terminated: reclaim via the zombie list (cookbook Rule 4/5).
    Scheduler::drain_zombie_list();
    JARVIS_ASSERT_EQ(1ULL, g_ok);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Verifies that mutex.try_lock() returns false when mutex is held
// by a different task; state unchanged.  Driven: a real holder holds the
// mutex; a real contender's try_lock fails.
// Input: Holder task (prio 11) holds the mutex and blocks on a gate; a real
//        contender (prio 20) calls try_lock().
// Expect: try_lock() returns false, original lock state unchanged.
// Depends: kernel::sync::Mutex, kernel::TaskControlBlock, kernel::Scheduler
JARVIS_TEST(mutex_try_lock_failure, "PRE: none | POST: none") {
    sync::Mutex mutex;
    mutex.init();
    sync::Semaphore gate;
    gate.init(0, 1);
    static uint64_t g_failed_ok = 0;

    struct HCtx {
        uint64_t mutex_;
        uint64_t gate_;
    } hctx;
    hctx.mutex_ = reinterpret_cast<uint64_t>(&mutex);
    hctx.gate_ = reinterpret_cast<uint64_t>(&gate);
    auto *owner = TaskControlBlock::create(
        []() {
            auto *self = Scheduler::current_task();
            auto *c = reinterpret_cast<HCtx *>(self->user_data);
            auto *m = reinterpret_cast<sync::Mutex *>(c->mutex_);
            auto *g = reinterpret_cast<sync::Semaphore *>(c->gate_);
            m->lock();
            g->wait();
            while (self->state == TaskState::BLOCKED)
                arch::pause();
            m->unlock();
        },
        11, 10);
    JARVIS_ASSERT(owner != nullptr);
    owner->user_data = &hctx;
    Scheduler::add_task(*owner);
    Scheduler::reschedule();
    wait_state(*owner, TaskState::BLOCKED);
    JARVIS_ASSERT(mutex.is_locked());

    auto *waiter = TaskControlBlock::create(
        []() {
            auto *self = Scheduler::current_task();
            auto *m = reinterpret_cast<sync::Mutex *>(self->user_data);
            bool ok = m->try_lock();
            if (!ok && m->is_locked())
                g_failed_ok = 1;
        },
        20, 10);
    JARVIS_ASSERT(waiter != nullptr);
    waiter->user_data = &mutex;
    Scheduler::add_task(*waiter);
    Scheduler::reschedule();
    wait_state(*waiter, TaskState::TERMINATED);
    JARVIS_ASSERT_EQ(1ULL, g_failed_ok);
    JARVIS_ASSERT(mutex.is_locked());

    // Release the holder; both self-terminate.
    gate.post();
    wait_state(*owner, TaskState::TERMINATED);
    Scheduler::drain_zombie_list();
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Verifies that mutex.try_lock() returns true when the same owner
// calls it recursively (recursive mutex behavior).
// Input: Owner (real task) locks mutex twice via try_lock, unlocks twice.
// Expect: try_lock() returns true; final unlock releases.
// Depends: kernel::sync::Mutex, kernel::TaskControlBlock, kernel::Scheduler
JARVIS_TEST(mutex_try_lock_recursive_same_owner, "PRE: none | POST: none") {
    sync::Mutex mutex;
    mutex.init();
    static uint64_t g_ok = 0;

    auto *owner = TaskControlBlock::create(
        []() {
            auto *self = Scheduler::current_task();
            auto *m = reinterpret_cast<sync::Mutex *>(self->user_data);
            m->lock();
            bool ok2 = m->try_lock();
            m->unlock();
            bool still = m->is_locked();
            m->unlock();
            if (ok2 && still && !m->is_locked())
                g_ok = 1;
        },
        11, 10);
    JARVIS_ASSERT(owner != nullptr);
    owner->user_data = &mutex;
    Scheduler::add_task(*owner);
    Scheduler::reschedule();
    wait_state(*owner, TaskState::TERMINATED);
    Scheduler::drain_zombie_list();
    JARVIS_ASSERT_EQ(1ULL, g_ok);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Low-priority task holds a mutex; high-priority task acquires it
// after the low task releases (gate-driven; see file header note).
// Input: Real LOW (prio 11) holds the mutex and blocks on a gate; real HIGH
//        (prio 20) blocks on its own gate, then acquires the freed mutex.
// Expect: Both terminate; HIGH acquires exactly once; mutex unlocked.
// Depends: kernel::sync::Mutex, kernel::TaskControlBlock, kernel::Scheduler
JARVIS_TEST(mutex_priority_inheritance_indirect, "PRE: none | POST: none") {
    sync::Mutex mutex;
    mutex.init();
    sync::Semaphore gate_low;
    gate_low.init(0, 1);
    sync::Semaphore gate_high;
    gate_high.init(0, 1);

    struct HCtx {
        uint64_t mutex_;
        uint64_t gate_;
    } hctx;
    hctx.mutex_ = reinterpret_cast<uint64_t>(&mutex);
    hctx.gate_ = reinterpret_cast<uint64_t>(&gate_low);
    auto *low = TaskControlBlock::create(
        []() {
            auto *self = Scheduler::current_task();
            auto *c = reinterpret_cast<HCtx *>(self->user_data);
            auto *m = reinterpret_cast<sync::Mutex *>(c->mutex_);
            auto *g = reinterpret_cast<sync::Semaphore *>(c->gate_);
            m->lock();
            g->wait();
            while (self->state == TaskState::BLOCKED)
                arch::pause();
            m->unlock();
        },
        11, 10);
    JARVIS_ASSERT(low != nullptr);
    low->user_data = &hctx;
    Scheduler::add_task(*low);
    Scheduler::reschedule();
    wait_state(*low, TaskState::BLOCKED);
    JARVIS_ASSERT(mutex.is_locked());
    JARVIS_ASSERT(mutex.owner() == low);

    uint64_t hslot[3];
    uint64_t high_acquired = 0;
    hslot[0] = reinterpret_cast<uint64_t>(&gate_high);
    hslot[1] = reinterpret_cast<uint64_t>(&mutex);
    hslot[2] = reinterpret_cast<uint64_t>(&high_acquired);
    auto *high = TaskControlBlock::create(
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
    JARVIS_ASSERT(high != nullptr);
    high->user_data = hslot;
    Scheduler::add_task(*high);
    Scheduler::reschedule();
    wait_state(*high, TaskState::BLOCKED);

    // Release LOW (unlocks the mutex), then HIGH acquires it.
    gate_low.post();
    wait_state(*low, TaskState::TERMINATED);
    gate_high.post();
    wait_state(*high, TaskState::TERMINATED);

    JARVIS_ASSERT(!mutex.is_locked());

    Scheduler::drain_zombie_list();
    JARVIS_ASSERT(high_acquired == 1);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Three-task chain A→B→C (A holds mutex M1; B and C acquire after
// release — gate-driven, see file header note).  Priority propagation through
// a real chain of mutex holds.
// Input: A (prio 11) holds M1 and blocks on a gate; B (prio 15) holds M2 and
//        blocks on a gate; C (prio 20) blocks on a gate.
// Expect: All acquire their mutexes in release order; all end unlocked.
// Depends: kernel::sync::Mutex, kernel::TaskControlBlock, kernel::Scheduler
JARVIS_TEST(mutex_priority_chain, "PRE: none | POST: none") {
    sync::Mutex m1, m2;
    m1.init();
    m2.init();
    sync::Semaphore gate_a;
    gate_a.init(0, 1);
    sync::Semaphore gate_b;
    gate_b.init(0, 1);
    sync::Semaphore gate_c;
    gate_c.init(0, 1);

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
            while (self->state == TaskState::BLOCKED)
                arch::pause();
            mm1->unlock();
        },
        11, 10);
    JARVIS_ASSERT(a != nullptr);
    a->user_data = &actx;
    Scheduler::add_task(*a);
    Scheduler::reschedule();
    wait_state(*a, TaskState::BLOCKED);
    JARVIS_ASSERT(m1.owner() == a);

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
    JARVIS_ASSERT(b != nullptr);
    b->user_data = &bctx;
    Scheduler::add_task(*b);
    Scheduler::reschedule();
    wait_state(*b, TaskState::BLOCKED);
    JARVIS_ASSERT(m2.owner() == b);

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
    JARVIS_ASSERT(c != nullptr);
    c->user_data = &cctx;
    Scheduler::add_task(*c);
    Scheduler::reschedule();
    wait_state(*c, TaskState::BLOCKED);

    // Release in order C, B, A.
    gate_c.post();
    wait_state(*c, TaskState::TERMINATED);
    gate_b.post();
    wait_state(*b, TaskState::TERMINATED);
    gate_a.post();
    wait_state(*a, TaskState::TERMINATED);

    JARVIS_ASSERT(!m1.is_locked());
    JARVIS_ASSERT(!m2.is_locked());

    Scheduler::drain_zombie_list();
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Multiple tasks acquire a mutex in priority order after the holder
// releases (gate-driven, see file header note).
// Input: Holder (prio 11) holds the mutex and blocks on a gate; waiters at
//        prio 14 and 20 block on gates, then acquire the freed mutex.
// Expect: Both waiters acquire; mutex ends unlocked.
// Depends: kernel::sync::Mutex, kernel::TaskControlBlock, kernel::Scheduler
JARVIS_TEST(mutex_waiter_priority_order, "PRE: none | POST: none") {
    sync::Mutex mutex;
    mutex.init();
    sync::Semaphore gate;
    gate.init(0, 1);
    sync::Semaphore gate_lo;
    gate_lo.init(0, 1);
    sync::Semaphore gate_hi;
    gate_hi.init(0, 1);

    struct HCtx {
        uint64_t mutex_;
        uint64_t gate_;
    } hctx;
    hctx.mutex_ = reinterpret_cast<uint64_t>(&mutex);
    hctx.gate_ = reinterpret_cast<uint64_t>(&gate);
    auto *holder = TaskControlBlock::create(
        []() {
            auto *self = Scheduler::current_task();
            auto *c = reinterpret_cast<HCtx *>(self->user_data);
            auto *m = reinterpret_cast<sync::Mutex *>(c->mutex_);
            auto *g = reinterpret_cast<sync::Semaphore *>(c->gate_);
            m->lock();
            g->wait();
            while (self->state == TaskState::BLOCKED)
                arch::pause();
            m->unlock();
        },
        11, 10);
    JARVIS_ASSERT(holder != nullptr);
    holder->user_data = &hctx;
    Scheduler::add_task(*holder);
    Scheduler::reschedule();
    wait_state(*holder, TaskState::BLOCKED);
    JARVIS_ASSERT(mutex.owner() == holder);

    uint64_t wlo_slot[3];
    uint64_t wlo_acquired = 0;
    wlo_slot[0] = reinterpret_cast<uint64_t>(&gate_lo);
    wlo_slot[1] = reinterpret_cast<uint64_t>(&mutex);
    wlo_slot[2] = reinterpret_cast<uint64_t>(&wlo_acquired);
    auto *waiter_low = TaskControlBlock::create(
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
        14, 10);
    JARVIS_ASSERT(waiter_low != nullptr);
    waiter_low->user_data = wlo_slot;
    Scheduler::add_task(*waiter_low);
    Scheduler::reschedule();
    wait_state(*waiter_low, TaskState::BLOCKED);

    uint64_t whi_slot[3];
    uint64_t whi_acquired = 0;
    whi_slot[0] = reinterpret_cast<uint64_t>(&gate_hi);
    whi_slot[1] = reinterpret_cast<uint64_t>(&mutex);
    whi_slot[2] = reinterpret_cast<uint64_t>(&whi_acquired);
    auto *waiter_high = TaskControlBlock::create(
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
    JARVIS_ASSERT(waiter_high != nullptr);
    waiter_high->user_data = whi_slot;
    Scheduler::add_task(*waiter_high);
    Scheduler::reschedule();
    wait_state(*waiter_high, TaskState::BLOCKED);

    JARVIS_ASSERT(waiter_low->state == TaskState::BLOCKED);
    JARVIS_ASSERT(waiter_high->state == TaskState::BLOCKED);

    // Release: holder unlocks, then the high-priority waiter, then low.
    gate.post();
    wait_state(*holder, TaskState::TERMINATED);
    gate_hi.post();
    wait_state(*waiter_high, TaskState::TERMINATED);
    gate_lo.post();
    wait_state(*waiter_low, TaskState::TERMINATED);

    JARVIS_ASSERT(!mutex.is_locked());

    Scheduler::drain_zombie_list();
    JARVIS_ASSERT(whi_acquired == 1);
    JARVIS_ASSERT(wlo_acquired == 1);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Mutex locked twice by the same owner does not deadlock (recursive
// mutex); correct unlock count required for full release.
// Input: A REAL task locks mutex twice, unlocks twice.
// Expect: No deadlock; after second unlock, mutex is unlocked.
// Depends: kernel::sync::Mutex, kernel::TaskControlBlock, kernel::Scheduler
JARVIS_TEST(mutex_double_lock_same_owner, "PRE: none | POST: none") {
    sync::Mutex mutex;
    mutex.init();
    static uint64_t g_ok = 0;

    auto *owner = TaskControlBlock::create(
        []() {
            auto *self = Scheduler::current_task();
            auto *m = reinterpret_cast<sync::Mutex *>(self->user_data);
            m->lock();
            m->lock();
            m->unlock();
            bool still = m->is_locked();
            m->unlock();
            if (still && !m->is_locked())
                g_ok = 1;
        },
        11, 10);
    JARVIS_ASSERT(owner != nullptr);
    owner->user_data = &mutex;
    Scheduler::add_task(*owner);
    Scheduler::reschedule();
    wait_state(*owner, TaskState::TERMINATED);
    Scheduler::drain_zombie_list();
    JARVIS_ASSERT_EQ(1ULL, g_ok);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: 100 rapid lock/unlock cycles on the same mutex; no corruption.
// Input: A REAL task performs 100 lock/unlock cycles.
// Expect: No crash; after loop, mutex unlocked.
// Depends: kernel::sync::Mutex, kernel::TaskControlBlock, kernel::Scheduler
JARVIS_TEST(mutex_lock_acquire_release_cycle, "PRE: none | POST: none") {
    sync::Mutex mutex;
    mutex.init();
    static uint64_t g_ok = 0;

    auto *owner = TaskControlBlock::create(
        []() {
            auto *self = Scheduler::current_task();
            auto *m = reinterpret_cast<sync::Mutex *>(self->user_data);
            for (uint64_t i = 0; i < 100; ++i) {
                m->lock();
                m->unlock();
            }
            if (!m->is_locked())
                g_ok = 1;
        },
        11, 10);
    JARVIS_ASSERT(owner != nullptr);
    owner->user_data = &mutex;
    Scheduler::add_task(*owner);
    Scheduler::reschedule();
    wait_state(*owner, TaskState::TERMINATED);
    Scheduler::drain_zombie_list();
    JARVIS_ASSERT_EQ(1ULL, g_ok);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Multiple tasks blocked on Semaphore::wait() at different
// priorities; post() wakes the highest-priority task first.  Driven: real
// dispatched tasks wait on a real semaphore.
// Input: Semaphore with count=0, real tasks at prio 11, 14, 20 wait.
// Expect: After post(), highest-priority task (20) wakes and completes.
// Depends: kernel::sync::Semaphore, kernel::TaskControlBlock, kernel::Scheduler
JARVIS_TEST(semaphore_wait_priority_order, "PRE: none | POST: none") {
    sync::Semaphore sem;
    sem.init(0, 3);
    static uint64_t g_high_woken = 0;

    auto *task_low = TaskControlBlock::create(
        []() {
            auto *self = Scheduler::current_task();
            auto *s = reinterpret_cast<sync::Semaphore *>(self->user_data);
            s->wait();
            while (self->state == TaskState::BLOCKED)
                arch::pause();
        },
        11, 10);
    JARVIS_ASSERT(task_low != nullptr);
    task_low->user_data = &sem;
    Scheduler::add_task(*task_low);
    Scheduler::reschedule();
    wait_state(*task_low, TaskState::BLOCKED);

    auto *task_mid = TaskControlBlock::create(
        []() {
            auto *self = Scheduler::current_task();
            auto *s = reinterpret_cast<sync::Semaphore *>(self->user_data);
            s->wait();
            while (self->state == TaskState::BLOCKED)
                arch::pause();
        },
        14, 10);
    JARVIS_ASSERT(task_mid != nullptr);
    task_mid->user_data = &sem;
    Scheduler::add_task(*task_mid);
    Scheduler::reschedule();
    wait_state(*task_mid, TaskState::BLOCKED);

    auto *task_high = TaskControlBlock::create(
        []() {
            auto *self = Scheduler::current_task();
            auto *s = reinterpret_cast<sync::Semaphore *>(self->user_data);
            s->wait();
            while (self->state == TaskState::BLOCKED)
                arch::pause();
            g_high_woken = 1;
        },
        20, 10);
    JARVIS_ASSERT(task_high != nullptr);
    task_high->user_data = &sem;
    Scheduler::add_task(*task_high);
    Scheduler::reschedule();
    wait_state(*task_high, TaskState::BLOCKED);

    JARVIS_ASSERT(task_low->state == TaskState::BLOCKED);
    JARVIS_ASSERT(task_mid->state == TaskState::BLOCKED);
    JARVIS_ASSERT(task_high->state == TaskState::BLOCKED);

    // Post enough to wake all three (highest priority first).
    sem.post();
    sem.post();
    sem.post();

    wait_state(*task_low, TaskState::TERMINATED);
    wait_state(*task_mid, TaskState::TERMINATED);
    wait_state(*task_high, TaskState::TERMINATED);


    Scheduler::drain_zombie_list();
    JARVIS_ASSERT_EQ(1ULL, g_high_woken);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: post(3) with 5 blocked waiters wakes exactly 3, leaves 2 blocked.
// Driven: real dispatched tasks wait on a real semaphore.
// Input: Semaphore with count=0, 5 real waiters, post(3).
// Expect: Exactly 3 tasks terminate (woken), 2 remain BLOCKED.
// Depends: kernel::sync::Semaphore, kernel::TaskControlBlock, kernel::Scheduler
JARVIS_TEST(semaphore_multi_waiter_partial_wake, "PRE: none | POST: none") {
    sync::Semaphore sem;
    sem.init(0, 5);

    TaskControlBlock *tasks[5];
    for (int i = 0; i < 5; ++i) {
        tasks[i] = TaskControlBlock::create(
            []() {
                auto *self = Scheduler::current_task();
                auto *s = reinterpret_cast<sync::Semaphore *>(self->user_data);
                s->wait();
                while (self->state == TaskState::BLOCKED)
                    arch::pause();
            },
            11 + static_cast<uint64_t>(i), 10);
        JARVIS_ASSERT(tasks[i] != nullptr);
        tasks[i]->user_data = &sem;
        Scheduler::add_task(*tasks[i]);
        Scheduler::reschedule();
        wait_state(*tasks[i], TaskState::BLOCKED);
    }

    // Post 3 times — exactly 3 waiters wake and terminate.
    sem.post();
    sem.post();
    sem.post();

    uint64_t start = arch::Timer::ticks();
    while (arch::Timer::ticks() - start < 500) {
        int terminated = 0;
        for (int i = 0; i < 5; ++i)
            if (tasks[i]->state == TaskState::TERMINATED)
                ++terminated;
        if (terminated == 3)
            break;
        arch::pause();
    }

    int ready_count = 0;
    int blocked_count = 0;
    for (int i = 0; i < 5; ++i) {
        if (tasks[i]->state == TaskState::TERMINATED)
            ready_count++;
        else if (tasks[i]->state == TaskState::BLOCKED)
            blocked_count++;
    }
    JARVIS_ASSERT_EQ(3, ready_count);
    JARVIS_ASSERT_EQ(2, blocked_count);

    // Wake the remaining 2 for cleanup.
    sem.post();
    sem.post();
    for (int i = 0; i < 5; ++i)
        wait_state(*tasks[i], TaskState::TERMINATED);

    Scheduler::drain_zombie_list();
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Semaphore initialized with count=0; a REAL task's wait() blocks
// immediately; post() wakes it.
// Input: Real task (prio 11) waits on count=0 semaphore.
// Expect: Task blocks; after post, it wakes and terminates.
// Depends: kernel::sync::Semaphore, kernel::TaskControlBlock, kernel::Scheduler
JARVIS_TEST(semaphore_initial_count_zero, "PRE: none | POST: none") {
    sync::Semaphore sem;
    sem.init(0, 1);
    static uint64_t g_woken = 0;

    auto *task = TaskControlBlock::create(
        []() {
            auto *self = Scheduler::current_task();
            auto *s = reinterpret_cast<sync::Semaphore *>(self->user_data);
            s->wait();
            while (self->state == TaskState::BLOCKED)
                arch::pause();
            g_woken = 1;
        },
        11, 10);
    JARVIS_ASSERT(task != nullptr);
    task->user_data = &sem;
    Scheduler::add_task(*task);
    Scheduler::reschedule();
    wait_state(*task, TaskState::BLOCKED);
    JARVIS_ASSERT(task->state == TaskState::BLOCKED);

    sem.post();
    wait_state(*task, TaskState::TERMINATED);

    Scheduler::drain_zombie_list();
    JARVIS_ASSERT_EQ(1ULL, g_woken);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Blocked senders/receivers in Queue are woken in priority order.
// Driven: real receiver tasks block on an empty queue; a real sender unblocks
// them in priority order.
// Input: Two real receivers (prio 14, 20) block on an empty queue.
// Expect: The highest-priority waiter is woken first.
// Depends: kernel::sync::Queue, kernel::TaskControlBlock, kernel::Scheduler
JARVIS_TEST(queue_send_receive_priority_ordering, "PRE: none | POST: none") {
    sync::Queue queue;
    queue.init();
    static uint64_t g_high_first = 0;
    static uint64_t g_high_woken = 0;
    static uint64_t g_low_woken = 0;

    struct QCtx {
        uint64_t queue_;
        uint64_t tag_;
    };
    auto make_receiver = [&](uint64_t prio, uint64_t tag) {
        static QCtx qctx[2];
        qctx[tag == 20 ? 0 : 1].queue_ = reinterpret_cast<uint64_t>(&queue);
        qctx[tag == 20 ? 0 : 1].tag_ = tag;
        auto *t = TaskControlBlock::create(
            []() {
                auto *self = Scheduler::current_task();
                auto *c = reinterpret_cast<QCtx *>(self->user_data);
                auto *q = reinterpret_cast<sync::Queue *>(c->queue_);
                uint8_t buf[32];
                size_t sz = sizeof(buf);
                bool ok = q->receive(buf, &sz);
                if (ok) {
                    if (c->tag_ == 20) {
                        if (!g_high_first)
                            g_high_first = 1;
                        g_high_woken = 1;
                    } else {
                        if (!g_high_woken)
                            g_high_first = 2; // low woke before high — wrong
                        g_low_woken = 1;
                    }
                }
            },
            prio, 10);
        if (t == nullptr)
            return t;
        t->user_data = &qctx[tag == 20 ? 0 : 1];
        return t;
    };

    auto *sender_high = make_receiver(20, 20);
    JARVIS_ASSERT(sender_high != nullptr);
    Scheduler::add_task(*sender_high);
    Scheduler::reschedule();
    wait_state(*sender_high, TaskState::BLOCKED);

    auto *sender_low = make_receiver(14, 14);
    JARVIS_ASSERT(sender_low != nullptr);
    Scheduler::add_task(*sender_low);
    Scheduler::reschedule();
    wait_state(*sender_low, TaskState::BLOCKED);

    // Send two messages — high-priority receiver wakes first.
    JARVIS_ASSERT(queue.try_send(reinterpret_cast<const uint8_t *>("a"), 1));
    JARVIS_ASSERT(queue.try_send(reinterpret_cast<const uint8_t *>("b"), 1));

    wait_state(*sender_high, TaskState::TERMINATED);
    wait_state(*sender_low, TaskState::TERMINATED);


    Scheduler::drain_zombie_list();
    JARVIS_ASSERT_EQ(1ULL, g_high_first);
    JARVIS_ASSERT_EQ(1ULL, g_high_woken);
    JARVIS_ASSERT_EQ(1ULL, g_low_woken);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Queue filled to capacity; a REAL sender's send() blocks; a
// receiver receive() wakes the blocked sender.
// Input: Fill queue; real sender (prio 11) blocks on send; harness drains.
// Expect: Sender blocks; after receive(), sender wakes and terminates.
// Depends: kernel::sync::Queue, kernel::TaskControlBlock, kernel::Scheduler
JARVIS_TEST(queue_send_to_full_blocks_and_wakes, "PRE: none | POST: none") {
    sync::Queue queue;
    queue.init();
    static uint64_t g_sent = 0;

    for (size_t i = 0; i < sync::QUEUE_MAX_MSG_COUNT; ++i) {
        uint8_t d[32] = {static_cast<uint8_t>(i)};
        JARVIS_ASSERT(queue.try_send(d, 1));
    }

    auto *sender = TaskControlBlock::create(
        []() {
            auto *self = Scheduler::current_task();
            auto *q = reinterpret_cast<sync::Queue *>(self->user_data);
            bool ok = q->send(reinterpret_cast<const uint8_t *>("test"), 4);
            if (ok)
                g_sent = 1;
        },
        11, 10);
    JARVIS_ASSERT(sender != nullptr);
    sender->user_data = &queue;
    Scheduler::add_task(*sender);
    Scheduler::reschedule();
    wait_state(*sender, TaskState::BLOCKED);
    JARVIS_ASSERT(sender->state == TaskState::BLOCKED);

    // Receiver drains one — wakes the blocked sender.
    uint8_t buf[32];
    size_t size = sizeof(buf);
    JARVIS_ASSERT(queue.try_receive(buf, &size));
    wait_state(*sender, TaskState::TERMINATED);

    Scheduler::drain_zombie_list();
    JARVIS_ASSERT_EQ(1ULL, g_sent);
    JARVIS_TEST_PASS();
}

void register_locking_tests() {
    Logger::info("Registering locking tests");
    JARVIS_REGISTER_TEST(mutex_try_lock_success);
    JARVIS_REGISTER_TEST(mutex_try_lock_failure);
    JARVIS_REGISTER_TEST(mutex_try_lock_recursive_same_owner);
    JARVIS_REGISTER_TEST(mutex_priority_inheritance_indirect);
    JARVIS_REGISTER_TEST(mutex_priority_chain);
    JARVIS_REGISTER_TEST(mutex_waiter_priority_order);
    JARVIS_REGISTER_TEST(mutex_double_lock_same_owner);
    JARVIS_REGISTER_TEST(mutex_lock_acquire_release_cycle);
    JARVIS_REGISTER_TEST(semaphore_wait_priority_order);
    JARVIS_REGISTER_TEST(semaphore_multi_waiter_partial_wake);
    JARVIS_REGISTER_TEST(semaphore_initial_count_zero);
    JARVIS_REGISTER_TEST(queue_send_receive_priority_ordering);
    JARVIS_REGISTER_TEST(queue_send_to_full_blocks_and_wakes);
}
#ifndef __clang__
#pragma GCC diagnostic pop
#endif
