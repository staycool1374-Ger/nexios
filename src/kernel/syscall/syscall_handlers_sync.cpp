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

#include <kernel/syscall/syscall.hpp>
#include <kernel/syscall/syscall_helpers.hpp>
#include <kernel/task/scheduler.hpp>
#include <kernel/task/task.hpp>
#include <kernel/sync/notify.hpp>
#include <kernel/sync/eventgroup.hpp>
#include <kernel/arch/timer.hpp>
#include <kernel/memory/checked_ptr.hpp>

namespace kernel {

uint64_t Syscall::sys_notify(uint64_t arg0, uint64_t arg1, uint64_t, uint64_t,
                             uint64_t *) {
    uint64_t target_id = arg0;
    uint64_t value = arg1;
    auto *t = Scheduler::find_task(target_id);
    if (!t)
        return static_cast<uint64_t>(-1);
    t->notify.notify(value);
    return 0;
}

uint64_t Syscall::sys_notify_wait(uint64_t arg0, uint64_t, uint64_t, uint64_t,
                                  uint64_t *) {
    auto *cur = syscall_task();
    if (!cur)
        return static_cast<uint64_t>(-1);
    uint64_t value = cur->notify.wait();
    if (syscall_is_user_task()) {
        // NOLINTNEXTLINE(performance-no-int-to-ptr)
        auto val_ptr = checked(reinterpret_cast<uint64_t *>(arg0));
        if (!val_ptr.valid())
            return static_cast<uint64_t>(-1);
        val_ptr.write(value);
    } else if (arg0 != 0) {
        // Kernel caller (e.g. a test driver task): CheckedPtr::write is
        // fail-closed for non-user addresses, so a kernel out-pointer must
        // be stored directly (issue #148: the old Notify::wait +
        // try_wait-fixup masked this — the syscall never delivered).
        // NOLINTNEXTLINE(performance-no-int-to-ptr)
        *reinterpret_cast<uint64_t *>(arg0) = value;
    }
    return 0;
}

uint64_t Syscall::sys_event_set(uint64_t arg0, uint64_t arg1, uint64_t,
                                uint64_t, uint64_t *) {
    uint64_t target_id = arg0;
    uint64_t bits = arg1;
    auto *t = Scheduler::find_task(target_id);
    if (!t)
        return static_cast<uint64_t>(-1);
    t->event_group.set_bits(bits);
    return 0;
}

uint64_t Syscall::sys_event_wait(uint64_t arg0, uint64_t arg1, uint64_t,
                                 uint64_t, uint64_t *) {
    uint64_t wanted = arg0;
    uint64_t clear_on_exit = arg1;
    auto *cur = syscall_task();
    if (!cur)
        return static_cast<uint64_t>(-1);
    cur->event_group.wait_bits(wanted, clear_on_exit != 0);
    return 0;
}

uint64_t Syscall::sys_alarm(uint64_t arg0, uint64_t arg1, uint64_t, uint64_t,
                            uint64_t *) {
    auto *cur = syscall_task();
    if (!cur)
        return static_cast<uint64_t>(-1);

    uint64_t seconds = arg0;
    uint64_t microseconds = arg1;

    if (seconds == 0 && microseconds == 0) {
        cur->alarm_armed = false;
        return 0;
    }

    uint64_t ticks = seconds * 1000 + (microseconds + 999) / 1000;
    cur->alarm_ticks = ticks;
    cur->alarm_armed = true;
    return 0;
}

} // namespace kernel
