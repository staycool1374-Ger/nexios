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

/// @file test_queue_pip.cpp
/// @brief Priority Inheritance Protocol tests for sync::Queue.
///
///        v0.3.10 rework (SIMULATED → DRIVEN): the sender and receiver are
///        REAL kernel tasks (prio ≥ 11) dispatched by the real timer ISR.
///        One blocks on the queue (full send / empty receive) while the
///        other holds the queue — the PIP boost is observed through genuine
///        blocking, never through set_current impersonation.

#include <test.hpp>
#include <logger.hpp>
#include <kernel/sync/queue.hpp>
#include <kernel/task/task.hpp>
#include <kernel/task/scheduler.hpp>
#include "test_sched_helpers.hpp"

using namespace kernel;

namespace {

/// @brief  Context for a task lambda (captureless lambdas only).
struct QueueCtx {
    uint64_t queue_;
    uint64_t out_;
};

/// @brief  Create a REAL kernel task (prio ≥ 11) whose lambda calls
///         @p fn(queue_ptr, out_ptr).  Dispatch and wait for BLOCKED state.
/// @param  fn  Function: `void(TaskControlBlock *self, sync::Queue *q,
///         uint64_t *out)`.
template <void (*Fn)(TaskControlBlock *, sync::Queue *, uint64_t *)>
TaskControlBlock *spawn_queue_task(sync::Queue &queue, uint64_t prio,
                                   QueueCtx &ctx, uint64_t *out) {
    ctx.queue_ = reinterpret_cast<uint64_t>(&queue);
    ctx.out_ = reinterpret_cast<uint64_t>(out);
    auto *t = TaskControlBlock::create(
        []() {
            auto *self = Scheduler::current_task();
            auto *c = reinterpret_cast<QueueCtx *>(self->user_data);
            auto *q = reinterpret_cast<sync::Queue *>(c->queue_);
            auto *o = reinterpret_cast<uint64_t *>(c->out_);
            Fn(self, q, o);
        },
        prio, 10);
    if (t == nullptr)
        return nullptr;
    t->user_data = &ctx;
    Scheduler::add_task(*t);
    Scheduler::reschedule();
    while (t->state != TaskState::BLOCKED)
        arch::pause();
    return t;
}

/// @brief  Create a REAL kernel task (prio ≥ 11) whose lambda calls a
///         NON-blocking @p fn, then wait for genuine termination.
template <void (*Fn)(TaskControlBlock *, sync::Queue *, uint64_t *)>
TaskControlBlock *spawn_queue_task_nonblocking(sync::Queue &queue,
                                               uint64_t prio, QueueCtx &ctx,
                                               uint64_t *out) {
    ctx.queue_ = reinterpret_cast<uint64_t>(&queue);
    ctx.out_ = reinterpret_cast<uint64_t>(out);
    auto *t = TaskControlBlock::create(
        []() {
            auto *self = Scheduler::current_task();
            auto *c = reinterpret_cast<QueueCtx *>(self->user_data);
            auto *q = reinterpret_cast<sync::Queue *>(c->queue_);
            auto *o = reinterpret_cast<uint64_t *>(c->out_);
            Fn(self, q, o);
        },
        prio, 10);
    if (t == nullptr)
        return nullptr;
    t->user_data = &ctx;
    Scheduler::add_task(*t);
    Scheduler::reschedule();
    kernel::test::wait_for_termination_safe(t);
    return t;
}

} // namespace

// ---------------------------------------------------------------------------
// Queue PIP — boost sender when high-pri receiver blocks on empty queue
// ---------------------------------------------------------------------------
// Low-pri sender (11) sends, then high-pri receiver (20) blocks on empty
// queue.  The last sender's priority is boosted to the receiver's priority.
static void recv_blocks_body(TaskControlBlock *, sync::Queue *q,
                             uint64_t *out) {
    uint8_t buf[32];
    size_t sz = sizeof(buf);
    bool ok = q->receive(buf, &sz); // blocks while the queue is empty
    __atomic_store_n(out, ok ? 1 : 0, __ATOMIC_RELEASE);
}

static void send_body(TaskControlBlock *, sync::Queue *q, uint64_t *out) {
    bool ok = q->try_send(reinterpret_cast<const uint8_t *>("x"), 1);
    __atomic_store_n(out, ok ? 1 : 0, __ATOMIC_RELEASE);
}

JARVIS_TEST(queue_pip_boost_sender, "PRE: none | POST: none") {
    sync::Queue queue;
    queue.init();

    // Low sender (prio 11) genuinely sends a message.
    QueueCtx sctx;
    uint64_t sent = 0;
    auto *low = spawn_queue_task_nonblocking<send_body>(queue, 11, sctx, &sent);
    JARVIS_ASSERT(low != nullptr);
    (void)sent;
    JARVIS_ASSERT(queue.available() == 1);

    // High receiver (prio 20) genuinely blocks on the (now empty after
    // drain) queue — drain first so receive blocks.
    uint8_t buf[32];
    size_t sz = sizeof(buf);
    JARVIS_ASSERT(queue.try_receive(buf, &sz));
    JARVIS_ASSERT(queue.available() == 0);

    uint64_t recvd = 0;
    QueueCtx rctx;
    auto *high = spawn_queue_task<recv_blocks_body>(queue, 20, rctx, &recvd);
    JARVIS_ASSERT(high != nullptr);
    JARVIS_ASSERT(high->state == TaskState::BLOCKED);

    // The last sender (low, prio 11) is boosted to the receiver's priority.
    JARVIS_ASSERT(low->priority >= high->priority);

    // Low sends to unblock high.
    JARVIS_ASSERT(queue.try_send(reinterpret_cast<const uint8_t *>("y"), 1));
    kernel::test::wait_for_termination_safe(high);


    kernel::test::terminate_and_drain2(low, high);
    JARVIS_ASSERT(recvd == 1);
    JARVIS_TEST_PASS();
}

// ---------------------------------------------------------------------------
// Queue PIP — boost receiver when high-pri sender blocks on full queue
// ---------------------------------------------------------------------------
// Low-pri receiver (11) receives, then queue filled, high-pri sender (20)
// blocks on full queue.  The last receiver's priority is boosted to the
// sender's priority.
static void send_blocks_body(TaskControlBlock *, sync::Queue *q,
                             uint64_t *out) {
    bool ok = q->send(reinterpret_cast<const uint8_t *>("blocked"), 7);
    __atomic_store_n(out, ok ? 1 : 0, __ATOMIC_RELEASE);
}

static void recv_body(TaskControlBlock *, sync::Queue *q, uint64_t *out) {
    uint8_t buf[32];
    size_t sz = sizeof(buf);
    bool ok = q->try_receive(buf, &sz);
    __atomic_store_n(out, ok ? 1 : 0, __ATOMIC_RELEASE);
}

JARVIS_TEST(queue_pip_boost_receiver, "PRE: none | POST: none") {
    sync::Queue queue;
    queue.init();

    // Low receiver (prio 11) genuinely receives one seed message.
    JARVIS_ASSERT(queue.try_send(reinterpret_cast<const uint8_t *>("s"), 1));
    QueueCtx rctx;
    uint64_t got = 0;
    auto *low =
        spawn_queue_task_nonblocking<recv_body>(queue, 11, rctx, &got);
    JARVIS_ASSERT(low != nullptr);
    (void)got;
    JARVIS_ASSERT(queue.available() == 0);

    // Fill the queue to capacity.
    for (size_t i = 0; i < sync::QUEUE_MAX_MSG_COUNT; ++i) {
        uint8_t d[4] = {static_cast<uint8_t>(i)};
        JARVIS_ASSERT(queue.try_send(d, 1));
    }
    JARVIS_ASSERT(queue.available() == sync::QUEUE_MAX_MSG_COUNT);

    // High sender (prio 20) genuinely blocks on the full queue.
    uint64_t done = 0;
    QueueCtx sctx;
    auto *high = spawn_queue_task<send_blocks_body>(queue, 20, sctx, &done);
    JARVIS_ASSERT(high != nullptr);
    JARVIS_ASSERT(high->state == TaskState::BLOCKED);

    // The last receiver (low, prio 11) is boosted to the sender's priority.
    JARVIS_ASSERT(low->priority >= high->priority);

    // Drain one → unblocks high (its blocked send completes).
    uint8_t buf[32];
    size_t sz = sizeof(buf);
    JARVIS_ASSERT(queue.try_receive(buf, &sz));
    kernel::test::wait_for_termination_safe(high);

    kernel::test::terminate_and_drain2(low, high);
    JARVIS_ASSERT(done == 1);
    JARVIS_TEST_PASS();
}

// ---------------------------------------------------------------------------
// Queue PIP — multiple senders, boost highest
// ---------------------------------------------------------------------------
// Two low-pri senders (11, 14) send.  Queue drained.  High-pri receiver (20)
// blocks on empty queue.  The last sender (prio 14) is boosted.
JARVIS_TEST(queue_pip_multiple_senders, "PRE: none | POST: none") {
    sync::Queue queue;
    queue.init();

    // Low1 (prio 11) and Low2 (prio 14) genuinely send.
    QueueCtx s1ctx;
    uint64_t s1done = 0;
    auto *low1 =
        spawn_queue_task_nonblocking<send_body>(queue, 11, s1ctx, &s1done);
    JARVIS_ASSERT(low1 != nullptr);

    QueueCtx s2ctx;
    uint64_t s2done = 0;
    auto *low2 =
        spawn_queue_task_nonblocking<send_body>(queue, 14, s2ctx, &s2done);
    JARVIS_ASSERT(low2 != nullptr);

    JARVIS_ASSERT(queue.available() == 2);

    // Drain the queue so receive will block.
    uint8_t buf[32];
    size_t sz = sizeof(buf);
    JARVIS_ASSERT(queue.try_receive(buf, &sz));
    JARVIS_ASSERT(queue.try_receive(buf, &sz));
    JARVIS_ASSERT(queue.available() == 0);

    // High receiver (prio 20) blocks on the empty queue → the last sender
    // (low2, prio 14) is boosted.
    uint64_t recvd = 0;
    QueueCtx rctx;
    auto *high = spawn_queue_task<recv_blocks_body>(queue, 20, rctx, &recvd);
    JARVIS_ASSERT(high != nullptr);
    JARVIS_ASSERT(high->state == TaskState::BLOCKED);
    JARVIS_ASSERT(low2->priority >= high->priority);

    // Send to unblock.
    JARVIS_ASSERT(queue.try_send(reinterpret_cast<const uint8_t *>("z"), 1));
    kernel::test::wait_for_termination_safe(high);

    JARVIS_ASSERT(recvd == 1);
    kernel::test::terminate_and_drain3(high, low2, low1);
    JARVIS_TEST_PASS();
}

void register_queue_pip_tests() {
    Logger::info("Registering Queue PIP tests");
    JARVIS_REGISTER_TEST(queue_pip_boost_sender);
    JARVIS_REGISTER_TEST(queue_pip_boost_receiver);
    JARVIS_REGISTER_TEST(queue_pip_multiple_senders);
}
