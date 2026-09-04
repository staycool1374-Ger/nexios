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

/// @file syscall_handlers_pager.cpp
/// @brief External pager syscalls (issue #107):
///   SYS_PAGER_REGISTER  — designate a pager for the caller's own faults.
///   SYS_PAGER_RECV      — drain the next pending fault (never blocks).
///   SYS_PAGER_MAP       — map the pager's FrameCap into the CLIENT's PML4.
///   SYS_PAGER_ABORT     — pager cannot satisfy a fault (unmap + poison VA).
///   SYS_PAGER_UNREGISTER — remove a pager registration.
/// These syscalls write user memory and touch cross-task page tables — they
/// MUST stay out of k_syscall_fast[] (issue #92 discipline).

#include <kernel/syscall/syscall.hpp>
#include <kernel/syscall/syscall_helpers.hpp>
#include <kernel/ipc/pager_registry.hpp>
#include <kernel/cap/cap.hpp>
#include <kernel/cap/frame.hpp>
#include <kernel/memory/checked_ptr.hpp>
#include <kernel/task/scheduler.hpp>
#include <kernel/task/task.hpp>

using namespace kernel;

uint64_t Syscall::sys_pager_register(uint64_t pager_pid, uint64_t, uint64_t,
                                     uint64_t, uint64_t *) {
    auto *t = Scheduler::current_task();
    if (!t)
        return static_cast<uint64_t>(-1);
    return ipc::PagerRegistry::register_client(*t, pager_pid)
               ? 0
               : static_cast<uint64_t>(-1);
}

uint64_t Syscall::sys_pager_recv(uint64_t out_ptr, uint64_t, uint64_t,
                                 uint64_t, uint64_t *) {
    auto *t = Scheduler::current_task();
    if (!t)
        return static_cast<uint64_t>(-1);

    // Validate the destination BEFORE consuming (auditor S2 pattern from
    // sys_death_recv): a failed recv must not lose the fault notification.
    auto *out = reinterpret_cast<ipc::PagerFaultMsg *>(out_ptr);
    ipc::PagerFaultMsg msg{};
    int got = 0;
    if (syscall_is_user_task()) {
        CheckedPtr<ipc::PagerFaultMsg> cptr(out, 1);
        if (!cptr.valid())
            return static_cast<uint64_t>(-1);
        got = ipc::PagerRegistry::recv(*t, msg);
        if (got == 0)
            return 0; // nothing pending — never blocks
        if (!cptr.copy_to(&msg))
            return static_cast<uint64_t>(-1);
    } else {
        if (!out)
            return static_cast<uint64_t>(-1);
        got = ipc::PagerRegistry::recv(*t, msg);
        if (got == 0)
            return 0;
        *out = msg;
    }
    return 1;
}

uint64_t Syscall::sys_pager_map(uint64_t fault_id, uint64_t cap_handle,
                                uint64_t count, uint64_t flags, uint64_t *) {
    auto *pager = Scheduler::current_task();
    if (!pager)
        return static_cast<uint64_t>(-1);

    // Capability-gated: the pager must hold the Frame capability it maps
    // (type + WRITE).  Mirrors sys_frame_map.
    pager->ensure_cspace();
    cap::CNode *cs = pager->get_cspace();
    if (!cs)
        return static_cast<uint64_t>(-1);
    KernelObject *obj =
        cap::lookup(cs, cap_handle, cap::CapType::Frame, cap::CAP_RIGHT_WRITE);
    if (!obj)
        return static_cast<uint64_t>(-1);
    auto *fc = static_cast<cap::FrameCap *>(obj);

    int r = ipc::PagerRegistry::map(*pager, fault_id, fc, count, flags);
    obj->release();
    return r == 0 ? 0 : static_cast<uint64_t>(-1);
}

uint64_t Syscall::sys_pager_abort(uint64_t fault_id, uint64_t, uint64_t,
                                  uint64_t, uint64_t *) {
    auto *t = Scheduler::current_task();
    if (!t)
        return static_cast<uint64_t>(-1);
    return ipc::PagerRegistry::abort(*t, fault_id) == 0
               ? 0
               : static_cast<uint64_t>(-1);
}

uint64_t Syscall::sys_pager_unregister(uint64_t client_pid, uint64_t, uint64_t,
                                       uint64_t, uint64_t *) {
    auto *t = Scheduler::current_task();
    if (!t)
        return static_cast<uint64_t>(-1);
    // Authority is enforced inside the registry: the caller may remove its own
    // registration, or its designated pager may drop it (checked by slot
    // match against the caller).
    return ipc::PagerRegistry::unregister(*t, client_pid)
               ? 0
               : static_cast<uint64_t>(-1);
}