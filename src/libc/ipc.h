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

#pragma once

#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

#define IPC_NONBLOCK 1

int ipc_send(unsigned long dest, unsigned long type, const void* data, unsigned long size, unsigned long flags);
int ipc_recv(void* buf, unsigned long max_size);
/// @brief Send a message and block until the reply arrives.
///        Contract (issue #317 option b): the reply PAYLOAD is written to
///        @p reply_buf (a real destination), NOT over @p data.  @p reply_buf
///        must be at least @p size bytes (the reply is bounded by the request
///        size); pass NULL for no reply payload.  The return value is the
///        reply message TYPE (negative on failure), not a result code.
///        (Historically the kernel wrote the reply over @p data and dropped
///        @p reply_buf — threaded through the 5th argument register now.)
int ipc_send_sync(unsigned long dest, unsigned long type, const void* data, unsigned long size, void* reply_buf);

int ipc_notify(unsigned long task_id, unsigned long value);
int ipc_notify_wait(unsigned long* value);

int ipc_event_set(unsigned long task_id, unsigned long bits);
int ipc_event_wait(unsigned long bits, int clear_on_exit);

#ifdef __cplusplus
}
#endif
