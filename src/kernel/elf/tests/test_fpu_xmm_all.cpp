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

/// @file test_fpu_xmm_all.cpp
/// @brief FPU full XMM register file tests.

#if defined(CONFIG_ARCH_X86_64)
#include <test.hpp>
#include <logger.hpp>
#include <kernel/arch/cpuid.hpp>
#include <kernel/arch/io.hpp>
#include <kernel/task/scheduler.hpp>
#include <kernel/task/task.hpp>
#include <kernel/memory/mempool.hpp>
#include <string.hpp>
#include "test_sched_helpers.hpp"

using namespace kernel;

// ── 16-XMM register context switch test ─────────────────────────────────

struct alignas(16) Sse128 {
    uint64_t d[2];
};

// 16 unique 128-bit patterns — one per XMM register (no C++ float types)
static const Sse128 XMM_PATTERN_A[16] = {
    {{0x1111111111111111ULL, 0xAAAAAAAAAAAAAAAAULL}}, // xmm0
    {{0x2222222222222222ULL, 0xBBBBBBBBBBBBBBBBULL}}, // xmm1
    {{0x3333333333333333ULL, 0xCCCCCCCCCCCCCCCCULL}}, // xmm2
    {{0x4444444444444444ULL, 0xDDDDDDDDDDDDDDDDULL}}, // xmm3
    {{0x5555555555555555ULL, 0xEEEEEEEEEEEEEEEEULL}}, // xmm4
    {{0x6666666666666666ULL, 0xFFFFFFFFFFFFFFFFULL}}, // xmm5
    {{0x7777777777777777ULL, 0x0123456789ABCDEFULL}}, // xmm6
    {{0x8888888888888888ULL, 0xFEDCBA9876543210ULL}}, // xmm7
    {{0x9999999999999999ULL, 0xAABBCCDDEEFF0011ULL}}, // xmm8
    {{0xAAAAAAAAAAAAAAAAULL, 0x2233445566778899ULL}}, // xmm9
    {{0xBBBBBBBBBBBBBBBBULL, 0x9988776655443322ULL}}, // xmm10
    {{0xCCCCCCCCCCCCCCCCULL, 0x00FFEEDDCCBBAA99ULL}}, // xmm11
    {{0xDDDDDDDDDDDDDDDDULL, 0x1122334455667788ULL}}, // xmm12
    {{0xEEEEEEEEEEEEEEEEULL, 0x8877665544332211ULL}}, // xmm13
    {{0xFFFFFFFFFFFFFFFFULL, 0xDEADBEEFCAFEBABEULL}}, // xmm14
    {{0x0011223344556677ULL, 0x0F1E2D3C4B5A6978ULL}}, // xmm15
};

static Sse128 XMM_PATTERN_B = {{0xA5A5A5A55A5A5A5AULL, 0x5A5A5A5AA5A5A5A5ULL}};
static Sse128 g_xmm_all_result[16];
static volatile int g_xmm_all_a_done = 0;
static volatile int g_xmm_all_b_done = 0;

static void xmm_all_task_a_entry() {
    // Load all 16 XMM registers via a single pointer operand + offsets
    asm volatile("movaps   0(%0), %%xmm0\n"
                 "movaps  16(%0), %%xmm1\n"
                 "movaps  32(%0), %%xmm2\n"
                 "movaps  48(%0), %%xmm3\n"
                 "movaps  64(%0), %%xmm4\n"
                 "movaps  80(%0), %%xmm5\n"
                 "movaps  96(%0), %%xmm6\n"
                 "movaps 112(%0), %%xmm7\n"
                 "movaps 128(%0), %%xmm8\n"
                 "movaps 144(%0), %%xmm9\n"
                 "movaps 160(%0), %%xmm10\n"
                 "movaps 176(%0), %%xmm11\n"
                 "movaps 192(%0), %%xmm12\n"
                 "movaps 208(%0), %%xmm13\n"
                 "movaps 224(%0), %%xmm14\n"
                 "movaps 240(%0), %%xmm15\n"
                 :
                 : "r"(XMM_PATTERN_A)
                 : "xmm0", "xmm1", "xmm2", "xmm3", "xmm4", "xmm5", "xmm6",
                   "xmm7", "xmm8", "xmm9", "xmm10", "xmm11", "xmm12", "xmm13",
                   "xmm14", "xmm15");

    g_xmm_all_a_done = 1;
    kernel::Scheduler::reschedule();

    // Read back all 16 XMM registers via a single pointer operand
    asm volatile("movaps %%xmm0,   0(%0)\n"
                 "movaps %%xmm1,  16(%0)\n"
                 "movaps %%xmm2,  32(%0)\n"
                 "movaps %%xmm3,  48(%0)\n"
                 "movaps %%xmm4,  64(%0)\n"
                 "movaps %%xmm5,  80(%0)\n"
                 "movaps %%xmm6,  96(%0)\n"
                 "movaps %%xmm7, 112(%0)\n"
                 "movaps %%xmm8, 128(%0)\n"
                 "movaps %%xmm9, 144(%0)\n"
                 "movaps %%xmm10,160(%0)\n"
                 "movaps %%xmm11,176(%0)\n"
                 "movaps %%xmm12,192(%0)\n"
                 "movaps %%xmm13,208(%0)\n"
                 "movaps %%xmm14,224(%0)\n"
                 "movaps %%xmm15,240(%0)\n"
                 :
                 : "r"(g_xmm_all_result)
                 : "xmm0", "xmm1", "xmm2", "xmm3", "xmm4", "xmm5", "xmm6",
                   "xmm7", "xmm8", "xmm9", "xmm10", "xmm11", "xmm12", "xmm13",
                   "xmm14", "xmm15", "memory");

    g_xmm_all_a_done = 2;
}

static void xmm_all_task_b_entry() {
    while (!g_xmm_all_a_done) {
        kernel::Scheduler::reschedule();
    }

    // Load different pattern into XMM0 to trigger #NM switch
    asm volatile("movaps %0, %%xmm0" : : "m"(XMM_PATTERN_B.d) : "xmm0");

    g_xmm_all_b_done = 1;
}

// Runmode: kernel
// Testidea: Lazy FPU/SSE switch preserves all 16 XMM registers
// Input: Task A loads XMM0–XMM15 with unique 128-bit patterns, yields;
//        Task B touches XMM0 (different pattern); switch back to A
// Expect: All 16 XMM registers read back their original patterns
JARVIS_TEST(sse_xmm_all_registers, "PRE: none | POST: none") {
    __builtin_memset(g_xmm_all_result, 0, sizeof(g_xmm_all_result));
    g_xmm_all_a_done = 0;
    g_xmm_all_b_done = 0;

    auto *task_a = TaskControlBlock::create(xmm_all_task_a_entry, 1, 10);
    auto *task_b = TaskControlBlock::create(xmm_all_task_b_entry, 2, 10);

    JARVIS_ASSERT(task_a != nullptr);
    JARVIS_ASSERT(task_b != nullptr);

    Scheduler::add_task(*task_a);
    Scheduler::add_task(*task_b);

    for (int i = 0; i < 200 && !g_xmm_all_b_done; ++i) {
        Scheduler::reschedule();
    }
    JARVIS_ASSERT_FMT(g_xmm_all_b_done,
                      "Task B did not complete within 200 reschedule calls");

    for (int i = 0; i < 200 && g_xmm_all_a_done != 2; ++i) {
        Scheduler::reschedule();
    }
    JARVIS_ASSERT_FMT(g_xmm_all_a_done == 2,
                      "Task A did not reach verification step (state=%d)",
                      g_xmm_all_a_done);

    for (int i = 0; i < 16; ++i) {
        JARVIS_ASSERT_FMT(
            g_xmm_all_result[i].d[0] == XMM_PATTERN_A[i].d[0] &&
                g_xmm_all_result[i].d[1] == XMM_PATTERN_A[i].d[1],
            "XMM%d mismatch: got {%016llx,%016llx} != expected {%016llx,%016llx}",
            i, (unsigned long long)g_xmm_all_result[i].d[0],
            (unsigned long long)g_xmm_all_result[i].d[1],
            (unsigned long long)XMM_PATTERN_A[i].d[0],
            (unsigned long long)XMM_PATTERN_A[i].d[1]);
    }

    kernel::test::terminate_and_drain2(task_a, task_b);

    JARVIS_TEST_PASS();
}

void register_fpu_xmm_all_tests() {
    Logger::info("Registering 16-XMM register context switch tests");
    JARVIS_REGISTER_TEST(sse_xmm_all_registers);
}
#endif
