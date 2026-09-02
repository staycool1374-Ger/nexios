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

/// @file syscall_handlers_cap.cpp
/// @brief SYS_CAP_* syscall handlers: grant/copy/revoke/mint against the
///        current task's root CNode (CSpace).  No blocking; all dereferences
///        through capability slots validated by cap::lookup.

#include <kernel/syscall/syscall.hpp>
#include <kernel/task/scheduler.hpp>
#include <kernel/task/task.hpp>
#include <kernel/cap/cap.hpp>
#include <kernel/cap/cap_types.hpp>

namespace kernel {

/// @brief Resolves the current task's root CNode, creating it lazily on the
///        first capability syscall.  Returns nullptr if the CSpace cannot be
///        allocated (MemPool exhaustion).
cap::CNode *current_cspace() {
    auto *t = Scheduler::current_task();
    if (!t)
        return nullptr;
    t->ensure_cspace();
    return t->get_cspace();
}

uint64_t Syscall::sys_cap_grant(uint64_t src_handle, uint64_t dst_handle,
                                uint64_t, uint64_t, uint64_t *) {
    cap::CNode *src = current_cspace();
    if (!src)
        return static_cast<uint64_t>(-1);
    // Destination CNode is addressed by a CapCNode handle in the caller's
    // own CSpace.  The dest CNode must itself be a capability we hold.
    KernelObject *dst_obj =
        cap::lookup(src, dst_handle, cap::CapType::CNode, cap::CAP_RIGHT_GRANT);
    if (!dst_obj)
        return static_cast<uint64_t>(-1);
    auto *dst = static_cast<cap::CNode *>(dst_obj);
    int idx = cap::grant(src, src_handle, dst);
    dst_obj->release();
    if (idx < 0)
        return static_cast<uint64_t>(-1);
    return static_cast<uint64_t>(idx);
}

uint64_t Syscall::sys_cap_copy(uint64_t src_handle, uint64_t dst_handle,
                               uint64_t, uint64_t, uint64_t *) {
    cap::CNode *src = current_cspace();
    if (!src)
        return static_cast<uint64_t>(-1);
    KernelObject *dst_obj =
        cap::lookup(src, dst_handle, cap::CapType::CNode, cap::CAP_RIGHT_COPY);
    if (!dst_obj)
        return static_cast<uint64_t>(-1);
    auto *dst = static_cast<cap::CNode *>(dst_obj);
    int idx = cap::copy(src, src_handle, dst);
    dst_obj->release();
    if (idx < 0)
        return static_cast<uint64_t>(-1);
    return static_cast<uint64_t>(idx);
}

uint64_t Syscall::sys_cap_revoke(uint64_t src_handle, uint64_t, uint64_t,
                                 uint64_t, uint64_t *) {
    cap::CNode *src = current_cspace();
    if (!src)
        return static_cast<uint64_t>(-1);
    if (!cap::revoke(src, src_handle))
        return static_cast<uint64_t>(-1);
    return 0;
}

uint64_t Syscall::sys_cap_mint(uint64_t src_handle, uint64_t dst_handle,
                               uint64_t rights_mask, uint64_t badge,
                               uint64_t *) {
    cap::CNode *src = current_cspace();
    if (!src)
        return static_cast<uint64_t>(-1);
    KernelObject *dst_obj =
        cap::lookup(src, dst_handle, cap::CapType::CNode, cap::CAP_RIGHT_COPY);
    if (!dst_obj)
        return static_cast<uint64_t>(-1);
    auto *dst = static_cast<cap::CNode *>(dst_obj);
    int idx =
        cap::mint(src, src_handle, dst, static_cast<uint32_t>(rights_mask),
                  static_cast<uint32_t>(badge));
    dst_obj->release();
    if (idx < 0)
        return static_cast<uint64_t>(-1);
    return static_cast<uint64_t>(idx);
}

uint64_t Syscall::sys_cap_retype(uint64_t untyped_handle, uint64_t target_type,
                                 uint64_t size, uint64_t rights, uint64_t *) {
    cap::CNode *src = current_cspace();
    if (!src)
        return static_cast<uint64_t>(-1);
    // Installs the retyped target (and, on a sub-range carve, the child
    // Untyped) into the caller's own root CNode.  cap::retype validates the
    // type/size (garbage target_type or oversize/unaligned -> -1, parent kept).
    int idx = cap::retype(src, untyped_handle,
                          static_cast<cap::CapType>(target_type),
                          static_cast<size_t>(size),
                          static_cast<uint32_t>(rights));
    if (idx < 0)
        return static_cast<uint64_t>(-1);
    return static_cast<uint64_t>(idx);
}

} // namespace kernel
