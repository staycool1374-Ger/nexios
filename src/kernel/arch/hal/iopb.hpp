#pragma once

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

/// @file iopb.hpp
/// @brief Arch-neutral per-task I/O-permission-bitmap (IOPB) interface
///        (v0.4.2, issues #3/#8).  x86_64: per-task TSS I/O bitmap delegation;
///        other arches: no-op stubs.  Issue #8 adds the grant ledger so
///        revoking/disposing an IO MmioCap retroactively clears the granted
///        port bits in live task bitmaps (closing the #3 revocation gap).

#include <types.hpp>

namespace kernel {
struct TaskControlBlock;
}

namespace arch {

#if defined(CONFIG_ARCH_X86_64)

/// @brief Claim an I/O-port bitmap pool slot for @p t (default-deny all-1s).
/// @return true on success (or if the task already holds a slot); false when
/// the pool is exhausted.
bool iopb_claim(kernel::TaskControlBlock &t);

/// @brief Release @p t's IOPB slot.  If @p t is the currently loaded TSS
/// owner, the TSS bitmap is re-masked to all-1s first (no dangling owner).
void iopb_release(kernel::TaskControlBlock &t);

/// @brief Clear the bitmap bits for ports [start, start+count) in @p t's
/// slot, granting access.  If @p t is the loaded TSS owner the change is
/// applied immediately.
/// @return true on success; false if @p t holds no slot.
bool iopb_grant_range(kernel::TaskControlBlock &t, uint16_t start,
                      uint32_t count);

/// @brief Apply the IOPB state for @p next on context switch.  No-op for
/// kernel tasks (CPL0 ignores the bitmap) and for the already-loaded owner.
void iopb_switch_to(const kernel::TaskControlBlock &next);

/// @brief Reset all IOPB pool/owner state and re-mask the TSS to default
/// deny.  Called by test isolation snapshot restore.
void iopb_snapshot_reset();

/// @brief Task currently loaded in the TSS I/O bitmap (test accessor).
/// @return Pointer to the owning TCB, or nullptr when default-deny.
const void *iopb_loaded_owner();

/// @brief Query whether @p port is allowed for @p t (test accessor).
/// @return true if @p t holds a slot with the port bit cleared.
bool iopb_port_allowed(const kernel::TaskControlBlock &t, uint16_t port);

// --- issue #8: capability-gated grant ledger (revocation closure) ---

/// @brief Records a granted port range [start, start+count) in @p t's bitmap
///        against the capability @p cap_ptr (non-owning, equality-match only —
///        never dereferenced).  Called by sys_ioport_grant AFTER a successful
///        iopb_grant_range, so a later iopb_ledger_clear_cap(cap) can restore
///        the exact denied state.
/// @return true when the entry was recorded; false when the ledger is full
///         (fail closed — the grant still stands but its revoke rollback is
///         best-effort; callers reserve BEFORE granting to avoid this).
bool iopb_ledger_add(kernel::TaskControlBlock &t, const void *cap_ptr,
                     uint16_t start, uint32_t count);

/// @brief Retroactively revokes every granted range recorded against @p cap_ptr:
///        re-masks those port bits to 1 (deny) in the owning task's bitmap and
///        re-loads the TSS if the task is the loaded owner.  Called from
///        MmioCap::dispose/revoke (issue #8 revocation closure).
void iopb_ledger_clear_cap(const void *cap_ptr);

/// @brief Drops every ledger entry owned by @p t.  Called from
///        TaskControlBlock::cleanup() (task teardown).
void iopb_ledger_drop_task(kernel::TaskControlBlock &t);

/// @brief Number of live ledger entries for @p t (test accessor).
size_t iopb_grant_count(const kernel::TaskControlBlock &t);

#else // CONFIG_ARCH_X86_64

inline bool iopb_claim(kernel::TaskControlBlock &) {
    return false;
}
inline void iopb_release(kernel::TaskControlBlock &) {
}
inline bool iopb_grant_range(kernel::TaskControlBlock &, uint16_t, uint32_t) {
    return false;
}
inline void iopb_switch_to(const kernel::TaskControlBlock &) {
}
inline void iopb_snapshot_reset() {
}
inline const void *iopb_loaded_owner() {
    return nullptr;
}
inline bool iopb_port_allowed(const kernel::TaskControlBlock &, uint16_t) {
    return false;
}
inline bool iopb_ledger_add(kernel::TaskControlBlock &, const void *,
                            uint16_t, uint32_t) {
    return false;
}
inline void iopb_ledger_clear_cap(const void *) {
}
inline void iopb_ledger_drop_task(kernel::TaskControlBlock &) {
}
inline size_t iopb_grant_count(const kernel::TaskControlBlock &) {
    return 0;
}

#endif // CONFIG_ARCH_X86_64

} // namespace arch