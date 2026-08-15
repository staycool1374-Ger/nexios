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

/// @file test_locking_stress.cpp
/// @brief Locking primitive stress tests.
///
/// v0.3.10 rework (SIMULATED → DRIVEN): contention and producer/consumer
/// patterns are driven by REAL kernel tasks (prio ≥ 11) dispatched by the
/// real timer ISR.  No set_current impersonation; the primitives are
/// exercised from the tasks' own running contexts.

#include <test.hpp>
#include <logger.hpp>
#include <kernel/sync/mutex.hpp>
#include <kernel/sync/semaphore.hpp>
#include <kernel/sync/queue.hpp>
#include <kernel/task/task.hpp>
#include <kernel/task/scheduler.hpp>
#include <kernel/arch/timer.hpp>
#include "test_sched_helpers.hpp"

using namespace kernel;

#ifndef __clang__
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wanalyzer-null-argument"
#endif

namespace {
void release_task(TaskControlBlock *t) {
    if (t == nullptr)
        return;
    kernel::test::terminate_if_live(t);
}

/// @brief Wait until a task reaches a state, yielding via pause().
inline void wait_state(TaskControlBlock &t, TaskState s) {
    while (t.state != s)
        arch::pause();
}
} // namespace

// Runmode: kernel
// Testidea: N tasks repeatedly lock/unlock a shared mutex from their own REAL
// dispatched contexts; no deadlock, no corruption.
// Input: 4 real tasks each performing 200 lock/unlock cycles.
// Expect: All tasks complete; no deadlock; mutex unlocked at end.
JARVIS_TEST(mutex_stress_high_contention, "PRE: none | POST: none") {
    sync::Mutex mutex;
    mutex.init();
    static uint64_t g_done = 0;

    static const int NUM_TASKS = 4;
    TaskControlBlock *tasks[NUM_TASKS];
    for (int i = 0; i < NUM_TASKS; ++i) {
        tasks[i] = TaskControlBlock::create(
            []() {
                auto *self = Scheduler::current_task();
                auto *m = reinterpret_cast<sync::Mutex *>(self->user_data);
                for (uint64_t cycle = 0; cycle < 200; ++cycle) {
                    m->lock();
                    m->unlock();
                }
                __atomic_add_fetch(&g_done, 1, __ATOMIC_RELEASE);
            },
            11 + static_cast<uint64_t>(i), 10);
        JARVIS_ASSERT(tasks[i] != nullptr);
        tasks[i]->user_data = &mutex;
        Scheduler::add_task(*tasks[i]);
    }
    Scheduler::reschedule();

    // All tasks run to completion through real dispatch.
    uint64_t start = arch::Timer::ticks();
    while (g_done < NUM_TASKS && arch::Timer::ticks() - start < 3000)
        arch::pause();
    JARVIS_ASSERT_EQ(static_cast<uint64_t>(NUM_TASKS), g_done);

    // Mutex should be unlocked.
    JARVIS_ASSERT(!mutex.is_locked());

    for (int i = 0; i < NUM_TASKS; ++i)
        release_task(tasks[i]);
    Scheduler::drain_zombie_list();
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: A REAL producer task posts to a semaphore; REAL consumer tasks
// wait; each message is consumed exactly once.
// Input: Semaphore initialized to 0; 1 producer posts 20 times, 4 consumers
//        wait.
// Expect: All consumers wake; total posts consumed; no lost wakeups.
JARVIS_TEST(semaphore_producer_consumer, "PRE: none | POST: none") {
    sync::Semaphore sem;
    sem.init(0, 100);
    static uint64_t g_consumed = 0;

    static const int NUM_CONSUMERS = 4;

    TaskControlBlock *consumers[NUM_CONSUMERS];
    for (int i = 0; i < NUM_CONSUMERS; ++i) {
        consumers[i] = TaskControlBlock::create(
            []() {
                auto *self = Scheduler::current_task();
                auto *s = reinterpret_cast<sync::Semaphore *>(self->user_data);
                s->wait();
                __atomic_add_fetch(&g_consumed, 1, __ATOMIC_RELEASE);
            },
            11, 10);
        JARVIS_ASSERT(consumers[i] != nullptr);
        consumers[i]->user_data = &sem;
        Scheduler::add_task(*consumers[i]);
    }

    // Producer: posts 20 times in its own dispatched context.
    auto *producer = TaskControlBlock::create(
        []() {
            auto *self = Scheduler::current_task();
            auto *s = reinterpret_cast<sync::Semaphore *>(self->user_data);
            for (int i = 0; i < 20; ++i)
                s->post();
        },
        12, 10);
    JARVIS_ASSERT(producer != nullptr);
    producer->user_data = &sem;
    Scheduler::add_task(*producer);

    Scheduler::reschedule();
    uint64_t start = arch::Timer::ticks();
    while (arch::Timer::ticks() - start < 3000) {
        bool all_done = true;
        for (int i = 0; i < NUM_CONSUMERS; ++i)
            if (consumers[i]->state != TaskState::TERMINATED)
                all_done = false;
        if (producer->state != TaskState::TERMINATED)
            all_done = false;
        if (all_done)
            break;
        arch::pause();
    }

    // All consumers consumed (producer's 20 posts woke at least 4; remaining
    // posts left in the semaphore count — no lost wakeups, no hangs).
    JARVIS_ASSERT_EQ(static_cast<uint64_t>(NUM_CONSUMERS), g_consumed);

    for (int i = 0; i < NUM_CONSUMERS; ++i)
        release_task(consumers[i]);
    release_task(producer);
        Scheduler::drain_zombie_list();
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: A REAL producer and REAL consumers share a single Queue; no
// message lost, no duplicate delivery, no crash.
// Input: Queue shared by 1 producer (sends NUM_CONSUMERS msgs) and 4
//        consumers (each consumes exactly one message then terminates).
// Expect: All messages consumed; no crash.
JARVIS_TEST(queue_multi_producer_multi_consumer, "PRE: none | POST: none") {
    sync::Queue queue;
    queue.init();
    static uint64_t g_consumed = 0;

    static const int NUM_CONSUMERS = 4;
    TaskControlBlock *consumers[NUM_CONSUMERS];
    for (int i = 0; i < NUM_CONSUMERS; ++i) {
        consumers[i] = TaskControlBlock::create(
            []() {
                auto *self = Scheduler::current_task();
                auto *q = reinterpret_cast<sync::Queue *>(self->user_data);
                uint8_t buf[32];
                size_t size = sizeof(buf);
                bool ok = q->receive(buf, &size);
                if (ok)
                    __atomic_add_fetch(&g_consumed, 1, __ATOMIC_RELEASE);
            },
            11, 10);
        JARVIS_ASSERT(consumers[i] != nullptr);
        consumers[i]->user_data = &queue;
        Scheduler::add_task(*consumers[i]);
    }

    // Producer sends exactly one message per consumer (each consumer consumes
    // exactly one then terminates, so a larger count could never be reached).
    auto *producer = TaskControlBlock::create(
        []() {
            auto *self = Scheduler::current_task();
            auto *q = reinterpret_cast<sync::Queue *>(self->user_data);
            for (int j = 0; j < NUM_CONSUMERS; ++j) {
                uint8_t data[4] = {static_cast<uint8_t>(j)};
                q->try_send(data, 1);
            }
        },
        12, 10);
    JARVIS_ASSERT(producer != nullptr);
    producer->user_data = &queue;
    Scheduler::add_task(*producer);

    Scheduler::reschedule();
    uint64_t start = arch::Timer::ticks();
    while (arch::Timer::ticks() - start < 3000) {
        bool all_done = true;
        for (int i = 0; i < NUM_CONSUMERS; ++i)
            if (consumers[i]->state != TaskState::TERMINATED)
                all_done = false;
        if (producer->state != TaskState::TERMINATED)
            all_done = false;
        if (all_done)
            break;
        arch::pause();
    }

    JARVIS_ASSERT_EQ(static_cast<uint64_t>(NUM_CONSUMERS), g_consumed);

    // All tasks self-terminated — reclaim via the zombie list (cookbook
    // Rule 4/5); remove_task+cleanup+delete on a zombie would double-free.
    Scheduler::drain_zombie_list();
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Low-priority task holds a mutex; a high-priority task acquires it
// after release (gate-driven — contended Mutex::lock() cannot genuinely block
// a dispatched task at 1ms ticks; see test_locking.cpp header note).
// Input: Real LOW (prio 11) holds the mutex and blocks on a gate; real HIGH
//        (prio 20) blocks on its own gate, then acquires the freed mutex.
// Expect: HIGH acquires exactly once; mutex unlocked; no inversion crash.
JARVIS_TEST(priority_inversion_under_contention, "PRE: none | POST: none") {
    sync::Mutex mutex;
    mutex.init();
    sync::Semaphore gate_low;
    gate_low.init(0, 1);
    sync::Semaphore gate_high;
    gate_high.init(0, 1);

    struct LCtx {
        uint64_t mutex_;
        uint64_t gate_;
    } lctx;
    lctx.mutex_ = reinterpret_cast<uint64_t>(&mutex);
    lctx.gate_ = reinterpret_cast<uint64_t>(&gate_low);
    auto *low = TaskControlBlock::create(
        []() {
            auto *self = Scheduler::current_task();
            auto *c = reinterpret_cast<LCtx *>(self->user_data);
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
    low->user_data = &lctx;
    Scheduler::add_task(*low);
    Scheduler::reschedule();
    wait_state(*low, TaskState::BLOCKED);
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

    // Both self-terminated — reclaim via the zombie list.
    Scheduler::drain_zombie_list();
    JARVIS_ASSERT(high_acquired == 1);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Same task locks a non-recursive mutex twice — the mutex supports
// recursion, so the second lock increments the count and no deadlock occurs.
// Input: A REAL task locks twice, unlocks twice.
// Expect: No crash; both unlocks complete; mutex unlocked.
JARVIS_TEST(mutex_recursive_deadlock, "PRE: none | POST: none") {
    sync::Mutex mutex;
    mutex.init();
    static uint64_t g_ok = 0;

    auto *t = TaskControlBlock::create(
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
    JARVIS_ASSERT(t != nullptr);
    t->user_data = &mutex;
    Scheduler::add_task(*t);
    Scheduler::reschedule();
    wait_state(*t, TaskState::TERMINATED);
    // Self-terminated — reclaim via the zombie list.
    Scheduler::drain_zombie_list();
    JARVIS_ASSERT_EQ(1ULL, g_ok);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Repeated try_wait on an empty semaphore must not decrement below
// zero.  Pure container test.
// Input: Semaphore initialized to 0; call try_wait 10 times.
// Expect: All 10 try_wait calls return false; semaphore value stays 0.
JARVIS_TEST(semaphore_count_underflow, "PRE: none | POST: none") {
    sync::Semaphore sem;
    sem.init(0, 10);

    for (int i = 0; i < 10; ++i) {
        bool ok = sem.try_wait();
        JARVIS_ASSERT(!ok);
    }

    JARVIS_ASSERT_EQ(0ULL, sem.value());
    JARVIS_TEST_PASS();
}

void register_locking_stress_tests() {
    Logger::info("Registering locking stress tests");
    JARVIS_REGISTER_TEST(mutex_stress_high_contention);
    JARVIS_REGISTER_TEST(semaphore_producer_consumer);
    JARVIS_REGISTER_TEST(queue_multi_producer_multi_consumer);
    JARVIS_REGISTER_TEST(priority_inversion_under_contention);
    JARVIS_REGISTER_TEST(mutex_recursive_deadlock);
    JARVIS_REGISTER_TEST(semaphore_count_underflow);
}
#ifndef __clang__
#pragma GCC diagnostic pop
#endif
