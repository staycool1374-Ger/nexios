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

#include <kernel/memory/vmm.hpp>
#include <kernel/memory/vmm_path.hpp> // issue #347 Slice D
#include <kernel/memory/vmm_internal.hpp> // issue #363 shared TU internals
#include <kernel/memory/pmm.hpp>
#include <kernel/cap/frame.hpp>
#include <kernel/cap/mmio.hpp>
#include <kernel/arch/pci.hpp>
#include <kernel/task/scheduler.hpp>
#include <kernel/arch/io.hpp>
#include <constants.hpp>
#include <kernel/arch/page_table.hpp>
#include <assert.hpp>
#include <kernel/memory/vmm_errors.hpp>
#include <kernel/arch/qemu_debugcon.hpp>

namespace kernel {

using namespace errors;

/// @brief Maps every frame owned by a FrameCap into @p pml4_phys.
///        Refuses a revoked cap — capability-gated memory must not map after
///        revocation (ROADMAP 0.4.1 CSpace).  The caller must hold a pin
///        (ScopedRef / live capability slot) on @p fc.
bool VMM::map_frame_from_cap(cap::FrameCap *fc, uint64_t virt_addr, bool user,
                              uint64_t pml4_phys) {
    return map_frame_from_cap_err(fc, virt_addr, user, pml4_phys) ==
           errors::VMM_ERR_OK;
}

errors::VmmError VMM::map_phys_range_try(uint64_t virt_addr,
                                             uint64_t phys_addr,
                                             size_t page_count, bool user,
                                             uint64_t pml4_phys,
                                             bool executable) {
    if ((virt_addr & (arch::PAGE_SIZE - 1)) != 0 ||
        (phys_addr & (arch::PAGE_SIZE - 1)) != 0) {
        vmm_step(VmmMapSteps::Reject); // issue #347 Slice D
        vmm_step(VmmMapSteps::Done);
        return errors::VMM_ERR_INVALID_ADDR;
    }
    if (page_count == 0 ||
        page_count > (UINT64_MAX - virt_addr) / arch::PAGE_SIZE) {
        vmm_step(VmmMapSteps::Reject); // issue #347 Slice D
        vmm_step(VmmMapSteps::Done);
        return errors::VMM_ERR_INVALID_ADDR;
    }
    if (page_count > (UINT64_MAX - phys_addr) / arch::PAGE_SIZE) {
        vmm_step(VmmMapSteps::Reject); // issue #347 Slice D
        vmm_step(VmmMapSteps::Done);
        return errors::VMM_ERR_INVALID_ADDR;
    }
    vmm_step(VmmMapSteps::Validate); // issue #347 Slice D
    for (size_t page_idx = 0; page_idx < page_count; ++page_idx) {
        const uint64_t page_va = virt_addr + page_idx * arch::PAGE_SIZE;
        const uint64_t page_pa = phys_addr + page_idx * arch::PAGE_SIZE;
        errors::VmmError step = map_page_in_pml4_try(
            page_va, page_pa, user, executable, pml4_phys);
        if (step != errors::VMM_ERR_OK) {
            for (size_t back = 0; back < page_idx; ++back) {
                unmap_page_in_pml4(virt_addr + back * arch::PAGE_SIZE,
                                   pml4_phys);
            }
            vmm_step(VmmMapSteps::Rollback); // issue #347 Slice D
            vmm_step(VmmMapSteps::Done);
            return step;
        }
    }
    vmm_step(VmmMapSteps::Commit); // issue #347 Slice D
    vmm_step(VmmMapSteps::Done);
    return errors::VMM_ERR_OK;
}

errors::VmmError VMM::map_frame_from_cap_err(cap::FrameCap *fc,
                                              uint64_t virt_addr, bool user,
                                              uint64_t pml4_phys,
                                              bool executable) {
    if (!fc) {
        return errors::VMM_ERR_INVALID_ADDR;
    }
    if (fc->revoked()) {
        return errors::VMM_ERR_REVOKED;
    }
    if (fc->phys == 0 || fc->count == 0) {
        return errors::VMM_ERR_EMPTY_RANGE;
    }
    return map_phys_range_try(virt_addr, fc->phys, fc->count, user, pml4_phys,
                              executable);
}

/// @brief Unmaps one page previously mapped via map_frame_from_cap().
///        Clear-leaf primitive (issue #325-C): never map-with-zero (that
///        installs a PRESENT supervisor-writable leaf to physical page
///        zero — silent kernel NULL-deref access instead of #PF).
void VMM::unmap_frame_from_cap(uint64_t virt_addr, uint64_t pml4_phys) {
    // Issue #157 H1 is served by unmap_page_in_pml4's own flush on the
    // mutating path (no-op paths change nothing and need none).
    unmap_page_in_pml4(virt_addr, pml4_phys);
}

/// @brief Maps an MmioCap's memory BAR range into @p pml4_phys.
bool VMM::map_mmio_from_cap(cap::MmioCap *mmio, uint64_t virt_addr, bool user,
                             uint64_t pml4_phys) {
    return map_mmio_from_cap_err(mmio, virt_addr, user, pml4_phys) ==
           errors::VMM_ERR_OK;
}

errors::VmmError VMM::map_mmio_from_cap_err(cap::MmioCap *mmio,
                                             uint64_t virt_addr, bool user,
                                             uint64_t pml4_phys,
                                             bool executable) {
    if (!mmio) {
        return errors::VMM_ERR_INVALID_ADDR;
    }
    if (mmio->revoked()) {
        return errors::VMM_ERR_REVOKED;
    }
    if (mmio->phys == 0 || mmio->size == 0) {
        return errors::VMM_ERR_EMPTY_RANGE;
    }
    if (mmio->bar_type == arch::PciBarType::IO) {
        return errors::VMM_ERR_INVALID_ADDR; // port I/O is delegated via
                                             // sys_ioport_grant, not MMIO
    }
    if (mmio->size > UINT64_MAX - (arch::PAGE_SIZE - 1)) {
        return errors::VMM_ERR_INVALID_ADDR;
    }
    const size_t pages =
        (mmio->size + arch::PAGE_SIZE - 1) / arch::PAGE_SIZE;
    return map_phys_range_try(virt_addr, mmio->phys, pages, user, pml4_phys,
                              executable);
}

/// @brief Unmaps an MmioCap's memory BAR range previously mapped via
///        map_mmio_from_cap().
void VMM::unmap_mmio_from_cap(cap::MmioCap *mmio, uint64_t virt_addr,
                              uint64_t pml4_phys) {
    if (!mmio || mmio->size == 0)
        return;
    const size_t pages =
        (mmio->size + arch::PAGE_SIZE - 1) / arch::PAGE_SIZE;
    for (size_t i = 0; i < pages; ++i) {
        // Clear-leaf primitive (issue #325-C, same rationale as
        // unmap_frame_from_cap); per-page flush rides along.
        unmap_page_in_pml4(virt_addr + i * arch::PAGE_SIZE, pml4_phys);
    }
}

} // namespace kernel
