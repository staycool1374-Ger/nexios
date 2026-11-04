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

/// @file thread_cap.hpp
/// @brief Capability-wrapped thread-join authority (issue #53, option Y).
/// Owning a ThreadCap for thread T is the authority to join/detach T via
/// THREAD_JOIN (the handle is the authority; a raw tid never suffices —
/// audit S1(a)). Does NOT own the thread: dispose/revoke only drop the
/// wrapper (never terminate or reap the target). Unlike WdogCap there is
/// no single-owner registry: several wrappers may name one thread (double
/// join still fails closed on the target's join_waiter_ slot), and no
/// snapshot_reset is needed (no global claim state).

#pragma once

#include <types.hpp>
#include <kernel/memory/kernel_object.hpp>

namespace kernel::cap {

/// @brief Join authority over one thread. Does NOT own the thread.
/// Shared-heap class (WdogCap pattern): installed into a CSpace with
/// CapType::Task, resolved at use time via find_task + generation match.
class ThreadCap : public KernelObject {
  public:
    /// @brief Wrapping discriminator: stale-slot guard so a future
    /// Task-typed object can never be mistaken for a ThreadCap after a
    /// static_cast from a lookup(Task) result (today ThreadCap is the
    /// only Task-typed object; the tag keeps that invariant checked).
    static constexpr uint32_t kThreadCapTag = 0x54485244U;  // 'THRD'

    /// @brief Tag word, always kThreadCapTag for a live ThreadCap.
    uint32_t tag = kThreadCapTag;
    /// @brief Joined target thread ID.
    uint64_t target_id = 0;
    /// @brief Target's TCB generation at wrap time (stale-slot guard).
    uint32_t target_gen = 0;

    /// @brief Allocates a ThreadCap from the MemPool and pool-marks it.
    ///        Returns nullptr when the target is not a live thread.
    ///        Folds into the existing cap_objects ResourceTracker counter.
    static ThreadCap *create(uint64_t target_id, uint32_t target_gen);

    /// @brief Final teardown: releases the MemPool block. Idempotent.
    ///        Never touches the target thread.
    void dispose() noexcept override;

    /// @brief Capability revocation: invalidates the cap. A revoked
    ///        cap refuses acquire() and subsequent THREAD_JOIN lookups
    ///        fail closed.
    void revoke() noexcept override;

    /// @brief Genuinely shared (referenced by capability slots).
    bool is_shared() const noexcept override {
        return true;
    }
};

}  // namespace kernel::cap
