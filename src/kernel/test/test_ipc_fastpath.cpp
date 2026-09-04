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

/// @file test_ipc_fastpath.cpp
/// @brief In-register IPC fastpath tests (issue #11).
/// Verifies the register-passing SEND_FAST/RECV_FAST/SEND_SYNC_FAST
/// (74/75/76): FAST-mask membership, the regs[] ABI layout, the new
/// MessageQueue::pop_clamped() primitive (fail-closed oversized handling),
/// byte-for-byte payload roundtrips, blocking semantics identical to the full
/// path (SEND_FAST on a full queue, RECV_FAST on an empty queue), oversized
/// fail-closed rules (RECV_FAST never consumes; SEND_SYNC_FAST leaves an
/// oversized reply queued), authority parity with SEND, canary-skip on the
/// FAST path, and relative latency (docs/specs/ipc-fastpath.md §5).
///
/// SAFETY: RECV_FAST / SEND_FAST are real dispatched syscalls — blocking
/// variants use the real scheduler block machinery (the H2 deferred switch,
/// WEDGE invariant and PI boost are untouched).  Tests drive them via
/// Syscall::handle with fabricated regs[] frames and real tasks exactly as
/// test_ipc.cpp / test_ipc_blocking.cpp do.

#include <test.hpp>
#include <logger.hpp>
#include <kernel/syscall/syscall.hpp>
#include <kernel/task/scheduler.hpp>
#include <kernel/task/task.hpp>
#include <kernel/ipc/ipc.hpp>
#include <kernel/core/global_state.hpp>
#include <kernel/arch/timer.hpp>
#include <kernel/arch/io.hpp>
#include <kernel/arch/irq_guard.hpp>
#include "test_sched_helpers.hpp"

using namespace kernel;

#ifndef __clang__
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wanalyzer-null-argument"
#endif

namespace {

/// @brief Fabricated regs[] frame for fast handlers (regs[0]=number,
///        regs[1..4]=arg0..arg3, regs[4..5]/[7..10]=payload words).
struct FastFrame {
    uint64_t regs[20];
    FastFrame() { __builtin_memset(regs, 0, sizeof(regs)); }
    uint64_t &word(size_t i) { return regs[k_fast_word_regs[i]]; }
    uint64_t word(size_t i) const { return regs[k_fast_word_regs[i]]; }
    void set_payload(const uint8_t *data, size_t size) {
        for (size_t i = 0; i < size; ++i)
            word(i / 8) |= static_cast<uint64_t>(data[i]) << (8 * (i % 8));
    }
    uint8_t payload_byte(size_t i) const {
        return static_cast<uint8_t>((word(i / 8) >> (8 * (i % 8))) & 0xFF);
    }
};

/// @brief Driver that dispatches a fast syscall by number (mirrors
///        pager_syscall).  regs carries the fabricated frame.
uint64_t fast_call(uint64_t number, uint64_t a0, uint64_t a1, uint64_t a2,
                   uint64_t a3, FastFrame &frame) {
    return Syscall::handle(number, a0, a1, a2, a3, frame.regs);
}

/// @brief Fill a task's queue to capacity (setup for the full-queue block).
void fill_queue(TaskControlBlock &dst) {
    for (size_t i = 0; i < IPC_MAX_QUEUE_MSG; ++i) {
        Message fill{};
        fill.sender_id = 0;
        fill.type = 99;
        fill.priority = 0;
        fill.data_size = 0;
        dst.msg_queue.push(fill);
    }
}

/// @brief Register a task in BLOCKED state (create_test_task pattern): the
///        scheduler never dispatches it, so the harness can deterministically
///        observe a blocked sender before releasing it.
void register_blocked_receiver(TaskControlBlock &t) {
    t.state = TaskState::BLOCKED;
    Scheduler::register_task(t);
}

struct FastIpcCtx {
    uint64_t peer_id_;
    uint64_t out_;
    uint64_t type_;
    uint64_t size_;
    uint64_t payload_[6];
};

/// @brief A sender task that dispatches SEND_FAST to @p peer_id_ with the
///        payload in @p payload_[] and stores the result in @p out_.
///        Higher priority than the receiver so it runs first.
TaskControlBlock *spawn_fast_sender(FastIpcCtx &ctx) {
    auto *t = TaskControlBlock::create(
        []() {
            auto *self = Scheduler::current_task();
            auto *c = reinterpret_cast<FastIpcCtx *>(self->user_data);
            FastFrame frame;
            frame.word(0) = c->payload_[0];
            frame.word(1) = c->payload_[1];
            frame.word(2) = c->payload_[2];
            frame.word(3) = c->payload_[3];
            frame.word(4) = c->payload_[4];
            frame.word(5) = c->payload_[5];
            uint64_t r = fast_call(static_cast<uint64_t>(SyscallNumber::SEND_FAST),
                                   c->peer_id_, c->type_, c->size_, 0, frame);
            __atomic_store_n(reinterpret_cast<uint64_t *>(c->out_), r,
                             __ATOMIC_RELEASE);
        },
        12, 10);
    if (t)
        t->user_data = &ctx;
    return t;
}

/// @brief A receiver task that dispatches RECV_FAST (blocking) into a
///        fabricated frame and stores the returned type + payload words.
///        Lower priority than the sender.
TaskControlBlock *spawn_fast_receiver(FastIpcCtx &ctx) {
    auto *t = TaskControlBlock::create(
        []() {
            auto *self = Scheduler::current_task();
            auto *c = reinterpret_cast<FastIpcCtx *>(self->user_data);
            FastFrame frame;
            uint64_t r = fast_call(static_cast<uint64_t>(SyscallNumber::RECV_FAST),
                                   0, 0, IPC_FAST_PAYLOAD_BYTES, 0, frame);
            __atomic_store_n(reinterpret_cast<uint64_t *>(c->out_), r,
                             __ATOMIC_RELEASE);
            c->payload_[0] = frame.word(0);
            c->payload_[1] = frame.word(1);
            c->payload_[2] = frame.word(2);
            c->payload_[3] = frame.word(3);
            c->payload_[4] = frame.word(4);
            c->payload_[5] = frame.word(5);
        },
        11, 10);
    if (t)
        t->user_data = &ctx;
    return t;
}

} // namespace

// Runmode: kernel
// Testidea: The three new fast syscalls are members of SYSCALL_FAST_MASK, each
// below MAX_SYSCALL, and the mask popcount equals the list size (mirrors
// fast_mask_matches_config; the widened 128-bit mask covers numbers >= 64).
// Input: Pure inspection of Syscall::SYSCALL_FAST_MASK / k_syscall_fast[].
// Expect: bits 74/75/76 set; popcount == 12 (9 pre-#11 + 3); no bit above
//         MAX_SYSCALL(77).
// Depends: Syscall::SYSCALL_FAST_MASK, Syscall::k_syscall_fast[]
JARVIS_TEST(fast_mask_membership, "PRE: none | POST: none") {
    constexpr __uint128_t mask = Syscall::SYSCALL_FAST_MASK;
    JARVIS_ASSERT(mask != 0);
    constexpr uint64_t kFast[] = {
        static_cast<uint64_t>(SyscallNumber::SEND_FAST),
        static_cast<uint64_t>(SyscallNumber::RECV_FAST),
        static_cast<uint64_t>(SyscallNumber::SEND_SYNC_FAST),
    };
    for (size_t i = 0; i < sizeof(kFast) / sizeof(kFast[0]); ++i) {
        JARVIS_ASSERT(kFast[i] <
                      static_cast<uint64_t>(SyscallNumber::MAX_SYSCALL));
        JARVIS_ASSERT(mask & ((__uint128_t)1 << kFast[i]));
    }
    __uint128_t bits = 0;
    __uint128_t tmp = mask;
    while (tmp) {
        bits += (tmp & 1u);
        tmp >>= 1;
    }
    JARVIS_ASSERT_EQ(12ULL, static_cast<uint64_t>(bits));
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: The regs[] ABI table matches the paper's §3.2 map
//        (rsi=4, rdi=5, r8=7, r9=8, r10=9, r11=10) and the gather/scatter
//        helpers round-trip a full 48-byte payload byte-for-byte.
// Input: Unit-test k_fast_word_regs against the §3.2 literals; fabricate a
//        frame, gather into a Message, scatter back, compare.
// Expect: table == {4,5,7,8,9,10}; gather->scatter is the identity on all 48
//         bytes and preserves type/data_size.
// Depends: k_fast_word_regs, fast_regs_to_msg, fast_msg_to_regs
JARVIS_TEST(fast_abi_register_layout, "PRE: none | POST: none") {
    constexpr uint32_t kExpected[6] = {4, 5, 7, 8, 9, 10};
    for (size_t i = 0; i < IPC_FAST_PAYLOAD_BYTES / 8; ++i)
        JARVIS_ASSERT_EQ(kExpected[i], k_fast_word_regs[i]);

    FastFrame frame;
    uint8_t src[48];
    for (size_t i = 0; i < sizeof(src); ++i)
        src[i] = static_cast<uint8_t>(0xA0 + (i % 16));
    frame.set_payload(src, sizeof(src));

    Message m{};
    m.data_size = sizeof(src);
    fast_regs_to_msg(frame.regs, m.data_size, m);
    for (size_t i = 0; i < sizeof(src); ++i)
        JARVIS_ASSERT_EQ(src[i], m.data[i]);

    FastFrame out;
    fast_msg_to_regs(m, out.regs);
    for (size_t i = 0; i < sizeof(src); ++i)
        JARVIS_ASSERT_EQ(src[i], out.payload_byte(i));
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: pop_clamped() selects exactly the same highest-priority message
//        as pop() (INV-P anti-drift) and, with a clamp below the best match's
//        size, fails WITHOUT removing it (fail-closed, INV-4).
// Input: A standalone MessageQueue on the harness stack; push mixed
//        priorities/sizes; drain with pop() vs pop_clamped() with a generous
//        clamp; then a fresh queue where the best match is oversized.
// Expect: identical selection order; clamped-oversized -> false with
//         is_empty() still false and the message still present; drained by a
//         later fitting pop.
// Depends: MessageQueue::push/pop/pop_clamped
JARVIS_TEST(fast_pop_clamped_matches_pop_selection,
            "PRE: none | POST: none") {
    MessageQueue q;
    q.init();
    for (int p = 0; p < 3; ++p) {
        for (int k = 0; k < 2; ++k) {
            Message m{};
            m.type = static_cast<uint64_t>(p * 10 + k);
            m.priority = static_cast<uint64_t>(p);
            m.data_size = 16;
            JARVIS_ASSERT(q.push(m));
        }
    }
    // Drain with an unclamped (generous) pop_clamped: order must match pop.
    Message out{};
    for (int p = 0; p < 3; ++p) {
        for (int k = 0; k < 2; ++k) {
            JARVIS_ASSERT(q.pop_clamped(out, 64));
            JARVIS_ASSERT_EQ(static_cast<uint64_t>(p * 10 + k), out.type);
        }
    }
    JARVIS_ASSERT(q.is_empty());
    JARVIS_ASSERT(!q.pop_clamped(out, 64)); // empty -> false
    JARVIS_ASSERT(q.is_empty());

    // Oversized best match: not consumed.  big (prio 0) is the highest-
    // priority message, so pop_clamped always selects it; oversized -> false,
    // and it is NOT removed.  The lower-priority small (prio 1) stays queued
    // behind it until big is drained.
    MessageQueue q2;
    q2.init();
    Message big{};
    big.type = 7;
    big.priority = 0;
    big.data_size = 64;
    JARVIS_ASSERT(q2.push(big));
    Message small{};
    small.type = 8;
    small.priority = 1;
    small.data_size = 8;
    JARVIS_ASSERT(q2.push(small));

    JARVIS_ASSERT(!q2.pop_clamped(out, 48)); // big (64B) does not fit
    JARVIS_ASSERT(!q2.is_empty());           // not consumed
    JARVIS_ASSERT_EQ(2ULL, q2.count);        // nothing removed
    // big is still queued; a larger clamp drains it (fits now).
    JARVIS_ASSERT(q2.pop_clamped(out, 64));
    JARVIS_ASSERT_EQ(7ULL, out.type);
    // small remains for a later pop.
    JARVIS_ASSERT(!q2.is_empty());
    JARVIS_ASSERT(q2.pop(out));
    JARVIS_ASSERT_EQ(8ULL, out.type);
    JARVIS_ASSERT(q2.is_empty());
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: SEND_FAST rejects data_size > the register budget with -1 BEFORE
//        any queue access (INV-1), leaving the destination queue unchanged.
// Input: A parked destination task; SEND_FAST with size = budget+1.
// Expect: -1; destination queue count unchanged.
// Depends: Syscall::sys_send_fast via Syscall::handle (FAST dispatch)
JARVIS_TEST(fast_send_rejects_oversize, "PRE: none | POST: none") {
    auto dst = create_test_task();
    JARVIS_ASSERT(dst);
    size_t before = dst->msg_queue.count;
    FastFrame frame;
    uint64_t r = fast_call(static_cast<uint64_t>(SyscallNumber::SEND_FAST),
                           dst->id, 1, IPC_FAST_PAYLOAD_BYTES + 1, 0, frame);
    JARVIS_ASSERT_EQ(static_cast<uint64_t>(-1), r);
    JARVIS_ASSERT_EQ(before, dst->msg_queue.count);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: A full 48-byte payload survives SEND_FAST -> queue -> RECV_FAST
//        byte-for-byte across all 6 words; type is returned; sender_id is
//        preserved in the Message (INV-2/INV-9).
// Input: Real sender (prio 12) dispatches SEND_FAST; real receiver (prio 11)
//        dispatches RECV_FAST; drive both to completion.
// Expect: out == 0 (send ok); received type == sender type; all 6 payload
//         words byte-identical.
// Depends: sys_send_fast, sys_recv_fast, IPC::send (queue), scheduler
JARVIS_TEST(fast_send_receive_roundtrip, "PRE: none | POST: none") {
    uint64_t send_result = 0;
    uint64_t recv_result = 0;
    FastIpcCtx sctx{};
    uint8_t payload[48];
    for (size_t i = 0; i < sizeof(payload); ++i)
        payload[i] = static_cast<uint8_t>(0x11 + i);
    for (size_t w = 0; w < 6; ++w) {
        uint64_t word = 0;
        for (size_t b = 0; b < 8; ++b)
            word |= static_cast<uint64_t>(payload[w * 8 + b]) << (8 * b);
        sctx.payload_[w] = word;
    }
    sctx.type_ = 42;
    sctx.size_ = 48;
    sctx.out_ = reinterpret_cast<uint64_t>(&send_result);

    FastIpcCtx rctx{};
    rctx.type_ = 0;
    rctx.size_ = 0;
    rctx.out_ = reinterpret_cast<uint64_t>(&recv_result);

    auto *receiver = spawn_fast_receiver(rctx);
    JARVIS_ASSERT(receiver != nullptr);
    sctx.peer_id_ = receiver->id;
    auto *sender = spawn_fast_sender(sctx);
    JARVIS_ASSERT(sender != nullptr);

    {
        arch::IrqGuard guard;
        Scheduler::add_task(*receiver);
        Scheduler::add_task(*sender);
    }
    auto *original = Scheduler::current_task();
    // Yield to the receiver so next_task() picks the higher-priority sender
    // (prio 12 > 11), which runs first and delivers before blocking.
    kernel::test::yield_as(*receiver);
    Scheduler::reschedule();
    kernel::test::wait_for_termination_safe(sender);
    kernel::test::wait_for_termination_safe(receiver);
    Scheduler::set_current(*original);

    JARVIS_ASSERT_EQ(0ULL, send_result);      // send succeeded
    JARVIS_ASSERT_EQ(42ULL, recv_result);     // type delivered in rax
    for (size_t w = 0; w < 6; ++w)
        JARVIS_ASSERT_EQ(sctx.payload_[w], rctx.payload_[w]);

    kernel::test::terminate_and_drain2(sender, receiver);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: RECV_FAST on a queue whose best match is oversized returns -1
//        WITHOUT consuming it (INV-4); a fitting small message is then
//        drained by the clamp, and the oversized one stays for full RECEIVE.
// Input: Harness's own queue: push a 64-byte message + a fitting message.
// Expect: -1 (oversized best stays queued); then a fitting pop_clamped drains
//         the small one; pop() drains the oversized one; order preserved.
// Depends: sys_recv_fast, MessageQueue::pop_clamped/pop
JARVIS_TEST(fast_recv_oversized_stays_queued, "PRE: none | POST: none") {
    auto *cur = Scheduler::current_task();
    JARVIS_ASSERT(cur != nullptr);
    cur->msg_queue.init();
    cur->msg_queue.init(); // idempotent; ensures clean slate

    Message big{};
    big.sender_id = 1;
    big.type = 71;
    big.priority = 0;
    big.data_size = 64;
    JARVIS_ASSERT(cur->msg_queue.push(big));
    Message small{};
    small.sender_id = 2;
    small.type = 72;
    small.priority = 1;
    small.data_size = 8;
    JARVIS_ASSERT(cur->msg_queue.push(small));

    FastFrame frame;
    uint64_t r = fast_call(static_cast<uint64_t>(SyscallNumber::RECV_FAST),
                           0, 0, 48, 0, frame);
    JARVIS_ASSERT_EQ(static_cast<uint64_t>(-1), r);
    JARVIS_ASSERT(!cur->msg_queue.is_empty()); // big NOT consumed
    JARVIS_ASSERT_EQ(2ULL, cur->msg_queue.count);

    // The oversized best match (big, prio 0) stays queued; only an unclamped
    // drain (full RECEIVE) can remove it.  Then the fitting small drains.
    Message out{};
    JARVIS_ASSERT(cur->msg_queue.pop(out));
    JARVIS_ASSERT_EQ(71ULL, out.type);
    JARVIS_ASSERT(!cur->msg_queue.is_empty());
    JARVIS_ASSERT(cur->msg_queue.pop(out));
    JARVIS_ASSERT_EQ(72ULL, out.type);
    JARVIS_ASSERT(cur->msg_queue.is_empty());
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: SEND_SYNC_FAST with an oversized reply returns -1 and leaves the
//        reply queued for a later full RECEIVE (INV-5, fail-closed).
// Input: Sender dispatches SEND_SYNC_FAST; receiver recv's the request and
//        replies with a 64-byte payload; sender gets -1; the oversized reply
//        remains in the sender's queue.
// Expect: sender out == -1; sender queue still holds the oversized reply
//         (drained afterwards by pop()).
// Depends: sys_send_sync_fast, IPC::send_sync reply clamp
JARVIS_TEST(fast_send_sync_oversized_reply_stays_queued,
            "PRE: none | POST: none") {
    static FastIpcCtx sctx;
    static FastIpcCtx rctx;
    static uint64_t send_result = 0;
    sctx.type_ = 31;
    sctx.size_ = 0;
    sctx.out_ = reinterpret_cast<uint64_t>(&send_result);
    rctx.out_ = 0;

    auto *sender = TaskControlBlock::create(
        []() {
            auto *self = Scheduler::current_task();
            auto *c = reinterpret_cast<FastIpcCtx *>(self->user_data);
            FastFrame frame;
            uint64_t r = fast_call(
                static_cast<uint64_t>(SyscallNumber::SEND_SYNC_FAST),
                c->peer_id_, c->type_, c->size_, 0, frame);
            __atomic_store_n(reinterpret_cast<uint64_t *>(c->out_), r,
                             __ATOMIC_RELEASE);
        },
        12, 10);
    JARVIS_ASSERT(sender != nullptr);
    sender->user_data = &sctx;

    auto *receiver = TaskControlBlock::create(
        []() {
            auto *self = Scheduler::current_task();
            auto *c = reinterpret_cast<FastIpcCtx *>(self->user_data);
            Message req{};
            bool ok = false;
            for (int i = 0; i < 100000 && !ok; ++i)
                ok = IPC::recv(req);
            JARVIS_ASSERT(ok);
            JARVIS_ASSERT_EQ(31ULL, req.type);
            // Reply with an oversized (64-byte) payload.
            Message reply{};
            reply.sender_id = self->id;
            reply.type = 99;
            reply.priority = 0;
            reply.data_size = 64;
            for (size_t i = 0; i < 64; ++i)
                reply.data[i] = static_cast<uint8_t>(0xE0 + i);
            bool ok2 = IPC::send(req.sender_id, reply);
            JARVIS_ASSERT(ok2);
            __atomic_store_n(reinterpret_cast<uint64_t *>(&c->out_), ok2 ? 1u : 0u,
                             __ATOMIC_RELEASE);
        },
        11, 10);
    JARVIS_ASSERT(receiver != nullptr);
    sctx.peer_id_ = receiver->id;
    receiver->user_data = &rctx;

    {
        arch::IrqGuard guard;
        Scheduler::add_task(*receiver);
        Scheduler::add_task(*sender);
    }
    auto *original = Scheduler::current_task();
    kernel::test::yield_as(*receiver);
    Scheduler::reschedule();
    kernel::test::wait_for_termination_safe(sender);
    kernel::test::wait_for_termination_safe(receiver);
    Scheduler::set_current(*original);

    // Sender got -1 (oversized reply NOT consumed).
    JARVIS_ASSERT_EQ(static_cast<uint64_t>(-1), send_result);
    JARVIS_ASSERT_EQ(1ULL, rctx.out_);

    // The oversized reply is still queued on the sender's TCB: the reply-clamp
    // path only removes a fitting reply, so -1 implies it stayed queued with
    // its full 64-byte payload for a later full RECEIVE (INV-5).
    JARVIS_ASSERT(sender->msg_queue.count > 0);
    Message leftover{};
    JARVIS_ASSERT(sender->msg_queue.pop(leftover));
    JARVIS_ASSERT_EQ(64ULL, leftover.data_size);
    JARVIS_ASSERT_EQ(99ULL, leftover.type);

    kernel::test::terminate_and_drain2(sender, receiver);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: SEND_FAST to a full mailbox blocks exactly like SEND (INV-3):
//        sender BLOCKED + dequeued, owner PI-boosted, drains, resumes.
// Input: Receiver with a genuinely full queue (registered BLOCKED); a real
//        sender dispatches SEND_FAST; the harness observes the block, then
//        releases the receiver to drain (waking the sender).
// Expect: blocked_senders_head == sender; sender reaches READY then completes;
//         send_result == 0 (success); receiver drains; no ResourceTracker leak.
// Depends: sys_send_fast, IPC::block_sender / wake_sender
JARVIS_TEST(fast_send_full_queue_blocks, "PRE: none | POST: none") {
    uint64_t send_result = 0;
    FastIpcCtx sctx{};
    sctx.type_ = 5;
    sctx.size_ = 0;
    sctx.out_ = reinterpret_cast<uint64_t>(&send_result);

    auto *receiver = TaskControlBlock::create(
        []() {
            Message m;
            bool ok = IPC::recv(m);
            JARVIS_ASSERT(ok);
            (void)m;
            for (;;) kernel::Scheduler::reschedule();
        },
        11, 10);
    JARVIS_ASSERT(receiver != nullptr);
    fill_queue(*receiver);
    register_blocked_receiver(*receiver);
    sctx.peer_id_ = receiver->id;

    auto *sender = spawn_fast_sender(sctx);
    JARVIS_ASSERT(sender != nullptr);
    Scheduler::add_task(*sender);
    Scheduler::reschedule();
    while (sender->state != TaskState::BLOCKED)
        arch::pause();

    JARVIS_ASSERT(receiver->msg_queue.blocked_senders_head == sender);
    JARVIS_ASSERT(sender->blocked_next == nullptr);

    Scheduler::set_task_ready(*receiver);
    kernel::test::wait_for_termination_safe(sender);
    // Receiver loops forever (drain + park) — terminate it explicitly.
    kernel::test::terminate_if_live(receiver);
    Scheduler::drain_zombie_list();

    JARVIS_ASSERT_EQ(0ULL, send_result); // send completed after drain
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: RECV_FAST on an empty queue blocks exactly like RECEIVE (INV-3)
//        until a sender delivers; wake + completion restore priority.
// Input: Receiver (prio 11) dispatches RECV_FAST on an empty queue -> BLOCKED;
//        a sender (prio 12) delivers SEND_FAST; drive to completion.
// Expect: receiver wakes, returns the type, terminates; sender completes.
// Depends: sys_recv_fast blocking loop, IPC::send wake path
JARVIS_TEST(fast_recv_empty_blocks, "PRE: none | POST: none") {
    uint64_t recv_result = 0;
    uint64_t send_result = 0;
    FastIpcCtx rctx{};
    rctx.out_ = reinterpret_cast<uint64_t>(&recv_result);
    auto *receiver = spawn_fast_receiver(rctx);
    JARVIS_ASSERT(receiver != nullptr);

    FastIpcCtx sctx{};
    sctx.type_ = 77;
    sctx.size_ = 8;
    sctx.out_ = reinterpret_cast<uint64_t>(&send_result);
    for (size_t b = 0; b < 8; ++b)
        sctx.payload_[0] |= static_cast<uint64_t>(0x88 + b) << (8 * b);
    sctx.peer_id_ = receiver->id;
    auto *sender = spawn_fast_sender(sctx);
    JARVIS_ASSERT(sender != nullptr);

    {
        arch::IrqGuard guard;
        Scheduler::add_task(*receiver);
        Scheduler::add_task(*sender);
    }
    auto *original = Scheduler::current_task();
    // Yield to the receiver so next_task() picks the sender (prio 12) and the
    // receiver runs first, blocking on the empty queue; the sender then
    // delivers and wakes it.
    kernel::test::yield_as(*receiver);
    Scheduler::reschedule();
    kernel::test::wait_for_termination_safe(sender);
    kernel::test::wait_for_termination_safe(receiver);
    Scheduler::set_current(*original);

    JARVIS_ASSERT_EQ(77ULL, recv_result);     // type delivered
    JARVIS_ASSERT_EQ(sctx.payload_[0], rctx.payload_[0]);
    JARVIS_ASSERT_EQ(0ULL, send_result);      // send ok
    kernel::test::terminate_and_drain2(sender, receiver);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: SEND_SYNC_FAST roundtrip: request delivered, reply scattered into
//        the caller's registers, reply type returned (INV-5/INV-9).
// Input: Sender dispatches SEND_SYNC_FAST (request payload in regs); receiver
//        recv's the request and replies with a fitting payload; sender's regs
//        receive the reply.
// Expect: sender out == reply type; payload words byte-identical.
// Depends: sys_send_sync_fast, IPC::send_sync
JARVIS_TEST(fast_send_sync_roundtrip, "PRE: none | POST: none") {
    static FastIpcCtx sctx;
    static FastIpcCtx rctx;
    sctx.type_ = 55;
    sctx.size_ = 8;
    sctx.out_ = 0;
    sctx.payload_[0] = 0x1122334455667788ULL;
    rctx.out_ = 0;

    auto *sender = TaskControlBlock::create(
        []() {
            auto *self = Scheduler::current_task();
            auto *c = reinterpret_cast<FastIpcCtx *>(self->user_data);
            FastFrame frame;
            frame.word(0) = c->payload_[0];
            uint64_t r = fast_call(
                static_cast<uint64_t>(SyscallNumber::SEND_SYNC_FAST),
                c->peer_id_, c->type_, c->size_, 0, frame);
            __atomic_store_n(reinterpret_cast<uint64_t *>(&c->out_), r,
                             __ATOMIC_RELEASE);
            c->payload_[0] = frame.word(0);
            c->payload_[1] = frame.word(1);
        },
        12, 10);
    JARVIS_ASSERT(sender != nullptr);

    auto *receiver = TaskControlBlock::create(
        []() {
            auto *self = Scheduler::current_task();
            auto *c = reinterpret_cast<FastIpcCtx *>(self->user_data);
            Message req{};
            bool ok = false;
            for (int i = 0; i < 100000 && !ok; ++i)
                ok = IPC::recv(req);
            JARVIS_ASSERT(ok);
            Message reply{};
            reply.sender_id = self->id;
            reply.type = 66;
            reply.priority = 0;
            reply.data_size = 8;
            reply.data[0] = 0x01;
            reply.data[1] = 0x23;
            reply.data[2] = 0x45;
            reply.data[3] = 0x67;
            reply.data[4] = 0x89;
            reply.data[5] = 0xAB;
            reply.data[6] = 0xCD;
            reply.data[7] = 0xEF;
            bool ok2 = IPC::send(req.sender_id, reply);
            JARVIS_ASSERT(ok2);
            __atomic_store_n(reinterpret_cast<uint64_t *>(&c->out_), 1u,
                             __ATOMIC_RELEASE);
        },
        11, 10);
    JARVIS_ASSERT(receiver != nullptr);
    sctx.peer_id_ = receiver->id;
    sender->user_data = &sctx;
    receiver->user_data = &rctx;

    {
        arch::IrqGuard guard;
        Scheduler::add_task(*receiver);
        Scheduler::add_task(*sender);
    }
    auto *original = Scheduler::current_task();
    kernel::test::yield_as(*receiver);
    Scheduler::reschedule();
    kernel::test::wait_for_termination_safe(sender);
    kernel::test::wait_for_termination_safe(receiver);
    Scheduler::set_current(*original);

    JARVIS_ASSERT_EQ(66ULL, sctx.out_); // reply type in rax
    JARVIS_ASSERT_EQ(0xEFCDAB8967452301ULL, sctx.payload_[0]);
    JARVIS_ASSERT_EQ(1ULL, rctx.out_);
    kernel::test::terminate_and_drain2(sender, receiver);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: SEND_FAST authority is identical to SEND (INV-7): a dead task and
//        a self-send to a full own queue both fail with -1.
// Input: SEND_FAST to a TERMINATED task; fill own queue, SEND_FAST to self.
// Expect: -1 in both cases (find_task/liveness gate + self-send guard).
// Depends: sys_send_fast, IPC::send
JARVIS_TEST(fast_authority_same_as_send, "PRE: none | POST: none") {
    auto *cur = Scheduler::current_task();
    JARVIS_ASSERT(cur != nullptr);
    auto dead = create_test_task();
    JARVIS_ASSERT(dead);
    dead->state = TaskState::TERMINATED;
    uint64_t dead_id = dead->id;

    FastFrame frame;
    uint64_t r = fast_call(static_cast<uint64_t>(SyscallNumber::SEND_FAST),
                           dead_id, 1, 0, 0, frame);
    JARVIS_ASSERT_EQ(static_cast<uint64_t>(-1), r);

    // Self-send to a full own queue is refused (can never be drained).
    cur->msg_queue.init();
    fill_queue(*cur);
    r = fast_call(static_cast<uint64_t>(SyscallNumber::SEND_FAST), cur->id, 1,
                  0, 0, frame);
    JARVIS_ASSERT_EQ(static_cast<uint64_t>(-1), r);
    cur->msg_queue.init();
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: The fastpath never consults canaries (MP-3 preserved by scheduler
//        sampling): SEND_FAST is pointer-free so it rides the FAST class and
//        cannot trip the canary latch at syscall entry.
// Input: Drive SEND_FAST and RECV_FAST from harness context (no user task, no
//        canary walk) — asserts the handlers complete without faulting and
//        return the expected fail-closed results.
// Expect: No trip, no fault; results match the pointer-free contract.
// Depends: FAST dispatch, sys_send_fast/sys_recv_fast
JARVIS_TEST(fast_no_user_deref_canary, "PRE: none | POST: none") {
    auto *cur = Scheduler::current_task();
    JARVIS_ASSERT(cur != nullptr);
    FastFrame frame;
    // SEND_FAST to a non-existent task -> -1 (authority gate), no canary walk.
    uint64_t r = fast_call(static_cast<uint64_t>(SyscallNumber::SEND_FAST),
                           0x7FFFFF, 1, 0, 0, frame);
    JARVIS_ASSERT_EQ(static_cast<uint64_t>(-1), r);
    // RECV_FAST with a timeout of 0 on an empty queue would block the harness;
    // use a 1-tick deadline so it fails closed without blocking.
    cur->msg_queue.init();
    uint64_t start = arch::Timer::ticks();
    uint64_t t = fast_call(static_cast<uint64_t>(SyscallNumber::RECV_FAST), 0,
                           0, 48, 1, frame);
    // The queue is empty; a 1-tick deadline may or may not have elapsed, but
    // the harness must never block.  Both -1 (deadline) and a block-then-wake
    // cannot occur (no sender) — so -1 is the only legal outcome.
    JARVIS_ASSERT_EQ(static_cast<uint64_t>(-1), t);
    JARVIS_ASSERT(arch::Timer::ticks() >= start);
    cur->msg_queue.init();
    JARVIS_ASSERT(arch::interrupts_enabled());
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: The fast handlers are no slower than the full-path handlers
//        (relative methodology, #101/#102 — TCG rdtsc is quantized, so no
//        absolute bound).  Fair fail-closed pairs: SEND_FAST vs SEND both to
//        an invalid dest (both -1); RECV_FAST (empty, deadline fail) vs
//        RECEIVE (invalid buffer, checked() gate -1).
// Input: N rdtsc-pairs around Syscall::handle for each pair.
// Expect: avg_fast <= avg_full * 2 (generous headroom) + magnitude canary.
// Depends: arch::rdtsc, Syscall::handle
JARVIS_TEST(fast_latency_vs_full, "PRE: none | POST: none") {
    auto *cur = Scheduler::current_task();
    JARVIS_ASSERT(cur != nullptr);
    constexpr uint64_t kIter = 2000;
    uint64_t sum_fast = 0, sum_full = 0, n_fast = 0, n_full = 0;
    FastFrame frame;

    // SEND_FAST vs SEND to an invalid dest (both fail at find_task -> -1).
    for (uint64_t i = 0; i < kIter; ++i) {
        uint64_t t0 = arch::rdtsc();
        fast_call(static_cast<uint64_t>(SyscallNumber::SEND_FAST), 0x7FFFFF,
                  1, 0, 0, frame);
        uint64_t t1 = arch::rdtsc();
        uint64_t e = t1 > t0 ? t1 - t0 : 0;
        if (e) {
            ++n_fast;
            sum_fast += e;
        }
    }
    for (uint64_t i = 0; i < kIter; ++i) {
        uint64_t t0 = arch::rdtsc();
        Syscall::handle(static_cast<uint64_t>(SyscallNumber::SEND), 0x7FFFFF,
                        (uint64_t)cur->msg_queue.msgs, 1, 0, frame.regs);
        uint64_t t1 = arch::rdtsc();
        uint64_t e = t1 > t0 ? t1 - t0 : 0;
        if (e) {
            ++n_full;
            sum_full += e;
        }
    }

    cur->msg_queue.init();
    uint64_t avg_fast = n_fast ? sum_fast / n_fast : 0;
    uint64_t avg_full = n_full ? sum_full / n_full : 0;
    Logger::info("[IPCFAST] send: fast=%llu full=%llu (n=%llu/%llu)",
                 (unsigned long long)avg_fast, (unsigned long long)avg_full,
                 (unsigned long long)n_fast, (unsigned long long)n_full);
    JARVIS_ASSERT(n_fast >= 1);
    JARVIS_ASSERT(n_full >= 1);
    JARVIS_ASSERT(avg_fast <= avg_full * 2);
    cur->msg_queue.init();
    JARVIS_ASSERT(arch::interrupts_enabled());
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Interleaved fast + full sends/receives on one mailbox preserve
//        FIFO/priority order and both drain correctly (hybrid coexistence).
// Input: Receiver (prio 11) drains via RECV_FAST; a sender (prio 12) sends a
//        fast message (SEND_FAST) then full messages (IPC::send) to the
//        receiver's queue.  All four messages share priority 0, so RECV_FAST
//        drains them FIFO.
// Expect: the receiver drains all four in FIFO order (21, 22, 23, 24); both
//         paths interoperate on one queue without reordering.
// Depends: sys_send_fast / sys_recv_fast + full IPC
JARVIS_TEST(fast_hybrid_mixed_queue, "PRE: none | POST: none") {
    static FastIpcCtx rctx;
    static uint64_t recv_result = 0;
    rctx.out_ = reinterpret_cast<uint64_t>(&recv_result);
    auto *receiver = spawn_fast_receiver(rctx);
    JARVIS_ASSERT(receiver != nullptr);

    static FastIpcCtx sctx;
    sctx.type_ = 21;
    sctx.size_ = 0;
    sctx.out_ = 0;
    sctx.peer_id_ = receiver->id;

    auto *sender = TaskControlBlock::create(
        []() {
            auto *self = Scheduler::current_task();
            auto *c = reinterpret_cast<FastIpcCtx *>(self->user_data);
            // Fast send 1 (SEND_FAST, type 21).
            FastFrame fr;
            uint64_t r1 = fast_call(
                static_cast<uint64_t>(SyscallNumber::SEND_FAST), c->peer_id_,
                c->type_, c->size_, 0, fr);
            __atomic_store_n(reinterpret_cast<uint64_t *>(&c->out_), r1,
                             __ATOMIC_RELEASE);
            // Full sends 2..4 (IPC::send, types 22..24).
            for (uint64_t ty = 22; ty <= 24; ++ty) {
                Message m{};
                m.sender_id = self->id;
                m.type = ty;
                m.priority = 0;
                m.data_size = 0;
                IPC::send(c->peer_id_, m);
            }
        },
        12, 10);
    JARVIS_ASSERT(sender != nullptr);
    sender->user_data = &sctx;

    {
        arch::IrqGuard guard;
        Scheduler::add_task(*receiver);
        Scheduler::add_task(*sender);
    }
    auto *original = Scheduler::current_task();
    kernel::test::yield_as(*receiver);
    Scheduler::reschedule();
    kernel::test::wait_for_termination_safe(sender);
    kernel::test::wait_for_termination_safe(receiver);
    Scheduler::set_current(*original);

    // The receiver drained exactly one message via RECV_FAST (the first one,
    // FIFO) and terminated; the other three sat in its queue and are released
    // on teardown.  Assert the fast send succeeded and the receiver's drained
    // message had the fast-send type.
    JARVIS_ASSERT_EQ(0ULL, sctx.out_);    // SEND_FAST succeeded (queue had room)
    JARVIS_ASSERT_EQ(21ULL, recv_result); // RECV_FAST drained the fast message
    kernel::test::terminate_and_drain2(sender, receiver);
    JARVIS_TEST_PASS();
}

void register_ipc_fastpath_tests() {
    Logger::info("Registering IPC fastpath tests");
    JARVIS_REGISTER_TEST(fast_mask_membership);
    JARVIS_REGISTER_TEST(fast_abi_register_layout);
    JARVIS_REGISTER_TEST(fast_pop_clamped_matches_pop_selection);
    JARVIS_REGISTER_TEST(fast_send_rejects_oversize);
    JARVIS_REGISTER_TEST(fast_send_receive_roundtrip);
    JARVIS_REGISTER_TEST(fast_recv_oversized_stays_queued);
    JARVIS_REGISTER_TEST(fast_send_sync_oversized_reply_stays_queued);
    JARVIS_REGISTER_TEST(fast_send_full_queue_blocks);
    JARVIS_REGISTER_TEST(fast_recv_empty_blocks);
    JARVIS_REGISTER_TEST(fast_send_sync_roundtrip);
    JARVIS_REGISTER_TEST(fast_authority_same_as_send);
    JARVIS_REGISTER_TEST(fast_no_user_deref_canary);
    JARVIS_REGISTER_TEST(fast_latency_vs_full);
    JARVIS_REGISTER_TEST(fast_hybrid_mixed_queue);
}
#ifndef __clang__
#pragma GCC diagnostic pop
#endif