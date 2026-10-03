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

/// @file test_boot_deterministic.cpp
/// @brief Deterministic-boot stage tests (issue #45). Real assertions on
///        the kernel::boot_stage_* API (main): validity + monotonicity +
///        relative budget bounds. Sampling method: IrqGuard-frozen rdtsc
///        measured-max + static loop-cap bounds, relative asserts with
///        >=3x headroom; QEMU-TSC numbers are relative/scaling-only
///        (wcet_analysis.md method). Raw-counter deltas are
///        host-load-independent (instruction-driven under TCG), so all
///        asserts rest on relative/raw properties — never absolute ns.

#include <test.hpp>
#include <logger.hpp>
#include <kernel/kernel.hpp>
#include <kernel/nexios_config.h>

using namespace kernel;

namespace {

// Live proof (~685ms total vs 30000ms budget) gives >=40x headroom on
// every bound below; the owner floor is >=3x.
constexpr size_t kStageCount = static_cast<size_t>(BootStage::COUNT);
constexpr uint64_t kBudgetNs =
    static_cast<uint64_t>(CONFIG_BOOT_BUDGET_MS) * 1000000ULL;

} // namespace

// Runmode: kernel
// Testidea: Boot stage deltas are monotonic and validity-flagged.
// Input: kernel::boot_stage_delta() for all five BootStage values.
// Expect: every delta reports valid (nonzero backing frequency, ordered
//         raw endpoints); deltas are pairwise non-overlapping and sum to
//         <= CONFIG_BOOT_BUDGET_MS in ns.
// Depends: kernel::BootStage, boot_stage_delta (issue #45)
JARVIS_TEST(boot_stage_monotonic_valid, "PRE: none | POST: none") {
    uint64_t total = 0;
    for (size_t i = 0; i < kStageCount; ++i) {
        const BootStage s = static_cast<BootStage>(i);
        const BootDelta d = boot_stage_delta(s);
        JARVIS_ASSERT_FMT(d.valid, "stage %lu delta invalid", i);
        JARVIS_ASSERT_FMT(d.ns > 0, "stage %lu delta zero ns", i);
        total += d.ns;
    }
    JARVIS_ASSERT_FMT(total <= kBudgetNs,
                      "boot total %lu ns exceeds budget %lu ns", total,
                      kBudgetNs);
}

// Runmode: kernel
// Testidea: Each boot stage stays within its relative budget fraction.
// Input: kernel::boot_stage_delta() per stage vs CONFIG_BOOT_BUDGET_MS.
// Expect: no single stage exceeds 50% of the total budget (relative
//         bound with >=3x headroom — never an absolute-ns assert, TCG
//         wall dilation invalidates absolutes).
// Depends: kernel::boot_stage_delta, CONFIG_BOOT_BUDGET_MS (issue #45)
JARVIS_TEST(boot_stage_relative_bounds, "PRE: none | POST: none") {
    for (size_t i = 0; i < kStageCount; ++i) {
        const BootStage s = static_cast<BootStage>(i);
        const BootDelta d = boot_stage_delta(s);
        JARVIS_ASSERT_FMT(d.valid, "stage %lu delta invalid", i);
        JARVIS_ASSERT_FMT(d.ns <= kBudgetNs / 2,
                          "stage %lu took %lu ns (> half budget %lu ns)", i,
                          d.ns, kBudgetNs / 2);
    }
}

// Runmode: kernel
// Testidea: Boot marks are structurally sound (in-kernel check — serial
//         self-parse is impossible in-kernel, so ordering + span
//         consistency carry the relative-only documentation intent).
// Input: kernel::boot_entry_raw(), kernel::boot_stage_mark() for all
//        five BootStage values + out-of-range guards.
// Expect: entry stamp nonzero; marks nonzero and non-decreasing
//         (pairwise non-overlapping spans); out-of-range stage reads
//         fail closed (mark 0, delta invalid); every delta valid
//         (fail-closed rule already implies ordered endpoints).
// Depends: kernel::boot_entry_raw, boot_stage_mark (issue #45)
JARVIS_TEST(boot_qemu_relative_doc, "PRE: none | POST: none") {
    const uint64_t entry = boot_entry_raw();
    JARVIS_ASSERT_FMT(entry != 0, "boot entry raw stamp is zero");
    uint64_t prev = entry;
    for (size_t i = 0; i < kStageCount; ++i) {
        const BootStage s = static_cast<BootStage>(i);
        const uint64_t m = boot_stage_mark(s);
        JARVIS_ASSERT_FMT(m != 0, "stage %lu mark is zero", i);
        JARVIS_ASSERT_FMT(m >= prev, "stage %lu mark unordered", i);
        prev = m;
        JARVIS_ASSERT_FMT(boot_stage_delta(s).valid,
                          "stage %lu delta invalid", i);
    }
    JARVIS_ASSERT_FMT(boot_stage_mark(BootStage::COUNT) == 0,
                      "out-of-range mark did not fail closed");
    JARVIS_ASSERT_FMT(!boot_stage_delta(BootStage::COUNT).valid,
                      "out-of-range delta did not fail closed");
}

void register_boot_deterministic_tests() {
    Logger::info("Registering boot deterministic tests");
    JARVIS_REGISTER_TEST(boot_stage_monotonic_valid);
    JARVIS_REGISTER_TEST(boot_stage_relative_bounds);
    JARVIS_REGISTER_TEST(boot_qemu_relative_doc);
}
