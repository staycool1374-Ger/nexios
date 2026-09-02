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

/// @file syscall_handlers_mmio.cpp
/// @brief SYS_IOPORT_GRANT / SYS_MMIO_MAP / SYS_MMIO_UNMAP handlers (issues
/// #3/#8): capability-gated, per-task x86_64 I/O-port delegation via the TSS
/// I/O permission bitmap, and capability-gated user-space MMIO page-frame
/// mapping for Ring 3 drivers.  Non-x86_64 builds return -1 (no port I/O).
/// No blocking; all dereferences through a capability slot validated by
/// cap::lookup.

#include <kernel/syscall/syscall.hpp>
#include <kernel/task/scheduler.hpp>
#include <kernel/task/task.hpp>
#include <kernel/cap/cap.hpp>
#include <kernel/cap/cap_types.hpp>
#include <kernel/cap/mmio.hpp>
#include <kernel/arch/hal/iopb.hpp>
#include <kernel/arch/pci.hpp>

namespace kernel {

// Shared with syscall_handlers_cap.cpp (same TU-visible helper).
cap::CNode *current_cspace();

uint64_t Syscall::sys_ioport_grant(uint64_t cap_handle, uint64_t port_start,
                                   uint64_t port_count, uint64_t,
                                   uint64_t *) {
#if defined(CONFIG_ARCH_X86_64)
    cap::CNode *src = current_cspace();
    if (!src)
        return static_cast<uint64_t>(-1);

    // Capability-gated: the caller must hold an IO-type MmioCap covering the
    // requested port range (WRITE = the right to mutate the port-access
    // state this grant installs).
    KernelObject *obj =
        cap::lookup(src, cap_handle, cap::CapType::Mmio, cap::CAP_RIGHT_WRITE);
    if (!obj)
        return static_cast<uint64_t>(-1);
    auto *mmio = static_cast<cap::MmioCap *>(obj);

    // IO BAR type required (memory BARs are delegated via MMIO mapping).
    if (mmio->bar_type != arch::PciBarType::IO) {
        obj->release();
        return static_cast<uint64_t>(-1);
    }

    // Range validation in 64-bit arithmetic — no wraparound.
    if (port_count == 0 || port_start >= 65536ULL ||
        port_count > 65536ULL - port_start ||
        static_cast<uint64_t>(mmio->phys) > port_start ||
        static_cast<uint64_t>(mmio->size) <
            static_cast<uint64_t>(port_start + port_count) -
                static_cast<uint64_t>(mmio->phys)) {
        obj->release();
        return static_cast<uint64_t>(-1);
    }

    auto *t = Scheduler::current_task();
    if (!t || !arch::iopb_claim(*t)) {
        obj->release();
        return static_cast<uint64_t>(-1); // pool exhausted — fail closed
    }
    // Reserve the ledger entry BEFORE installing the grant (issue #8): a full
    // ledger fails the grant closed so there is never a grant without a
    // ledger record (the revoke-rollback would otherwise silently weaken).
    if (!arch::iopb_ledger_add(*t, mmio, static_cast<uint16_t>(port_start),
                               static_cast<uint32_t>(port_count))) {
        obj->release();
        return static_cast<uint64_t>(-1);
    }
    if (!arch::iopb_grant_range(*t, static_cast<uint16_t>(port_start),
                                static_cast<uint32_t>(port_count))) {
        obj->release();
        return static_cast<uint64_t>(-1);
    }

    // Make the grant immediately effective for the running task (the loaded
    // owner is only updated on context switch; a user task must not wait for
    // its next dispatch to use the granted ports).  No-op for kernel tasks.
    arch::iopb_switch_to(*t);

    obj->release();
    return 0;
#else
    (void)cap_handle;
    (void)port_start;
    (void)port_count;
    return static_cast<uint64_t>(-1);
#endif
}

uint64_t Syscall::sys_mmio_map(uint64_t cap_handle, uint64_t, uint64_t,
                               uint64_t, uint64_t *) {
    cap::CNode *src = current_cspace();
    if (!src)
        return static_cast<uint64_t>(-1);

    // Capability-gated: the caller must hold a MEMORY-type MmioCap (WRITE =
    // the right to install the mapping).  IO-type caps are delegated via
    // sys_ioport_grant, not MMIO.
    KernelObject *obj =
        cap::lookup(src, cap_handle, cap::CapType::Mmio, cap::CAP_RIGHT_WRITE);
    if (!obj)
        return static_cast<uint64_t>(-1);
    auto *mmio = static_cast<cap::MmioCap *>(obj);

    auto *t = Scheduler::current_task();
    if (!t) {
        obj->release();
        return static_cast<uint64_t>(-1);
    }
    uint64_t va = cap::MmioUserMap::map(*t, *mmio);
    obj->release();
    return va;
}

uint64_t Syscall::sys_mmio_unmap(uint64_t va, uint64_t, uint64_t, uint64_t,
                                 uint64_t *) {
    auto *t = Scheduler::current_task();
    if (!t)
        return static_cast<uint64_t>(-1);
    return cap::MmioUserMap::unmap(*t, va) ? 0 : static_cast<uint64_t>(-1);
}

} // namespace kernel