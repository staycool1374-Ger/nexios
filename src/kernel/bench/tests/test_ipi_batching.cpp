/*
 * NexIOS RTOS — SMP bring-up (Phase 5)
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

/// @file test_ipi_batching.cpp
/// @brief IPI-batching tests (issue #85, module 16).  The transport
///        half (INIT/SIPI acceptance, FIXED delivery) is covered by
///        the smp_ipi class; no batching layer exists, so this file
///        pins the current unbatched baseline (real) plus documented
///        batching stubs.

#if defined(CONFIG_ARCH_X86_64)
#include <test.hpp>
#include <logger.hpp>
#include <kernel/arch/apic.hpp>
#include <kernel/arch/idt.hpp>
#include <kernel/arch/timer.hpp>
#include <kernel/arch/io.hpp>
#include <kernel/arch/x86_64/hal/percpu.hpp>
#include <kernel/arch/x86_64/hal/shootdown_ipi.hpp>
#include <kernel/nexios_config.h>

using namespace kernel;

namespace {

// Batch-probe vector: class 0x70, free (0x71 is the TPR probe; timer
// 0xE0, syscall 0x80, self-test 0xEF, sched 0xEC, spurious 0xFF taken).
constexpr uint8_t kBatchProbeVector = 0x72;

// Delivery counter for the baseline test.
volatile uint64_t g_batch_ipi_count = 0;

// Register the shootdown-batch ISR (self-IPI delivery for tests).
// EOI stays with the registrant, mirroring the 0x72 probe pattern.
void register_batch_handler() {
    arch::IDT::register_handler_raw(
        arch::APIC::SHOOTDOWN_BATCH_VECTOR,
        [](uint64_t, uint64_t, uint64_t) {
            arch::ShootdownBatch::handle_cpu(arch::cpu_index());
            arch::APIC::eoi();
        });
}

}  // namespace

// Runmode: kernel
// Testidea: Pin the unbatched baseline: K FIXED self-IPIs produce
//           exactly K handler runs (1:1, no coalescing today).  A future
//           batching layer must preserve delivery count while cutting
//           IPI count — this test documents the denominator.
// Input: 5x send_ipi(self, 0x72, FIXED); TSC-bounded poll for count 5.
// Expect: send accepted x5; counter reaches exactly 5 within 200 ms.
// Depends: arch::APIC::send_ipi, IDT::register_handler_raw
JARVIS_TEST(ipi_batching_unbatched_baseline, "PRE: iocd | POST: none") {
    JARVIS_ASSERT(arch::APIC::is_enabled());
    uint64_t freq = arch::Timer::tsc_freq_hz();
    JARVIS_ASSERT(freq > 0);
    arch::IDT::register_handler_raw(
        kBatchProbeVector, [](uint64_t, uint64_t, uint64_t) {
            g_batch_ipi_count = g_batch_ipi_count + 1;
            arch::APIC::eoi();
        });
    uint32_t bsp = arch::APIC::lapic_id();
    g_batch_ipi_count = 0;
    for (uint64_t i = 0; i < 5; ++i) {
        JARVIS_ASSERT(arch::APIC::send_ipi(bsp, kBatchProbeVector,
                                           arch::APIC::IpiMode::FIXED));
    }
    uint64_t start = arch::rdtsc();
    uint64_t limit = freq / 5;
    while (g_batch_ipi_count < 5) {
        if (arch::rdtsc() - start > limit) {
            break;
        }
        asm volatile("pause");
    }
    JARVIS_ASSERT_EQ(static_cast<uint64_t>(5), g_batch_ipi_count);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Multiple pending flushes collapse into a single IPI.
// Input: N queued shootdown requests for self, one flush.
// Expect: Exactly one IPI accepted (wrapper send counter) and all N
//         entries applied (handler counter).  No APIC-coalescing
//         ambiguity: the send counter is deterministic.
// Depends: ShootdownBatch queue/flush delivery (issue #159)
JARVIS_TEST(ipi_batching_collapses, "PRE: none | POST: none") {
    register_batch_handler();
    arch::ShootdownBatch::reset_for_test();
    uint64_t self = arch::cpu_index();
    constexpr uint64_t k_n = 8;
    for (uint64_t i = 0; i < k_n; ++i) {
        JARVIS_ASSERT(arch::ShootdownBatch::queue_remote(
            self, 0x35000000ULL + i * 0x1000, 1));
    }
    uint64_t sends = arch::ShootdownBatch::flush_target(self);
    JARVIS_ASSERT_EQ(1ULL, sends);
    JARVIS_ASSERT_EQ(1ULL, arch::ShootdownBatch::sends_for_test());
    JARVIS_ASSERT_EQ(k_n, arch::ShootdownBatch::applied_for_test());
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Batch processing order is deterministic (address order).
// Input: Shuffled flush requests, one flush.
// Expect: Handler applies VAs in ascending order every run (slot
//         stays sorted on insert; applied log proves it).
// Depends: ShootdownBatch sorted insert (issue #159)
JARVIS_TEST(ipi_batching_order, "PRE: none | POST: none") {
    register_batch_handler();
    arch::ShootdownBatch::reset_for_test();
    uint64_t self = arch::cpu_index();
    constexpr uint64_t k_vas[] = {0x36007000ULL, 0x36001000ULL,
                                  0x36005000ULL, 0x36000000ULL,
                                  0x36003000ULL, 0x36002000ULL,
                                  0x36006000ULL, 0x36004000ULL};
    constexpr uint64_t k_n = sizeof(k_vas) / sizeof(k_vas[0]);
    for (uint64_t i = 0; i < k_n; ++i) {
        JARVIS_ASSERT(arch::ShootdownBatch::queue_remote(self, k_vas[i],
                                                         2));
    }
    JARVIS_ASSERT_EQ(1ULL, arch::ShootdownBatch::flush_target(self));
    JARVIS_ASSERT_EQ(k_n, arch::ShootdownBatch::applied_for_test());
    for (uint64_t i = 1; i < k_n; ++i) {
        JARVIS_ASSERT(arch::ShootdownBatch::applied_va_for_test(i - 1) <
                      arch::ShootdownBatch::applied_va_for_test(i));
    }
    JARVIS_ASSERT(arch::ShootdownBatch::applied_va_for_test(0) ==
                  0x36000000ULL);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Batch size never overflows the IPI message buffer.
// Input: More requests than one slot holds (16 + 1).
// Expect: Bounded multi-batch (exactly 2 sends), all 17 applied —
//         never fail-closed, never overwritten.
// Depends: ShootdownBatch overflow protocol (issue #159)
JARVIS_TEST(ipi_batching_no_overflow, "PRE: none | POST: none") {
    register_batch_handler();
    arch::ShootdownBatch::reset_for_test();
    uint64_t self = arch::cpu_index();
    constexpr uint64_t k_cap = arch::ShootdownBatch::BATCH_SLOTS;
    for (uint64_t i = 0; i < k_cap; ++i) {
        JARVIS_ASSERT(arch::ShootdownBatch::queue_remote(
            self, 0x37000000ULL + i * 0x1000, 3));
    }
    JARVIS_ASSERT(!arch::ShootdownBatch::queue_remote(self, 0x38000000ULL,
                                                      3));
    JARVIS_ASSERT_EQ(1ULL, arch::ShootdownBatch::flush_target(self));
    JARVIS_ASSERT_EQ(k_cap, arch::ShootdownBatch::applied_for_test());
    JARVIS_ASSERT(arch::ShootdownBatch::queue_remote(self, 0x38000000ULL,
                                                     3));
    JARVIS_ASSERT_EQ(1ULL, arch::ShootdownBatch::flush_target(self));
    JARVIS_ASSERT_EQ(k_cap + 1,
                     arch::ShootdownBatch::applied_for_test());
    JARVIS_ASSERT_EQ(2ULL, arch::ShootdownBatch::sends_for_test());
    JARVIS_TEST_PASS();
}

void register_ipi_batching_tests() {
    Logger::info("Registering ipi batching tests");
    JARVIS_REGISTER_TEST(ipi_batching_unbatched_baseline);
    JARVIS_REGISTER_TEST(ipi_batching_collapses);
    JARVIS_REGISTER_TEST(ipi_batching_order);
    JARVIS_REGISTER_TEST(ipi_batching_no_overflow);
}
#endif  // CONFIG_ARCH_X86_64
