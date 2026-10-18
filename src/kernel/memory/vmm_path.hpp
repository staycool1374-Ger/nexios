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

/// @file vmm_path.hpp
/// @brief VMM map/rollback step registry (issue #347 Slice D).
///
/// Traces the transactional multi-page map primitive
/// VMM::map_phys_range_try() (issue #325-B): a bounded per-call chain, not
/// per-iteration — the per-page commit is folded into one Commit step so
/// the chain cannot grow with page_count. Rollback emits the compensating
/// Rollback step after the prefix unmap loop, so a failed transaction
/// validates against the rollback value and never against the success one.
///
/// Terminal paths: validation failures record Reject + Done only (no
/// Validate). Slot is shared with the dispatch/task registries — tests own
/// reset and must hold IF=0 across reset → drive → capture.
///
/// Table-alloc fault injector contract (issue #361, test-only, absent in
/// release): VMM::test_arm_table_alloc_fail_at(n) makes the n-th table
/// allocation attempt of the next map_phys_range_try() call fail instead
/// of allocating (1-based; 0 disarms; firing is single-shot and disarms).
/// Countdown covers attempts only — the 3 gates sit after presence/
/// split-need checks, immediately before the PMM::alloc_* calls
/// (get_table split + fresh sites, map_page_in_pml4_try split site), so
/// present-table hits never consume it. map_phys_range_try() resets the
/// countdown per call. Arm/disarm/snapshot-reset backstop the armed
/// window; the whole arm → drive → capture → teardown sequence must run
/// under one IrqGuard on the BSP test thread.

#pragma once

#include <types.hpp>
#include <kernel/core/path_tracer.hpp>

namespace kernel {

/// @brief map_phys_range_try() steps (issue #347 Slice D).
enum class VmmMapSteps : uint64_t {
    Validate = 1ULL << 0, ///< align/overflow checks passed
    Commit   = 1ULL << 1, ///< every page in the range mapped
    Rollback = 1ULL << 2, ///< a page failed; mapped prefix unmapped
    Reject   = 1ULL << 3, ///< entry validation refused the call
    Done     = 1ULL << 4, ///< returning to the caller
};

/// @brief Successful range map: Validate → Commit → Done.
inline constexpr uint64_t k_vmm_map_success_expected =
    path_expected(VmmMapSteps::Validate, VmmMapSteps::Commit,
                  VmmMapSteps::Done);

/// @brief Failed range map with compensating unmap: Validate → Rollback →
///        Done.
inline constexpr uint64_t k_vmm_map_rollback_expected =
    path_expected(VmmMapSteps::Validate, VmmMapSteps::Rollback,
                  VmmMapSteps::Done);

/// @brief Refused range map: Reject → Done (no Begin).
inline constexpr uint64_t k_vmm_map_reject_expected =
    path_expected(VmmMapSteps::Reject, VmmMapSteps::Done);

// Non-zero and pairwise-distinct (a fold typo would collapse these).
static_assert(k_vmm_map_success_expected != 0);
static_assert(k_vmm_map_success_expected != k_vmm_map_rollback_expected);
static_assert(k_vmm_map_success_expected != k_vmm_map_reject_expected);
static_assert(k_vmm_map_rollback_expected != k_vmm_map_reject_expected);

// Bit-distinctness: bits 0..4 — a duplicated bit would hide a skipped step.
static_assert((static_cast<uint64_t>(to_underlying(VmmMapSteps::Validate)) |
               static_cast<uint64_t>(to_underlying(VmmMapSteps::Commit)) |
               static_cast<uint64_t>(to_underlying(VmmMapSteps::Rollback)) |
               static_cast<uint64_t>(to_underlying(VmmMapSteps::Reject)) |
               static_cast<uint64_t>(to_underlying(VmmMapSteps::Done))) ==
              0x1FULL);

#if defined(CONFIG_DEBUG)
/// @brief Fault-injection mask for vmm_step() (test-only, Slice D).
///        Compiled out in release — production can never observe it.
extern uint64_t g_test_skip_vmm_step;
#endif

/// @brief Record one VMM-path step (honors the test skip mask in debug).
inline void vmm_step(uint64_t step_bit) noexcept {
#if defined(CONFIG_DEBUG)
    if ((g_test_skip_vmm_step & step_bit) != 0) {
        return;
    }
#endif
    path_step(step_bit);
}

/// @brief Record one VMM-path step from the registry.
inline void vmm_step(VmmMapSteps step_value) noexcept {
    vmm_step(static_cast<uint64_t>(to_underlying(step_value)));
}

} // namespace kernel
