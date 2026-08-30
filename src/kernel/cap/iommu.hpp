/*
 * NexIOS RTOS — Capability-Based Access Control (CSpace)
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

/// @file iommu.hpp
/// @brief Capability-wrapped IOMMU DMA protection domain (v0.4.2, issue #4).
/// Owning the cap is the authority to program DMA mappings for ONE private
/// IOMMU domain.  The domain is bound to the creating task (single owner):
/// only that task's sys_iommu_map/unmap calls are accepted, and every mapped
/// frame must come from the caller's own CSpace (FrameCap).  dispose/revoke
/// destroy the domain — a revoked driver retains ZERO DMA access
/// (fail-closed).  Shared-heap class (IrqCap pattern).

#pragma once

#include <types.hpp>
#include <kernel/memory/kernel_object.hpp>

namespace kernel::cap {

class IoMmuDmaCap : public KernelObject {
  public:
    /// @brief Index of the wrapped IOMMU domain (-1 = none).
    int16_t domain_idx_ = -1;
    /// @brief Task id allowed to program the domain (the creator).
    uint32_t owner_task_id_ = 0;

    /// @brief Allocates an IoMmuDmaCap from the MemPool and creates its
    ///        private IOMMU domain (bound to the calling task).  Returns
    ///        nullptr when no IOMMU is present (graceful degradation), when
    ///        CONFIG_CAP_MAX_IOMMU / CONFIG_IOMMU_MAX_DOMAINS is exhausted
    ///        or on allocation failure (any partial state is rolled back).
    static IoMmuDmaCap *create();

    /// @brief Final teardown: destroys the IOMMU domain (all mappings and
    ///        table pages freed) and releases the MemPool block.
    ///        Idempotent on an already-destroyed domain.
    void dispose() noexcept override;

    /// @brief Capability revocation: destroys the domain BEFORE marking the
    ///        cap revoked (authority loss must remove DMA access first).
    void revoke() noexcept override;

    /// @brief Genuinely shared (referenced by capability slots).
    bool is_shared() const noexcept override {
        return true;
    }
};

} // namespace kernel::cap
