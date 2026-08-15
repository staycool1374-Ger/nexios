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

/// @file test_signals.cpp
/// @brief Signal delivery and handling tests.

#include <test.hpp>
#include <logger.hpp>
#include <signal.hpp>
#include <kernel/sync/semaphore.hpp>
#include <kernel/task/scheduler.hpp>
#include <kernel/syscall/syscall.hpp>
#include <kernel/test/test_sched_helpers.hpp>

using namespace kernel;

static void test_signal_handler(int sig);

// Runmode: kernel
// Testidea: Tests that exception numbers map to the correct signals and
// default actions via exception_to_signal().
// Input: Calls exception_to_signal for exceptions 0, 6, 13, 14, 16.
// Expect: JARVIS_ASSERT_EQ checks exception 0 -> SIGFPE/TERMINATE, 6 ->
// SIGILL, 13/14 -> SIGSEGV, 16 -> SIGFPE.
// Depends: kernel::signal, kernel::exception_to_signal
JARVIS_TEST(signal_exception_to_signal_mapping, "PRE: none | POST: none") {
    auto map0 = exception_to_signal(0);
    JARVIS_ASSERT_EQ(static_cast<uint64_t>(Signal::SIGFPE),
                     static_cast<uint64_t>(map0.signal));
    JARVIS_ASSERT_EQ(static_cast<uint64_t>(SignalAction::TERMINATE),
                     static_cast<uint64_t>(map0.action));

    auto map6 = exception_to_signal(6);
    JARVIS_ASSERT_EQ(static_cast<uint64_t>(Signal::SIGILL),
                     static_cast<uint64_t>(map6.signal));

    auto map13 = exception_to_signal(13);
    JARVIS_ASSERT_EQ(static_cast<uint64_t>(Signal::SIGSEGV),
                     static_cast<uint64_t>(map13.signal));

    auto map14 = exception_to_signal(14);
    JARVIS_ASSERT_EQ(static_cast<uint64_t>(Signal::SIGSEGV),
                     static_cast<uint64_t>(map14.signal));

    auto map16 = exception_to_signal(16);
    JARVIS_ASSERT_EQ(static_cast<uint64_t>(Signal::SIGFPE),
                     static_cast<uint64_t>(map16.signal));
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Tests signal_is_fatal() returns true only for SIGKILL and false
// for other signals (SIGSEGV, SIGFPE, SIGILL, SIGTERM).
// Input: Calls signal_is_fatal on SIGKILL, SIGSEGV, SIGFPE, SIGILL, SIGTERM.
// Expect: JARVIS_ASSERT checks SIGKILL is fatal; others are not.
// Depends: kernel::signal, kernel::signal_is_fatal
JARVIS_TEST(signal_is_fatal_check, "PRE: none | POST: none") {
    JARVIS_ASSERT(signal_is_fatal(static_cast<uint64_t>(Signal::SIGKILL)));
    JARVIS_ASSERT(!signal_is_fatal(static_cast<uint64_t>(Signal::SIGSEGV)));
    JARVIS_ASSERT(!signal_is_fatal(static_cast<uint64_t>(Signal::SIGFPE)));
    JARVIS_ASSERT(!signal_is_fatal(static_cast<uint64_t>(Signal::SIGILL)));
    JARVIS_ASSERT(!signal_is_fatal(static_cast<uint64_t>(Signal::SIGTERM)));
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Tests that default_signal_action() returns the correct action
// for different signals.
// Input: Calls default_signal_action on SIGSEGV, SIGCHLD, SIGKILL.
// Expect: JARVIS_ASSERT_EQ checks SIGSEGV -> TERMINATE, SIGCHLD -> IGNORE,
// SIGKILL -> TERMINATE.
// Depends: kernel::signal, kernel::default_signal_action
JARVIS_TEST(signal_default_action, "PRE: none | POST: none") {
    auto term = default_signal_action(static_cast<uint64_t>(Signal::SIGSEGV));
    JARVIS_ASSERT_EQ(static_cast<uint64_t>(SignalAction::TERMINATE),
                     static_cast<uint64_t>(term));

    auto ignore = default_signal_action(static_cast<uint64_t>(Signal::SIGCHLD));
    JARVIS_ASSERT_EQ(static_cast<uint64_t>(SignalAction::IGNORE),
                     static_cast<uint64_t>(ignore));

    auto fatal = default_signal_action(static_cast<uint64_t>(Signal::SIGKILL));
    JARVIS_ASSERT_EQ(static_cast<uint64_t>(SignalAction::TERMINATE),
                     static_cast<uint64_t>(fatal));
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Tests registering, querying, and unregistering a signal handler
// on the current task's TCB.
// Input: Sets SIGSEGV handler to 0xDEAD, checks has/get, then clears it to
// nullptr.
// Expect: JARVIS_ASSERT and JARVIS_ASSERT_EQ verify handler registration
// state transitions correctly.
// Depends: kernel::task::Scheduler, kernel::signal
JARVIS_TEST(signal_handler_tcb_registration, "PRE: none | POST: none") {
    auto *cur = Scheduler::current_task();
    JARVIS_ASSERT(cur != nullptr);

    JARVIS_ASSERT(
        !cur->has_signal_handler(static_cast<uint64_t>(Signal::SIGSEGV)));

    auto handler =
        reinterpret_cast<sighandler_t>(static_cast<uintptr_t>(0xDEAD));
    cur->set_signal_handler(static_cast<uint64_t>(Signal::SIGSEGV), handler);
    JARVIS_ASSERT(
        cur->has_signal_handler(static_cast<uint64_t>(Signal::SIGSEGV)));
    JARVIS_ASSERT_EQ(reinterpret_cast<uint64_t>(handler),
                     reinterpret_cast<uint64_t>(cur->get_signal_handler(
                         static_cast<uint64_t>(Signal::SIGSEGV))));

    cur->set_signal_handler(static_cast<uint64_t>(Signal::SIGSEGV), nullptr);
    JARVIS_ASSERT(
        !cur->has_signal_handler(static_cast<uint64_t>(Signal::SIGSEGV)));
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Tests that out-of-bounds signal numbers (100, 31) are silently
// rejected by the TCB handler functions.
// Input: Sets handler for signal 100 and 31, then checks has_signal_handler.
// Expect: JARVIS_ASSERT checks has_signal_handler returns false for both
// out-of-bounds indices.
// Depends: kernel::task::Scheduler, kernel::signal
JARVIS_TEST(signal_handler_out_of_bounds, "PRE: none | POST: none") {
    auto *cur = Scheduler::current_task();
    JARVIS_ASSERT(cur != nullptr);

    cur->set_signal_handler(
        100, reinterpret_cast<sighandler_t>(static_cast<uintptr_t>(0xFF)));
    JARVIS_ASSERT(!cur->has_signal_handler(100));

    cur->set_signal_handler(31, nullptr);
    JARVIS_ASSERT(!cur->has_signal_handler(31));
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Tests that the pending_signals bitmask on the current task can
// be set and cleared correctly.
// Input: Sets SIGSEGV bit in pending_signals, checks it, then clears it.
// Expect: JARVIS_ASSERT_EQ checks pending_signals starts at 0, has the bit
// after set, and returns to 0 after clear.
// Depends: kernel::task::Scheduler, kernel::signal
JARVIS_TEST(signal_pending_bitmask, "PRE: none | POST: none") {
    auto *cur = Scheduler::current_task();
    JARVIS_ASSERT(cur != nullptr);

    JARVIS_ASSERT_EQ(0ULL, cur->pending_signals);

    cur->pending_signals |= (1ULL << static_cast<uint64_t>(Signal::SIGSEGV));
    JARVIS_ASSERT(cur->pending_signals &
                  (1ULL << static_cast<uint64_t>(Signal::SIGSEGV)));

    cur->pending_signals &= ~(1ULL << static_cast<uint64_t>(Signal::SIGSEGV));
    JARVIS_ASSERT_EQ(0ULL, cur->pending_signals);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: The KILL syscall, invoked from a REAL dispatched task, sets the
// pending signal bit for SIGUSR1 on a target task that has registered a
// handler via the SIGNAL syscall.
// Input: Receiver task (prio 11) registers a SIGUSR1 handler (SIGNAL syscall)
//        and blocks on a gate; sender task (prio 12) calls KILL(receiver,
//        SIGUSR1).
// Expect: KILL returns 0; receiver's pending_signals has SIGUSR1 set; after
// the gate is posted the receiver terminates normally.
// Depends: kernel::syscall::Syscall (KILL), kernel::task::Scheduler,
// kernel::signal
JARVIS_TEST(signal_kill_delivers, "PRE: none | POST: none") {
    static uint64_t g_sender_ret = 0;
    static uint64_t g_receiver_id = 0;
    static uint64_t g_registered = 0;

    sync::Semaphore gate;
    gate.init(0, 1);

    // Receiver: registers its own SIGUSR1 handler via the SIGNAL syscall,
    // then genuinely blocks on the gate.
    auto *receiver = TaskControlBlock::create(
        []() {
            auto *self = Scheduler::current_task();
            uint64_t ret = Syscall::handle(
                static_cast<uint64_t>(SyscallNumber::SIGNAL),
                static_cast<uint64_t>(Signal::SIGUSR1),
                reinterpret_cast<uint64_t>(test_signal_handler), 0, 0,
                nullptr);
            if (ret == 0)
                g_registered = 1;
            sync::Semaphore *g = reinterpret_cast<sync::Semaphore *>(
                self->user_data);
            g->wait();
            // Semaphore::wait() only sets BLOCKED and defers the switch
            // (INV-4); it returns immediately.  Spin until the timer ISR
            // actually suspends this task, then exit when post() wakes it.
            while (self->state == TaskState::BLOCKED)
                arch::pause();
        },
        11, 10);
    JARVIS_ASSERT(receiver != nullptr);
    receiver->user_data = &gate;
    Scheduler::add_task(*receiver);
    Scheduler::reschedule();
    while (receiver->state != TaskState::BLOCKED)
        arch::pause();
    JARVIS_ASSERT_EQ(1ULL, g_registered);
    g_receiver_id = receiver->id;

    // Sender: dispatches and calls KILL on the receiver.
    auto *sender = TaskControlBlock::create(
        []() {
            g_sender_ret = Syscall::handle(
                static_cast<uint64_t>(SyscallNumber::KILL), g_receiver_id,
                static_cast<uint64_t>(Signal::SIGUSR1), 0, 0, nullptr);
        },
        12, 10);
    JARVIS_ASSERT(sender != nullptr);
    Scheduler::add_task(*sender);
    Scheduler::reschedule();
    kernel::test::wait_for_termination_safe(sender);
    JARVIS_ASSERT_EQ(0ULL, g_sender_ret);
    JARVIS_ASSERT(receiver->pending_signals &
                  (1ULL << static_cast<uint64_t>(Signal::SIGUSR1)));

    gate.post();
    kernel::test::wait_for_termination_safe(receiver);
    // Cleanup BEFORE asserting (cookbook Rule 5): both self-terminated.
    Scheduler::drain_zombie_list();
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Tests that a registered signal handler can be set and the
// pending bit can be manually raised.
// Input: Registers SIGUSR1 handler, then sets pending_signals bit for SIGUSR1.
// Expect: JARVIS_ASSERT checks has_signal_handler returns true; pending bit
// is set.
// Depends: kernel::task::Scheduler, kernel::signal
JARVIS_TEST(signal_handler_invoked, "PRE: none | POST: none") {
    auto *cur = Scheduler::current_task();
    JARVIS_ASSERT(cur != nullptr);

    cur->set_signal_handler(static_cast<uint64_t>(Signal::SIGUSR1),
                            test_signal_handler);
    JARVIS_ASSERT(
        cur->has_signal_handler(static_cast<uint64_t>(Signal::SIGUSR1)));

    cur->pending_signals |= (1ULL << static_cast<uint64_t>(Signal::SIGUSR1));
    JARVIS_TEST_PASS();
}

static void test_signal_handler(int sig) {
    (void)sig;
}

// Runmode: kernel
// Testidea: Registers all signal test cases into the test framework.
// Input: None
// Expect: Each JARVIS_REGISTER_TEST call adds the test to the suite.
// Depends: kernel::test
void register_signals_tests() {
    Logger::info("Registering signal tests");

    JARVIS_REGISTER_TEST(signal_exception_to_signal_mapping);
    JARVIS_REGISTER_TEST(signal_is_fatal_check);
    JARVIS_REGISTER_TEST(signal_default_action);
    JARVIS_REGISTER_TEST(signal_handler_tcb_registration);
    JARVIS_REGISTER_TEST(signal_handler_out_of_bounds);
    JARVIS_REGISTER_TEST(signal_pending_bitmask);
    JARVIS_REGISTER_TEST(signal_kill_delivers);
    JARVIS_REGISTER_TEST(signal_handler_invoked);
}
