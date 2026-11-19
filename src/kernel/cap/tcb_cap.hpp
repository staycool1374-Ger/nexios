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

/// @file tcb_cap.hpp
/// @brief Capability-wrapped TCB control authority (F4, issue #557).
/// Owning a TcbCap for task T is the authority to configure/suspend/
/// resume T via TCB_CONFIGURE/SUSPEND/RESUME (the handle is the authority;
/// a raw tid never suffices — audit S1(a) pattern). Does NOT own the
/// task: dispose/revoke only drop the wrapper (never terminate or park
/// the target). ThreadCap shape (thread_cap.hpp), so F2
/// derivation/revoke cascade applies with no new machinery.
/// Scope binding (INV-ADM-12 branch 6): CREATOR_ONLY caps are the
/// F4-minted creator-facing form; SUPERVISOR caps are minted only by
/// kernel compose code (TcbCap::create_supervised — no syscall path)
/// and bind (grantee_id, grantee_gen) so a copied-onward cap cannot be
/// exercised by the copy's new holder.

#pragma once

#include <types.hpp>
#include <kernel/memory/kernel_object.hpp>

namespace kernel::cap {

/// @brief Control authority over one task. Does NOT own the task.
/// Shared-heap class (ThreadCap pattern): installed into a CSpace with
/// CapType::TCB, resolved at use time via find_task + generation match.
class TcbCap : public KernelObject {
  public:
    /// @brief Wrapping discriminator: stale-slot guard so a future
    /// TCB-typed object (e.g. a carved TCB, M12/M14) can never be
    /// mistaken for a TcbCap after a static_cast from a lookup(TCB)
    /// result (today TcbCap is the only TCB-typed object; the tag keeps
    /// that invariant checked).
    static constexpr uint32_t kTcbCapTag = 0x54434243U; // 'TCBC'

    /// @brief Authority scope carried by this cap (INV-ADM-12).
    enum class Authority : uint8_t {
        CREATOR_ONLY = 0, ///< creator-facing form (F4 mint)
        SUPERVISOR = 1,   ///< kernel-compose mint only (M5/M12)
    };

    /// @brief Tag word, always kTcbCapTag for a live TcbCap.
    uint32_t tag = kTcbCapTag;
    /// @brief Controlled target task ID.
    uint64_t target_id = 0;
    /// @brief Target's TCB generation at wrap time (stale-slot guard).
    uint32_t target_gen = 0;
    /// @brief Authority scope (see Authority).
    uint8_t scope = static_cast<uint8_t>(Authority::CREATOR_ONLY);
    /// @brief Supervisor grantee task ID (SUPERVISOR scope only).
    uint64_t grantee_id = 0;
    /// @brief Supervisor grantee generation (SUPERVISOR scope only).
    uint32_t grantee_gen = 0;

    /// @brief Allocates a CREATOR_ONLY TcbCap from the MemPool and
    ///        pool-marks it.  Returns nullptr when the target is not a
    ///        live task.  Folds into the existing cap_objects
    ///        ResourceTracker counter.
    static TcbCap *create(uint64_t target_id, uint32_t target_gen);

    /// @brief Allocates a SUPERVISOR-scope TcbCap bound to
    ///        (grantee_id, grantee_gen).  Kernel compose code only —
    ///        no syscall producer (INV-ADM-12 branch 6 is deny-all in
    ///        production until M5/M12, fail closed).  Returns nullptr
    ///        when the target is not a live task.
    // NOLINTBEGIN(bugprone-easily-swappable-parameters)
    static TcbCap *create_supervised(uint64_t target_id,
                                     uint32_t target_gen,
                                     uint64_t grantee_id,
                                     uint32_t grantee_gen);
    // NOLINTEND(bugprone-easily-swappable-parameters)

    /// @brief Final teardown: releases the MemPool block. Idempotent.
    ///        Never touches the target task.
    void dispose() noexcept override;

    /// @brief Capability revocation: invalidates the cap. A revoked
    ///        cap refuses acquire() and subsequent TCB_* lookups fail
    ///        closed.
    void revoke() noexcept override;

    /// @brief Genuinely shared (referenced by capability slots).
    bool is_shared() const noexcept override {
        return true;
    }
};

} // namespace kernel::cap
