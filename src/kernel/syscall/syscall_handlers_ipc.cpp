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

/// @file syscall_handlers_ipc.cpp
/// @brief Syscall handlers for IPC operations: send, receive, sync send,
/// mailbox, notify, events.

#include <kernel/syscall/syscall.hpp>
#include <kernel/syscall/syscall_helpers.hpp>
#include <kernel/task/scheduler.hpp>
#include <kernel/task/task.hpp>
#include <kernel/task/sporadic_server.hpp>
#include <kernel/arch/timer.hpp>
#include <kernel/ipc/ipc.hpp>
#include <kernel/ipc/buffer_pool.hpp>
#include <kernel/memory/checked_ptr.hpp>
#include <kernel/arch/io.hpp>

namespace kernel {

uint64_t Syscall::sys_send(uint64_t arg0, uint64_t arg1, uint64_t arg2,
                           uint64_t arg3, uint64_t *) {
    uint64_t dest_id = arg0;
    uint64_t flags = 0;
    Message msg{};
    auto *cur = syscall_task();
    msg.sender_id = cur ? cur->id : 0;
    msg.type = arg2;
    msg.data_size = arg3 < IPC_MAX_MSG_SIZE ? arg3 : IPC_MAX_MSG_SIZE;
    // NOLINTNEXTLINE(performance-no-int-to-ptr)
    auto data = checked(reinterpret_cast<const uint8_t *>(arg1), msg.data_size);
    if (!data.valid())
        return static_cast<uint64_t>(-1);
    for (size_t i = 0; i < msg.data_size; ++i) {
        msg.data[i] = data.read(i);
    }
    return IPC::send(dest_id, msg, flags) ? 0 : static_cast<uint64_t>(-1);
}

uint64_t Syscall::sys_receive(uint64_t, uint64_t arg1, uint64_t arg2,
                              uint64_t arg3, uint64_t *) {
    uint64_t max_size = arg2;
    // VULN-W3: arg3 is the bounded-wait timeout in ticks.  0 = block forever
    // (preserves current behaviour for non-real-time callers).
    uint64_t timeout_ticks = arg3;
    uint64_t deadline =
        timeout_ticks ? arch::Timer::ticks() + timeout_ticks : 0;
    // NOLINTNEXTLINE(performance-no-int-to-ptr)
    auto buf = checked(reinterpret_cast<uint8_t *>(arg1), max_size);
    if (!buf.valid())
        return static_cast<uint64_t>(-1);
    uint8_t *raw_buf = buf.unsafe_ptr();
    Message msg{};
    auto *cur = syscall_task();
    if (!cur)
        return static_cast<uint64_t>(-1);
    bool ok = false;
    bool was_blocked = false;
    while (!(ok = IPC::recv(msg))) {
        // VULN-W3: bounded-wait deadline — fail with -1 once the budget is
        // exhausted instead of blocking indefinitely.
        if (deadline && arch::Timer::ticks() >= deadline)
            return static_cast<uint64_t>(-1);
        if (cur->sporadic_server) {
            cur->sporadic_server->on_completion(arch::Timer::ticks());
        }
        cur->state = TaskState::BLOCKED;
        was_blocked = true;
        Scheduler::reschedule();
        // v0.4.0 MP-1: sti/hlt/cli is the USER-task blocked-wait pattern;
        // key on is_user_ (every task now owns a PML4).
        if (cur->is_user_) {
            arch::sti();
            arch::hlt();
            arch::cli();
        }
    }
    if (was_blocked) {
        cur->remaining_ticks = cur->period_ticks;
        if (cur->sporadic_server) {
            cur->sporadic_server->on_activation(arch::Timer::ticks());
        }
    }
    uint64_t copy_size = msg.data_size;
    if (copy_size > max_size)
        copy_size = max_size;
    for (size_t i = 0; i < copy_size; ++i)
        raw_buf[i] = msg.data[i];
    return msg.type;
}

uint64_t Syscall::sys_send_sync(uint64_t arg0, uint64_t arg1, uint64_t arg2,
                                uint64_t arg3, uint64_t *) {
    uint64_t dest_id = arg0;
    uint64_t type = arg2;
    uint64_t data_size = arg3 < IPC_MAX_MSG_SIZE ? arg3 : IPC_MAX_MSG_SIZE;
    // NOLINTNEXTLINE(performance-no-int-to-ptr)
    auto data = checked(reinterpret_cast<const uint8_t *>(arg1), data_size);
    if (!data.valid())
        return static_cast<uint64_t>(-1);
    // NOLINTNEXTLINE(performance-no-int-to-ptr)
    auto data_rw = checked(reinterpret_cast<uint8_t *>(arg1), data_size);
    Message msg{};
    auto *cur = syscall_task();
    if (!cur)
        return static_cast<uint64_t>(-1);
    msg.sender_id = cur->id;
    msg.type = type;
    msg.data_size = data_size;
    for (size_t i = 0; i < data_size; ++i)
        msg.data[i] = data.read(i);
    Message reply{};
    if (!IPC::send_sync(dest_id, msg, reply))
        return static_cast<uint64_t>(-1);
    uint64_t copy_size = reply.data_size;
    if (copy_size > IPC_MAX_MSG_SIZE)
        copy_size = IPC_MAX_MSG_SIZE;
    for (size_t i = 0; i < copy_size; ++i)
        data_rw.write(reply.data[i], i);
    return reply.type;
}

uint64_t Syscall::sys_create_mailbox(uint64_t, uint64_t, uint64_t, uint64_t,
                                     uint64_t *) {
    return 0;
}

uint64_t Syscall::sys_destroy_mailbox(uint64_t, uint64_t, uint64_t, uint64_t,
                                      uint64_t *) {
    return 0;
}

uint64_t Syscall::sys_buf_alloc(uint64_t arg0, uint64_t, uint64_t, uint64_t,
                                uint64_t *) {
    auto *cur = syscall_task();
    if (!cur)
        return 0;
    uint64_t va = arg0;
    return BufferPool::alloc(*cur, va);
}

uint64_t Syscall::sys_buf_free(uint64_t arg0, uint64_t, uint64_t, uint64_t,
                               uint64_t *) {
    auto *cur = syscall_task();
    if (!cur)
        return static_cast<uint64_t>(-1);
    if (!BufferPool::free(*cur, arg0))
        return static_cast<uint64_t>(-1);
    return 0;
}

uint64_t Syscall::sys_buf_map(uint64_t arg0, uint64_t arg1, uint64_t, uint64_t,
                              uint64_t *) {
    auto *cur = syscall_task();
    if (!cur)
        return static_cast<uint64_t>(-1);
    uint64_t handle = arg0;
    uint64_t va = arg1;
    if (!BufferPool::map(*cur, handle, va))
        return static_cast<uint64_t>(-1);
    return 0;
}

uint64_t Syscall::sys_buf_unmap(uint64_t arg0, uint64_t, uint64_t, uint64_t,
                                uint64_t *) {
    auto *cur = syscall_task();
    if (!cur)
        return static_cast<uint64_t>(-1);
    if (!BufferPool::unmap(*cur, arg0))
        return static_cast<uint64_t>(-1);
    return 0;
}

} // namespace kernel
