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

/// @file syscall_handlers_thread.cpp
/// @brief THREAD_CREATE(93) handler (issue #52): spawn a thread sharing
/// an address space.  TCB-per-thread scheduler integration — the child is
/// a first-class schedulable task sharing the owner's root PML4 with a
/// private window stack (see TaskControlBlock::spawn_thread).
///
/// Authority model (audit S2-1): self-spawn (arg0 == 0) is capless —
/// same authority as FORK.  Cross-task spawn (arg0 = pid) requires the
/// caller to hold a live Task capability carrying the SHARE right (never
/// implied by WRITE); the handle is the authority token, the pid selects
/// the validated target.  Target-bound ThreadCap (WdogCap-style) is a
/// documented follow-up; v1 has no production SHARE minters, so the
/// cross-task path is fail-closed by construction.  Stays out of
/// k_syscall_fast[] (page-table-touching, issue #92 discipline).

#include <kernel/syscall/syscall.hpp>
#include <kernel/syscall/syscall_errors.hpp>
#include <kernel/syscall/syscall_helpers.hpp>
#include <kernel/task/scheduler.hpp>
#include <kernel/task/task.hpp>
#include <kernel/cap/cap.hpp>
#include <kernel/cap/cap_types.hpp>

namespace kernel {

uint64_t Syscall::sys_thread_create(uint64_t arg0, uint64_t arg1,
                                    uint64_t arg2, uint64_t arg3,
                                    uint64_t *) {
    auto *caller = syscall_task();
    if (caller == nullptr || !caller->is_user_) {
        return static_cast<uint64_t>(errors::SYS_ERR_GENERIC_FAIL);
    }
    TaskControlBlock *owner = nullptr;
    if (arg0 == 0) {
        // Self-spawn: reserved handle must be zero (fail-closed).
        if (arg3 != 0) {
            return static_cast<uint64_t>(errors::SYS_ERR_GENERIC_FAIL);
        }
        owner = caller;
    } else {
        // Cross-task spawn: a live (Task, SHARE) authority token is
        // mandatory; no token type is ever implicitly sufficient.
        if (arg3 == 0) {
            return static_cast<uint64_t>(errors::SYS_ERR_GENERIC_FAIL);
        }
        caller->ensure_cspace();
        cap::CNode *cspace = caller->get_cspace();
        if (cspace == nullptr) {
            return static_cast<uint64_t>(errors::SYS_ERR_GENERIC_FAIL);
        }
        KernelObject *token = cap::lookup(cspace, arg3, cap::CapType::Task,
                                          cap::CapRights::SHARE);
        if (token == nullptr) {
            return static_cast<uint64_t>(errors::SYS_ERR_GENERIC_FAIL);
        }
        token->release();
        // Issue #374: fail fast on never-issued IDs without probing.
        if (!Scheduler::validate_id(arg0)) {
            return static_cast<uint64_t>(errors::SYS_ERR_GENERIC_FAIL);
        }
        auto *target = Scheduler::find_task(arg0);
        if (!TaskControlBlock::is_valid(target)) {
            return static_cast<uint64_t>(errors::SYS_ERR_GENERIC_FAIL);
        }
        owner = target;
    }
    TaskControlBlock *thread =
        TaskControlBlock::spawn_thread(*owner, arg1, arg2);
    if (thread == nullptr) {
        return static_cast<uint64_t>(errors::SYS_ERR_GENERIC_FAIL);
    }
    // Issue #20 precedent (sys_fork): admission gate before the thread is
    // observable; on denial destroy (never registered, no zombie).
    if (Scheduler::add_task_err(*thread) != errors::SCHED_ERR_OK) {
        TaskControlBlock::destroy(thread);
        return static_cast<uint64_t>(errors::SYS_ERR_GENERIC_FAIL);
    }
    return thread->id;
}

} // namespace kernel
