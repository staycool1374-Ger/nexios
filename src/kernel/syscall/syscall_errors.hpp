#pragma once

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

/// @file syscall_errors.hpp
/// @brief Syscall error codes aggregated from all subsystems (X-macro table).

#pragma once

#include <types.hpp>
#include <assert.hpp>

namespace kernel::errors {

#define SYSCALL_ERROR_CODES                                                    \
    X(OK, 0, "OK")                                                             \
    X(INVALID_NUMBER, 1, "Invalid syscall number")                             \
    X(INVALID_ARGS, 2, "Invalid syscall arguments")                            \
    X(FAULT, 3, "Page fault during syscall")                                   \
    /* Task subsystem errors (100+) */                                         \
    X(TASK_OOM, 101, "Task out of memory")                                     \
    X(TASK_TABLE_FULL, 102, "Task table full")                                 \
    X(TASK_STACK_ALLOC, 103, "Kernel stack allocation failed")                 \
    X(TASK_USTACK_ALLOC, 104, "User stack allocation failed")                  \
    X(TASK_PML4_CLONE, 105, "PML4 clone failed")                               \
    X(TASK_NOT_FOUND, 106, "Task not found")                                   \
    X(TASK_INVALID_ARG, 107, "Task invalid argument")                          \
    X(TASK_INVALID_STATE, 108, "Task invalid state")                           \
    X(TLS_INVALID_BASE, 109, "Invalid TLS base address")                       \
    /* PMM subsystem errors (200+) */                                          \
    X(PMM_OOM, 201, "Physical memory exhausted")                               \
    X(PMM_USER_OOM, 202, "User physical memory exhausted")                     \
    X(PMM_TABLE_OOM, 203, "Page table pool exhausted")                         \
    X(PMM_INVALID, 204, "Invalid physical address")                            \
    /* VMM subsystem errors (300+) */                                          \
    X(VMM_PAGE_ALLOC, 301, "Failed to allocate page table page")               \
    X(VMM_PML4_ALLOC, 302, "Failed to allocate PML4 page")                     \
    X(VMM_INVALID_ADDR, 303, "Invalid virtual address")                        \
    X(VMM_NOT_MAPPED, 304, "Virtual address not mapped")                       \
    /* Sync subsystem errors (400+) */                                         \
    X(SYNC_NO_TASK, 401, "No current task context")                            \
    X(SYNC_MAX_WAITERS, 402, "Maximum waiters exceeded")                       \
    X(SYNC_ALREADY_INIT, 403, "Already initialized")                           \
    X(SYNC_NOT_OWNER, 404, "Not the lock owner")                               \
    X(SYNC_NOT_LOCKED, 405, "Lock not held")                                   \
    X(SYNC_QUEUE_FULL, 406, "Message queue full")                              \
    X(SYNC_QUEUE_EMPTY, 407, "Message queue empty")                            \
    X(SYNC_MSG_TOO_LARGE, 408, "Message too large")                            \
    X(SYNC_ALREADY_WAITING, 409, "Already waiting")                            \
    X(SYNC_INVALID_ARGS, 410, "Invalid arguments")                             \
    X(SYNC_BUFFER_FULL, 411, "Buffer full")                                    \
    X(SYNC_BUFFER_EMPTY, 412, "Buffer empty")                                  \
    X(SYNC_NO_WAITER, 413, "No waiter to notify")                              \
    X(SYNC_INTERRUPTED, 414, "Interrupted by signal")                          \
    /* VFS subsystem errors (500+) */                                          \
    X(VFS_INVALID_FD, 501, "Invalid file descriptor")                          \
    X(VFS_FD_TABLE_FULL, 502, "FD table full")                                 \
    X(VFS_NOT_FOUND, 503, "Path not found")                                    \
    X(VFS_NOT_DIR, 504, "Not a directory")                                     \
    X(VFS_IS_DIR, 505, "Is a directory")                                       \
    X(VFS_EXISTS, 506, "Already exists")                                       \
    X(VFS_NOT_EMPTY, 507, "Directory not empty")                               \
    X(VFS_NO_DEVICE, 508, "No such device")                                    \
    X(VFS_NO_SPACE, 509, "No space left")                                      \
    X(VFS_PERMISSION, 510, "Permission denied")                                \
    X(VFS_INVALID_ARGS, 511, "Invalid arguments")                              \
    X(VFS_IO_ERROR, 512, "I/O error")                                          \
    X(VFS_NOT_SUPPORTED, 513, "Not supported")                                 \
    X(VFS_MOUNT_BUSY, 514, "Mount point busy")                                 \
    X(VFS_NO_SUCH_FS, 515, "Filesystem not found")                             \
    /* IPC subsystem errors (600+) */                                          \
    X(IPC_NO_QUEUE, 601, "No message queue")                                   \
    X(IPC_QUEUE_FULL, 602, "Queue full")                                       \
    X(IPC_QUEUE_EMPTY, 603, "Queue empty")                                     \
    X(IPC_NO_DEST, 604, "Destination not found")                               \
    X(IPC_NO_REPLY, 605, "No reply")                                           \
    X(IPC_TIMEOUT, 606, "Timeout")                                             \
    X(IPC_INVALID_MSG, 607, "Invalid message")                                 \
    X(IPC_NO_BUFFER, 608, "No buffer")                                         \
    X(IPC_INVALID_ARGS, 609, "Invalid arguments")                              \
    /* BufferPool subsystem errors (700+) */                                   \
    X(BUF_OOM, 701, "Buffer OOM")                                              \
    X(BUF_MAX_BUFFERS, 702, "Max buffers reached")                             \
    X(BUF_INVALID_HANDLE, 703, "Invalid handle")                               \
    X(BUF_NOT_OWNER, 704, "Not owner")                                         \
    X(BUF_VA_IN_USE, 705, "VA in use")                                         \
    X(BUF_VA_OUT_OF_RANGE, 706, "VA out of range")                             \
    X(BUF_NOT_MAPPED, 707, "Not mapped")                                       \
    /* Scheduler subsystem errors (800+) */                                    \
    X(SCHED_TABLE_FULL, 801, "Task table full")                                \
    X(SCHED_NOT_FOUND, 802, "Task not found")                                  \
    X(SCHED_DUPLICATE_ID, 803, "Duplicate task ID")                            \
    X(SCHED_INVALID_STATE, 804, "Invalid task state")                          \
    X(SCHED_NO_CURRENT, 805, "No current task")                                \
    X(SCHED_NO_IDLE, 806, "No idle task")                                      \
    X(SCHED_PREEMPT_DISABLED, 807, "Preemption disabled")                      \
    X(SCHED_ZOMBIE, 808, "Zombie task")                                        \
    X(SCHED_INVALID_MAGIC, 809, "Invalid TCB magic")                           \
    X(SCHED_NO_SHELL, 810, "No shell task")                                    \
    X(SCHED_INVALID_ARGS, 811, "Invalid arguments")                            \
    /* MemPool subsystem errors (900+) */                                      \
    X(MEMPOOL_OOM, 901, "MemPool OOM")                                         \
    X(MEMPOOL_TOO_LARGE, 902, "Size too large")                                \
    X(MEMPOOL_INVALID_PTR, 903, "Invalid pointer")                             \
    X(MEMPOOL_DOUBLE_FREE, 904, "Double free")                                 \
    X(MEMPOOL_UNINIT, 905, "Pool uninitialized")                               \
    X(MEMPOOL_INVALID_POOL, 906, "Invalid pool index")                         \
    X(MEMPOOL_CORRUPTED, 907, "Pool corrupted")

/// @brief System-call-level error codes (aggregated from all kernel
/// subsystems).
// NOLINTNEXTLINE(performance-enum-size)
enum SyscallError : uint64_t {
#define X(name, num, msg) SYS_ERR_##name = (num),
    SYSCALL_ERROR_CODES
#undef X
};

/// @brief Return a human-readable string for a syscall error code.
template <> inline const char *error_string(SyscallError e) {
    switch (e) {
#define X(name, num, msg)                                                      \
    case SYS_ERR_##name:                                                       \
        return msg;
        SYSCALL_ERROR_CODES
#undef X
    }
    return "Unknown syscall error";
}

/// @brief Convert a task subsystem error to SyscallError (offset 100).
inline SyscallError from_task_error(uint64_t e) {
    return static_cast<SyscallError>(100 + e);
}
inline SyscallError from_pmm_error(uint64_t e) {
    return static_cast<SyscallError>(200 + e);
}
inline SyscallError from_vmm_error(uint64_t e) {
    return static_cast<SyscallError>(300 + e);
}
inline SyscallError from_sync_error(uint64_t e) {
    return static_cast<SyscallError>(400 + e);
}
inline SyscallError from_vfs_error(uint64_t e) {
    return static_cast<SyscallError>(500 + e);
}
inline SyscallError from_ipc_error(uint64_t e) {
    return static_cast<SyscallError>(600 + e);
}
inline SyscallError from_bufpool_error(uint64_t e) {
    return static_cast<SyscallError>(700 + e);
}
inline SyscallError from_sched_error(uint64_t e) {
    return static_cast<SyscallError>(800 + e);
}
inline SyscallError from_mempool_error(uint64_t e) {
    return static_cast<SyscallError>(900 + e);
}

} // namespace kernel::errors