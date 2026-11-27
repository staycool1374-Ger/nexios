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

/// @file syscall_handlers_mem.cpp
/// @brief R-MEM supervisor observation syscalls (issue #575): WATCH
/// (self-registration, idempotent, fail-closed) and RECV (32-B record
/// copy, never blocks, never clears the latch).

#include <kernel/syscall/syscall.hpp>
#include <kernel/syscall/syscall_errors.hpp>
#include <kernel/syscall/syscall_helpers.hpp>
#include <kernel/task/scheduler.hpp>
#include <kernel/task/task.hpp>
#include <kernel/memory/mem_pressure_notify.hpp>
#include <kernel/memory/checked_ptr.hpp>

namespace kernel {

uint64_t Syscall::sys_mem_pressure_watch(uint64_t, uint64_t, uint64_t,
                                         uint64_t, uint64_t *) {
    TaskControlBlock *caller = Scheduler::current_task();
    if (caller == nullptr ||
        caller->magic != TaskControlBlock::TCB_MAGIC) {
        return static_cast<uint64_t>(errors::SYS_ERR_GENERIC_FAIL);
    }
    if (!mem_pressure::register_supervisor(*caller)) {
        return static_cast<uint64_t>(
            errors::SYS_ERR_MEMPRESSURE_SUPERVISOR_TAKEN);
    }
    return static_cast<uint64_t>(errors::SYS_ERR_OK);
}

uint64_t Syscall::sys_mem_pressure_recv(uint64_t arg0, uint64_t, uint64_t,
                                        uint64_t, uint64_t *) {
    if (arg0 == 0) {
        return static_cast<uint64_t>(errors::SYS_ERR_GENERIC_FAIL);
    }
    mem_pressure::PressureRecord rec = mem_pressure::record();
    // MP-4 (SMAP): local + safe_copy_to_user (stac-wrapped), same shape
    // as sys_uname — the record is a fixed 32-B POD.
    if (syscall_is_user_task()) {
        auto rec_ptr =
            checked(reinterpret_cast<mem_pressure::PressureRecord *>(arg0));
        if (!rec_ptr.valid()) {
            return static_cast<uint64_t>(errors::SYS_ERR_GENERIC_FAIL);
        }
        if (!safe_copy_to_user(rec_ptr.unsafe_ptr(), &rec, 1)) {
            return static_cast<uint64_t>(errors::SYS_ERR_GENERIC_FAIL);
        }
    } else {
        // NOLINTNEXTLINE(performance-no-int-to-ptr)
        *reinterpret_cast<mem_pressure::PressureRecord *>(arg0) = rec;
    }
    return static_cast<uint64_t>(errors::SYS_ERR_OK);
}

} // namespace kernel
