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

/// @file test_ipc_inv4b.cpp
/// @brief Cycle D QE (issue #545): INV-4b in-guard try-once probe + loop
///        gate tests (ipc class).  No production code touched.
///
///        Scope note (pinned): the try-once probe *race* (a reply landing
///        between the loop-top check and the publish) is timing-only and
///        has no deterministic in-harness driver — it is covered by the
///        soak.  These tests pin the deterministic legs: loop-top
///        consume-without-blocking, the gate-denied take skip, the
///        Contended probe outcome + waker pairing, and the block_sender
///        atomic release.  Probe/wake-gate counters are logged; the race
///        legs stay open as #545 follow-ups, not gates.

#include <test.hpp>
#include <logger.hpp>
#include <kernel/arch/hal/irq_guard.hpp>
#include <kernel/arch/cpu_context.hpp>
#include <kernel/ipc/ipc.hpp>
#include <kernel/sync/park_lock.hpp>
#include <kernel/task/scheduler.hpp>
#include <kernel/task/task.hpp>
#include "test_sched_helpers.hpp"
#include "task_ptr.hpp"

using namespace kernel;

namespace ipc_inv4b {

// --- probe_hit rendezvous: receiver R + sender S ---
TaskControlBlock *g_probe_receiver = nullptr;
volatile bool g_probe_sender_ok = false;
volatile uint64_t g_probe_sender_reply_type = 0;

void probe_receiver_entry() {
    Message msg;
    bool ok = false;
    for (int i = 0; i < 100000 && !ok; ++i)
        ok = IPC::recv(msg);
    if (!ok)
        return;
    // No reply needed: the sender's wait was already satisfied by the
    // pre-queued message.  Reply send is best-effort (the sender may
    // already have terminated).
    Message reply;
    reply.sender_id = Scheduler::current_task()->id;
    reply.type = 99;
    reply.priority = 0;
    reply.data_size = 0;
    (void)IPC::send(msg.sender_id, reply);
}

void probe_sender_entry() {
    auto *self = Scheduler::current_task();
    (void)self;
    Message msg;
    msg.sender_id = self->id;
    msg.type = 42;
    msg.priority = 0;
    msg.data_size = 0;
    Message reply;
    const bool ok =
        IPC::send_sync(g_probe_receiver->id, msg, reply);
    if (ok && reply.type == 77) {
        g_probe_sender_ok = true;
        g_probe_sender_reply_type = reply.type;
    }
}

// --- contended rendezvous state ---
TaskControlBlock *g_cont_waiter = nullptr;
volatile bool g_cont_holder_has_lock = false;
volatile bool g_cont_probed = false;
volatile bool g_cont_probe_was_contended = false;
volatile bool g_cont_pushed = false;
volatile bool g_cont_waiter_got = false;

void cont_holder_entry() {
    if (g_cont_waiter == nullptr)
        return;
    g_cont_waiter->msg_queue.lock_.lock();
    g_cont_holder_has_lock = true;
    // Hold across yields so the waiter observes Contended (INV-C1 WARN
    // is warn-only here — expected, proves the check is live).
    for (uint64_t i = 0; i < 50000ULL && !g_cont_probed; ++i)
        Scheduler::reschedule();
    g_cont_waiter->msg_queue.lock_.unlock();
    Message msg;
    msg.sender_id = Scheduler::current_task()->id;
    msg.type = 55;
    msg.priority = 0;
    msg.data_size = 0;
    if (g_cont_waiter->msg_queue.push(msg)) {
        Scheduler::set_task_ready(*g_cont_waiter);
        g_cont_pushed = true;
    }
}

void cont_waiter_entry() {
    auto *self = Scheduler::current_task();
    for (uint64_t i = 0; i < 200000ULL && !g_cont_holder_has_lock; ++i) {
        Scheduler::reschedule();
        arch::hlt();
    }
    if (!g_cont_holder_has_lock)
        return;
    {
        Message out;
        const MessageQueue::QueueProbe probe =
            self->msg_queue.probe_pop(out);
        g_cont_probed = true;
        g_cont_probe_was_contended =
            (probe == MessageQueue::QueueProbe::Contended);
    }
    for (uint64_t i = 0; i < 200000ULL && !g_cont_pushed; ++i) {
        arch::sti();
        arch::hlt();
        arch::cli();
    }
    if (!g_cont_pushed)
        return;
    Message got;
    if (self->msg_queue.pop(got) && got.type == 55)
        g_cont_waiter_got = true;
}

} // namespace ipc_inv4b

// Runmode: kernel
// Testidea: Cycle D QE (issue #545, INV-4b): a reply queued before the
// send_sync wait is consumed without blocking — no park, no fallback,
// no watchdog involvement.
// Input: Harness pre-queues a type-77 message from R into S's inbox via
// the queue push; S send_syncs to R (R recvs, best-effort reply).
// Expect: send_sync returns OK with the pre-queued reply; S never
// BLOCKED (resched_park entries delta 0); probe/wake-gate deltas logged
// (the in-guard race probe is timing-only — loop-top consumes first, so
// no hit is *required* here).  Both tasks terminate, tracker delta 0.
// Depends: IPC::send_sync loop-top fast path, queue push, park counters
JARVIS_TEST(ipc_send_sync_probe_hit, "PRE: none | POST: none") {
    ipc_inv4b::g_probe_receiver = nullptr;
    ipc_inv4b::g_probe_sender_ok = false;
    ipc_inv4b::g_probe_sender_reply_type = 0;
    const uint64_t e0 = Scheduler::resched_park_entries_count();
    const uint64_t hit0 = IPC::ipc_probe_hit_count();
    const uint64_t cont0 = IPC::ipc_probe_contended_count();
    const uint64_t pend0 = IPC::ipc_block_with_pending_inbox_count();
    auto *receiver = TaskControlBlock::create(
        ipc_inv4b::probe_receiver_entry, 11, 10);
    JARVIS_ASSERT(receiver != nullptr);
    JARVIS_ASSERT(Scheduler::set_sched_policy(*receiver, SchedPolicy::FIXED));
    auto *sender = TaskControlBlock::create(ipc_inv4b::probe_sender_entry,
                                            12, 5);
    JARVIS_ASSERT(sender != nullptr);
    JARVIS_ASSERT(Scheduler::set_sched_policy(*sender, SchedPolicy::FIXED));
    ipc_inv4b::g_probe_receiver = receiver;
    // Pre-queue the reply before either task runs (deterministic: no
    // dispatch happens between create and the handshake drive below).
    {
        Message early;
        early.sender_id = receiver->id;
        early.type = 77;
        early.priority = 0;
        early.data_size = 0;
        JARVIS_ASSERT(sender->msg_queue.push(early));
    }
    {
        arch::IrqGuard guard;
        Scheduler::add_task(*receiver);
        Scheduler::add_task(*sender);
    }
    test::yield_as(*receiver);
    Scheduler::reschedule();
    test::wait_for_termination_safe(sender);
    test::wait_for_termination_safe(receiver);
    Logger::info("[IPC-INV4B] probe_hit ok=%lu type=%lu entries=%lu "
                 "hit=%lu cont=%lu pend=%lu",
                 static_cast<uint64_t>(ipc_inv4b::g_probe_sender_ok),
                 ipc_inv4b::g_probe_sender_reply_type,
                 Scheduler::resched_park_entries_count() - e0,
                 IPC::ipc_probe_hit_count() - hit0,
                 IPC::ipc_probe_contended_count() - cont0,
                 IPC::ipc_block_with_pending_inbox_count() - pend0);
    JARVIS_ASSERT(ipc_inv4b::g_probe_sender_ok);
    JARVIS_ASSERT_EQ(77ULL, ipc_inv4b::g_probe_sender_reply_type);
    // Never blocked: the wait was satisfied without arming.
    JARVIS_ASSERT_EQ(e0, Scheduler::resched_park_entries_count());
    test::terminate_and_drain2(sender, receiver);
    ipc_inv4b::g_probe_receiver = nullptr;
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Cycle D QE (issue #545, INV-4b): the gate-denied take skip —
// a BLOCKED caller whose reschedule park is denied completes a
// pop_from_sender without touching the slow path.
// Input: Pre-queued message from peer P in consumer queue; harness
// forced BLOCKED inside IrqGuard calls pop_from_sender (site
// mq:pop_from_sender).
// Expect: Pops the message (fast take); fallback delta 0; state restored
// RUNNING; IF preserved (0 inside, 1 after).
// Depends: MessageQueue::pop_from_sender fast path, fallback counter
JARVIS_TEST(ipc_blocked_no_take, "PRE: none | POST: none") {
    SimpleTaskPtr peer(TaskControlBlock::create(test::forever_entry, 5, 10));
    JARVIS_ASSERT(peer);
    SimpleTaskPtr consumer(
        TaskControlBlock::create(test::forever_entry, 5, 10));
    JARVIS_ASSERT(consumer);
    {
        Message m;
        m.sender_id = peer->id;
        m.type = 61;
        m.priority = 0;
        m.data_size = 0;
        JARVIS_ASSERT(consumer->msg_queue.push(m));
    }
    const uint64_t fb0 = sync::lock_spin_fallback_count();
    auto *self = Scheduler::current_task();
    JARVIS_ASSERT(self != nullptr);
    bool got = false;
    Message out;
    {
        arch::IrqGuard irq_guard{};
        self->state = TaskState::BLOCKED;
        got = consumer->msg_queue.pop_from_sender(out, peer->id);
        JARVIS_ASSERT(self->state == TaskState::BLOCKED);
        self->state = TaskState::RUNNING;
    }
    JARVIS_ASSERT(got);
    JARVIS_ASSERT_EQ(61ULL, out.type);
    JARVIS_ASSERT_EQ(fb0, sync::lock_spin_fallback_count());
    JARVIS_ASSERT(self->state == TaskState::RUNNING);
    JARVIS_ASSERT(arch::interrupts_enabled());
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Cycle D QE (issue #545, INV-4b): Contended probe outcome —
// a waiter probing a lock held by a live holder observes Contended with
// no side effects, then is woken by the holder's push (no lost wakeup).
// Input: Holder H takes W's queue lock raw, yields until W probes;
// releases, pushes a message, readies W.
// Expect: Probe == Contended; W receives the pushed message;
// ipc_wake_gate_missed_count() == 0 (no lost-wakeup window); both
// terminate, tracker delta 0.
// Depends: QueueProbe::Contended path, push/wake pairing, wake-gate probe
JARVIS_TEST(ipc_probe_contended_wake, "PRE: none | POST: none") {
    ipc_inv4b::g_cont_waiter = nullptr;
    ipc_inv4b::g_cont_holder_has_lock = false;
    ipc_inv4b::g_cont_probed = false;
    ipc_inv4b::g_cont_probe_was_contended = false;
    ipc_inv4b::g_cont_pushed = false;
    ipc_inv4b::g_cont_waiter_got = false;
    const uint64_t miss0 = IPC::ipc_wake_gate_missed_count();
    auto *waiter = TaskControlBlock::create(ipc_inv4b::cont_waiter_entry,
                                            12, 10);
    JARVIS_ASSERT(waiter != nullptr);
    JARVIS_ASSERT(Scheduler::set_sched_policy(*waiter, SchedPolicy::FIXED));
    auto *holder = TaskControlBlock::create(ipc_inv4b::cont_holder_entry,
                                            11, 10);
    JARVIS_ASSERT(holder != nullptr);
    JARVIS_ASSERT(Scheduler::set_sched_policy(*holder, SchedPolicy::FIXED));
    ipc_inv4b::g_cont_waiter = waiter;
    {
        arch::IrqGuard guard;
        Scheduler::add_task(*waiter);
        Scheduler::add_task(*holder);
    }
    Scheduler::reschedule();
    test::wait_for_termination_safe(waiter);
    test::wait_for_termination_safe(holder);
    Logger::info("[IPC-INV4B] contended probed=%lu was_contended=%lu "
                 "got=%lu miss=%lu",
                 static_cast<uint64_t>(ipc_inv4b::g_cont_probed),
                 static_cast<uint64_t>(
                     ipc_inv4b::g_cont_probe_was_contended),
                 static_cast<uint64_t>(ipc_inv4b::g_cont_waiter_got),
                 IPC::ipc_wake_gate_missed_count() - miss0);
    JARVIS_ASSERT(ipc_inv4b::g_cont_holder_has_lock);
    JARVIS_ASSERT(ipc_inv4b::g_cont_probed);
    JARVIS_ASSERT(ipc_inv4b::g_cont_probe_was_contended);
    JARVIS_ASSERT(ipc_inv4b::g_cont_pushed);
    JARVIS_ASSERT(ipc_inv4b::g_cont_waiter_got);
    JARVIS_ASSERT_EQ(miss0, IPC::ipc_wake_gate_missed_count());
    test::terminate_and_drain2(waiter, holder);
    ipc_inv4b::g_cont_waiter = nullptr;
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Cycle D QE (issue #545, F1): block_sender releases the queue
// lock under the caller's IrqGuard — BLOCKED-marking and unlock are one
// atomic IF=0 step, so a tick cannot preempt between them and strand the
// lock with the task descheduled.
// Input: Two queued-but-undispatched tasks (no reschedule between create
// and block_sender, so no dispatch can interleave); block_sender(S).
// Expect: S BLOCKED + linked; q.lock_ free (try_lock succeeds);
// spinlock_held_by_current_count() == 0; wake_sender rolls back cleanly
// (head null, S READY); tasks drain, tracker delta 0.
// Depends: IPC::block_sender release, order-table census, wake_sender
JARVIS_TEST(lock_park_block_sender_release, "PRE: none | POST: none") {
    sync::spinlock_order_reset_for_test();
    auto *owner = TaskControlBlock::create(test::forever_entry, 5, 10);
    JARVIS_ASSERT(owner != nullptr);
    auto *sender = TaskControlBlock::create(test::forever_entry, 5, 10);
    JARVIS_ASSERT(sender != nullptr);
    {
        arch::IrqGuard guard;
        Scheduler::add_task(*owner);
        Scheduler::add_task(*sender);
    }
    MessageQueue &q = owner->msg_queue;
    JARVIS_ASSERT(q.blocked_senders_head == nullptr);
    {
        arch::IrqGuard caller_guard{};
        const bool blocked = IPC::block_sender(q, *sender);
        JARVIS_ASSERT(blocked);
        // Released under the caller's guard: the lock is free and the
        // census shows nothing held — even though the caller is BLOCKED.
        JARVIS_ASSERT(q.lock_.try_lock());
        q.lock_.unlock();
        JARVIS_ASSERT_EQ(0ULL, sync::spinlock_held_by_current_count());
    }
    JARVIS_ASSERT(sender->state == TaskState::BLOCKED);
    JARVIS_ASSERT(q.blocked_senders_head == sender);
    IPC::wake_sender(q, *owner);
    JARVIS_ASSERT(q.blocked_senders_head == nullptr);
    JARVIS_ASSERT(sender->state == TaskState::READY);
    test::terminate_and_drain2(owner, sender);
    sync::spinlock_order_reset_for_test();
    JARVIS_TEST_PASS();
}

void register_ipc_inv4b_tests() {
    JARVIS_REGISTER_TEST(ipc_send_sync_probe_hit);
    JARVIS_REGISTER_TEST(ipc_blocked_no_take);
    JARVIS_REGISTER_TEST(ipc_probe_contended_wake);
    JARVIS_REGISTER_TEST(lock_park_block_sender_release);
}
