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
/// @brief SYS_IOPORT_GRANT handler (issue #3): capability-gated, per-task
/// x86_64 I/O-port delegation via the TSS I/O permission bitmap.  Non-x86_64
/// builds return -1 (no port I/O).  No blocking; all dereferences through a
/// capability slot validated by cap::lookup.

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
    if (!arch::iopb_grant_range(*t, static_cast<uint16_t>(port_start),
                                static_cast<uint32_t>(port_count))) {
        obj->release();
        return static_cast<uint64_t>(-1);
    }

    obj->release();
    return 0;
#else
    (void)cap_handle;
    (void)port_start;
    (void)port_count;
    return static_cast<uint64_t>(-1);
#endif
}

} // namespace kernel