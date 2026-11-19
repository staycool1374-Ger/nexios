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

/// @file syscall_handlers_vspace.cpp
/// @brief F5 VSPACE_MAP syscall (issue #558): link a carved PageTable
///        object at a 9-bit-component path under a carved VSpace root.
///        Four-arg form only.  Every path is resolve -> validate ->
///        authorize -> mechanism, every return is SYS_ERR_OK or a typed
///        VSPACE_* code via the 1100-block; no raw -1.  Allocates
///        nothing (INV-VS-2); bounded (arch depth, 32-entry registry,
///        one 8-byte store); never records a derivation edge for a table
///        link (uses-not-caps).

#include <kernel/syscall/syscall.hpp>
#include <kernel/syscall/syscall_errors.hpp>
#include <kernel/cap/cap.hpp>
#include <kernel/cap/carved_object.hpp>
#include <kernel/cap/vspace_errors.hpp>
#include <kernel/cap/vspace_map.hpp>
#include <kernel/task/scheduler.hpp>
#include <kernel/task/task.hpp>
#include <lib/scope_guard.hpp>

#if defined(CONFIG_DEBUG)
extern "C" void debug_write(const char *s);
extern "C" void debug_write_hex(uint64_t value);
#endif

namespace kernel {

namespace {

/// @brief Resolves the current task's root CNode (mirrors
///        syscall_handlers_shm.cpp::shm_current_cspace).
cap::CNode *vspace_current_cspace() {
    auto *task = Scheduler::current_task();
    if (!task)
        return nullptr;
    task->ensure_cspace();
    return task->get_cspace();
}

/// @brief Map a VspaceError to the syscall return encoding: 0 on OK,
///        else -(1100 + code) via from_vspace_error.  Zero side effects.
uint64_t vspace_return(errors::VspaceError err) noexcept {
    if (err == errors::VSPACE_ERR_OK)
        return static_cast<uint64_t>(errors::SYS_ERR_OK);
    return static_cast<uint64_t>(
        -errors::from_vspace_error(static_cast<uint64_t>(err)));
}

} // namespace

uint64_t Syscall::sys_vspace_map(uint64_t arg0, uint64_t arg1, uint64_t arg2,
                                 uint64_t arg3, uint64_t *) {
    // Truncation-trap guard (LEARNINGS #337): arg3 narrows to uint8_t
    // for the mechanism.  Reject > 255 BEFORE the narrowing — after it,
    // 256 would masquerade as a valid length 0 re-check downstream.
    if (arg3 > 0xFFu)
        return vspace_return(errors::VSPACE_ERR_INVALID_ARGS);
    cap::CNode *cs = vspace_current_cspace();
    if (!cs)
        return static_cast<uint64_t>(errors::SYS_ERR_GENERIC_FAIL);
    // Capability-gated: WRITE on the VSpace is the authority to reshape
    // the tree; READ on the PageTable is the authority to install it.
    // Q9 is N/A to N4 (subtree rule governs task control, not address
    // spaces) but cap-gating is still required — no ambient authority.
    KernelObject *vs_obj = cap::lookup(cs, arg0, cap::CapType::VSpace,
                                       cap::CapRights::WRITE);
    if (!vs_obj)
        return vspace_return(errors::VSPACE_ERR_BAD_HANDLE);
    auto vs_release = ScopeGuard([&]() { vs_obj->release(); });
    KernelObject *pt_obj = cap::lookup(cs, arg1, cap::CapType::PageTable,
                                       cap::CapRights::READ);
    if (!pt_obj)
        return vspace_return(errors::VSPACE_ERR_BAD_HANDLE);
    auto pt_release = ScopeGuard([&]() { pt_obj->release(); });
    // Stale-slot guard (F4 S1(a) pattern): a static_cast from a lookup
    // result must re-verify the wrapping discriminator and the kind.
    auto *vs = static_cast<cap::CarvedObjectCap *>(vs_obj);
    auto *pt = static_cast<cap::CarvedObjectCap *>(pt_obj);
    if (vs->tag != cap::CarvedObjectCap::kCarvedObjectTag ||
        vs->kind != cap::CapType::VSpace ||
        pt->tag != cap::CarvedObjectCap::kCarvedObjectTag ||
        pt->kind != cap::CapType::PageTable)
        return vspace_return(errors::VSPACE_ERR_BAD_OBJECT);
    // A kernel-owned page must never become user-visible hierarchy
    // (the FrameUserMap !is_user refusal is the precedent).
    if (vs->is_user != pt->is_user)
        return vspace_return(errors::VSPACE_ERR_OWNERSHIP_MISMATCH);
    const errors::VspaceError link_err = cap::VSpaceMap::link(
        vs->phys, pt->phys, arg2, static_cast<uint8_t>(arg3));
#if defined(CONFIG_DEBUG)
    // Diagnostic only (no control-flow divergence): name the refused
    // link on the serial console so a failed test names its reason.
    if (link_err != errors::VSPACE_ERR_OK) {
        debug_write("[vspace_map] link refused err=0x");
        debug_write_hex(static_cast<uint64_t>(link_err));
        debug_write("\n");
    }
#endif
    return vspace_return(link_err);
}

} // namespace kernel
