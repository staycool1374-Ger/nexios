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

/// @file test_profiler_sampler.cpp
/// @brief Sampling-profiler tests (milestone v0.4.10 issue #129): the
///        in-kernel Sampler API had no test — it was exercised only by
///        `make profile-class`, which is not a test class.
/// @note  Why this matters beyond coverage: the sampler is initialised and
///        ENABLED unconditionally at boot (kernel.cpp:933-935, no
///        CONFIG_DEBUG gate) and `record_sample()` runs inside the timer ISR
///        (hal/timer.cpp:100) in every build.  A bounds or ring-wrap bug is
///        therefore an interrupt-path bug in production, not a dead
///        diagnostic.
/// @note  Determinism: `record_sample()` is also driven by the timer ISR, so
///        every measurement runs inside an arch::IrqGuard (scheduler frozen)
///        and starts from `clear()` (which resets the sample sequence, so the
///        rate gate is reproducible).  Each test restores the boot state
///        (enabled, rate 10, cleared) BEFORE its first assertion, so an
///        assertion early-return can never leave the profiler disabled.

#include <test.hpp>
#include <logger.hpp>
#include <kernel/profiling/sampler.hpp>
#include <kernel/arch/irq_guard.hpp>
#include <string.hpp>

using namespace kernel;
using kernel::profiling::Sampler;

namespace {

/// @brief Sentinel returned by find_function() when no range contains ip.
constexpr uint32_t k_no_function = 0xFFFFFFFFu;

/// @brief Documented sampling-buffer capacity (sampler.cpp).
constexpr size_t k_buffer_size = 4096;

/// @brief Boot-time configuration that every test must restore.
constexpr uint32_t k_boot_rate = 10;

/// @brief Restores the boot sampler state (enabled, rate 10, no samples).
void restore_boot_state() {
    Sampler::clear();
    Sampler::set_sample_rate(k_boot_rate);
    Sampler::set_enabled(true);
}

} // namespace

// Runmode: kernel
// Testidea: The rate gate is a modulo over an internal monotonic sequence,
//           not over Timer::ticks() (which the test harness rewinds): with
//           rate N exactly every N-th record_sample() call is kept.  The gate
//           must also honour set_enabled() — a disabled sampler records
//           nothing at all — and is_enabled() must report the live flag.
// Input: With the scheduler frozen: enable + rate 4 + 8 calls; rate 1 + 8
//        calls; then disable + 8 calls.
// Expect: 2 samples at rate 4 (seq 0 and 4), 8 at rate 1, and 0 additional
//         while disabled; is_enabled() true/false accordingly.
// Depends: profiling::Sampler, arch::IrqGuard
JARVIS_TEST(sampler_rate_gate_counts_samples,
            "PRE: vfsd, iocd | POST: none") {
    size_t gated = 0;
    size_t ungated = 0;
    size_t while_disabled = 0;
    bool enabled_on = false;
    bool enabled_off = false;

    {
        arch::IrqGuard irq_guard{};

        Sampler::set_enabled(true);
        enabled_on = Sampler::is_enabled();
        Sampler::set_sample_rate(4);
        Sampler::clear();
        for (int call = 0; call < 8; ++call)
            Sampler::record_sample(0x1000 + static_cast<uint64_t>(call));
        gated = Sampler::get_sample_count();

        Sampler::set_sample_rate(1);
        Sampler::clear();
        for (int call = 0; call < 8; ++call)
            Sampler::record_sample(0x2000 + static_cast<uint64_t>(call));
        ungated = Sampler::get_sample_count();

        Sampler::set_enabled(false);
        enabled_off = Sampler::is_enabled();
        Sampler::clear();
        for (int call = 0; call < 8; ++call)
            Sampler::record_sample(0x3000 + static_cast<uint64_t>(call));
        while_disabled = Sampler::get_sample_count();
    }

    restore_boot_state();

    JARVIS_ASSERT(enabled_on);
    JARVIS_ASSERT(!enabled_off);
    JARVIS_ASSERT_EQ(static_cast<size_t>(2), gated);
    JARVIS_ASSERT_EQ(static_cast<size_t>(8), ungated);
    JARVIS_ASSERT_EQ(static_cast<size_t>(0), while_disabled);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: The sample ring is a power-of-two array with a free-running
//           head; wrapping must lose OLD SAMPLES but never corrupt the
//           monotonic sample counter, and must never write outside the
//           buffer.  Recording more than the capacity is the exact condition
//           a long profiling run hits.
// Input: With the scheduler frozen: enable + rate 1 + clear, then
//        record_sample() called k_buffer_size + 10 times.
// Expect: get_sample_count() equals the number of calls (4106) — the counter
//         is not clamped by the ring — and remains strictly greater than the
//         buffer capacity.
// Depends: profiling::Sampler, arch::IrqGuard
JARVIS_TEST(sampler_ring_wraps_without_losing_count,
            "PRE: vfsd, iocd | POST: none") {
    constexpr size_t k_calls = k_buffer_size + 10;
    size_t counted = 0;

    {
        arch::IrqGuard irq_guard{};
        Sampler::set_enabled(true);
        Sampler::set_sample_rate(1);
        Sampler::clear();
        for (size_t call = 0; call < k_calls; ++call)
            Sampler::record_sample(0x4000 + call);
        counted = Sampler::get_sample_count();
    }

    restore_boot_state();

    JARVIS_ASSERT_EQ(k_calls, counted);
    JARVIS_ASSERT(counted > k_buffer_size);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: dump_to_serial() is the export side of the profiler — it must
//           render the collected samples WITHOUT consuming them, so a dump
//           can be repeated and does not perturb the run being profiled.
// Input: With the scheduler frozen: record 3 samples, snapshot the count,
//        call dump_to_serial(), snapshot again.
// Expect: Three samples are recorded; the count is identical before and
//         after the dump.
// Depends: profiling::Sampler, arch::IrqGuard
JARVIS_TEST(sampler_dump_is_non_destructive,
            "PRE: vfsd, iocd | POST: none") {
    size_t before_dump = 0;
    size_t after_dump = 0;

    {
        arch::IrqGuard irq_guard{};
        Sampler::set_enabled(true);
        Sampler::set_sample_rate(1);
        Sampler::clear();
        for (int call = 0; call < 3; ++call)
            Sampler::record_sample(0xAAAA0000 + static_cast<uint64_t>(call));
        before_dump = Sampler::get_sample_count();
        Sampler::dump_to_serial();
        after_dump = Sampler::get_sample_count();
    }

    restore_boot_state();

    JARVIS_ASSERT_EQ(static_cast<size_t>(3), before_dump);
    JARVIS_ASSERT_EQ(before_dump, after_dump);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: add_function()/find_function() implement half-open address
//           ranges [start, end): the start address and the last byte inside
//           resolve to the registered index, while the byte before start and
//           the end address itself are outside.  A range the profiler does
//           not know returns the not-found sentinel.
// Input: add_function(0x1000, 0x1100) — no other caller registers ranges, so
//        the symbol table starts empty; four find_function() probes.
// Expect: find_function(0x1000) == find_function(0x10FF) != sentinel;
//         find_function(0x0FFF) and find_function(0x1100) are the sentinel.
// Depends: profiling::Sampler
JARVIS_TEST(sampler_symbol_lookup_range_bounds,
            "PRE: vfsd, iocd | POST: none") {
    const uint32_t unknown_before = Sampler::find_function(0x1000);

    Sampler::add_function(0x1000, 0x1100);

    const uint32_t at_start = Sampler::find_function(0x1000);
    const uint32_t at_last = Sampler::find_function(0x10FF);
    const uint32_t below_start = Sampler::find_function(0x0FFF);
    const uint32_t at_end = Sampler::find_function(0x1100);

    restore_boot_state();

    JARVIS_ASSERT_EQ(k_no_function, unknown_before);
    JARVIS_ASSERT(at_start != k_no_function);
    JARVIS_ASSERT_EQ(at_start, at_last);
    JARVIS_ASSERT_EQ(k_no_function, below_start);
    JARVIS_ASSERT_EQ(k_no_function, at_end);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: register_from_symbol_table() parses the "start,end,name\0"
//           hex record stream produced from the ELF symbol table: well
//           formed records become resolvable ranges, addresses in the gap
//           between them stay unknown, and malformed records (start 0, or
//           end <= start) are rejected instead of creating a bogus range
//           that would mis-attribute every sample.
// Input: A synthetic table with 0x1000-0x1100 "alpha" and 0x2000-0x2100
//        "beta", then a second table with the malformed record
//        "0,0,bad\0" plus a truncated "3000" record.
// Expect: Both real ranges resolve; 0x1500 (gap) does not; the malformed
//        table adds nothing (0x3000 stays unknown).
// Depends: profiling::Sampler
JARVIS_TEST(sampler_register_from_symbol_table,
            "PRE: vfsd, iocd | POST: none") {
    static const char k_table[] = "1000,1100,alpha\0"
                                  "2000,2100,beta\0";
    Sampler::register_from_symbol_table(
        reinterpret_cast<const uint8_t *>(k_table), sizeof(k_table));

    const uint32_t alpha = Sampler::find_function(0x1000);
    const uint32_t beta = Sampler::find_function(0x2000);
    const uint32_t gap = Sampler::find_function(0x1500);

    static const char k_bad[] = "0,0,bad\0"
                                "3000";
    Sampler::register_from_symbol_table(
        reinterpret_cast<const uint8_t *>(k_bad), sizeof(k_bad));

    const uint32_t rejected = Sampler::find_function(0x3000);
    const uint32_t truncated = Sampler::find_function(0x1000);

    restore_boot_state();

    JARVIS_ASSERT(alpha != k_no_function);
    JARVIS_ASSERT(beta != k_no_function);
    JARVIS_ASSERT(alpha != beta);
    JARVIS_ASSERT_EQ(k_no_function, gap);
    JARVIS_ASSERT_EQ(k_no_function, rejected);
    JARVIS_ASSERT_EQ(alpha, truncated);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: init() is the boot-time reset: it must drop every sample,
//           restart the sequence counter and leave the sampler DISABLED so
//           that no sample is taken before the rate and enable flag are
//           configured.
// Input: With the scheduler frozen: record samples, then init(); read
//        get_sample_count() and is_enabled().
// Expect: Count is 0, is_enabled() is false, and a subsequent
//         record_sample() is dropped (count stays 0).
// Depends: profiling::Sampler, arch::IrqGuard
JARVIS_TEST(sampler_init_resets_and_disables,
            "PRE: vfsd, iocd | POST: none") {
    size_t after_init = 0;
    size_t after_record = 0;
    bool enabled_after_init = true;

    {
        arch::IrqGuard irq_guard{};
        Sampler::set_enabled(true);
        Sampler::set_sample_rate(1);
        Sampler::clear();
        for (int call = 0; call < 4; ++call)
            Sampler::record_sample(0x5000 + static_cast<uint64_t>(call));

        Sampler::init();
        after_init = Sampler::get_sample_count();
        enabled_after_init = Sampler::is_enabled();

        Sampler::record_sample(0x6000);
        after_record = Sampler::get_sample_count();
    }

    restore_boot_state();

    JARVIS_ASSERT_EQ(static_cast<size_t>(0), after_init);
    JARVIS_ASSERT(!enabled_after_init);
    JARVIS_ASSERT_EQ(static_cast<size_t>(0), after_record);
    JARVIS_TEST_PASS();
}

void register_profiler_sampler_tests() {
    Logger::info("Registering profiler sampler tests");
    JARVIS_REGISTER_TEST(sampler_rate_gate_counts_samples);
    JARVIS_REGISTER_TEST(sampler_ring_wraps_without_losing_count);
    JARVIS_REGISTER_TEST(sampler_dump_is_non_destructive);
    JARVIS_REGISTER_TEST(sampler_symbol_lookup_range_bounds);
    JARVIS_REGISTER_TEST(sampler_register_from_symbol_table);
    JARVIS_REGISTER_TEST(sampler_init_resets_and_disables);
}
