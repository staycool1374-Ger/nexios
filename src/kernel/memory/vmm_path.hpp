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
    (1ULL << 0) ^ (1ULL << 1) ^ (1ULL << 4);

/// @brief Failed range map with compensating unmap: Validate → Rollback →
///        Done.
inline constexpr uint64_t k_vmm_map_rollback_expected =
    (1ULL << 0) ^ (1ULL << 2) ^ (1ULL << 4);

/// @brief Refused range map: Reject → Done (no Begin).
inline constexpr uint64_t k_vmm_map_reject_expected =
    (1ULL << 3) ^ (1ULL << 4);

static_assert(k_vmm_map_success_expected == 0x13ULL);
static_assert(k_vmm_map_rollback_expected == 0x15ULL);
static_assert(k_vmm_map_reject_expected == 0x18ULL);

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
