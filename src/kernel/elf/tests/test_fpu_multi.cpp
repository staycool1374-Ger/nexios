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

/// @file test_fpu_multi.cpp
/// @brief FPU multi-task interference tests.

#if defined(CONFIG_ARCH_X86_64)
#include <test.hpp>
#include <logger.hpp>
#include <kernel/arch/io.hpp>
#include <kernel/task/scheduler.hpp>
#include <kernel/task/task.hpp>
#include <kernel/memory/mempool.hpp>
#include "test_sched_helpers.hpp"

using namespace kernel;

// IEEE 754 double constants (raw bits, no C++ float types)
static constexpr uint64_t FPU_PI_BITS = 0x400921F9F01B866EULL;
static constexpr uint64_t FPU_EULER_BITS = 0x4005BF0A8B145769ULL;
static constexpr uint64_t FPU_SQRT2_BITS = 0x3FF6A09E667F3BCDULL;

static volatile uint64_t g_multi_a_result = 0;
static volatile uint64_t g_multi_b_result = 0;
static volatile uint64_t g_multi_c_result = 0;
static volatile int g_multi_a_done = 0;
static volatile int g_multi_b_done = 0;
static volatile int g_multi_c_done = 0;

static void multi_task_a_entry() {
    uint64_t pi = FPU_PI_BITS;
    uint64_t out = 0;

    asm volatile("finit\nfldl %0" : : "m"(pi) : "memory");
    g_multi_a_done = 1;
    kernel::Scheduler::reschedule();

    asm volatile("fstpl %0\nfinit" : "=m"(out) : : "st");
    g_multi_a_result = out;
    g_multi_a_done = 2;
}

static void multi_task_b_entry() {
    while (g_multi_a_done < 1)
        kernel::Scheduler::reschedule();

    uint64_t euler = FPU_EULER_BITS;
    uint64_t out = 0;

    asm volatile("finit\nfldl %0" : : "m"(euler) : "memory");
    g_multi_b_done = 1;
    kernel::Scheduler::reschedule();

    asm volatile("fstpl %0\nfinit" : "=m"(out) : : "st");
    g_multi_b_result = out;
    g_multi_b_done = 2;
}

static void multi_task_c_entry() {
    while (g_multi_b_done < 1)
        kernel::Scheduler::reschedule();

    uint64_t sqrt2 = FPU_SQRT2_BITS;
    uint64_t out = 0;

    asm volatile("finit\nfldl %0" : : "m"(sqrt2) : "memory");
    g_multi_c_done = 1;
    kernel::Scheduler::reschedule();

    asm volatile("fstpl %0\nfinit" : "=m"(out) : : "st");
    g_multi_c_result = out;
    g_multi_c_done = 2;
}

// Runmode: kernel
// Testidea: 3-way FPU lazy switch preserves distinct IEEE 754 values
// Input: Three tasks load pi, euler, sqrt2 respectively, yield, read back
// Expect: Each task reads back its original value
JARVIS_TEST(fpu_multi_context_switch, "PRE: none | POST: none") {
    g_multi_a_result = 0;
    g_multi_b_result = 0;
    g_multi_c_result = 0;
    g_multi_a_done = 0;
    g_multi_b_done = 0;
    g_multi_c_done = 0;

    auto *task_a = TaskControlBlock::create(multi_task_a_entry, 1, 10);
    auto *task_b = TaskControlBlock::create(multi_task_b_entry, 2, 10);
    auto *task_c = TaskControlBlock::create(multi_task_c_entry, 3, 10);

    JARVIS_ASSERT(task_a != nullptr);
    JARVIS_ASSERT(task_b != nullptr);
    JARVIS_ASSERT(task_c != nullptr);

    // Register all three cooperating tasks under one IrqGuard so a timer tick
    // cannot split the registration (cookbook Rule 2).
    {
        arch::IrqGuard guard;
        Scheduler::add_task(*task_a);
        Scheduler::add_task(*task_b);
        Scheduler::add_task(*task_c);
    }

    // Wait for all tasks to reach verification step (done == 2)
    for (int i = 0; i < 600; ++i) {
        if (g_multi_a_done == 2 && g_multi_b_done == 2 && g_multi_c_done == 2)
            break;
        Scheduler::reschedule();
    }

    JARVIS_ASSERT_FMT(g_multi_a_done == 2, "Task A did not finish (state=%d)",
                      g_multi_a_done);
    JARVIS_ASSERT_FMT(g_multi_b_done == 2, "Task B did not finish (state=%d)",
                      g_multi_b_done);
    JARVIS_ASSERT_FMT(g_multi_c_done == 2, "Task C did not finish (state=%d)",
                      g_multi_c_done);


    kernel::test::terminate_and_drain3(task_a, task_b, task_c);
    JARVIS_ASSERT_FMT(
        g_multi_a_result == FPU_PI_BITS, "Task A 0x%016llx != pi 0x%016llx",
        (unsigned long long)g_multi_a_result, (unsigned long long)FPU_PI_BITS);
    JARVIS_ASSERT_FMT(g_multi_b_result == FPU_EULER_BITS,
                      "Task B 0x%016llx != euler 0x%016llx",
                      (unsigned long long)g_multi_b_result,
                      (unsigned long long)FPU_EULER_BITS);
    JARVIS_ASSERT_FMT(g_multi_c_result == FPU_SQRT2_BITS,
                      "Task C 0x%016llx != sqrt2 0x%016llx",
                      (unsigned long long)g_multi_c_result,
                      (unsigned long long)FPU_SQRT2_BITS);

    JARVIS_TEST_PASS();
}

void register_fpu_multi_tests() {
    Logger::info("Registering 3-way FPU context switch tests");
    JARVIS_REGISTER_TEST(fpu_multi_context_switch);
}
#endif
