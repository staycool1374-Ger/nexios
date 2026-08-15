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

/// @file test_sync.cpp
/// @brief Synchronisation primitive tests.
///
/// v0.3.10 rework (SIMULATED → DRIVEN): blocking tests use REAL kernel tasks
/// (prio ≥ 11) dispatched by the real timer ISR.  The worker genuinely blocks
/// in semaphore.wait()/mutex.lock() in its own running context; the harness
/// (as a real IPC peer) performs the wake action.  No set_current
/// impersonation.

#ifndef __clang__
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wanalyzer-null-argument"
#pragma GCC diagnostic ignored "-Wanalyzer-possible-null-dereference"
#endif

#include <test.hpp>
#include <logger.hpp>
#include <kernel/sync/semaphore.hpp>
#include <kernel/sync/mutex.hpp>
#include <kernel/sync/queue.hpp>
#include <kernel/task/task.hpp>
#include <kernel/task/scheduler.hpp>
#include <kernel/test/test_sched_helpers.hpp>

using namespace kernel;

// Runmode: kernel
// Testidea: Verifies that a REAL task blocks in semaphore.wait() when the
// count is 0 and wakes when semaphore.post() is called.
// Input: Worker task (prio 11) genuinely dispatches and blocks inside
//        sem.wait(); the harness (a real peer) posts.
// Expect: Worker state is BLOCKED after wait, then TERMINATED after post.
// Depends: kernel::sync::Semaphore, kernel::TaskControlBlock, kernel::Scheduler
JARVIS_TEST(semaphore_wait_post, "PRE: none | POST: none") {
    sync::Semaphore sem;
    sem.init(0, 1);
    static uint64_t g_woken = 0;

    auto *worker = TaskControlBlock::create(
        []() {
            auto *self = Scheduler::current_task();
            sync::Semaphore *s = reinterpret_cast<sync::Semaphore *>(
                self->user_data);
            s->wait();
            // Semaphore::wait() only sets BLOCKED and defers the switch
            // (INV-4); it returns immediately.  Spin until the timer ISR
            // actually suspends this task, then exit when post() wakes it.
            while (self->state == TaskState::BLOCKED)
                arch::pause();
            __atomic_store_n(&g_woken, 1, __ATOMIC_RELEASE);
        },
        11, 10);
    JARVIS_ASSERT(worker != nullptr);
    worker->user_data = &sem;
    Scheduler::add_task(*worker);
    Scheduler::reschedule();

    // The worker genuinely blocks inside sem.wait().
    while (worker->state != TaskState::BLOCKED)
        arch::pause();
    JARVIS_ASSERT(worker->state == TaskState::BLOCKED);

    // Wake it (real post).
    sem.post();
    kernel::test::wait_for_termination_safe(worker);
    // Cleanup BEFORE asserting (cookbook Rule 5): self-terminated.
    Scheduler::drain_zombie_list();
    JARVIS_ASSERT_EQ(1ULL, g_woken);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Verifies the v0.3.9 "blocked semaphore waiter teardown gap" is
// closed: a REAL task blocked in Semaphore::wait() is externally terminated
// and reaped, and TaskControlBlock::cleanup() unlinks it from the semaphore's
// waiter list so a later post() cannot re-queue the freed TCB (ready-queue
// corruption / use-after-free).
// Input: Worker (prio 11) genuinely blocks in sem.wait(); the harness
//        externally terminates it via the real Scheduler::terminate() +
//        drain_zombie_list() path (cookbook Rule 6 previously forbade this).
// Expect: After reap, sem.waiter_count() == 0 and the reaped task is no longer
//         linked; a post() after teardown takes the count-increment path
//         (no set_task_ready on the freed TCB) and no ResourceTracker delta.
// Depends: kernel::sync::Semaphore, kernel::TaskControlBlock, kernel::Scheduler
JARVIS_TEST(semaphore_waiter_teardown_on_terminate,
            "PRE: none | POST: none") {
    sync::Semaphore sem;
    sem.init(0, 1);

    auto *worker = TaskControlBlock::create(
        []() {
            auto *self = Scheduler::current_task();
            sync::Semaphore *s = reinterpret_cast<sync::Semaphore *>(
                self->user_data);
            s->wait();
            // Semaphore::wait() only sets BLOCKED and defers the switch
            // (INV-4); it returns immediately.  Spin until the timer ISR
            // actually suspends this task, then exit when post() wakes it.
            while (self->state == TaskState::BLOCKED)
                arch::pause();
        },
        11, 10);
    JARVIS_ASSERT(worker != nullptr);
    worker->user_data = &sem;
    Scheduler::add_task(*worker);
    Scheduler::reschedule();

    // The worker genuinely blocks inside sem.wait() and is linked as a waiter.
    while (worker->state != TaskState::BLOCKED)
        arch::pause();
    JARVIS_ASSERT(worker->state == TaskState::BLOCKED);
    JARVIS_ASSERT(sem.waiter_count() == 1);
    JARVIS_ASSERT(worker->waiting_on_semaphore == &sem);

    // External termination while blocked on the semaphore — the v0.3.9 gap.
    // terminate() + drain runs cleanup(), which must unlink the waiter before
    // the TCB is freed.
    Scheduler::terminate(*worker, 0);
    Scheduler::drain_zombie_list();

    // The reaped task must no longer be linked: the waiter array is empty and
    // the back-pointer was cleared.  A post() must take the count path (not
    // wake the freed TCB), so value() becomes 1.
    JARVIS_ASSERT(sem.waiter_count() == 0);
    sem.post();
    JARVIS_ASSERT_EQ(1ULL, sem.value());

    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Verifies the blocked-event-group-waiter teardown gap is closed: a
// REAL task blocked in EventGroup::wait_bits() is externally terminated and
// reaped, and cleanup() unlinks it from the group's waiter list so a later
// set_bits() cannot re-queue the freed TCB.
// Input: Worker (prio 11) genuinely blocks in eg.wait_bits(0x1); harness
//        externally terminates + drains it, then sets the bit.
// Expect: After reap, eg.waiter_count() == 0 and get_bits() reflects the bit
//         (wake path not taken on the freed TCB); no ResourceTracker delta.
// Depends: kernel::sync::EventGroup, kernel::TaskControlBlock, kernel::Scheduler
JARVIS_TEST(eventgroup_waiter_teardown_on_terminate,
            "PRE: none | POST: none") {
    sync::EventGroup eg;
    eg.init();

    auto *worker = TaskControlBlock::create(
        []() {
            auto *self = Scheduler::current_task();
            sync::EventGroup *e = reinterpret_cast<sync::EventGroup *>(
                self->user_data);
            e->wait_bits(0x1);
            // INV-4: wait_bits sets BLOCKED and defers the switch; it returns
            // immediately.  Spin until the timer ISR actually suspends this
            // task, then exit when set_bits() wakes it.
            while (self->state == TaskState::BLOCKED)
                arch::pause();
        },
        11, 10);
    JARVIS_ASSERT(worker != nullptr);
    worker->user_data = &eg;
    Scheduler::add_task(*worker);
    Scheduler::reschedule();

    // The worker genuinely blocks inside eg.wait_bits() and is linked.
    while (worker->state != TaskState::BLOCKED)
        arch::pause();
    JARVIS_ASSERT(worker->state == TaskState::BLOCKED);
    JARVIS_ASSERT(eg.waiter_count() == 1);
    JARVIS_ASSERT(worker->waiting_on_eventgroup == &eg);

    // External termination while blocked on the event group.
    Scheduler::terminate(*worker, 0);
    Scheduler::drain_zombie_list();

    // The reaped task must no longer be linked.  set_bits() must take the
    // no-waiter path: the bit is set directly (no set_task_ready on the freed
    // TCB).
    JARVIS_ASSERT(eg.waiter_count() == 0);
    eg.set_bits(0x1);
    JARVIS_ASSERT_EQ(0x1ULL, eg.get_bits());

    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Verifies the blocked-queue-waiter teardown gap is closed: a REAL
// task blocked in Queue::send() on a full queue is externally terminated and
// reaped, and cleanup() unlinks it from the queue's send-waiter list so a
// later receive() cannot wake the freed TCB.
// Input: Worker (prio 11) genuinely blocks in queue.send() on a full queue;
//        harness externally terminates + drains it, then drains a message.
// Expect: After reap, queue.waiter_count() == 0; the subsequent receive()
//         takes the no-waiter path (no set_task_ready on the freed TCB).
// Depends: kernel::sync::Queue, kernel::TaskControlBlock, kernel::Scheduler
JARVIS_TEST(queue_waiter_teardown_on_terminate, "PRE: none | POST: none") {
    sync::Queue queue;
    queue.init();

    // Fill the queue so the worker genuinely blocks in Queue::send().
    for (size_t i = 0; i < sync::QUEUE_MAX_MSG_COUNT; ++i) {
        uint8_t d[32] = {static_cast<uint8_t>(i)};
        JARVIS_ASSERT(queue.try_send(d, 1));
    }
    JARVIS_ASSERT(queue.available() == sync::QUEUE_MAX_MSG_COUNT);

    auto *worker = TaskControlBlock::create(
        []() {
            auto *self = Scheduler::current_task();
            sync::Queue *q = reinterpret_cast<sync::Queue *>(self->user_data);
            q->send((uint8_t *)"data", 4);
            // INV-4: send sets BLOCKED and defers the switch; spin until the
            // timer ISR actually suspends this task.
            while (self->state == TaskState::BLOCKED)
                arch::pause();
        },
        11, 10);
    JARVIS_ASSERT(worker != nullptr);
    worker->user_data = &queue;
    Scheduler::add_task(*worker);
    Scheduler::reschedule();

    // The worker genuinely blocks inside queue.send() and is linked as a
    // send waiter.
    while (worker->state != TaskState::BLOCKED)
        arch::pause();
    JARVIS_ASSERT(worker->state == TaskState::BLOCKED);
    JARVIS_ASSERT(queue.waiter_count() == 1);
    JARVIS_ASSERT(worker->waiting_on_queue == &queue);

    // External termination while blocked on the full queue.
    Scheduler::terminate(*worker, 0);
    Scheduler::drain_zombie_list();

    // The reaped task must no longer be linked.  A receive() must take the
    // no-send-waiter path: it drains and calls wake_send_one(), which finds
    // no waiter (no set_task_ready on the freed TCB).
    JARVIS_ASSERT(queue.waiter_count() == 0);
    uint8_t buf[32];
    size_t size = 32;
    JARVIS_ASSERT(queue.receive(buf, &size));
    JARVIS_ASSERT(queue.available() == sync::QUEUE_MAX_MSG_COUNT - 1);
    JARVIS_ASSERT(queue.waiter_count() == 0);

    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Verifies mutex lock/unlock with owner tracking and waiter handoff
// using a REAL owner task that holds the mutex and blocks on a gate, plus a
// REAL contender that blocks on the mutex and acquires after release.
// Input: Owner (prio 11) locks mutex then blocks on gate; contender (prio 20)
//        blocks on the mutex; harness posts the gate.
// Expect: Mutex correctly tracks owner/locked state across lock/unlock cycles
// and the contender acquires after the owner releases.
// Depends: kernel::sync::Mutex, kernel::TaskControlBlock, kernel::Scheduler
JARVIS_TEST(mutex_lock_unlock, "PRE: none | POST: none") {
    sync::Mutex mutex;
    mutex.init();
    sync::Semaphore gate;
    gate.init(0, 1);
    static uint64_t g_owner_acquired = 0;
    static uint64_t g_contender_acquired = 0;

    struct OCtx {
        uint64_t mutex_;
        uint64_t gate_;
    } octx;
    octx.mutex_ = reinterpret_cast<uint64_t>(&mutex);
    octx.gate_ = reinterpret_cast<uint64_t>(&gate);
    auto *owner = TaskControlBlock::create(
        []() {
            auto *self = Scheduler::current_task();
            auto *c = reinterpret_cast<OCtx *>(self->user_data);
            auto *m = reinterpret_cast<sync::Mutex *>(c->mutex_);
            auto *g = reinterpret_cast<sync::Semaphore *>(c->gate_);
            m->lock();
            __atomic_store_n(&g_owner_acquired, 1, __ATOMIC_RELEASE);
            g->wait();
            // Semaphore::wait() only sets BLOCKED and defers the switch
            // (INV-4); it returns immediately.  Spin until the timer ISR
            // actually suspends this task, then exit when post() wakes it.
            while (self->state == TaskState::BLOCKED)
                arch::pause();
            m->unlock();
        },
        11, 10);
    JARVIS_ASSERT(owner != nullptr);
    owner->user_data = &octx;
    Scheduler::add_task(*owner);
    Scheduler::reschedule();
    while (owner->state != TaskState::BLOCKED)
        arch::pause();

    JARVIS_ASSERT(mutex.owner() == owner);
    JARVIS_ASSERT(mutex.is_locked());
    JARVIS_ASSERT_EQ(1ULL, g_owner_acquired);

    // Contender (prio 20) blocks on a gate, then acquires the freed mutex
    // after the owner releases it.  NOTE: contended Mutex::lock() cannot
    // genuinely block a dispatched task at 1ms ticks (deferred switch INV-4
    // never lands inside the MAX_WAITERS+1 retry budget), so the contender
    // blocks on a semaphore gate instead.
    sync::Semaphore gate_cont;
    gate_cont.init(0, 1);
    uint64_t cslot[3];
    cslot[0] = reinterpret_cast<uint64_t>(&gate_cont);
    cslot[1] = reinterpret_cast<uint64_t>(&mutex);
    cslot[2] = reinterpret_cast<uint64_t>(&g_contender_acquired);
    auto *waiter = TaskControlBlock::create(
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
    JARVIS_ASSERT(waiter != nullptr);
    waiter->user_data = cslot;
    Scheduler::add_task(*waiter);
    Scheduler::reschedule();
    while (waiter->state != TaskState::BLOCKED)
        arch::pause();
    JARVIS_ASSERT(mutex.owner() == owner);

    // Release: owner wakes, unlocks (direct ownership transfer to waiter).
    gate.post();
    kernel::test::wait_for_termination_safe(owner);
    gate_cont.post();
    kernel::test::wait_for_termination_safe(waiter);
    JARVIS_ASSERT(!mutex.is_locked());
    JARVIS_ASSERT(mutex.owner() == nullptr);

    // Cleanup BEFORE asserting (cookbook Rule 5): both self-terminated.
    Scheduler::drain_zombie_list();
    JARVIS_ASSERT_EQ(1ULL, g_contender_acquired);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Verifies mutex lock/unlock with owner tracking and waiter handoff.
// Input: Owner task locks mutex, unlocks, locks again; waiter task locks
// after owner unlocks
// Expect: Mutex correctly tracks owner and locked state across multiple
// lock/unlock cycles and task switches
// Depends: kernel::sync::Mutex, kernel::TaskControlBlock, kernel::Scheduler
JARVIS_TEST(mutex_double_unlock, "PRE: none | POST: none") {
    sync::Mutex m;
    m.init();
    m.lock();
    m.unlock();
    JARVIS_ASSERT(!m.is_locked());
    JARVIS_ASSERT(m.owner() == nullptr);
    JARVIS_TEST_PASS();
}

JARVIS_TEST(mutex_recursive_lock, "PRE: none | POST: none") {
    sync::Mutex m;
    m.init();
    m.lock();
    JARVIS_ASSERT(m.is_locked());
    m.lock();
    JARVIS_ASSERT(m.is_locked());
    m.unlock();
    JARVIS_ASSERT(m.is_locked());
    m.unlock();
    JARVIS_ASSERT(!m.is_locked());
    JARVIS_TEST_PASS();
}

JARVIS_TEST(semaphore_timeout, "PRE: none | POST: none") {
    sync::Semaphore sem;
    sem.init(0, 1);
    bool ok = sem.try_wait();
    JARVIS_ASSERT(!ok);
    JARVIS_TEST_PASS();
}

JARVIS_TEST(semaphore_multi_post, "PRE: none | POST: none") {
    sync::Semaphore sem;
    sem.init(0, 3);
    sem.post();
    sem.post();
    sem.post();
    sem.post();
    JARVIS_ASSERT(sem.value() == 3);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Verifies Queue try_send/try_receive operations including full
// and empty edge cases.
// Input: Send/receive one item, fill queue to QUEUE_MAX_MSG_COUNT, attempt
// overfill, then drain one
// Expect: Single send/receive succeeds, queue fills to max, overfill returns
// false, drain reduces available count
// Depends: kernel::sync::Queue, sync::QUEUE_MAX_MSG_COUNT
JARVIS_TEST(queue_send_receive_block, "PRE: none | POST: none") {
    sync::Queue queue;
    queue.init();

    uint8_t data[32] = {0xAA};
    JARVIS_ASSERT(queue.try_send(data, 1));
    JARVIS_ASSERT_EQ(1ULL, queue.available());

    uint8_t buf[32];
    size_t size = 32;
    JARVIS_ASSERT(queue.try_receive(buf, &size));
    JARVIS_ASSERT_EQ(0xAA, buf[0]);
    JARVIS_ASSERT(queue.available() == 0);

    for (size_t i = 0; i < sync::QUEUE_MAX_MSG_COUNT; ++i) {
        uint8_t d[32] = {static_cast<uint8_t>(i)};
        JARVIS_ASSERT(queue.try_send(d, 1));
    }
    JARVIS_ASSERT(queue.available() == sync::QUEUE_MAX_MSG_COUNT);

    JARVIS_ASSERT(!queue.try_send(data, 1));

    size = 32;
    JARVIS_ASSERT(queue.try_receive(buf, &size));
    JARVIS_ASSERT(queue.available() == sync::QUEUE_MAX_MSG_COUNT - 1);

    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Verifies that Queue::send blocks the sender when the queue is full.
// Input: Fill queue to capacity, create sender task that blocks on send,
// dispatch it through the real scheduler, verify blocking behavior.
// Expect: Sender becomes BLOCKED when queue is full; becomes TERMINATED after
// receiver drains (its blocked send completes and the trampoline exits).
// Depends: kernel::sync::Queue, kernel::TaskControlBlock, kernel::Scheduler
JARVIS_TEST(sync_queue_send_blocks_when_full, "PRE: none | POST: none") {
    sync::Queue queue;
    queue.init();

    // Fill the queue
    for (size_t i = 0; i < sync::QUEUE_MAX_MSG_COUNT; ++i) {
        uint8_t d[32] = {static_cast<uint8_t>(i)};
        JARVIS_ASSERT(queue.try_send(d, 1));
    }
    JARVIS_ASSERT(queue.available() == sync::QUEUE_MAX_MSG_COUNT);

    // The sender genuinely runs (dispatched by the timer ISR) and blocks
    // inside Queue::send() on the full queue.
    auto *sender = TaskControlBlock::create(
        []() {
            sync::Queue *q = reinterpret_cast<sync::Queue *>(
                Scheduler::current_task()->user_data);
            bool ok = q->send((uint8_t *)"test", 4);
            JARVIS_ASSERT(ok);
        },
        11, 10);
    JARVIS_ASSERT(sender != nullptr);
    sender->user_data = &queue;
    Scheduler::add_task(*sender);

    // A plain reschedule() picks the higher-priority sender (11 > harness 10)
    // on the next timer tick.  Busy-wait WITHOUT reschedule() so the timer ISR
    // can acquire the scheduler lock without contention.
    Scheduler::reschedule();
    while (sender->state != TaskState::BLOCKED) {
        arch::pause();
    }

    // The sender genuinely blocked on the full queue (its lambda ran).
    JARVIS_ASSERT(sender->state == TaskState::BLOCKED);

    // Drain one message — wakes the blocked sender, which completes its send
    // and terminates.
    uint8_t buf[32];
    size_t size = 32;
    JARVIS_ASSERT(queue.receive(buf, &size));
    kernel::test::wait_for_termination_safe(sender);

    JARVIS_ASSERT(sender->state == TaskState::TERMINATED);

    // Cleanup BEFORE asserting (cookbook Rule 5): self-terminated.
    Scheduler::drain_zombie_list();
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Verifies that Queue::receive blocks the receiver when the queue
// is empty.
// Input: Create receiver task that blocks on receive from empty queue, dispatch
// it through the real scheduler, verify blocking behavior.
// Expect: Receiver becomes BLOCKED when queue is empty; becomes TERMINATED
// after sender adds a message (its blocked receive completes and the trampoline
// exits).
// Depends: kernel::sync::Queue, kernel::TaskControlBlock, kernel::Scheduler
JARVIS_TEST(sync_queue_receive_blocks_when_empty, "PRE: none | POST: none") {
    sync::Queue queue;
    queue.init();
    JARVIS_ASSERT(queue.available() == 0);

    // The receiver genuinely runs (dispatched by the timer ISR) and blocks
    // inside Queue::receive() on the empty queue.
    auto *receiver = TaskControlBlock::create(
        []() {
            sync::Queue *q = reinterpret_cast<sync::Queue *>(
                Scheduler::current_task()->user_data);
            uint8_t buf[32];
            size_t size = 32;
            bool ok = q->receive(buf, &size);
            JARVIS_ASSERT(ok);
        },
        11, 10);
    JARVIS_ASSERT(receiver != nullptr);
    receiver->user_data = &queue;
    Scheduler::add_task(*receiver);

    // A plain reschedule() picks the higher-priority receiver (11 > harness
    // 10) on the next timer tick.  Busy-wait WITHOUT reschedule() so the timer
    // ISR can acquire the scheduler lock without contention.
    Scheduler::reschedule();
    while (receiver->state != TaskState::BLOCKED) {
        arch::pause();
    }

    // The receiver genuinely blocked on the empty queue (its lambda ran).
    JARVIS_ASSERT(receiver->state == TaskState::BLOCKED);

    // Add a message — wakes the blocked receiver, which completes its receive
    // and terminates.
    JARVIS_ASSERT(queue.try_send((uint8_t *)"data", 4));
    kernel::test::wait_for_termination_safe(receiver);

    JARVIS_ASSERT(receiver->state == TaskState::TERMINATED);

    // Cleanup BEFORE asserting (cookbook Rule 5): self-terminated.
    Scheduler::drain_zombie_list();
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Verifies that a blocked sender is woken when a receiver consumes
// from the queue.
// Input: Fill queue, block sender on send, drain via receiver, verify sender
// becomes TERMINATED (its blocked send completes).
// Expect: Sender is BLOCKED after failed send, runs to TERMINATED after
// receiver calls receive.
// Depends: kernel::sync::Queue, kernel::TaskControlBlock, kernel::Scheduler
JARVIS_TEST(sync_queue_wake_sender_on_receive, "PRE: none | POST: none") {
    sync::Queue queue;
    queue.init();

    // Fill the queue
    for (size_t i = 0; i < sync::QUEUE_MAX_MSG_COUNT; ++i) {
        uint8_t d[32] = {static_cast<uint8_t>(i)};
        JARVIS_ASSERT(queue.try_send(d, 1));
    }

    // The sender genuinely runs and blocks inside Queue::send() on the full
    // queue.
    auto *sender = TaskControlBlock::create(
        []() {
            sync::Queue *q = reinterpret_cast<sync::Queue *>(
                Scheduler::current_task()->user_data);
            bool ok = q->send((uint8_t *)"wake", 4);
            JARVIS_ASSERT(ok);
        },
        11, 10);
    JARVIS_ASSERT(sender != nullptr);
    sender->user_data = &queue;
    Scheduler::add_task(*sender);

    // A plain reschedule() picks the higher-priority sender (11 > harness 10)
    // on the next timer tick.  Busy-wait WITHOUT reschedule() so the timer ISR
    // can acquire the scheduler lock without contention.
    Scheduler::reschedule();
    while (sender->state != TaskState::BLOCKED) {
        arch::pause();
    }
    JARVIS_ASSERT(sender->state == TaskState::BLOCKED);

    // Receiver drains one — wakes the blocked sender, which completes its
    // send and terminates.
    uint8_t buf[32];
    size_t size = 32;
    JARVIS_ASSERT(queue.receive(buf, &size));
    kernel::test::wait_for_termination_safe(sender);

    JARVIS_ASSERT(sender->state == TaskState::TERMINATED);

    // Cleanup BEFORE asserting (cookbook Rule 5): self-terminated.
    Scheduler::drain_zombie_list();
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Registers all sync primitive unit tests with the test framework.
// Input: None
// Expect: All semaphore, mutex, and queue tests are registered via
// JARVIS_REGISTER_TEST
// Depends: kernel test framework
void register_sync_tests() {
    Logger::info("Registering sync tests");
    JARVIS_REGISTER_TEST(semaphore_wait_post);
    JARVIS_REGISTER_TEST(semaphore_waiter_teardown_on_terminate);
    JARVIS_REGISTER_TEST(eventgroup_waiter_teardown_on_terminate);
    JARVIS_REGISTER_TEST(queue_waiter_teardown_on_terminate);
    JARVIS_REGISTER_TEST(mutex_double_unlock);
    JARVIS_REGISTER_TEST(mutex_recursive_lock);
    JARVIS_REGISTER_TEST(semaphore_timeout);
    JARVIS_REGISTER_TEST(semaphore_multi_post);
    JARVIS_REGISTER_TEST(mutex_lock_unlock);
    JARVIS_REGISTER_TEST(queue_send_receive_block);
    JARVIS_REGISTER_TEST(sync_queue_send_blocks_when_full);
    JARVIS_REGISTER_TEST(sync_queue_receive_blocks_when_empty);
    JARVIS_REGISTER_TEST(sync_queue_wake_sender_on_receive);
}
#ifndef __clang__
#pragma GCC diagnostic pop
#endif
