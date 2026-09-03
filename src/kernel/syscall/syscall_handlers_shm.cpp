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

/// @file syscall_handlers_shm.cpp
/// @brief Capability-gated shared-memory syscalls (issue #106 Part B):
///   SYS_FRAME_CREATE  — allocate contiguous user frames into a new FrameCap
///                       installed in the caller's CSpace (returns the slot).
///   SYS_FRAME_MAP      — map a FrameCap's frames into the caller's user VA
///                       window (FrameUserMap::map).
///   SYS_FRAME_UNMAP    — unmap a user frame mapping by VA.
/// These syscalls touch page tables and MUST stay out of k_syscall_fast[]
/// (issue #92 discipline).

#include <kernel/syscall/syscall.hpp>
#include <kernel/cap/cap.hpp>
#include <kernel/cap/frame.hpp>
#include <kernel/cap/frame_map.hpp>
#include <kernel/memory/pmm.hpp>
#include <kernel/task/scheduler.hpp>
#include <kernel/task/task.hpp>
#include <kernel/sync/spinlock_guard.hpp>

using namespace kernel;

/// @brief Resolves the current task's root CNode (mirrors
///        syscall_handlers_cap.cpp::current_cspace).
static cap::CNode *shm_current_cspace() {
    auto *t = Scheduler::current_task();
    if (!t)
        return nullptr;
    t->ensure_cspace();
    return t->get_cspace();
}

uint64_t Syscall::sys_frame_create(uint64_t count, uint64_t, uint64_t,
                                   uint64_t, uint64_t *) {
    cap::CNode *cs = shm_current_cspace();
    if (!cs)
        return static_cast<uint64_t>(-1);
    if (count == 0 || count > CONFIG_CAP_MAX_FRAME_MAPS)
        return static_cast<uint64_t>(-1);

    // Allocate contiguous USER-owned frames (fail closed on exhaustion).
    uint64_t phys = PMM::alloc_user_contiguous(static_cast<size_t>(count));
    if (phys == 0)
        return static_cast<uint64_t>(-1);

    auto *fc = cap::FrameCap::create(phys, static_cast<size_t>(count), true);
    if (!fc) {
        // Roll back the allocation on cap-create failure.
        for (size_t i = 0; i < count; ++i)
            PMM::free_page(phys + i * arch::PAGE_SIZE);
        return static_cast<uint64_t>(-1);
    }

    int idx = cs->install(fc, cap::CapType::Frame,
                          cap::CAP_RIGHT_READ | cap::CAP_RIGHT_WRITE);
    if (idx < 0) {
        fc->release(); // never installed — drop the creator reference
        return static_cast<uint64_t>(-1);
    }
    fc->release(); // slot holds a reference; drop the creator's
    return static_cast<uint64_t>(idx);
}

uint64_t Syscall::sys_frame_map(uint64_t cap_handle, uint64_t, uint64_t,
                                uint64_t, uint64_t *) {
    cap::CNode *src = shm_current_cspace();
    if (!src)
        return static_cast<uint64_t>(-1);

    // Capability-gated: the caller must hold a Frame capability (WRITE = the
    // right to install the mapping).  Mirrors sys_mmio_map.
    KernelObject *obj =
        cap::lookup(src, cap_handle, cap::CapType::Frame, cap::CAP_RIGHT_WRITE);
    if (!obj)
        return static_cast<uint64_t>(-1);
    auto *fc = static_cast<cap::FrameCap *>(obj);

    auto *t = Scheduler::current_task();
    if (!t) {
        obj->release();
        return static_cast<uint64_t>(-1);
    }
    uint64_t va = cap::FrameUserMap::map(*t, *fc);
    obj->release();
    return va;
}

uint64_t Syscall::sys_frame_unmap(uint64_t va, uint64_t, uint64_t, uint64_t,
                                  uint64_t *) {
    auto *t = Scheduler::current_task();
    if (!t)
        return static_cast<uint64_t>(-1);
    return cap::FrameUserMap::unmap(*t, va) ? 0 : static_cast<uint64_t>(-1);
}