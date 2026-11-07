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

/// @file test_tlb_latency.cpp
/// @brief TLB-shootdown latency profiling (issue #160; stubs from issue
///        #85, module 17).  Test-side exact-sample recorder only — hot
///        paths stay uninstrumented.  No icount here (wall-clock TSC),
///        so comparisons use min-of-N plus margins, never tight bounds.
///        cpu_scaling is reframed as batch-size scaling (no AP boots in
///        this harness, so 1-vs-2-CPU comparison is unimplementable).

#if defined(CONFIG_ARCH_X86_64)
#include <test.hpp>
#include <logger.hpp>
#include <kernel/arch/io.hpp>
#include <kernel/arch/timer.hpp>
#include <kernel/arch/hal/irq_guard.hpp>
#include <kernel/arch/apic.hpp>
#include <kernel/arch/idt.hpp>
#include <kernel/arch/x86_64/hal/percpu.hpp>
#include <kernel/arch/x86_64/hal/shootdown_ipi.hpp>
#include <kernel/arch/page_table.hpp>

using namespace kernel;

namespace {

// Unbatched-leg probe vector: class 0x70, free (0x71 TPR probe, 0x72
// batch probe, 0x73 shootdown batch, 0xE0 timer, 0x80 syscall, 0xEF
// self-test, 0xEC sched, 0xFF spurious taken).
constexpr uint8_t kUnbatchedVector = 0x74;

volatile uint64_t g_tlb_unbatched_count = 0;

// Exact-sample recorder (test-side only): fixed ring, min/avg/p99 over
// stored samples.  Zero deltas tolerated (coarse virtualized TSC).
constexpr uint64_t kMaxSamples = 256;
uint64_t t_samples[kMaxSamples] = {};
uint64_t t_sample_count = 0;

void tlb_lat_reset() {
    t_sample_count = 0;
    for (uint64_t i = 0; i < kMaxSamples; ++i)
        t_samples[i] = 0;
}

bool tlb_lat_record(uint64_t delta) {
    if (t_sample_count >= kMaxSamples)
        return false;
    t_samples[t_sample_count++] = delta;
    return true;
}

uint64_t tlb_lat_min() {
    uint64_t best = 0xFFFFFFFFFFFFFFFFULL;
    for (uint64_t i = 0; i < t_sample_count; ++i) {
        if (t_samples[i] < best)
            best = t_samples[i];
    }
    return (t_sample_count == 0) ? 0 : best;
}

uint64_t tlb_lat_max() {
    uint64_t best = 0;
    for (uint64_t i = 0; i < t_sample_count; ++i) {
        if (t_samples[i] > best)
            best = t_samples[i];
    }
    return best;
}

uint64_t tlb_lat_avg() {
    if (t_sample_count == 0)
        return 0;
    uint64_t sum = 0;
    for (uint64_t i = 0; i < t_sample_count; ++i)
        sum += t_samples[i];
    return sum / t_sample_count;
}

uint64_t tlb_lat_p99() {
    if (t_sample_count == 0)
        return 0;
    uint64_t sorted[kMaxSamples];
    for (uint64_t i = 0; i < t_sample_count; ++i)
        sorted[i] = t_samples[i];
    for (uint64_t i = 1; i < t_sample_count; ++i) {
        uint64_t key = sorted[i];
        uint64_t j = i;
        while (j > 0 && sorted[j - 1] > key) {
            sorted[j] = sorted[j - 1];
            --j;
        }
        sorted[j] = key;
    }
    uint64_t idx = (99 * t_sample_count + 99) / 100 - 1;
    if (idx >= t_sample_count)
        idx = t_sample_count - 1;
    return sorted[idx];
}

void tlb_lat_dump(const char *tag) {
    Logger::info("[TLB] ");
    Logger::info(tag);
    Logger::info(": n=");
    Logger::print_dec(t_sample_count);
    Logger::info(" min=");
    Logger::print_dec(tlb_lat_min());
    Logger::info(" avg=");
    Logger::print_dec(tlb_lat_avg());
    Logger::info(" max=");
    Logger::print_dec(tlb_lat_max());
    Logger::info(" p99=");
    Logger::print_dec(tlb_lat_p99());
    Logger::info("");
}

uint64_t tlb_delta(uint64_t start, uint64_t end) {
    return (end > start) ? (end - start) : 0;
}

} // namespace

// Runmode: kernel
// Testidea: Every TLB flush records its latency (TSC delta).
// Input: N=32 direct flushes, sample count afterwards.
// Expect: 32 samples recorded; max > 0 (zero deltas tolerated —
//         coarse virtualized TSC can repeat).
// Depends: Test-side recorder + ArchPageTable::tlb_flush (issue #160)
JARVIS_TEST(tlb_latency_recorded, "PRE: none | POST: none") {
    tlb_lat_reset();
    {
        arch::IrqGuard irq_guard{};
        for (uint64_t i = 0; i < 32; ++i) {
            uint64_t start = arch::rdtsc();
            arch::ArchPageTable::tlb_flush(0x39000000ULL + i * 0x1000);
            uint64_t end = arch::rdtsc();
            JARVIS_ASSERT(tlb_lat_record(tlb_delta(start, end)));
        }
    }
    JARVIS_ASSERT_EQ(32ULL, t_sample_count);
    JARVIS_ASSERT(tlb_lat_max() > 0);
    tlb_lat_dump("recorded");
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Batched average latency beats the unbatched baseline.
// Input: 16 requests unbatched (16 self-IPIs) vs batched (16 entries,
//        1 IPI); min-of-5 totals each leg, IF=1 throughout.
// Expect: min_batched_total < min_unbatched_total (gap is ~15 IPI
//         round-trips — strict-less is robust under host noise).
// Depends: ShootdownBatch delivery vs raw send_ipi (issue #160)
JARVIS_TEST(tlb_latency_avg_improves, "PRE: none | POST: none") {
    JARVIS_ASSERT(arch::APIC::is_enabled());
    uint64_t freq = arch::Timer::tsc_freq_hz();
    JARVIS_ASSERT(freq > 0);
    arch::IDT::register_handler_raw(
        kUnbatchedVector, [](uint64_t, uint64_t, uint64_t) {
            g_tlb_unbatched_count = g_tlb_unbatched_count + 1;
            arch::APIC::eoi();
        });
    arch::IDT::register_handler_raw(
        arch::APIC::SHOOTDOWN_BATCH_VECTOR,
        [](uint64_t, uint64_t, uint64_t) {
            arch::ShootdownBatch::handle_cpu(arch::cpu_index());
            arch::APIC::eoi();
        });
    uint32_t bsp = arch::APIC::lapic_id();
    uint64_t self = arch::cpu_index();
    constexpr uint64_t k_n = 16;
    constexpr int k_repeats = 5;
    uint64_t best_unbatched = 0xFFFFFFFFFFFFFFFFULL;
    uint64_t best_batched = 0xFFFFFFFFFFFFFFFFULL;
    for (int r = 0; r < k_repeats; ++r) {
        // Unbatched leg: 16 separate self-IPIs.
        g_tlb_unbatched_count = 0;
        uint64_t t0 = arch::rdtsc();
        for (uint64_t i = 0; i < k_n; ++i) {
            JARVIS_ASSERT(arch::APIC::send_ipi(
                bsp, kUnbatchedVector, arch::APIC::IpiMode::FIXED));
        }
        uint64_t start = arch::rdtsc();
        uint64_t limit = freq / 5;
        while (g_tlb_unbatched_count < k_n) {
            if (arch::rdtsc() - start > limit)
                break;
            asm volatile("pause");
        }
        uint64_t t1 = arch::rdtsc();
        JARVIS_ASSERT_EQ(k_n, g_tlb_unbatched_count);
        uint64_t unbatched = tlb_delta(t0, t1);
        if (unbatched < best_unbatched)
            best_unbatched = unbatched;
        // Batched leg: 16 entries, 1 IPI.
        arch::ShootdownBatch::reset_for_test();
        for (uint64_t i = 0; i < k_n; ++i) {
            JARVIS_ASSERT(arch::ShootdownBatch::queue_remote(
                self, 0x3A000000ULL + i * 0x1000, 1));
        }
        uint64_t applied_before =
            arch::ShootdownBatch::applied_for_test();
        t0 = arch::rdtsc();
        JARVIS_ASSERT_EQ(1ULL, arch::ShootdownBatch::flush_target(self));
        start = arch::rdtsc();
        while (arch::ShootdownBatch::applied_for_test() <
               applied_before + k_n) {
            if (arch::rdtsc() - start > limit)
                break;
            asm volatile("pause");
        }
        t1 = arch::rdtsc();
        JARVIS_ASSERT_EQ(applied_before + k_n,
                         arch::ShootdownBatch::applied_for_test());
        uint64_t batched = tlb_delta(t0, t1);
        if (batched < best_batched)
            best_batched = batched;
    }
    JARVIS_ASSERT(best_batched < best_unbatched);
    Logger::info("[TLB] avg16: min_unbatched=");
    Logger::print_dec(best_unbatched);
    Logger::info(" min_batched=");
    Logger::print_dec(best_batched);
    Logger::info("");
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: P99 flush latency stays within 2x the average.
// Input: 200 NONZERO single-flush samples (coarse virtualized TSC
//        yields mostly-zero deltas — collecting zeros would drag the
//        average below any nonzero p99 by construction) under one
//        IrqGuard.
// Expect: p99 <= 2 * avg over the nonzero population (idx 197 of 200
//         tolerates the 2 worst host-stall outliers; uniform op keeps
//         the body tight).
// Depends: Test-side recorder (issue #160)
JARVIS_TEST(tlb_latency_p99_bound, "PRE: none | POST: none") {
    tlb_lat_reset();
    {
        arch::IrqGuard irq_guard{};
        uint64_t collected = 0;
        for (uint64_t i = 0; i < 20000 && collected < 200; ++i) {
            uint64_t start = arch::rdtsc();
            arch::ArchPageTable::tlb_flush(0x3B000000ULL +
                                           (i % 64) * 0x1000);
            uint64_t end = arch::rdtsc();
            uint64_t delta = tlb_delta(start, end);
            if (delta == 0)
                continue;
            JARVIS_ASSERT(tlb_lat_record(delta));
            ++collected;
        }
        JARVIS_ASSERT_EQ(200ULL, collected);
    }
    JARVIS_ASSERT_EQ(200ULL, t_sample_count);
    uint64_t avg = tlb_lat_avg();
    JARVIS_ASSERT(avg > 0);
    JARVIS_ASSERT(tlb_lat_p99() <= 2 * avg);
    tlb_lat_dump("p99");
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Flush cost amortizes with batch size (reframed cpu_scaling:
//           no AP boots here, so 1-vs-2-CPU comparison is
//           unimplementable — batch-1 vs batch-16 per-request cost
//           instead).
// Input: Per-request cost at batch 1 vs batch 16, min-of-5 each, IF=1.
// Expect: min_per_req_16 <= min_per_req_1 (one IPI amortized over 16
//         flushes vs one IPI per flush).
// Depends: ShootdownBatch delivery (issue #160)
JARVIS_TEST(tlb_latency_cpu_scaling, "PRE: none | POST: none") {
    arch::IDT::register_handler_raw(
        arch::APIC::SHOOTDOWN_BATCH_VECTOR,
        [](uint64_t, uint64_t, uint64_t) {
            arch::ShootdownBatch::handle_cpu(arch::cpu_index());
            arch::APIC::eoi();
        });
    uint64_t self = arch::cpu_index();
    uint64_t freq = arch::Timer::tsc_freq_hz();
    JARVIS_ASSERT(freq > 0);
    constexpr int k_repeats = 5;
    uint64_t best_per_1 = 0xFFFFFFFFFFFFFFFFULL;
    uint64_t best_per_16 = 0xFFFFFFFFFFFFFFFFULL;
    for (int r = 0; r < k_repeats; ++r) {
        // Batch-1 leg: 1 entry, 1 IPI.
        arch::ShootdownBatch::reset_for_test();
        JARVIS_ASSERT(arch::ShootdownBatch::queue_remote(self,
                                                         0x3C000000ULL,
                                                         1));
        uint64_t applied_before =
            arch::ShootdownBatch::applied_for_test();
        uint64_t t0 = arch::rdtsc();
        JARVIS_ASSERT_EQ(1ULL, arch::ShootdownBatch::flush_target(self));
        uint64_t start = arch::rdtsc();
        uint64_t limit = freq / 5;
        while (arch::ShootdownBatch::applied_for_test() <
               applied_before + 1) {
            if (arch::rdtsc() - start > limit)
                break;
            asm volatile("pause");
        }
        uint64_t t1 = arch::rdtsc();
        JARVIS_ASSERT_EQ(applied_before + 1,
                         arch::ShootdownBatch::applied_for_test());
        uint64_t per_1 = tlb_delta(t0, t1);
        if (per_1 < best_per_1)
            best_per_1 = per_1;
        // Batch-16 leg: 16 entries, still 1 IPI.
        arch::ShootdownBatch::reset_for_test();
        for (uint64_t i = 0; i < 16; ++i) {
            JARVIS_ASSERT(arch::ShootdownBatch::queue_remote(
                self, 0x3D000000ULL + i * 0x1000, 1));
        }
        applied_before = arch::ShootdownBatch::applied_for_test();
        t0 = arch::rdtsc();
        JARVIS_ASSERT_EQ(1ULL, arch::ShootdownBatch::flush_target(self));
        start = arch::rdtsc();
        while (arch::ShootdownBatch::applied_for_test() <
               applied_before + 16) {
            if (arch::rdtsc() - start > limit)
                break;
            asm volatile("pause");
        }
        t1 = arch::rdtsc();
        JARVIS_ASSERT_EQ(applied_before + 16,
                         arch::ShootdownBatch::applied_for_test());
        uint64_t per_16 = tlb_delta(t0, t1) / 16;
        if (per_16 < best_per_16)
            best_per_16 = per_16;
    }
    JARVIS_ASSERT(best_per_16 <= best_per_1);
    Logger::info("[TLB] scale: min_per_1=");
    Logger::print_dec(best_per_1);
    Logger::info(" min_per_16=");
    Logger::print_dec(best_per_16);
    Logger::info("");
    JARVIS_TEST_PASS();
}

void register_tlb_latency_tests() {
    Logger::info("Registering tlb latency tests");
    JARVIS_REGISTER_TEST(tlb_latency_recorded);
    JARVIS_REGISTER_TEST(tlb_latency_avg_improves);
    JARVIS_REGISTER_TEST(tlb_latency_p99_bound);
    JARVIS_REGISTER_TEST(tlb_latency_cpu_scaling);
}
#endif  // CONFIG_ARCH_X86_64
