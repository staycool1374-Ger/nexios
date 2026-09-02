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

/// @file syscall_handlers_iommu.cpp
/// @brief SYS_IOMMU_MAP / SYS_IOMMU_UNMAP handlers (issue #4): capability-
/// gated programming of the caller's private IOMMU DMA domain with frames
/// the caller owns (a FrameCap from its own CSpace).  Strict single-owner
/// validation (cap rights + owner task + domain liveness); every failure
/// returns -1 with NO table mutation.  Non-x86_64 builds return -1 (no
/// IOMMU translation support).  No blocking; caps pinned via cap::lookup.

#include <kernel/syscall/syscall.hpp>
#include <kernel/task/scheduler.hpp>
#include <kernel/task/task.hpp>
#include <kernel/cap/cap.hpp>
#include <kernel/cap/cap_types.hpp>
#include <kernel/cap/frame.hpp>
#include <kernel/cap/iommu.hpp>
#include <kernel/iommu/iommu.hpp>
#include <kernel/iommu/vtd.hpp>
#include <kernel/nexios_config.h>

namespace kernel {

// Shared with syscall_handlers_cap.cpp (same TU-visible helper).
cap::CNode *current_cspace();

namespace {

/// @brief Validates the pinned IoMmuDmaCap + FrameCap pair for the calling
///        task and derives the SL entry flags from the FRAME SLOT's granted
///        rights (READ -> R, WRITE -> W).  The slot is re-read (type, object
///        identity, generation) so the flags come from the exact slot the
///        handle addressed.  Single-core/task-context: cap mutation is task-
///        context only (SMP derivation tracking is a documented follow-up).
/// @return SL flags, or 0 on any validation failure (caller releases pins).
#if defined(CONFIG_ARCH_X86_64)
uint32_t validate_iommu_pair(cap::CNode *cs, uint64_t frame_handle,
                             const KernelObject *fobj,
                             const cap::IoMmuDmaCap *dma) {
    auto *t = Scheduler::current_task();
    if (!t)
        return 0;
    // Strict single-owner: only the creating task may program the domain.
    if (dma->owner_task_id_ != static_cast<uint32_t>(t->id))
        return 0;
    if (dma->domain_idx_ < 0)
        return 0;
    if (!iommu::IoMmuManager::domain_valid(dma->domain_idx_))
        return 0;
    uint32_t slot = cap::handle_slot(frame_handle);
    if (slot >= static_cast<uint32_t>(CONFIG_CSLOT_COUNT))
        return 0;
    const cap::CSlot &s = cs->slots[slot];
    if (!s.occupied || s.obj != fobj || s.type != cap::CapType::Frame)
        return 0;
    if (cap::handle_gen(frame_handle) != s.gen)
        return 0;
    uint32_t flags = 0;
    if ((s.rights & cap::CAP_RIGHT_READ) != 0)
        flags |= iommu::vtd::kSlEntryRead;
    if ((s.rights & cap::CAP_RIGHT_WRITE) != 0)
        flags |= iommu::vtd::kSlEntryWrite;
    return flags;
}
#endif // CONFIG_ARCH_X86_64

} // namespace

uint64_t Syscall::sys_iommu_map(uint64_t dma_handle, uint64_t frame_handle,
                                uint64_t, uint64_t, uint64_t *) {
#if defined(CONFIG_ARCH_X86_64)
    cap::CNode *cs = current_cspace();
    if (!cs)
        return static_cast<uint64_t>(-1);
    // Presence gate at entry: absent IOMMU fails gracefully (no table
    // exists to program; kernel DMA is untouched).
    if (!iommu::IoMmuManager::probe())
        return static_cast<uint64_t>(-1);

    // WRITE on the DMA cap = the arming authority (issue #2/#3 pattern).
    KernelObject *dma_obj = cap::lookup(
        cs, dma_handle, cap::CapType::IoMmuDma, cap::CAP_RIGHT_WRITE);
    if (!dma_obj)
        return static_cast<uint64_t>(-1);
    auto *dma = static_cast<cap::IoMmuDmaCap *>(dma_obj);

    KernelObject *fobj = cap::lookup(cs, frame_handle, cap::CapType::Frame, 0);
    if (!fobj) {
        dma_obj->release();
        return static_cast<uint64_t>(-1);
    }

    uint32_t flags = validate_iommu_pair(cs, frame_handle, fobj, dma);
    if (flags == 0) {
        fobj->release();
        dma_obj->release();
        return static_cast<uint64_t>(-1);
    }

    bool ok = iommu::IoMmuManager::map_frame(dma->domain_idx_,
                                             *static_cast<cap::FrameCap *>(fobj),
                                             flags);
    fobj->release();
    dma_obj->release();
    return ok ? 0 : static_cast<uint64_t>(-1);
#else
    (void)dma_handle;
    (void)frame_handle;
    return static_cast<uint64_t>(-1);
#endif
}

uint64_t Syscall::sys_iommu_unmap(uint64_t dma_handle, uint64_t frame_handle,
                                  uint64_t, uint64_t, uint64_t *) {
#if defined(CONFIG_ARCH_X86_64)
    cap::CNode *cs = current_cspace();
    if (!cs)
        return static_cast<uint64_t>(-1);
    if (!iommu::IoMmuManager::probe())
        return static_cast<uint64_t>(-1);

    KernelObject *dma_obj = cap::lookup(
        cs, dma_handle, cap::CapType::IoMmuDma, cap::CAP_RIGHT_WRITE);
    if (!dma_obj)
        return static_cast<uint64_t>(-1);
    auto *dma = static_cast<cap::IoMmuDmaCap *>(dma_obj);

    KernelObject *fobj = cap::lookup(cs, frame_handle, cap::CapType::Frame, 0);
    if (!fobj) {
        dma_obj->release();
        return static_cast<uint64_t>(-1);
    }

    uint32_t flags = validate_iommu_pair(cs, frame_handle, fobj, dma);
    if (flags == 0) {
        fobj->release();
        dma_obj->release();
        return static_cast<uint64_t>(-1);
    }

    bool ok = iommu::IoMmuManager::unmap_frame(
        dma->domain_idx_, *static_cast<cap::FrameCap *>(fobj));
    fobj->release();
    dma_obj->release();
    return ok ? 0 : static_cast<uint64_t>(-1);
#else
    (void)dma_handle;
    (void)frame_handle;
    return static_cast<uint64_t>(-1);
#endif
}

} // namespace kernel
