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

/// @file wdog.hpp
/// @brief Capability-wrapped cross-task watchdog authority (issue #277).
/// Owning a WdogCap for task T is the authority to arm/kick T's watchdog
/// via WATCHDOG_CREATE/KICK with a pid argument (the self path needs no
/// cap). Single live cap per target pid — create() fails closed when
/// another live WdogCap already claims the same task.

#pragma once

#include <types.hpp>
#include <kernel/memory/kernel_object.hpp>

namespace kernel::cap {

/// @brief A capability-gated watchdog authority over one task. Does NOT own
/// the task — dispose/revoke only disarm the target (never kill it) and
/// free the MemPool block. Shared-heap class (IrqCap pattern).
class WdogCap : public KernelObject {
  public:
    /// @brief Supervised target task ID.
    uint64_t target_pid = 0;
    /// @brief Target's wdog_gen at create time (stale-slot guard).
    uint32_t target_gen = 0;

    /// @brief Allocates a WdogCap from the MemPool and pool-marks it.
    ///        Validates the target task exists and that no other live
    ///        WdogCap claims the same pid (single-owner). Returns nullptr
    ///        on failure or when CONFIG_CAP_MAX_WDOG live objects are
    ///        reached.
    static WdogCap *create(uint64_t target_pid);

    /// @brief Final teardown: disarms the target's watchdog (no kill) and
    ///        releases the MemPool block. Idempotent.
    void dispose() noexcept override;

    /// @brief Capability revocation: disarms the target and invalidates
    ///        the cap. A revoked cap refuses acquire() and subsequent
    ///        cross-task CREATE/KICK fail closed.
    void revoke() noexcept override;

    /// @brief Genuinely shared (referenced by capability slots).
    bool is_shared() const noexcept override {
        return true;
    }

    /// @brief Test-isolation rewind: clears the claim registry so a
    ///        recycled pid is claimable again (MsixCap precedent).
    static void snapshot_reset() noexcept;
};

} // namespace kernel::cap
