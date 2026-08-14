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

/// @file dmesg.cpp
/// @brief DmesgBuffer member-function template instantiations.

#include <kernel/log/dmesg.hpp>
#include <kernel/arch/timer.hpp>
#include <kernel/task/scheduler.hpp>

namespace kernel::log {

DmesgBuffer<DMESG_CAPACITY> g_dmesg;

/// @brief Push an entry (overwrites oldest if full).
/// @return true unless an entry was overwritten.
template <size_t Capacity>
bool DmesgBuffer<Capacity>::push(ErrorSubsystem subsys, uint64_t err_code,
                                 const char *msg, uintptr_t ctx) {
    if (s_suppressed_)
        return true;
    const uint64_t ts = arch::Timer::ticks();
    const uint64_t tid = kernel::Scheduler::current_task()
                             ? kernel::Scheduler::current_task()->id
                             : 0;

    size_t h = atomic_load(&head, __ATOMIC_RELAXED);
    size_t t = atomic_load(&tail, __ATOMIC_ACQUIRE);

    size_t next_h = (h + 1) & MASK;
    bool overwrote = false;

    if (next_h == t) {
        overwrote = true;
        atomic_store(&tail, (t + 1) & MASK, __ATOMIC_RELEASE);
    }

    buffer[h] = LogEntry{};
    buffer[h].timestamp = ts;
    buffer[h].task_id = tid;
    buffer[h].subsystem = subsys;
    buffer[h].error_code = err_code;
    buffer[h].context = ctx;
    // Owned copy (SIL3): the entry stores a bounded char array, so producers
    // may pass transient/ring buffers (e.g. the ELF loader's message slots)
    // without dangling once the source is reused.
    size_t i = 0;
    if (msg) {
        while (msg[i] && i < LogEntry::kMessageCap - 1) {
            buffer[h].message[i] = msg[i];
            ++i;
        }
    }
    buffer[h].message[i] = '\0';

    atomic_store(&head, next_h, __ATOMIC_RELEASE);
    return !overwrote;
}

/// @brief Pop the oldest entry.
/// @return true if an entry was available.
template <size_t Capacity> bool DmesgBuffer<Capacity>::pop(LogEntry &entry) {
    size_t t = atomic_load(&tail, __ATOMIC_RELAXED);
    size_t h = atomic_load(&head, __ATOMIC_ACQUIRE);

    if (t == h)
        return false;

    entry = buffer[t];
    atomic_store(&tail, (t + 1) & MASK, __ATOMIC_RELEASE);
    return true;
}

/// @brief Check whether the buffer is empty.
template <size_t Capacity> bool DmesgBuffer<Capacity>::empty() const {
    return atomic_load(&head, __ATOMIC_ACQUIRE) ==
           atomic_load(&tail, __ATOMIC_ACQUIRE);
}

/// @brief Return the number of entries currently in the buffer.
template <size_t Capacity> size_t DmesgBuffer<Capacity>::size() const {
    size_t h = atomic_load(&head, __ATOMIC_ACQUIRE);
    size_t t = atomic_load(&tail, __ATOMIC_ACQUIRE);
    return (h - t) & MASK;
}

/// @brief Discard all entries (reset tail to head).
template <size_t Capacity> void DmesgBuffer<Capacity>::clear() {
    size_t h = atomic_load(&head, __ATOMIC_RELAXED);
    atomic_store(&tail, h, __ATOMIC_RELEASE);
}

template class DmesgBuffer<DMESG_CAPACITY>;

} // namespace kernel::log