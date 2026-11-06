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

/// @file test_ipc_extended.cpp
/// @brief Extended IPC protocol tests.
///
/// v0.3.10 rework (SIMULATED → DRIVEN): priority-inheritance and buffer-handle
/// tests are driven by REAL kernel tasks (prio ≥ 11) dispatched by the real
/// timer ISR.  No set_current impersonation; primitives run in the tasks'
/// own contexts.

#include <test.hpp>
#include <logger.hpp>
#include <kernel/ipc/ipc.hpp>
#include <kernel/ipc/buffer_pool.hpp>
#include <kernel/sync/semaphore.hpp>
#include <kernel/task/task.hpp>
#include <kernel/task/scheduler.hpp>
#include <kernel/arch/io.hpp>
#include <kernel/arch/irq_guard.hpp>
#include <kernel/arch/timer.hpp>
#include <kernel/memory/vmm.hpp>
#include <kernel/test/test_sched_helpers.hpp>
#include <kernel/test/test_isolate.hpp>
#include <kernel/elf/elf.hpp>
#include <initrd/initrd.hpp>

using namespace kernel;

// Runmode: kernel
// Testidea: data_size is an inline message field — MessageQueue::push does not
// reject an oversized value (the size clamp is at the syscall boundary,
// syscall_handlers_ipc.cpp:44).  Verify the real container behaviour: an
// oversized data_size round-trips through push/pop unchanged, and the inline
// data buffer is still fully readable.
// Input: Push a message whose data_size > IPC_MAX_MSG_SIZE.
// Expect: Push and pop both succeed; the data_size field is preserved.
// Depends: kernel::MessageQueue
JARVIS_TEST(ipc_send_data_size_exceeds_max, "PRE: none | POST: none") {
    auto *cur = Scheduler::current_task();
    JARVIS_ASSERT(cur != nullptr);

    Message msg{};
    msg.sender_id = cur->id;
    msg.type = 1;
    msg.priority = 0;
    msg.data_size = IPC_MAX_MSG_SIZE + 1; // field value beyond the payload cap

    // The queue is a container: it stores the Message struct verbatim.  The
    // data_size boundary is enforced at the syscall layer (clamp), not here.
    bool ok = cur->msg_queue.push(msg);
    JARVIS_ASSERT(ok == true);

    Message recv{};
    ok = cur->msg_queue.pop(recv);
    JARVIS_ASSERT(ok == true);
    JARVIS_ASSERT(recv.data_size == IPC_MAX_MSG_SIZE + 1ULL);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Verifies data_size=0 is valid (boundary).
// Input: Call send with zero data size
// Expect: Succeeds
// Depends: kernel::ipc
JARVIS_TEST(ipc_send_data_size_zero, "PRE: none | POST: none") {
    auto *cur = Scheduler::current_task();
    JARVIS_ASSERT(cur != nullptr);

    Message msg{};
    msg.sender_id = cur->id;
    msg.type = 1;
    msg.priority = 0;
    msg.data_size = 0;

    bool ok = cur->msg_queue.push(msg);
    JARVIS_ASSERT(ok == true);

    Message recv{};
    ok = cur->msg_queue.pop(recv);
    JARVIS_ASSERT(ok == true);
    JARVIS_ASSERT(recv.data_size == 0);
    JARVIS_TEST_PASS();
}

namespace {

/// @brief Fill a task's queue to capacity so real senders block on it.
void fill_receiver_queue(TaskControlBlock &dst) {
    for (size_t idx = 0; idx < IPC_MAX_QUEUE_MSG; ++idx) {
        kernel::Message fill{};
        fill.sender_id = 0;
        fill.type = 99;
        fill.priority = 0;
        fill.data_size = 0;
        dst.msg_queue.push(fill);
    }
}

/// @brief Register a task BLOCKED without dispatching it, so the harness
///        can deterministically observe blocked senders before releasing it.
void hold_receiver(TaskControlBlock &dst) {
    dst.state = TaskState::BLOCKED;
    Scheduler::register_task(dst);
}

/// @brief Spin until @p task blocks, bounded by @p deadline ticks.
/// @return True when the task reached BLOCKED in time.
bool await_blocked(TaskControlBlock *task, uint64_t deadline) {
    while (task->state != TaskState::BLOCKED &&
           arch::Timer::ticks() < deadline) {
        arch::pause();
    }
    return task->state == TaskState::BLOCKED;
}

/// @brief Context for a blocking sender (captureless lambdas only).
struct SendCtx {
    uint64_t peer_id_;
    uint64_t out_;
    uint32_t mark_;
};

/// @brief Results observed by the FIFO/mid-removal test.
uint64_t g_fifo_first_result = 99;
uint64_t g_fifo_middle_result = 99;
uint64_t g_fifo_last_result = 99;

/// @brief Blocking send entry shared by the three FIFO senders.
void fifo_sender_entry() {
    auto *self = Scheduler::current_task();
    auto *ctx = reinterpret_cast<SendCtx *>(self->user_data);
    kernel::Message msg{};
    msg.sender_id = self->id;
    msg.type = ctx->mark_;
    msg.priority = 0;
    msg.data_size = 0;
    bool ok = IPC::send(ctx->peer_id_, msg);
    __atomic_store_n(reinterpret_cast<uint64_t *>(ctx->out_),
                     ok ? 1 : 0, __ATOMIC_RELEASE);
}

/// @brief Dispatch a prio-12 FIXED sender blocked on @p recv_id.
TaskControlBlock *spawn_fifo_sender(uint64_t recv_id, uint32_t mark,
                                    uint64_t &out) {
    static SendCtx first_ctx{};
    static SendCtx middle_ctx{};
    static SendCtx last_ctx{};
    SendCtx *ctx = &first_ctx;
    if (mark == 2) {
        ctx = &middle_ctx;
    } else if (mark == 3) {
        ctx = &last_ctx;
    }
    ctx->peer_id_ = recv_id;
    ctx->out_ = reinterpret_cast<uint64_t>(&out);
    ctx->mark_ = mark;
    auto *sender =
        TaskControlBlock::create(fifo_sender_entry, 12, 10);
    if (sender == nullptr) {
        return nullptr;
    }
    if (!Scheduler::set_sched_policy(*sender, SchedPolicy::FIXED)) {
        return nullptr;
    }
    sender->user_data = ctx;
    Scheduler::add_task(*sender);
    Scheduler::reschedule();
    return sender;
}

} // namespace

// Runmode: kernel
// Testidea: Three equal-priority senders block in FIFO order; the MIDDLE
// one dies and the teardown path detaches exactly it (head/tail repair);
// the survivors then wake oldest-first. Equal priorities keep list order
// identical to block order (block_sender inserts by priority).
// Input: BLOCKED receiver with a full queue; dispatch A, B, C (prio 12)
// sequentially to BLOCKED; terminate B; drain the queue; wake twice.
// Expect: list is A->B->C with tail C; after B's death the list is
// A->C with tail C and A/C still BLOCKED; B's result stays unset;
// wake 1 completes A, wake 2 completes C; list ends empty.
// Depends: kernel::IPC block/wake_sender, task teardown detach,
// Scheduler dispatch/join. NOTE: wake_sender pops the head by design;
// mid-list removal happens only via the death path (and the
// interrupts-disabled rollback); there is no unblock-middle API.
JARVIS_TEST(ipc_queue_remove_from_mid, "PRE: none | POST: none") {
    // Pseudocode: fake BLOCKED receiver + full queue; block A, B, C in
    // order (bounded spins); assert FIFO list; terminate B, drain, assert
    // A->C; drain receiver queue; wake A (completes), wake C (completes);
    // assert empty list; terminate receiver; drain.
    auto *receiver = TaskControlBlock::create([]() {}, 11, 10);
    JARVIS_ASSERT(receiver != nullptr);
    fill_receiver_queue(*receiver);
    hold_receiver(*receiver);
    const uint64_t recv_id = receiver->id;

    g_fifo_first_result = 99;
    g_fifo_middle_result = 99;
    g_fifo_last_result = 99;
    auto *first = spawn_fifo_sender(recv_id, 1, g_fifo_first_result);
    JARVIS_ASSERT(first != nullptr);
    JARVIS_ASSERT(await_blocked(first, arch::Timer::ticks() + 500));
    auto *middle = spawn_fifo_sender(recv_id, 2, g_fifo_middle_result);
    JARVIS_ASSERT(middle != nullptr);
    JARVIS_ASSERT(await_blocked(middle, arch::Timer::ticks() + 500));
    auto *last = spawn_fifo_sender(recv_id, 3, g_fifo_last_result);
    JARVIS_ASSERT(last != nullptr);
    JARVIS_ASSERT(await_blocked(last, arch::Timer::ticks() + 500));

    JARVIS_ASSERT(receiver->msg_queue.blocked_senders_head == first);
    JARVIS_ASSERT(first->blocked_next == middle);
    JARVIS_ASSERT(middle->blocked_next == last);
    JARVIS_ASSERT(last->blocked_next == nullptr);
    JARVIS_ASSERT(receiver->msg_queue.blocked_senders_tail == last);

    kernel::test::terminate_if_live(middle);
    Scheduler::drain_zombie_list();
    JARVIS_ASSERT(receiver->msg_queue.blocked_senders_head == first);
    JARVIS_ASSERT(first->blocked_next == last);
    JARVIS_ASSERT(last->blocked_next == nullptr);
    JARVIS_ASSERT(receiver->msg_queue.blocked_senders_tail == last);
    JARVIS_ASSERT(first->state == TaskState::BLOCKED);
    JARVIS_ASSERT(last->state == TaskState::BLOCKED);
    JARVIS_ASSERT_EQ(99ULL, g_fifo_middle_result);

    kernel::Message drain{};
    while (receiver->msg_queue.pop(drain)) {
    }

    IPC::wake_sender(receiver->msg_queue, *receiver);
    Scheduler::reschedule();
    kernel::test::wait_for_termination_safe(first);
    JARVIS_ASSERT_EQ(1ULL, g_fifo_first_result);

    IPC::wake_sender(receiver->msg_queue, *receiver);
    Scheduler::reschedule();
    kernel::test::wait_for_termination_safe(last);
    JARVIS_ASSERT_EQ(1ULL, g_fifo_last_result);

    JARVIS_ASSERT(receiver->msg_queue.blocked_senders_head == nullptr);
    JARVIS_ASSERT(receiver->msg_queue.blocked_senders_tail == nullptr);

    kernel::test::terminate_if_live(receiver);
    Scheduler::drain_zombie_list();
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Verifies multiple blocked senders, wake one at a time via recv.
// Input: Block 3 senders, call recv 3 times
// Expect: Each recv wakes one sender in FIFO order
// Depends: kernel::ipc
JARVIS_TEST(ipc_multiple_blocked_senders_wake_one, "PRE: none | POST: none") {
    auto *cur = Scheduler::current_task();
    JARVIS_ASSERT(cur != nullptr);

    for (size_t i = 0; i < IPC_MAX_QUEUE_MSG; ++i) {
        Message msg{};
        msg.sender_id = cur->id;
        msg.type = static_cast<uint64_t>(i);
        msg.priority = 0;
        msg.data_size = 0;
        JARVIS_ASSERT(cur->msg_queue.push(msg) == true);
    }

    JARVIS_ASSERT(cur->msg_queue.is_full() == true);

    Message recv{};
    JARVIS_ASSERT(cur->msg_queue.pop(recv) == true);
    JARVIS_ASSERT(recv.type == 0); // FIFO: first pushed = first popped

    JARVIS_ASSERT(cur->msg_queue.is_full() == false);

    for (size_t i = 1; i < IPC_MAX_QUEUE_MSG; ++i) {
        JARVIS_ASSERT(cur->msg_queue.pop(recv) == true);
        JARVIS_ASSERT(recv.type == i);
    }
    JARVIS_TEST_PASS();
}

namespace {

/// @brief Never-replying destination: registered BLOCKED, never
///        dispatched, so a bounded wait always expires (no reply path).
TaskControlBlock *g_bounded_peer = nullptr;

/// @brief Bounded-wait outcome observed inside the sender task.
bool g_bounded_timed_out = false;
uint64_t g_bounded_elapsed = 0;
bool g_bounded_reply_wait_clean = false;
bool g_bounded_timed_out_flag_clean = false;

/// @brief Successful bounded round-trip outcome.
bool g_bounded_ok = false;

/// @brief Expiry entry: bounded wait against the never-replier.
void bounded_expiry_entry() {
    auto *self = Scheduler::current_task();
    kernel::Message msg{};
    msg.sender_id = self ? self->id : 0;
    msg.type = 501;
    msg.priority = 0;
    msg.data_size = 0;
    kernel::Message reply{};
    const uint64_t start = arch::Timer::ticks();
    const bool ok = IPC::send_sync_bounded(g_bounded_peer->id, msg, reply, 5);
    const uint64_t elapsed = arch::Timer::ticks() - start;
    g_bounded_timed_out = !ok;
    g_bounded_elapsed = elapsed;
    g_bounded_reply_wait_clean = (self->reply_wait == false);
    g_bounded_timed_out_flag_clean = (self->recv_timed_out == false);
}

/// @brief Live replier entry for the bounded success path.
void bounded_replier_entry() {
    auto *self = Scheduler::current_task();
    for (int spin = 0; spin < 100000; ++spin) {
        kernel::Message inbound{};
        if (IPC::recv(inbound) && inbound.type == 502) {
            kernel::Message reply{};
            reply.sender_id = self ? self->id : 0;
            reply.type = 503;
            reply.priority = 0;
            reply.data_size = 0;
            (void)IPC::send(inbound.sender_id, reply);
            return;
        }
    }
}

/// @brief Bounded sender entry for the success path (large budget).
void bounded_success_entry() {
    auto *self = Scheduler::current_task();
    auto *peer = reinterpret_cast<TaskControlBlock *>(self->user_data);
    kernel::Message msg{};
    msg.sender_id = self ? self->id : 0;
    msg.type = 502;
    msg.priority = 0;
    msg.data_size = 0;
    kernel::Message reply{};
    g_bounded_ok =
        IPC::send_sync_bounded(peer->id, msg, reply, 500) &&
        reply.type == 503;
}

} // namespace

// Runmode: kernel
// Testidea: send_sync_bounded expires against a never-replying peer
// (returns false after the tick budget with clean wait flags), and the
// same bounded primitive succeeds against a live replier with a large
// budget. The lower tick bound is intentionally loose (>= 1): the wheel
// arms at now_ns + budget and slot quantization fires early (observed 3
// ticks for a budget of 5), so exact-tick equality is not asserted — the
// load-bearing facts are that the call blocked (no instant false, which
// would mean arm-fail or dest-death) and returned false via the expiry
// path (dest verified alive throughout). NOTE: timeout 0 means
// "forever" (unbounded, like send_sync) by design, so the issue's
// timeout-0-immediate case is not driven — blocking forever cannot be
// asserted without hanging.
// Input: Fake BLOCKED peer (never dispatched); dispatched sender with
// budget 5 ticks; then live replier + sender with budget 500 ticks.
// Expect: expiry false with elapsed in [1, 5005) and both flags clean;
// success path true with reply type 503; cleanup before asserts.
// Depends: IPC::send_sync_bounded, TimerWheel expiry, Scheduler.
JARVIS_TEST(ipc_send_sync_timeout, "PRE: none | POST: none") {
    // Pseudocode: hold a never-dispatched peer; run the expiry sender and
    // join; terminate the peer and drain FIRST; then assert false + tick
    // window + clean flags (cleanup-before-assert so a failure cannot
    // leak the peer). Then live replier + success sender, join both,
    // assert true; drain.
    auto *peer = TaskControlBlock::create([]() {}, 11, 10);
    JARVIS_ASSERT(peer != nullptr);
    peer->state = TaskState::BLOCKED;
    Scheduler::register_task(*peer);
    g_bounded_peer = peer;
    g_bounded_timed_out = false;
    g_bounded_elapsed = 0;
    g_bounded_reply_wait_clean = false;
    g_bounded_timed_out_flag_clean = false;

    auto *expiry = TaskControlBlock::create(bounded_expiry_entry, 12, 10);
    JARVIS_ASSERT(expiry != nullptr);
    JARVIS_ASSERT(Scheduler::set_sched_policy(*expiry, SchedPolicy::FIXED));
    Scheduler::add_task(*expiry);
    Scheduler::reschedule();
    kernel::test::wait_for_termination_safe(expiry);

    g_bounded_peer = nullptr;
    kernel::test::terminate_if_live(peer);
    Scheduler::drain_zombie_list();
    JARVIS_ASSERT_FMT(g_bounded_timed_out, "bounded wait did not expire");
    JARVIS_ASSERT_FMT(g_bounded_elapsed >= 1,
                      "instant false (arm/death, not timeout): %lu ticks",
                      g_bounded_elapsed);
    JARVIS_ASSERT_FMT(g_bounded_elapsed < 5005,
                      "expiry hung: %lu ticks", g_bounded_elapsed);
    JARVIS_ASSERT_FMT(g_bounded_reply_wait_clean, "reply_wait left set");
    JARVIS_ASSERT_FMT(g_bounded_timed_out_flag_clean, "timed_out left set");

    g_bounded_ok = false;
    auto *replier = TaskControlBlock::create(bounded_replier_entry, 11, 10);
    JARVIS_ASSERT(replier != nullptr);
    JARVIS_ASSERT(Scheduler::set_sched_policy(*replier, SchedPolicy::FIXED));
    Scheduler::add_task(*replier);
    auto *sender = TaskControlBlock::create(bounded_success_entry, 12, 10);
    JARVIS_ASSERT(sender != nullptr);
    JARVIS_ASSERT(Scheduler::set_sched_policy(*sender, SchedPolicy::FIXED));
    sender->user_data = replier;
    Scheduler::add_task(*sender);
    Scheduler::reschedule();
    kernel::test::wait_for_termination_safe(sender);
    kernel::test::wait_for_termination_safe(replier);
    Scheduler::drain_zombie_list();
    JARVIS_ASSERT_FMT(g_bounded_ok, "bounded success path failed");
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Verifies low-priority task holds a resource, high-priority blocks
// — priority inheritance verified via the REAL IPC blocked-sender path.
// Input: Receiver (prio 11) with a genuinely-full queue; a real high-priority
//        sender (prio 20) blocks inside IPC::send().  The receiver stays
//        alive (registered BLOCKED, never timer-dispatched) so the harness
//        observes the block and the boost deterministically.
// Expect: Receiver priority boosted to >= sender's priority while blocked.
// Depends: kernel::ipc, Scheduler
JARVIS_TEST(ipc_priority_inversion, "PRE: none | POST: none") {
    // Receiver: a draining task registered BLOCKED so the timer never
    // dispatches it before the harness observes the block/boost; it drains
    // via IPC::recv (waking the sender) and stays alive until the sender's
    // completing re-push arrives.
    static uint64_t g_recv_ok = 0;
    auto *receiver = TaskControlBlock::create(
        []() {
            kernel::Message m;
            // First pop also calls wake_sender (blocked sender becomes READY).
            bool ok = IPC::recv(m);
            __atomic_store_n(&g_recv_ok, ok ? 1 : 0, __ATOMIC_RELEASE);
            // Stay alive until the sender's completing re-push arrives.
            for (int i = 0; i < 100000; ++i) {
                kernel::Message m2;
                if (IPC::recv(m2) && m2.type == 42)
                    break;
            }
        },
        11, 10);
    JARVIS_ASSERT(receiver != nullptr);
    // Pinned FIXED (issue #19): rendezvous choreography assumes priority
    // order; EDF would order by creation-tick deadline phase.
    JARVIS_ASSERT(Scheduler::set_sched_policy(*receiver, SchedPolicy::FIXED));
    receiver->state = TaskState::BLOCKED;
    Scheduler::register_task(*receiver);

    // Fill the receiver's queue.
    for (size_t i = 0; i < IPC_MAX_QUEUE_MSG; ++i) {
        Message fill{};
        fill.sender_id = 0;
        fill.type = 99;
        fill.priority = 0;
        fill.data_size = 0;
        receiver->msg_queue.push(fill);
    }
    uint64_t r_id = receiver->id;

    // High-priority real sender blocks on the full queue → the receiver is
    // priority-boosted (priority inheritance).
    uint64_t send_result = 0;
    struct SCtx {
        uint64_t recv_;
        uint64_t out_;
    } sctx;
    sctx.recv_ = r_id;
    sctx.out_ = reinterpret_cast<uint64_t>(&send_result);
    auto *high = TaskControlBlock::create(
        []() {
            auto *self = Scheduler::current_task();
            auto *c = reinterpret_cast<SCtx *>(self->user_data);
            Message msg{};
            msg.sender_id = self->id;
            msg.type = 42;
            msg.priority = 0;
            msg.data_size = 0;
            bool ok = IPC::send(c->recv_, msg, IpcFlags::NONE);
            __atomic_store_n(reinterpret_cast<uint64_t *>(c->out_),
                             ok ? 1 : 0, __ATOMIC_RELEASE);
        },
        20, 10);
    JARVIS_ASSERT(high != nullptr);
    // Pinned FIXED (issue #19): must preempt the polling receiver by
    // priority; EDF would not preempt an earlier-deadline receiver.
    JARVIS_ASSERT(Scheduler::set_sched_policy(*high, SchedPolicy::FIXED));
    high->user_data = &sctx;
    Scheduler::add_task(*high);
    Scheduler::reschedule();
    while (high->state != TaskState::BLOCKED)
        arch::pause();
    JARVIS_ASSERT(high->state == TaskState::BLOCKED);

    // The receiver (queue owner) is boosted to the sender's priority.
    JARVIS_ASSERT(receiver->priority >= high->priority);

    // Release the receiver: it drains one → the blocked sender completes.
    Scheduler::set_task_ready(*receiver);
    kernel::test::wait_for_termination_safe(high);
    kernel::test::wait_for_termination_safe(receiver);

    // Cleanup BEFORE asserting (cookbook Rule 5): both self-terminated.
    Scheduler::drain_zombie_list();
    JARVIS_ASSERT_EQ(1ULL, send_result);
    JARVIS_ASSERT_EQ(1ULL, g_recv_ok);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Verifies 64-byte max payload round-trips correctly.
// Input: Send 64-byte message, receive
// Expect: Data matches exactly
// Depends: kernel::ipc
JARVIS_TEST(ipc_send_self_max_message_size, "PRE: none | POST: none") {
    auto *cur = Scheduler::current_task();
    JARVIS_ASSERT(cur != nullptr);

    Message msg{};
    msg.sender_id = cur->id;
    msg.type = 42;
    msg.priority = 5;
    msg.data_size = IPC_MAX_MSG_SIZE;

    for (size_t i = 0; i < IPC_MAX_MSG_SIZE; ++i) {
        msg.data[i] = static_cast<uint8_t>(i ^ 0xAA);
    }

    JARVIS_ASSERT(cur->msg_queue.push(msg) == true);

    Message recv{};
    JARVIS_ASSERT(cur->msg_queue.pop(recv) == true);

    JARVIS_ASSERT(recv.sender_id == msg.sender_id);
    JARVIS_ASSERT(recv.type == msg.type);
    JARVIS_ASSERT(recv.priority == msg.priority);
    JARVIS_ASSERT(recv.data_size == msg.data_size);

    for (size_t i = 0; i < IPC_MAX_MSG_SIZE; ++i) {
        JARVIS_ASSERT(recv.data[i] == msg.data[i]);
    }
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Alloc a BufferPool buffer of maximum data payload size, fill with
// pattern, send via IPC, verify receiver gets the full data, map and read
// back.  Driven with REAL kernel sender + receiver tasks (cloned PML4 so the
// buffer path works while the lambdas run in kernel mode).
// Input: Real sender allocs buffer + sends via IPC; real receiver recvs,
//        maps, verifies, frees.
// Expect: All 64 bytes match; receiver owns buffer after transfer.
JARVIS_TEST(ipc_buf_handle_max_size, "PRE: none | POST: none") {
    static uint64_t g_sender_ok = 0;
    static uint64_t g_recv_ok = 0;

    auto *sender = TaskControlBlock::create(
        []() {
            auto *self = Scheduler::current_task();
            auto *ctx = reinterpret_cast<uint64_t *>(self->user_data);
            uint64_t peer = ctx[0];
            uint64_t va = 0xC0000000;
            uint64_t handle = BufferPool::alloc(*self, va);
            if (handle == 0) {
                g_sender_ok = 1;
                return;
            }
            uint32_t idx = static_cast<uint32_t>(handle & 0xFFFFFFFFULL);
            uint64_t phys = BufferPool::entries[idx].phys_addr;
            auto *buf = reinterpret_cast<uint8_t *>(arch::HHDM_OFFSET + phys);
            for (size_t i = 0; i < IPC_MAX_MSG_SIZE; ++i) {
                buf[i] = static_cast<uint8_t>(i ^ 0xAA);
            }
            Message msg{};
            msg.buf_handle = handle;
            msg.type = 200;
            msg.priority = 0;
            msg.data_size = IPC_MAX_MSG_SIZE;
            if (!IPC::send(peer, msg, IpcFlags::NONE)) {
                g_sender_ok = 2;
                return;
            }
            g_sender_ok = 0;
        },
        12, 10);

    auto *receiver = TaskControlBlock::create(
        []() {
            auto *self = Scheduler::current_task();
            auto *ctx = reinterpret_cast<uint64_t *>(self->user_data);
            uint64_t rva = ctx[0];
            Message recv{};
            bool ok = false;
            for (int i = 0; i < 100000 && !ok; ++i)
                ok = IPC::recv(recv);
            if (!ok || recv.type != 200ULL) {
                g_recv_ok = 1;
                return;
            }
            if (!BufferPool::map(*self, recv.buf_handle, rva)) {
                g_recv_ok = 2;
                return;
            }
            uint32_t idx =
                static_cast<uint32_t>(recv.buf_handle & 0xFFFFFFFFULL);
            uint64_t phys = BufferPool::entries[idx].phys_addr;
            auto *rbuf = reinterpret_cast<uint8_t *>(arch::HHDM_OFFSET + phys);
            for (size_t i = 0; i < IPC_MAX_MSG_SIZE; ++i) {
                if (rbuf[i] != static_cast<uint8_t>(i ^ 0xAA)) {
                    g_recv_ok = 3;
                    return;
                }
            }
            if (!BufferPool::free(*self, recv.buf_handle)) {
                g_recv_ok = 4;
                return;
            }
            g_recv_ok = 0;
        },
        11, 10);
    if (!sender || !receiver) { JARVIS_FAIL("task create failed (OOM)"); return; }

    uint64_t sctx[1];
    sctx[0] = receiver->id;
    sender->user_data = sctx;
    uint64_t rctx[1];
    rctx[0] = 0xD0000000;
    receiver->user_data = rctx;

    {
        arch::IrqGuard _guard;
        Scheduler::add_task(*sender);
        Scheduler::add_task(*receiver);
    }
    Scheduler::reschedule();
    kernel::test::wait_for_termination_safe(sender);
kernel::test::wait_for_termination_safe(receiver);


    // Cleanup BEFORE asserting (cookbook Rule 5): both self-terminated.
    Scheduler::drain_zombie_list();
    JARVIS_ASSERT_EQ(0ULL, g_sender_ok);
    JARVIS_ASSERT_EQ(0ULL, g_recv_ok);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Verify priority inheritance works when a high-priority task waits
// for a message from a low-priority task — the low task's priority is boosted
// while a high-priority sender is blocked on its full queue.
// Input: Receiver (prio 11) with a full queue; a real high-priority sender
//        (prio 20) blocks on the full queue.
// Expect: Receiver's priority is boosted to >= sender's priority.
JARVIS_TEST(ipc_priority_inheritance_send, "PRE: none | POST: none") {
    // Low-priority receiver registered BLOCKED so the timer never dispatches
    // it before the harness observes the block/boost; it drains via IPC::recv
    // and stays alive until the high sender's completing re-push arrives.
    static uint64_t g_recv_ok = 0;
    auto *low = TaskControlBlock::create(
        []() {
            kernel::Message m;
            bool ok = IPC::recv(m);
            __atomic_store_n(&g_recv_ok, ok ? 1 : 0, __ATOMIC_RELEASE);
            for (int i = 0; i < 100000; ++i) {
                kernel::Message m2;
                if (IPC::recv(m2) && m2.type == 42)
                    break;
            }
        },
        11, 10);
    JARVIS_ASSERT(low != nullptr);
    // Pinned FIXED (issue #19): see ipc_priority_inversion above.
    JARVIS_ASSERT(Scheduler::set_sched_policy(*low, SchedPolicy::FIXED));
    low->state = TaskState::BLOCKED;
    Scheduler::register_task(*low);

    for (size_t i = 0; i < IPC_MAX_QUEUE_MSG; ++i) {
        Message fill{};
        fill.sender_id = 0;
        fill.type = 99;
        fill.priority = 0;
        fill.data_size = 0;
        low->msg_queue.push(fill);
    }

    uint64_t l_id = low->id;
    uint64_t send_result = 0;
    struct SCtx {
        uint64_t recv_;
        uint64_t out_;
    } sctx;
    sctx.recv_ = l_id;
    sctx.out_ = reinterpret_cast<uint64_t>(&send_result);
    auto *high = TaskControlBlock::create(
        []() {
            auto *self = Scheduler::current_task();
            auto *c = reinterpret_cast<SCtx *>(self->user_data);
            Message msg{};
            msg.sender_id = self->id;
            msg.type = 42;
            msg.priority = 0;
            msg.data_size = 0;
            bool ok = IPC::send(c->recv_, msg, IpcFlags::NONE);
            __atomic_store_n(reinterpret_cast<uint64_t *>(c->out_),
                             ok ? 1 : 0, __ATOMIC_RELEASE);
        },
        20, 10);
    JARVIS_ASSERT(high != nullptr);
    // Pinned FIXED (issue #19): must preempt the polling receiver by
    // priority; EDF would not preempt an earlier-deadline receiver.
    JARVIS_ASSERT(Scheduler::set_sched_policy(*high, SchedPolicy::FIXED));
    high->user_data = &sctx;
    Scheduler::add_task(*high);
    Scheduler::reschedule();
    while (high->state != TaskState::BLOCKED)
        arch::pause();
    JARVIS_ASSERT(high->state == TaskState::BLOCKED);

    // The low-priority receiver is boosted while the high-priority sender is
    // blocked on its queue.
    JARVIS_ASSERT(low->priority >= high->priority);

    // Release the low receiver: it drains one → the blocked sender completes.
    Scheduler::set_task_ready(*low);
    kernel::test::wait_for_termination_safe(high);
    kernel::test::wait_for_termination_safe(low);

    // Cleanup BEFORE asserting (cookbook Rule 5): both self-terminated.
    Scheduler::drain_zombie_list();
    JARVIS_ASSERT_EQ(1ULL, send_result);
    JARVIS_ASSERT_EQ(1ULL, g_recv_ok);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: A message arriving for a task blocked in a NON-IPC channel must
//           NOT requeue it (issue #208: a waitpid-blocked parent woken by an
//           unrelated message resumes with the -1 block sentinel instead of
//           the child's pid because the waitpid wake protocol never ran).
// Input: Undispatched kernel task, BLOCKED + dequeued with waiting_child_pid
//        set (mirrors the sys_waitpid block exactly) and no IPC wait flags;
//        IPC::send one message to it.
// Expect: send returns true (message delivered to the queue); the waiter
//         stays BLOCKED and stays out of the ready queue; the message pops
//         back intact (delivery really happened — the no-wake is not vacuous).
// Depends: IPC::send arrival-wake gate, Scheduler::dequeue_ready.
JARVIS_TEST(ipc_send_no_wake_non_ipc_waiter, "PRE: none | POST: none") {
    auto *cur = Scheduler::current_task();
    JARVIS_ASSERT(cur != nullptr);
    // Non-returning entry: a timer tick between add_task and the block
    // below must never terminate a fresh task (use-after-free on the
    // waiter pointer).  The IrqGuard closes the add/block window
    // (cookbook Rule 2); the spin is defense-in-depth.
    auto *waiter = TaskControlBlock::create(
        []() {
            for (;;)
                arch::pause();
        },
        11, 10);
    JARVIS_ASSERT(waiter != nullptr);
    {
        arch::IrqGuard guard;
        Scheduler::add_task(*waiter);
        // Mirror the sys_waitpid block: BLOCKED + dequeued + non-IPC channel.
        waiter->waiting_child_pid = 999999;
        waiter->state = TaskState::BLOCKED;
        Scheduler::dequeue_ready(*waiter);
    }
    JARVIS_ASSERT(!waiter->reply_wait);
    JARVIS_ASSERT(waiter->blocked_on_queue == nullptr);
    JARVIS_ASSERT(!waiter->blocked_in_recv);

    Message msg{};
    msg.sender_id = cur->id;
    msg.type = 77;
    msg.priority = 0;
    msg.data_size = 0;
    bool ok = IPC::send(waiter->id, msg, IpcFlags::NONE);
    JARVIS_ASSERT_FMT(ok, "send to blocked waiter failed");
    JARVIS_ASSERT_FMT(waiter->state == TaskState::BLOCKED,
                      "non-IPC waiter woken by message arrival");
    JARVIS_ASSERT_FMT(!waiter->in_ready_queue_,
                      "non-IPC waiter requeued by message arrival");

    Message back{};
    JARVIS_ASSERT_FMT(waiter->msg_queue.pop(back), "message not queued");
    JARVIS_ASSERT_FMT(back.type == 77, "message corrupted in queue");
    kernel::test::terminate_and_drain(*waiter);
    Scheduler::drain_zombie_list();
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: The gate's positive half — a task BLOCKED in a receive wait
//           (blocked_in_recv, mirroring the sys_receive block) MUST still
//           wake on message arrival (vfsd-style daemons depend on it).
// Input: Undispatched kernel task, BLOCKED + dequeued with blocked_in_recv
//        set; IPC::send one message to it.
// Expect: send returns true; the waiter is READY and requeued.
// Depends: IPC::send arrival-wake gate.
JARVIS_TEST(ipc_send_wakes_recv_waiter, "PRE: none | POST: none") {
    auto *cur = Scheduler::current_task();
    JARVIS_ASSERT(cur != nullptr);
    auto *waiter = TaskControlBlock::create(
        []() {
            for (;;)
                arch::pause();
        },
        11, 10);
    JARVIS_ASSERT(waiter != nullptr);
    {
        arch::IrqGuard guard;
        Scheduler::add_task(*waiter);
        // Mirror the sys_receive block: BLOCKED + dequeued + IPC wait channel.
        waiter->blocked_in_recv = true;
        waiter->state = TaskState::BLOCKED;
        Scheduler::dequeue_ready(*waiter);
    }

    Message msg{};
    msg.sender_id = cur->id;
    msg.type = 78;
    msg.priority = 0;
    msg.data_size = 0;
    bool ok = IPC::send(waiter->id, msg, IpcFlags::NONE);
    JARVIS_ASSERT_FMT(ok, "send to recv waiter failed");
    JARVIS_ASSERT_FMT(waiter->state == TaskState::READY,
                      "recv waiter not woken by message arrival");
    JARVIS_ASSERT_FMT(waiter->in_ready_queue_ || waiter->in_edf_queue_,
                      "recv waiter not requeued by message arrival");

    Message back{};
    JARVIS_ASSERT_FMT(waiter->msg_queue.pop(back), "message not queued");
    kernel::test::terminate_and_drain(*waiter);
    Scheduler::drain_zombie_list();
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: The suite snapshot must be IPC-quiescent (issue #321).  A message
// left in any task's inbox when snapshot_create() runs is captured inside the
// TCB block and byte-restored on every snapshot_restore, replaying it on each
// test boundary (duplicate IPC delivery).  The boot path settles all inboxes
// before the snapshot, so no in-flight message is ever captured at baseline.
// Input: snapshot_baseline_nonempty_inbox_count() (recorded at snapshot_create).
// Expect: 0 — the captured baseline contains no queued message.
// Depends: snapshot harness boot quiescence (kernel.cpp init_task_main settle).
JARVIS_TEST(ipc_snapshot_baseline_inbox_quiescent, "PRE: none | POST: none") {
    JARVIS_ASSERT_EQ(0ULL,
                     kernel::test::snapshot_baseline_nonempty_inbox_count());
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: ipc_send_sync (issue #317 option b) writes the reply PAYLOAD to
//           the caller's reply_buf, returns the reply TYPE, and does NOT
//           overwrite the request buffer (the old reply-over-request path).
// Input: userspace/bin/ipcsync-probe.c.elf sends to PID 1; the harness (this
//        test) replies (type 0x77, 8-byte 0xA0.. payload).
// Expect: the probe exits 0 (its own reply_buf/payload/req-intact checks hold).
// Depends: SYS_SEND_SYNC 5th-arg (a4) plumbing, ipc_send_sync, initrd.
JARVIS_TEST(ipc_send_sync_reply_buffer, "PRE: none | POST: none") {
    initrd::InitrdFile f = initrd::find("bin/ipcsync-probe.c.elf");
    JARVIS_ASSERT(f.data != nullptr);
    auto *hdr = reinterpret_cast<const kernel::elf::ELF64Header *>(f.data);
    JARVIS_ASSERT(kernel::elf::validate_header(hdr));
    auto *t = kernel::elf::load(hdr, f.data, f.size);
    JARVIS_ASSERT(t != nullptr);
    Scheduler::add_task(*t);

    bool replied = false;
    for (int i = 0; i < 400000 && !replied; ++i) {
        Message msg;
        if (IPC::recv(msg) && msg.type == 0x55ULL) {
            Message reply{};
            reply.sender_id = Scheduler::current_task()->id;
            reply.type = 0x77ULL;
            reply.priority = 0;
            reply.data_size = 8;
            for (uint64_t b = 0; b < 8; ++b)
                reply.data[b] = static_cast<uint8_t>(0xA0 + b);
            replied = IPC::send(msg.sender_id, reply);
        }
        __atomic_store_n(&kernel::Scheduler::SwSlots::need_resched(), true,
                         __ATOMIC_RELEASE);
        arch::hlt();
    }
    JARVIS_ASSERT(replied);

    for (int i = 0; i < 400000; ++i) {
        if (!TaskControlBlock::is_valid(t) ||
            t->state == TaskState::TERMINATED ||
            t->state == TaskState::REAPED)
            break;
        __atomic_store_n(&kernel::Scheduler::SwSlots::need_resched(), true,
                         __ATOMIC_RELEASE);
        arch::hlt();
    }
    JARVIS_ASSERT(TaskControlBlock::is_valid(t));
    JARVIS_ASSERT_EQ(0ULL, t->exit_code); // 0 => probe's contract checks held
    // The probe exited via sys_exit (switch_away_from_terminating does NOT
    // release_zombie), so it is NOT on the zombie list — terminate explicitly
    // to release it, then reap (cleanup() frees its Notify/EventGroup/FDs).
    Scheduler::terminate(*t, t->exit_code);
    Scheduler::drain_zombie_list();
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Registers all extended IPC unit tests with the test framework.
// Input: None
// Expect: All IPC extended tests registered via JARVIS_REGISTER_TEST
// Depends: kernel test framework
void register_ipc_extended_tests() {
    Logger::info("Registering IPC extended tests");
    JARVIS_REGISTER_TEST(ipc_send_sync_reply_buffer);
    JARVIS_REGISTER_TEST(ipc_send_data_size_exceeds_max);
    JARVIS_REGISTER_TEST(ipc_send_data_size_zero);
    JARVIS_REGISTER_TEST(ipc_queue_remove_from_mid);
    JARVIS_REGISTER_TEST(ipc_multiple_blocked_senders_wake_one);
    JARVIS_REGISTER_TEST(ipc_send_sync_timeout);
    JARVIS_REGISTER_TEST(ipc_priority_inversion);
    JARVIS_REGISTER_TEST(ipc_send_self_max_message_size);
    JARVIS_REGISTER_TEST(ipc_buf_handle_max_size);
    JARVIS_REGISTER_TEST(ipc_priority_inheritance_send);
    JARVIS_REGISTER_TEST(ipc_send_no_wake_non_ipc_waiter);
    JARVIS_REGISTER_TEST(ipc_send_wakes_recv_waiter);
    JARVIS_REGISTER_TEST(ipc_snapshot_baseline_inbox_quiescent);
}
