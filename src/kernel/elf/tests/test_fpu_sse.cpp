/// @file test_fpu_sse.cpp
/// @brief FPU SSE register preservation tests.

#if defined(CONFIG_ARCH_X86_64)
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

#include <test.hpp>
#include <logger.hpp>
#include <kernel/arch/cpuid.hpp>
#include <kernel/arch/io.hpp>
#include <kernel/task/scheduler.hpp>
#include <kernel/task/task.hpp>
#include <kernel/memory/mempool.hpp>
#include "test_sched_helpers.hpp"

using namespace kernel;

// Runmode: kernel
// Testidea: CPUID reports SSE and SSE2 feature flags on x86_64
// Expect: Both bits should be set on any modern x86_64 CPU/emulator
JARVIS_TEST(sse_cpuid_detection, "PRE: none | POST: none") {
    JARVIS_ASSERT_FMT(arch::has_sse(), "CPUID leaf 1 EDX bit 25 (SSE) not set");
    uint32_t edx = arch::cpuid(1).edx;
    JARVIS_ASSERT_FMT(edx & arch::CPUID_EDX1_SSE2,
                      "CPUID leaf 1 EDX bit 26 (SSE2) not set");
    JARVIS_ASSERT_FMT(arch::has_fxsr(),
                      "CPUID leaf 1 EDX bit 24 (FXSR) not set");
    JARVIS_TEST_PASS();
}

// ── MXCSR context switch test ──────────────────────────────────────────────

static constexpr uint32_t MXCSR_ROUND_ZERO = 0x7F80;
static volatile uint64_t g_sse_test_a_done = 0;
static volatile uint64_t g_sse_test_b_done = 0;
static volatile uint32_t g_mxcsr_result = 0;

static void sse_task_a_entry() {
    // Set MXCSR to round-toward-zero (bits 13:14 = 0b11, all masks set)
    arch::ldmxcsr(MXCSR_ROUND_ZERO);

    g_sse_test_a_done = 1;
    kernel::Scheduler::reschedule();

    // Read MXCSR back
    uint32_t mxcsr = 0;
    asm volatile("stmxcsr %0" : "=m"(mxcsr));
    g_mxcsr_result = mxcsr;
    g_sse_test_a_done = 2;
}

static void sse_task_b_entry() {
    while (!g_sse_test_a_done) {
        kernel::Scheduler::reschedule();
    }

    // Load different MXCSR to trigger #NM and FPU/SSE switch
    arch::ldmxcsr(0x1F80);

    g_sse_test_b_done = 1;
}

// Runmode: kernel
// Testidea: Lazy FPU/SSE switch preserves MXCSR state across task boundaries
// Input: Task A sets MXCSR to 0x7F80 (round-toward-zero), yields; Task B
//        sets MXCSR to 0x1F80 (default); switch back to A
// Expect: Task A reads back MXCSR = 0x7F80
JARVIS_TEST(sse_mxcsr_context_switch, "PRE: none | POST: none") {
    g_mxcsr_result = 0;
    g_sse_test_a_done = 0;
    g_sse_test_b_done = 0;

    auto *task_a = TaskControlBlock::create(sse_task_a_entry, 1, 10);
    auto *task_b = TaskControlBlock::create(sse_task_b_entry, 2, 10);

    JARVIS_ASSERT(task_a != nullptr);
    JARVIS_ASSERT(task_b != nullptr);

    Scheduler::add_task(*task_a);
    Scheduler::add_task(*task_b);

    for (int i = 0; i < 200 && !g_sse_test_b_done; ++i) {
        Scheduler::reschedule();
    }
    JARVIS_ASSERT_FMT(g_sse_test_b_done,
                      "Task B did not complete within 200 reschedule calls");

    for (int i = 0; i < 200 && g_sse_test_a_done != 2; ++i) {
        Scheduler::reschedule();
    }
    JARVIS_ASSERT_FMT(g_sse_test_a_done == 2,
                      "Task A did not reach verification step (state=%llu)",
                      (unsigned long long)g_sse_test_a_done);

    uint32_t mxcsr = g_mxcsr_result;

    kernel::test::terminate_and_drain2(task_a, task_b);
    JARVIS_ASSERT_FMT(mxcsr == MXCSR_ROUND_ZERO,
                      "MXCSR after switch 0x%08x != expected 0x%08x", mxcsr,
                      MXCSR_ROUND_ZERO);

    JARVIS_TEST_PASS();
}

// ── XMM register context switch test ───────────────────────────────────────

// 128-bit SSE test patterns as uint64_t pairs (no C++ float types)
struct alignas(16) Sse128 {
    uint64_t d[2];
};
static const Sse128 SSE_PATTERN_A = {
    {0x0123456789ABCDEFULL, 0xFEDCBA9876543210ULL}};
static const Sse128 SSE_PATTERN_B = {
    {0xAABBCCDDEEFF0011ULL, 0x2233445566778899ULL}};
static volatile uint64_t g_xmm_test_a_done = 0;
static volatile uint64_t g_xmm_test_b_done = 0;
static Sse128 g_xmm_result = {{0, 0}};

static void xmm_task_a_entry() {
    // Load XMM0 with pattern A
    asm volatile("movaps %0, %%xmm0" : : "m"(SSE_PATTERN_A.d) : "xmm0");

    g_xmm_test_a_done = 1;
    kernel::Scheduler::reschedule();

    // Read XMM0 back
    asm volatile("movaps %%xmm0, %0" : "=m"(g_xmm_result.d) : : "xmm0");

    g_xmm_test_a_done = 2;
}

static void xmm_task_b_entry() {
    while (!g_xmm_test_a_done) {
        kernel::Scheduler::reschedule();
    }

    // Load XMM0 with different pattern to trigger #NM and FPU/SSE switch
    asm volatile("movaps %0, %%xmm0" : : "m"(SSE_PATTERN_B.d) : "xmm0");

    g_xmm_test_b_done = 1;
}

// Runmode: kernel
// Testidea: Lazy FPU/SSE switch preserves XMM register state
// Input: Task A loads XMM0 with known 128-bit pattern, yields; Task B loads
//        XMM0 with different pattern; switch back to A
// Expect: Task A reads back original 128-bit pattern from XMM0
JARVIS_TEST(sse_xmm_context_switch, "PRE: none | POST: none") {
    g_xmm_result.d[0] = 0;
    g_xmm_result.d[1] = 0;
    g_xmm_test_a_done = 0;
    g_xmm_test_b_done = 0;

    auto *task_a = TaskControlBlock::create(xmm_task_a_entry, 3, 10);
    auto *task_b = TaskControlBlock::create(xmm_task_b_entry, 4, 10);

    JARVIS_ASSERT(task_a != nullptr);
    JARVIS_ASSERT(task_b != nullptr);

    Scheduler::add_task(*task_a);
    Scheduler::add_task(*task_b);

    for (int i = 0; i < 200 && !g_xmm_test_b_done; ++i) {
        Scheduler::reschedule();
    }
    JARVIS_ASSERT_FMT(g_xmm_test_b_done,
                      "Task B did not complete within 200 reschedule calls");

    for (int i = 0; i < 200 && g_xmm_test_a_done != 2; ++i) {
        Scheduler::reschedule();
    }
    JARVIS_ASSERT_FMT(g_xmm_test_a_done == 2,
                      "Task A did not reach verification step (state=%llu)",
                      (unsigned long long)g_xmm_test_a_done);


    kernel::test::terminate_and_drain2(task_a, task_b);
    JARVIS_ASSERT_FMT(g_xmm_result.d[0] == SSE_PATTERN_A.d[0],
                      "XMM0[0] after switch 0x%016llx != expected 0x%016llx",
                      (unsigned long long)g_xmm_result.d[0],
                      (unsigned long long)SSE_PATTERN_A.d[0]);
    JARVIS_ASSERT_FMT(g_xmm_result.d[1] == SSE_PATTERN_A.d[1],
                      "XMM0[1] after switch 0x%016llx != expected 0x%016llx",
                      (unsigned long long)g_xmm_result.d[1],
                      (unsigned long long)SSE_PATTERN_A.d[1]);

    JARVIS_TEST_PASS();
}

void register_fpu_sse_tests() {
    Logger::info("Registering SSE/XMM context switch tests");
    JARVIS_REGISTER_TEST(sse_cpuid_detection);
    JARVIS_REGISTER_TEST(sse_mxcsr_context_switch);
    JARVIS_REGISTER_TEST(sse_xmm_context_switch);
}
#endif
