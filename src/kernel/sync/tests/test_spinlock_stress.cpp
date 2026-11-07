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

/// @file test_spinlock_stress.cpp
/// @brief Spinlock stress / contention tests.

#include <test.hpp>
#include <logger.hpp>
#include <scope_guard.hpp>
#include <kernel/sync/spinlock.hpp>
#include <kernel/sync/spinlock_guard.hpp>
#include <kernel/task/scheduler.hpp>
#include <kernel/task/task.hpp>

using namespace kernel;

static sync::SpinLock stress_lock_;
static uint64_t stress_counter_ = 0;
static uint64_t stress_acquires_[4] = {};

static void stress_worker() {
    auto *cur = Scheduler::current_task();
    size_t idx = cur ? (cur->id % 4) : 0;
    for (int i = 0; i < 100; ++i) {
        SpinLockGuard<sync::SpinLock> guard(stress_lock_);
        uint64_t val = stress_counter_;
        arch::pause();
        stress_counter_ = val + 1;
        ++stress_acquires_[idx];
    }
    if (cur)
        cur->state = TaskState::TERMINATED;
}

// Runmode: kernel
// Testidea: Multiple kernel tasks contend for the same SpinLock.
// Input: 4 tasks all loop 100× each, lock, increment shared counter, unlock.
// Expect: Final counter == 400, each task made progress.
// Depends: SpinLock, Scheduler
JARVIS_TEST(spinlock_multi_task_contention, "PRE: none | POST: none") {
    stress_counter_ = 0;
    for (auto &a : stress_acquires_)
        a = 0;

    TaskControlBlock *workers[4] = {};
    for (int i = 0; i < 4; ++i) {
        workers[i] = TaskControlBlock::create(stress_worker, 5, 10);
        JARVIS_ASSERT(workers[i] != nullptr);
        Scheduler::add_task(*workers[i]);
    }

    auto guard = ScopeGuard([&]() {
        for (int i = 0; i < 4; ++i) {
            if (workers[i]) {
                if (workers[i]->state != TaskState::TERMINATED)
                    Scheduler::remove_task(*workers[i]);
                workers[i]->cleanup();
                delete workers[i];
            }
        }
    });

    for (int t = 0; t < 500; ++t) {
        Scheduler::reschedule();
        arch::hlt();
        if (stress_counter_ >= 400)
            break;
    }

    JARVIS_ASSERT_EQ(400ULL, stress_counter_);
    for (int i = 0; i < 4; ++i) {
        JARVIS_ASSERT_FMT(stress_acquires_[i] > 0,
                          "Worker %d acquired lock %lu times (expected > 0)", i,
                          stress_acquires_[i]);
    }

    guard.dismiss();
    for (int i = 0; i < 4; ++i) {
        if (workers[i]) {
            if (workers[i]->state != TaskState::TERMINATED)
                Scheduler::remove_task(*workers[i]);
            workers[i]->cleanup();
            delete workers[i];
        }
    }

    JARVIS_TEST_PASS();
}

void register_spinlock_stress_tests() {
    Logger::info("Registering spinlock stress tests");
    JARVIS_REGISTER_TEST(spinlock_multi_task_contention);
}
