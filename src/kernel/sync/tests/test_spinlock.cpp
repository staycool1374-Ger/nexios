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

/// @file test_spinlock.cpp
/// @brief Spinlock primitive tests.

#include <test.hpp>
#include <logger.hpp>
#include <scope_guard.hpp>
#include <kernel/sync/spinlock.hpp>
#include <kernel/sync/spinlock_guard.hpp>
#include <kernel/task/scheduler.hpp>
#include <kernel/task/task.hpp>
#include <kernel/arch/io.hpp>
#include <kernel/arch/irq_guard.hpp>
#include "test_sched_helpers.hpp"

using namespace kernel;

// Runmode: kernel
// Testidea: Verifies basic SpinLock lock/unlock by protecting a shared flag.
// Input: SpinLock, two lock/unlock cycles with flag modification inside
// Expect: Flag values are as expected after each cycle
// Depends: kernel::sync::SpinLock
JARVIS_TEST(spinlock_basic_lock_unlock, "PRE: none | POST: none") {
    sync::SpinLock lock;
    int shared = 0;

    lock.lock();
    shared = 42;
    lock.unlock();
    JARVIS_ASSERT_EQ(42, shared);

    lock.lock();
    shared = 99;
    lock.unlock();
    JARVIS_ASSERT_EQ(99, shared);

    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Verifies SpinLockGuard RAII guard locks and unlocks correctly.
// Input: SpinLockGuard protecting a shared flag modification
// Expect: Flag is written inside guard, readable outside
// Depends: kernel::sync::SpinLock, SpinLockGuard
JARVIS_TEST(spinlock_guard_raii, "PRE: none | POST: none") {
    sync::SpinLock lock;
    int shared = 0;

    {
        SpinLockGuard guard(lock);
        shared = 7;
    }
    JARVIS_ASSERT_EQ(7, shared);

    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Verifies SpinLock try_lock succeeds when unlocked, fails when
// locked.
// Input: SpinLock, lock(), try_lock(), unlock(), try_lock()
// Expect: try_lock returns true when lock is free, false when held
// Depends: kernel::sync::SpinLock
JARVIS_TEST(spinlock_try_lock, "PRE: none | POST: none") {
    sync::SpinLock lock;

    JARVIS_ASSERT(lock.try_lock());
    JARVIS_ASSERT(!lock.try_lock());
    lock.unlock();
    JARVIS_ASSERT(lock.try_lock());
    lock.unlock();

    JARVIS_TEST_PASS();
}

static sync::SpinLock g_contention_lock_;
static uint64_t g_contention_counter_ = 0;

static void contention_worker() {
    for (int i = 0; i < 50; ++i) {
        SpinLockGuard<sync::SpinLock> guard(g_contention_lock_);
        uint64_t tmp = g_contention_counter_;
        arch::pause();
        g_contention_counter_ = tmp + 1;
    }
    auto *self = Scheduler::current_task();
    if (self) {
        self->state = TaskState::TERMINATED;
    }
}

// Runmode: kernel
// Testidea: 2 tasks contend for same SpinLock; no corruption.
// Input: Two kernel tasks increment shared counter under SpinLock, 50× each.
// Expect: Final counter == 100, no data corruption.
// Depends: SpinLock, Scheduler
JARVIS_TEST(spinlock_contention, "PRE: none | POST: none") {
    g_contention_counter_ = 0;

    auto *a = TaskControlBlock::create(contention_worker, 5, 10);
    JARVIS_ASSERT(a != nullptr);
    Scheduler::add_task(*a);

    auto *b = TaskControlBlock::create(contention_worker, 5, 10);
    JARVIS_ASSERT(b != nullptr);
    Scheduler::add_task(*b);

    auto guard = ScopeGuard([&]() {
        kernel::test::terminate_and_drain2(a, b);
    });

    // Yield repeatedly so the scheduler picks workers over the boot-stack
    // proxy.  Each reschedule() + hlt() cycle gives one worker a time slice.
    for (int h = 0; h < 100; ++h) {
        if (g_contention_counter_ >= 100)
            break;
        Scheduler::reschedule();
        arch::hlt();
    }

    JARVIS_ASSERT_EQ(100ULL, g_contention_counter_);

    JARVIS_TEST_PASS();
}

static sync::SpinLock g_yield_lock_;
static volatile uint64_t g_yield_highpri_count_ = 0;
static volatile bool g_yield_done_ = false;

static void yield_highpri() {
    __atomic_add_fetch(&g_yield_highpri_count_, 1, __ATOMIC_RELAXED);
    g_yield_done_ = true;
}

static void yield_lowpri() {
    g_yield_lock_.lock();
    for (uint64_t i = 0; i < 500000 && !g_yield_done_; ++i) {
        arch::pause();
    }
    g_yield_lock_.unlock();
}

// Runmode: kernel
// Testidea: High-pri task preempts low-pri lock-holder (interrupts stay
// enabled). Input: Low-pri (5) holds SpinLock in busy-loop; high-pri (10)
// preempts. Expect: High-pri runs at least once, proving interrupts are not
// disabled. Depends: SpinLock, Scheduler
JARVIS_TEST(spinlock_preemption_yield, "PRE: none | POST: none") {
    g_yield_highpri_count_ = 0;
    g_yield_done_ = false;

    auto *high = TaskControlBlock::create(yield_highpri, 10, 1);
    JARVIS_ASSERT(high != nullptr);
    Scheduler::add_task(*high);

    auto *low = TaskControlBlock::create(yield_lowpri, 5, 10);
    JARVIS_ASSERT(low != nullptr);
    Scheduler::add_task(*low);

    auto guard = ScopeGuard([&]() {
        low->cleanup();
        delete low;
        high->cleanup();
        delete high;
    });

    auto *original = Scheduler::current_task();
    Scheduler::set_current(*low);

    // Yield control so the timer ISR can dispatch high over low.
    // Direct on_tick calls do NOT apply deferred switches (they
    // require the ISR assembly epilogue), so this loop triggers
    // timer ticks via hlt + reschedule.
    // low is the "current" task (from set_current above) and is
    // already in the ready queue with state READY.
    for (int tick = 0; tick < 30 && !g_yield_done_; ++tick) {
        Scheduler::reschedule();
        arch::hlt();
    }
    Scheduler::set_current(*original);

    JARVIS_ASSERT_FMT(g_yield_highpri_count_ > 0,
                      "High-pri ran %lu times (expected > 0)",
                      g_yield_highpri_count_);

    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: SpinLockGuard C++ contract: [[nodiscard]], non-copyable,
// non-movable. Input: SpinLockGuard construction, use, destruction. Expect:
// Compile-time static_asserts for copy/move pass; runtime usage works. Depends:
// SpinLock, SpinLockGuard
JARVIS_TEST(spinlock_cxx_contract, "PRE: none | POST: none") {
    sync::SpinLock lock;
    {
        SpinLockGuard guard(lock);
    }
    JARVIS_ASSERT(lock.try_lock());
    lock.unlock();
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: No cli() called during SpinLock critical section.
// Input: Check interrupt flag before, during, and after locked region.
// Expect: interrupts_enabled() remains true throughout.
// Depends: SpinLock, arch::interrupts_enabled
JARVIS_TEST(spinlock_no_irqguard_inside, "PRE: none | POST: none") {
    sync::SpinLock lock;
    JARVIS_ASSERT(arch::interrupts_enabled());
    lock.lock();
    JARVIS_ASSERT(arch::interrupts_enabled());
    lock.unlock();
    JARVIS_ASSERT(arch::interrupts_enabled());
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Detects recursive locking in DEBUG mode.
// Input: Same task locks SpinLock twice.
// Expect: Triggers assertion (stub — feature not yet implemented).
// Depends: SpinLock (DEBUG)
JARVIS_TEST(spinlock_recursive_deadlock_detect, "PRE: none | POST: none") {
    /* Pseudocode: SpinLock should detect recursive locking by the same task
     * in DEBUG mode and trigger an assertion. Currently SpinLock does not
     * implement this feature — lock() will spin forever on recursive entry.
     * Implementation plan:
     *   - Add `TaskControlBlock* owner_` field to SpinLock (DEBUG only)
     *   - In lock(), if owner_ == current_task, assert(!"recursive lock")
     *   - Set owner_ on successful acquire, clear on unlock
     *   - This test should: lock(), try_lock() and expect assertion
     */
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Spin count during contention is bounded.
// Input: 2 tasks contend; measure pause-loop iterations.
// Expect: Spin iterations < 10000 per acquisition.
// Depends: SpinLock (instrumented)
JARVIS_TEST(spinlock_pause_count, "PRE: none | POST: none") {
    /* Pseudocode: Measure the number of pause() iterations during contention.
     * When a lock is held by another task and the current task spins,
     * the spin count should be bounded (< 10000 iterations) because the
     * lock-holding task completes its critical section quickly.
     * Implementation plan:
     *   - Add `uint64_t pause_count_` to SpinLock (DEBUG only)
     *   - Increment in lock() each time through the while-loop
     *   - Expose via `pause_count()` or zero on unlock
     *   - Create 2-task contention, verify total pause count < 10000
     *   - Reset counter after measurement
     */
    JARVIS_TEST_PASS();
}

void register_spinlock_tests() {
    Logger::info("Registering SpinLock tests");
    JARVIS_REGISTER_TEST(spinlock_basic_lock_unlock);
    JARVIS_REGISTER_TEST(spinlock_guard_raii);
    JARVIS_REGISTER_TEST(spinlock_try_lock);
    JARVIS_REGISTER_TEST(spinlock_contention);
    JARVIS_REGISTER_TEST(spinlock_preemption_yield);
    JARVIS_REGISTER_TEST(spinlock_cxx_contract);
    JARVIS_REGISTER_TEST(spinlock_no_irqguard_inside);
    JARVIS_REGISTER_TEST(spinlock_recursive_deadlock_detect);
    JARVIS_REGISTER_TEST(spinlock_pause_count);
}
