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

/// @file test_ipc_timeout.cpp
/// @brief Wheel-armed bounded receive tests (issue #18, v0.4.7).

#include <test.hpp>
#include <logger.hpp>
#include <kernel/syscall/syscall.hpp>
#include <kernel/task/scheduler.hpp>
#include <kernel/task/task.hpp>
#include <kernel/ipc/ipc.hpp>
#include <kernel/arch/timer.hpp>
#include <kernel/arch/io.hpp>
#include <kernel/arch/irq_guard.hpp>
#include <kernel/time/timer_wheel.hpp>
#include <kernel/nexios_config.h>
#include "test_sched_helpers.hpp"

using namespace kernel;

#ifndef __clang__
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wanalyzer-null-argument"
#endif

namespace {

/// @brief Fabricated regs[] frame for fast handlers (mirrors the fastpath
/// test driver; internal linkage so no ODR clash across test TUs).
struct ToFastFrame {
    uint64_t regs[20];
    ToFastFrame() {
        __builtin_memset(regs, 0, sizeof(regs));
    }
};

/// @brief Dispatch a fast syscall by number with a fabricated frame.
uint64_t to_fast_call(uint64_t number, uint64_t arg0, uint64_t arg1,
                      uint64_t arg2, uint64_t arg3, ToFastFrame &frame) {
    return Syscall::handle(number, arg0, arg1, arg2, arg3, frame.regs);
}

/// @brief Shared driver context for timeout receiver/sender tasks.
struct ToRecvCtx {
    uint64_t peer_id;
    uint64_t result;
    uint64_t timeout_ticks;
    uint64_t send_type;
    uint64_t spin_iters;
};

/// @brief Receiver task: RECV_FAST with ctx timeout, stores the result.
void to_receiver_entry() {
    auto *self = Scheduler::current_task();
    auto *ctx = reinterpret_cast<ToRecvCtx *>(self->user_data);
    ToFastFrame frame;
    const uint64_t recv_res = to_fast_call(
        static_cast<uint64_t>(SyscallNumber::RECV_FAST), 0, 0, 8,
        ctx->timeout_ticks, frame);
    __atomic_store_n(&ctx->result, recv_res, __ATOMIC_RELEASE);
}

/// @brief Sender task: optional spin, then SEND_FAST to the peer.
void to_sender_entry() {
    auto *self = Scheduler::current_task();
    auto *ctx = reinterpret_cast<ToRecvCtx *>(self->user_data);
    for (uint64_t spin = 0; spin < ctx->spin_iters; ++spin) {
        arch::pause();
    }
    ToFastFrame frame;
    const uint64_t send_res = to_fast_call(
        static_cast<uint64_t>(SyscallNumber::SEND_FAST), ctx->peer_id,
        ctx->send_type, 8, 0, frame);
    __atomic_store_n(&ctx->result, send_res, __ATOMIC_RELEASE);
}

/// @brief Spawn a timeout receiver (prio 11, lower than the sender).
TaskControlBlock *to_spawn_receiver(ToRecvCtx &ctx) {
    auto *task = TaskControlBlock::create(
        []() {
            to_receiver_entry();
        },
        11, 10);
    if (task != nullptr) {
        task->user_data = &ctx;
    }
    return task;
}

/// @brief Spawn a delayed sender (prio 12, runs first unless yielded).
TaskControlBlock *to_spawn_sender(ToRecvCtx &ctx) {
    auto *task = TaskControlBlock::create(
        []() {
            to_sender_entry();
        },
        12, 10);
    if (task != nullptr) {
        task->user_data = &ctx;
    }
    return task;
}

/// @brief Null callback for wheel-filling (tolerates null context).
void to_dummy_fire(void *context) {
    (void)context;
}

} // namespace

// Runmode: kernel
// Testidea: fast-path message returns without arming the wheel.
// Input: Dispatched task self-sends, RECV_FAST(timeout=10) with queued msg
// Expect: returns msg type immediately, wheel live_count stays 0
// Depends: sys_recv_fast fast path, IPC::recv_wait_arm
JARVIS_TEST(recv_timeout_fastpath_no_arm, "PRE: none | POST: none") {
    ToRecvCtx recv_ctx{};
    recv_ctx.peer_id = 0;
    recv_ctx.result = 0;
    recv_ctx.timeout_ticks = 10;
    recv_ctx.send_type = 41;
    recv_ctx.spin_iters = 0;
    auto *receiver = TaskControlBlock::create(
        []() {
            auto *self = Scheduler::current_task();
            auto *ctx = reinterpret_cast<ToRecvCtx *>(self->user_data);
            Message msg{};
            msg.sender_id = self->id;
            msg.type = ctx->send_type;
            msg.priority = 0;
            msg.data_size = 0;
            if (!IPC::send(self->id, msg)) {
                return;
            }
            ToFastFrame frame;
            const uint64_t recv_res = to_fast_call(
                static_cast<uint64_t>(SyscallNumber::RECV_FAST), 0, 0, 8,
                ctx->timeout_ticks, frame);
            __atomic_store_n(&ctx->result, recv_res, __ATOMIC_RELEASE);
        },
        11, 10);
    JARVIS_ASSERT(receiver != nullptr);
    receiver->user_data = &recv_ctx;
    {
        arch::IrqGuard guard;
        Scheduler::add_task(*receiver);
    }
    auto *original = Scheduler::current_task();
    kernel::test::yield_as(*receiver);
    Scheduler::reschedule();
    kernel::test::wait_for_termination_safe(receiver);
    Scheduler::set_current(*original);
    JARVIS_ASSERT_EQ(41ULL, recv_ctx.result);
    JARVIS_ASSERT_EQ((uint64_t)0, time::TimerWheel::live_count(0));
    kernel::test::terminate_and_drain(*receiver);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: empty queue with timeout returns -1 and reclaims the slot.
// Input: Dispatched task RECV_FAST(timeout=5) on empty queue, no sender
// Expect: returns -1, wheel live_count back to 0, flags cleared
// Depends: wheel expiry, tick-tail apply, resume recheck
JARVIS_TEST(recv_timeout_fires_minus_one, "PRE: none | POST: none") {
    ToRecvCtx recv_ctx{};
    recv_ctx.result = 0;
    recv_ctx.timeout_ticks = 5;
    uint64_t armed_after = 0;
    auto *receiver = to_spawn_receiver(recv_ctx);
    JARVIS_ASSERT(receiver != nullptr);
    {
        arch::IrqGuard guard;
        Scheduler::add_task(*receiver);
    }
    auto *original = Scheduler::current_task();
    kernel::test::yield_as(*receiver);
    Scheduler::reschedule();
    kernel::test::wait_for_termination_safe(receiver);
    Scheduler::set_current(*original);
    JARVIS_ASSERT_EQ(0xFFFFFFFFFFFFFFFFULL, recv_ctx.result);
    JARVIS_ASSERT_EQ((uint64_t)0, time::TimerWheel::live_count(0));
    armed_after = receiver->recv_timeout_armed ? 1ULL : 0ULL;
    JARVIS_ASSERT_EQ(0ULL, armed_after);
    kernel::test::terminate_and_drain(*receiver);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: message arriving before timeout returns the message, not -1.
// Input: Receiver RECV_FAST(timeout=100); sender delivers after short spin
// Expect: receiver returns msg type, armed timer cancelled (live 0)
// Depends: sender wake path, resume cancel, msg-wins recheck
JARVIS_TEST(recv_timeout_msg_wins, "PRE: none | POST: none") {
    ToRecvCtx recv_ctx{};
    recv_ctx.result = 0;
    recv_ctx.timeout_ticks = 100;
    ToRecvCtx send_ctx{};
    send_ctx.result = 0;
    send_ctx.send_type = 77;
    send_ctx.spin_iters = 2000;
    auto *receiver = to_spawn_receiver(recv_ctx);
    JARVIS_ASSERT(receiver != nullptr);
    auto *sender = to_spawn_sender(send_ctx);
    JARVIS_ASSERT(sender != nullptr);
    send_ctx.peer_id = receiver->id;
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
    JARVIS_ASSERT_EQ(77ULL, recv_ctx.result);
    JARVIS_ASSERT_EQ(0ULL, send_ctx.result);
    JARVIS_ASSERT_EQ((uint64_t)0, time::TimerWheel::live_count(0));
    kernel::test::terminate_and_drain2(sender, receiver);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: timeout 0 still blocks forever until a send wakes it.
// Input: Receiver RECV_FAST(timeout=0); sender delivers after short spin
// Expect: receiver returns msg type, wheel never armed (live 0 throughout)
// Depends: 0=forever legacy contract preservation
JARVIS_TEST(recv_timeout_forever_wakes_on_send, "PRE: none | POST: none") {
    ToRecvCtx recv_ctx{};
    recv_ctx.result = 0;
    recv_ctx.timeout_ticks = 0;
    ToRecvCtx send_ctx{};
    send_ctx.result = 0;
    send_ctx.send_type = 78;
    send_ctx.spin_iters = 2000;
    auto *receiver = to_spawn_receiver(recv_ctx);
    JARVIS_ASSERT(receiver != nullptr);
    auto *sender = to_spawn_sender(send_ctx);
    JARVIS_ASSERT(sender != nullptr);
    send_ctx.peer_id = receiver->id;
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
    JARVIS_ASSERT_EQ(78ULL, recv_ctx.result);
    JARVIS_ASSERT_EQ((uint64_t)0, time::TimerWheel::live_count(0));
    kernel::test::terminate_and_drain2(sender, receiver);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: killing a BLOCKED armed waiter is crash-free and leak-free.
// Input: Receiver RECV_FAST(timeout=30000) blocks; terminate it; run ticks
// Expect: no crash, wheel live_count back to 0 (cleanup cancelled)
// Depends: TaskControlBlock::cleanup cancel, snapshot_reset
JARVIS_TEST(recv_timeout_kill_while_armed, "PRE: none | POST: none") {
    ToRecvCtx recv_ctx{};
    recv_ctx.result = 0;
    recv_ctx.timeout_ticks = 30000;
    auto *receiver = to_spawn_receiver(recv_ctx);
    JARVIS_ASSERT(receiver != nullptr);
    {
        arch::IrqGuard guard;
        Scheduler::add_task(*receiver);
    }
    auto *original = Scheduler::current_task();
    kernel::test::yield_as(*receiver);
    Scheduler::reschedule();
    // Bounded wait until the receiver blocks with its timeout armed.
    bool saw_armed = false;
    for (uint64_t spin = 0; spin < 10000000ULL; ++spin) {
        if (receiver->state == TaskState::BLOCKED &&
            receiver->recv_timeout_armed &&
            time::TimerWheel::live_count(0) == 1) {
            saw_armed = true;
            break;
        }
        arch::pause();
    }
    JARVIS_ASSERT(saw_armed);
    kernel::test::terminate_and_drain(*receiver);
    // Let stray ticks run: nothing may fire and nothing may leak.
    const uint64_t tick_start = arch::Timer::ticks();
    while (arch::Timer::ticks() - tick_start < 3) {
        arch::pause();
    }
    JARVIS_ASSERT_EQ((uint64_t)0, time::TimerWheel::live_count(0));
    Scheduler::set_current(*original);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: stale fire after TCB reuse is a no-op (generation check).
// Input: Arm wheel directly with ctx=live task, bump task timeout-gen,
//        tick past expiry
// Expect: no timed_out set, no wake, no crash
// Depends: IPC::recv_timeout_fire generation validation
JARVIS_TEST(recv_timeout_stale_fire_noop, "PRE: none | POST: none") {
    time::TimerWheel::snapshot_reset();
    auto *self = Scheduler::current_task();
    JARVIS_ASSERT(self != nullptr);
    const uint32_t real_gen = self->generation;
    // Simulate an armed waiter, then a TCB reuse (generation advanced).
    self->recv_timeout_armed = true;
    self->recv_timed_out = false;
    self->recv_timeout_gen = real_gen + 1;
    time::TimerWheel::Handle handle{};
    const uint64_t now_ns = arch::Timer::ns_monotonic();
    JARVIS_ASSERT(time::TimerWheel::arm(0, now_ns + 1000000ULL,
                                        IPC::recv_timeout_fire, self,
                                        &handle));
    JARVIS_ASSERT_EQ((uint32_t)1,
                     time::TimerWheel::on_tick(now_ns + 2000000ULL, 0));
    JARVIS_ASSERT(!self->recv_timed_out);
    JARVIS_ASSERT_EQ((uint64_t)0, time::TimerWheel::live_count(0));
    self->recv_timeout_armed = false;
    self->recv_timed_out = false;
    self->recv_timeout_gen = real_gen;
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: full wheel falls back to coarse poll and still times out.
// Input: Fill all 64 wheel slots, RECV_FAST(timeout=5) on empty queue
// Expect: returns -1 via fallback path, no crash, slots drained after
// Depends: arm()==false fallback, coarse deadline poll
JARVIS_TEST(recv_timeout_full_wheel_fallback, "PRE: none | POST: none") {
    time::TimerWheel::snapshot_reset();
    const uint64_t now_ns = arch::Timer::ns_monotonic();
    for (uint32_t fill_idx = 0;
         fill_idx < time::TimerWheel::kMaxTimersPerCpu; ++fill_idx) {
        time::TimerWheel::Handle filler{};
        JARVIS_ASSERT(time::TimerWheel::arm(0, now_ns + 1000000000000ULL,
                                            to_dummy_fire, nullptr,
                                            &filler));
    }
    JARVIS_ASSERT_EQ((uint64_t)time::TimerWheel::kMaxTimersPerCpu,
                     time::TimerWheel::live_count(0));
    ToRecvCtx recv_ctx{};
    recv_ctx.result = 0;
    recv_ctx.timeout_ticks = 5;
    auto *receiver = to_spawn_receiver(recv_ctx);
    JARVIS_ASSERT(receiver != nullptr);
    {
        arch::IrqGuard guard;
        Scheduler::add_task(*receiver);
    }
    auto *original = Scheduler::current_task();
    kernel::test::yield_as(*receiver);
    Scheduler::reschedule();
    kernel::test::wait_for_termination_safe(receiver);
    Scheduler::set_current(*original);
    JARVIS_ASSERT_EQ(0xFFFFFFFFFFFFFFFFULL, recv_ctx.result);
    kernel::test::terminate_and_drain(*receiver);
    time::TimerWheel::snapshot_reset();
    JARVIS_ASSERT_EQ((uint64_t)0, time::TimerWheel::live_count(0));
    JARVIS_TEST_PASS();
}

void register_ipc_timeout_tests() {
    Logger::info("Registering ipc_timeout tests");
    JARVIS_REGISTER_TEST(recv_timeout_fastpath_no_arm);
    JARVIS_REGISTER_TEST(recv_timeout_fires_minus_one);
    JARVIS_REGISTER_TEST(recv_timeout_msg_wins);
    JARVIS_REGISTER_TEST(recv_timeout_forever_wakes_on_send);
    JARVIS_REGISTER_TEST(recv_timeout_kill_while_armed);
    JARVIS_REGISTER_TEST(recv_timeout_stale_fire_noop);
    JARVIS_REGISTER_TEST(recv_timeout_full_wheel_fallback);
}

#ifndef __clang__
#pragma GCC diagnostic pop
#endif
