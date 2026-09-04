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

/// @file syscall_handlers_death.cpp
/// @brief Asynchronous task-death notification syscalls (issue #105 Part B):
///   SYS_DEATH_WATCH  — register a death watch on a task (authority: the
///                      caller is the watched task or the supervisor).
///   SYS_DEATH_RECV   — drain the next pending death record (never blocks).
///   SYS_DEATH_UNWATCH — remove a death watch.
/// These syscalls take the registry lock and DEATH_RECV writes user memory —
/// they MUST stay out of k_syscall_fast[] (issue #92 discipline).

#include <kernel/syscall/syscall.hpp>
#include <kernel/syscall/syscall_helpers.hpp>
#include <kernel/ipc/death_notify.hpp>
#include <kernel/memory/checked_ptr.hpp>
#include <kernel/task/scheduler.hpp>
#include <kernel/task/task.hpp>

using namespace kernel;

uint64_t Syscall::sys_death_watch(uint64_t watched_pid, uint64_t supervisor_pid,
                                  uint64_t, uint64_t, uint64_t *) {
    auto *t = Scheduler::current_task();
    if (!t)
        return static_cast<uint64_t>(-1);
    bool ok = ipc::DeathNotify::watch(*t, watched_pid, supervisor_pid);
    return ok ? 0 : static_cast<uint64_t>(-1);
}

uint64_t Syscall::sys_death_recv(uint64_t out_ptr, uint64_t, uint64_t,
                                 uint64_t, uint64_t *) {
    auto *t = Scheduler::current_task();
    if (!t)
        return static_cast<uint64_t>(-1);

    // MP-4 (SMAP): a user task writes via safe_copy_to_user (stac-wrapped);
    // a kernel task writes directly.
    auto *out = reinterpret_cast<ipc::DeathRecord *>(out_ptr);
    // Validate the destination BEFORE consuming a record: a failed recv must
    // not permanently lose the death notification (issue #105 Part B audit).
    ipc::DeathRecord rec{};
    int got = 0;
    if (syscall_is_user_task()) {
        CheckedPtr<ipc::DeathRecord> cptr(out, 1);
        if (!cptr.valid())
            return static_cast<uint64_t>(-1);
        got = ipc::DeathNotify::recv(*t, rec);
        if (got == 0)
            return 0; // nothing pending — never blocks
        if (!cptr.copy_to(&rec))
            return static_cast<uint64_t>(-1);
    } else {
        if (!out)
            return static_cast<uint64_t>(-1);
        got = ipc::DeathNotify::recv(*t, rec);
        if (got == 0)
            return 0; // nothing pending — never blocks
        *out = rec;
    }
    return 1;
}

uint64_t Syscall::sys_death_unwatch(uint64_t watched_pid, uint64_t, uint64_t,
                                    uint64_t, uint64_t *) {
    auto *t = Scheduler::current_task();
    if (!t)
        return static_cast<uint64_t>(-1);
    ipc::DeathNotify::unwatch(*t, watched_pid);
    return 0;
}