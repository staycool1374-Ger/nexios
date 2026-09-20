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

/// @file syscall_handlers_posix_time.cpp
/// @brief POSIX time/timer syscalls: clock_gettime, nanosleep,
/// timer_create (multiplex), timerfd_create (issue #76, v0.5.0).

#include <kernel/syscall/syscall.hpp>
#include <kernel/syscall/syscall_helpers.hpp>
#include <kernel/task/scheduler.hpp>
#include <kernel/task/task.hpp>
#include <kernel/time/posix_time.hpp>
#include <kernel/arch/io.hpp>
#include <kernel/vfs/vfs.hpp>
#include <kernel/memory/checked_ptr.hpp>
#include <kernel/memory/mempool.hpp>
#include <kernel/test/test_isolate.hpp>

namespace kernel {

namespace {

// NOLINTNEXTLINE(performance-no-int-to-ptr)
constexpr uint64_t kNeg(int64_t err) {
    return static_cast<uint64_t>(-err);
}

/// @brief User ABI timespec (matches src/libc/time.h layout).
struct AbiTimespec {
    int64_t tv_sec;
    int64_t tv_nsec;
};
static_assert(sizeof(AbiTimespec) == 16, "timespec width");

/// @brief User ABI itimerspec (POSIX order: interval + value, matches
/// src/libc/time.h struct itimerspec).
struct AbiItimerspec {
    AbiTimespec it_interval;
    AbiTimespec it_value;
};
static_assert(sizeof(AbiItimerspec) == 32, "itimerspec width");

/// @brief Timerfd vnode tag: packed (slot_gen << 32) | index in private_data.
/// No allocation — the tag travels by value, never dereferenced.
uint64_t timerfd_tag(const vfs::Vnode *node) {
    return reinterpret_cast<uint64_t>(node->private_data);
}

uint32_t timerfd_tag_index(uint64_t tag) {
    return static_cast<uint32_t>(tag & 0xFFFFFFFFULL);
}

uint32_t timerfd_tag_gen(uint64_t tag) {
    return static_cast<uint32_t>((tag >> 32) & 0xFFFFFFFFULL);
}

uint64_t make_timerfd_tag(uint32_t index, uint32_t gen) {
    return (static_cast<uint64_t>(gen) << 32) | static_cast<uint64_t>(index);
}

/// @brief timerfd vnode close: free the registry slot iff the closer owns
/// it (fork-inherited closes from non-owners drop only the vnode ref),
/// then release the vnode (pipe_read_close precedent).
void timerfd_close(vfs::Vnode &self) {
    const uint64_t tag = timerfd_tag(&self);
    const uint32_t index = timerfd_tag_index(tag);
    const uint32_t gen = timerfd_tag_gen(tag);
    auto *cur = Scheduler::current_task();
    if (cur != nullptr) {
        time::PosixTime::timerfd_slot_free(index, gen, cur->id,
                                           cur->generation);
    }
    self.private_data = nullptr;
    test::ResourceTracker::instance().track_vnode_remove();
    MemPool::free(&self);
}

int timerfd_fstat(vfs::Vnode &, vfs::VfsStat &vfs_stat) {
    vfs_stat.st_size = 0;
    vfs_stat.st_mode = vfs::S_IFCHR;
    return 0;
}

const vfs::VnodeOps timerfd_ops = {
    nullptr,       // read (served by the sys_read timerfd branch)
    nullptr,       // write
    nullptr,       // open
    timerfd_close, // close
    nullptr,       // lseek
    timerfd_fstat, // fstat
    nullptr,       // ioctl
    nullptr,       // readdir
    nullptr,       // lookup
    nullptr,       // mkdir
    nullptr,       // unlink
    nullptr,       // create
};

bool copy_timespec_in(const AbiTimespec *user_ptr, bool is_user,
                      time::TimespecU &out) {
    if (user_ptr == nullptr) {
        return false;
    }
    AbiTimespec local{};
    if (is_user) {
        auto checked_ptr = checked(user_ptr);
        if (!checked_ptr.valid()) {
            return false;
        }
        if (!safe_copy_from_user(&local, checked_ptr.unsafe_ptr(), 1)) {
            return false;
        }
    } else {
        local = *user_ptr;
    }
    out.sec = local.tv_sec;
    out.nsec = local.tv_nsec;
    return true;
}

bool copy_timespec_out(AbiTimespec *user_ptr, bool is_user,
                       const time::TimespecU &value) {
    if (user_ptr == nullptr) {
        return false;
    }
    AbiTimespec local{};
    local.tv_sec = value.sec;
    local.tv_nsec = value.nsec;
    if (is_user) {
        auto checked_ptr = checked(user_ptr);
        if (!checked_ptr.valid()) {
            return false;
        }
        if (!safe_copy_to_user(checked_ptr.unsafe_ptr(), &local, 1)) {
            return false;
        }
    } else {
        *user_ptr = local;
    }
    return true;
}

} // namespace

/// @brief True when a vnode is a timerfd (ops-pointer identity — never
/// parses private_data of foreign vnodes). External linkage for the
/// sys_read branch; timerfd_ops itself stays TU-local.
bool is_timerfd_vnode(const vfs::Vnode *node) {
    return node != nullptr && node->ops == &timerfd_ops;
}

uint64_t Syscall::sys_clock_gettime(uint64_t arg0, uint64_t arg1, uint64_t,
                                    uint64_t, uint64_t *) {
    const uint64_t clock_id = arg0;
    if (clock_id != static_cast<uint64_t>(time::PosixClock::REALTIME) &&
        clock_id != static_cast<uint64_t>(time::PosixClock::MONOTONIC)) {
        return kNeg(time::kPosixErrInvalid);
    }
    if (arg1 == 0) {
        return kNeg(time::kPosixErrFault);
    }
    uint64_t now_ns = 0;
    if (clock_id == static_cast<uint64_t>(time::PosixClock::MONOTONIC)) {
        now_ns = time::PosixTime::clock_monotonic_ns();
    } else {
        now_ns = time::PosixTime::clock_realtime_ns();
    }
    const time::TimespecU split = time::PosixTime::ns_to_timespec(now_ns);
    // NOLINTNEXTLINE(performance-no-int-to-ptr)
    auto out_ptr = checked(reinterpret_cast<AbiTimespec *>(arg1));
    const bool is_user = syscall_is_user_task();
    if (is_user && !out_ptr.valid()) {
        return kNeg(time::kPosixErrFault);
    }
    if (!copy_timespec_out(out_ptr.unsafe_ptr(), is_user, split)) {
        return kNeg(time::kPosixErrFault);
    }
    return 0;
}

uint64_t Syscall::sys_nanosleep(uint64_t arg0, uint64_t arg1, uint64_t,
                                uint64_t, uint64_t *) {
    auto *cur = syscall_task();
    if (cur == nullptr) {
        return static_cast<uint64_t>(-1);
    }
    const bool is_user = syscall_is_user_task();
    // NOLINTNEXTLINE(performance-no-int-to-ptr)
    const auto *req_ptr = reinterpret_cast<const AbiTimespec *>(arg0);
    time::TimespecU req{};
    if (!copy_timespec_in(req_ptr, is_user, req)) {
        return kNeg(time::kPosixErrFault);
    }
    uint64_t total_ns = 0;
    if (!time::PosixTime::timespec_to_ns(req, total_ns)) {
        return kNeg(time::kPosixErrInvalid);
    }
    // Zero timeout returns immediately: no arm, no block (§11.3 — no
    // infinite-wait encoding exists on this call).
    if (total_ns == 0) {
        return 0;
    }
    if (!time::PosixTime::sleep_arm(*cur, total_ns)) {
        return kNeg(time::kPosixErrAgain);
    }
    // §11.1/11.2 block (sys_receive shape): no locks held across the
    // reschedule; BLOCKED always leaves the ready queue.
    cur->state = TaskState::BLOCKED;
    Scheduler::dequeue_ready(*cur);
    Scheduler::reschedule();
    if (cur->is_user_) {
        arch::sti();
        arch::hlt();
        arch::cli();
    }
    // Resume: expiry beats signals; signals beat re-block. The wheel
    // guarantees the expiry fires, so this spin is bounded (§11.3).
    while (cur->state == TaskState::BLOCKED &&
           !__atomic_load_n(&cur->sleep_expired, __ATOMIC_ACQUIRE) &&
           __atomic_load_n(&cur->pending_signals, __ATOMIC_ACQUIRE) == 0) {
        arch::pause();
    }
    const bool expired =
        __atomic_load_n(&cur->sleep_expired, __ATOMIC_ACQUIRE);
    const bool signaled =
        __atomic_load_n(&cur->pending_signals, __ATOMIC_ACQUIRE) != 0;
    time::PosixTime::sleep_cancel(*cur);
    if (signaled && !expired) {
        // Report the remainder (saturating, never negative).
        if (arg1 != 0) {
            const uint64_t now_ns = time::PosixTime::clock_monotonic_ns();
            uint64_t remaining_ns = 0;
            if (cur->sleep_expiry_ns > now_ns) {
                remaining_ns = cur->sleep_expiry_ns - now_ns;
            }
            const time::TimespecU rem =
                time::PosixTime::ns_to_timespec(remaining_ns);
            // NOLINTNEXTLINE(performance-no-int-to-ptr)
            auto *rem_ptr = reinterpret_cast<AbiTimespec *>(arg1);
            if (!copy_timespec_out(rem_ptr, is_user, rem)) {
                return kNeg(time::kPosixErrFault);
            }
        }
        return kNeg(time::kPosixErrIntr);
    }
    return 0;
}

uint64_t Syscall::sys_timer_create(uint64_t arg0, uint64_t arg1, uint64_t arg2,
                                   uint64_t arg3, uint64_t *) {
    auto *cur = syscall_task();
    if (cur == nullptr) {
        return static_cast<uint64_t>(-1);
    }
    const bool is_user = syscall_is_user_task();
    const uint64_t op = arg0;
    if (op == static_cast<uint64_t>(time::PosixTimerOp::CREATE)) {
        uint64_t timer_id = 0;
        const int64_t created = time::PosixTime::timer_create(
            arg1, cur->id, cur->generation, timer_id);
        if (created != 0) {
            return kNeg(-created);
        }
        if (arg2 == 0) {
            time::PosixTime::timer_delete(timer_id, cur->id, cur->generation);
            return kNeg(time::kPosixErrFault);
        }
        // NOLINTNEXTLINE(performance-no-int-to-ptr)
        auto *out_ptr = reinterpret_cast<uint64_t *>(arg2);
        if (is_user) {
            auto checked_ptr = checked(out_ptr);
            if (!checked_ptr.valid()) {
                time::PosixTime::timer_delete(timer_id, cur->id,
                                             cur->generation);
                return kNeg(time::kPosixErrFault);
            }
            checked_ptr.write(timer_id, 0);
        } else {
            *out_ptr = timer_id;
        }
        return 0;
    }
    if (op == static_cast<uint64_t>(time::PosixTimerOp::SETTIME)) {
        const bool is_timerfd = (arg3 & 0x1ULL) != 0;
        // NOLINTNEXTLINE(performance-no-int-to-ptr)
        const auto *spec_ptr = reinterpret_cast<const AbiItimerspec *>(arg2);
        time::TimespecU value{};
        time::TimespecU interval{};
        if (spec_ptr == nullptr) {
            return kNeg(time::kPosixErrFault);
        }
        AbiItimerspec local{};
        if (is_user) {
            auto checked_ptr = checked(spec_ptr);
            if (!checked_ptr.valid()) {
                return kNeg(time::kPosixErrFault);
            }
            if (!safe_copy_from_user(&local, checked_ptr.unsafe_ptr(), 1)) {
                return kNeg(time::kPosixErrFault);
            }
        } else {
            local = *spec_ptr;
        }
        value.sec = local.it_value.tv_sec;
        value.nsec = local.it_value.tv_nsec;
        interval.sec = local.it_interval.tv_sec;
        interval.nsec = local.it_interval.tv_nsec;
        const time::ItimerspecU spec{value, interval};
        if (!is_timerfd) {
            const int64_t armed = time::PosixTime::timer_settime(
                arg1, spec, cur->id, cur->generation);
            if (armed != 0) {
                return kNeg(-armed);
            }
            return 0;
        }
        const int timerfd_fd = static_cast<int>(arg1);
        auto *desc = cur->fd_table.get(timerfd_fd);
        if (desc == nullptr || !is_timerfd_vnode(desc->vnode)) {
            return kNeg(time::kPosixErrBadFd);
        }
        const uint64_t tag = timerfd_tag(desc->vnode);
        const uint32_t resolved_index = timerfd_tag_index(tag);
        const uint32_t resolved_gen = timerfd_tag_gen(tag);
        const int64_t armed = time::PosixTime::timerfd_settime(
            resolved_index, resolved_gen, spec, cur->id, cur->generation);
        if (armed != 0) {
            return kNeg(-armed);
        }
        return 0;
    }
    if (op == static_cast<uint64_t>(time::PosixTimerOp::GETTIME)) {
        if (arg2 == 0) {
            return kNeg(time::kPosixErrFault);
        }
        time::ItimerspecU spec{};
        const int64_t read = time::PosixTime::timer_gettime(
            arg1, spec, cur->id, cur->generation);
        if (read != 0) {
            return kNeg(-read);
        }
        AbiItimerspec local{};
        local.it_value.tv_sec = spec.value.sec;
        local.it_value.tv_nsec = spec.value.nsec;
        local.it_interval.tv_sec = spec.interval.sec;
        local.it_interval.tv_nsec = spec.interval.nsec;
        // NOLINTNEXTLINE(performance-no-int-to-ptr)
        auto *out_ptr = reinterpret_cast<AbiItimerspec *>(arg2);
        if (is_user) {
            auto checked_ptr = checked(out_ptr);
            if (!checked_ptr.valid()) {
                return kNeg(time::kPosixErrFault);
            }
            if (!safe_copy_to_user(checked_ptr.unsafe_ptr(), &local, 1)) {
                return kNeg(time::kPosixErrFault);
            }
        } else {
            *out_ptr = local;
        }
        return 0;
    }
    if (op == static_cast<uint64_t>(time::PosixTimerOp::DELETE)) {
        const int64_t deleted = time::PosixTime::timer_delete(
            arg1, cur->id, cur->generation);
        if (deleted != 0) {
            return kNeg(-deleted);
        }
        return 0;
    }
    return kNeg(time::kPosixErrInvalid);
}

uint64_t Syscall::sys_timerfd_create(uint64_t arg0, uint64_t arg1, uint64_t,
                                     uint64_t, uint64_t *) {
    auto *cur = syscall_task();
    if (cur == nullptr) {
        return static_cast<uint64_t>(-1);
    }
    const uint64_t flags = arg1;
    const uint64_t known_flags = static_cast<uint64_t>(vfs::O_NONBLOCK);
    if ((flags & ~known_flags) != 0) {
        return kNeg(time::kPosixErrInvalid);
    }
    const int64_t allocated = time::PosixTime::timerfd_slot_alloc(
        arg0, cur->id, cur->generation);
    if (allocated < 0) {
        return kNeg(-allocated);
    }
    const uint32_t index = static_cast<uint32_t>(allocated);
    // Resolve the slot generation for the vnode tag (owner-validated;
    // the slot was just allocated to this task, so this cannot fail —
    // the check below is defense-in-depth, fail-closed).
    const uint32_t slot_gen = time::PosixTime::timerfd_slot_gen_for_owner(
        index, cur->id, cur->generation);
    if (slot_gen == 0) {
        time::PosixTime::timerfd_slot_free(index, 0, cur->id, cur->generation);
        return kNeg(time::kPosixErrInvalid);
    }
    auto *node =
        static_cast<vfs::Vnode *>(MemPool::alloc(sizeof(vfs::Vnode)));
    if (node == nullptr) {
        time::PosixTime::timerfd_slot_free(index, slot_gen, cur->id,
                                           cur->generation);
        return static_cast<uint64_t>(-1);
    }
    node->ops = &timerfd_ops;
    node->ino = static_cast<uint64_t>(index);
    node->size = 0;
    node->mode = vfs::S_IFCHR;
    node->private_data =
        reinterpret_cast<void *>(make_timerfd_tag(index, slot_gen));
    node->refcount = 1;
    node->parent = nullptr;
    test::ResourceTracker::instance().track_vnode_add();
    const int created_fd = syscall_task_open(node, flags);
    if (created_fd < 0) {
        node->private_data = nullptr;
        test::ResourceTracker::instance().track_vnode_remove();
        MemPool::free(node);
        time::PosixTime::timerfd_slot_free(index, slot_gen, cur->id,
                                           cur->generation);
        return kNeg(time::kPosixErrAgain);
    }
    return static_cast<uint64_t>(created_fd);
}

/// @brief Blocking timerfd read helper (sys_read branch target).
/// @return 0 + out count, -EAGAIN (empty/nonblock), -EBADF (stale),
/// -EINTR (signals), -EFAULT (bad buffer).
uint64_t timerfd_read_entry(vfs::Vnode &node, uint8_t *buffer, uint64_t count,
                            bool nonblock) {
    auto *cur = syscall_task();
    if (cur == nullptr) {
        return static_cast<uint64_t>(-1);
    }
    if (count < 8) {
        return kNeg(time::kPosixErrInvalid);
    }
    const uint64_t tag = timerfd_tag(&node);
    const uint32_t index = timerfd_tag_index(tag);
    const uint32_t slot_gen = timerfd_tag_gen(tag);
    const bool is_user = syscall_is_user_task();
    // Fast path: data already available — no wait, no waiter state.
    uint64_t expiries = 0;
    int64_t consumed = time::PosixTime::timerfd_consume(
        index, slot_gen, cur->id, cur->generation, expiries);
    if (consumed == 0) {
        if (is_user) {
            auto checked_ptr = checked(reinterpret_cast<uint64_t *>(buffer));
            if (!checked_ptr.valid()) {
                return kNeg(time::kPosixErrFault);
            }
            checked_ptr.write(expiries, 0);
        } else {
            if (buffer == nullptr) {
                return kNeg(time::kPosixErrFault);
            }
            // NOLINTNEXTLINE(performance-no-int-to-ptr)
            *reinterpret_cast<uint64_t *>(buffer) = expiries;
        }
        return 8;
    }
    if (consumed != -time::kPosixErrAgain) {
        // Stale/foreign slot: fail closed (EBADF), never block.
        return kNeg(-consumed);
    }
    if (nonblock) {
        return kNeg(time::kPosixErrAgain);
    }
    // Blocking wait (§11.1/11.2 shape): arm the waiter BEFORE the
    // re-check (no lost wakeup), release everything, then block.
    if (!time::PosixTime::timerfd_waiter_arm(index, slot_gen, *cur)) {
        return kNeg(time::kPosixErrBadFd);
    }
    consumed = time::PosixTime::timerfd_consume(
        index, slot_gen, cur->id, cur->generation, expiries);
    if (consumed == 0) {
        time::PosixTime::timerfd_waiter_clear(index, slot_gen, *cur);
        if (is_user) {
            auto checked_ptr = checked(reinterpret_cast<uint64_t *>(buffer));
            if (!checked_ptr.valid()) {
                return kNeg(time::kPosixErrFault);
            }
            checked_ptr.write(expiries, 0);
        } else {
            if (buffer == nullptr) {
                return kNeg(time::kPosixErrFault);
            }
            // NOLINTNEXTLINE(performance-no-int-to-ptr)
            *reinterpret_cast<uint64_t *>(buffer) = expiries;
        }
        return 8;
    }
    if (consumed != -time::kPosixErrAgain) {
        time::PosixTime::timerfd_waiter_clear(index, slot_gen, *cur);
        return kNeg(-consumed);
    }
    cur->state = TaskState::BLOCKED;
    Scheduler::dequeue_ready(*cur);
    Scheduler::reschedule();
    if (cur->is_user_) {
        arch::sti();
        arch::hlt();
        arch::cli();
    }
    // Bounded by the armed expiry or owner death (§11.3): the waiter is
    // always consumed by reapply_wakes or drain_owner.
    while (cur->state == TaskState::BLOCKED &&
           __atomic_load_n(&cur->pending_signals, __ATOMIC_ACQUIRE) == 0) {
        // Peek without clearing: the scan consumes waiter state on wake.
        uint64_t peek = 0;
        const int64_t peeked = time::PosixTime::timerfd_consume(
            index, slot_gen, cur->id, cur->generation, peek);
        if (peeked == 0) {
            // Data arrived but the wake has not been applied yet — unwind
            // the waiter and serve immediately (no double-consume: the
            // counter was already cleared by this consume).
            time::PosixTime::timerfd_waiter_clear(index, slot_gen, *cur);
            expiries = peek;
            if (is_user) {
                auto checked_ptr =
                    checked(reinterpret_cast<uint64_t *>(buffer));
                if (!checked_ptr.valid()) {
                    return kNeg(time::kPosixErrFault);
                }
                checked_ptr.write(expiries, 0);
            } else {
                if (buffer == nullptr) {
                    return kNeg(time::kPosixErrFault);
                }
                // NOLINTNEXTLINE(performance-no-int-to-ptr)
                *reinterpret_cast<uint64_t *>(buffer) = expiries;
            }
            return 8;
        }
        if (peeked != -time::kPosixErrAgain) {
            time::PosixTime::timerfd_waiter_clear(index, slot_gen, *cur);
            return kNeg(-peeked);
        }
        arch::pause();
    }
    const bool signaled =
        __atomic_load_n(&cur->pending_signals, __ATOMIC_ACQUIRE) != 0;
    time::PosixTime::timerfd_waiter_clear(index, slot_gen, *cur);
    if (signaled) {
        return kNeg(time::kPosixErrIntr);
    }
    consumed = time::PosixTime::timerfd_consume(
        index, slot_gen, cur->id, cur->generation, expiries);
    if (consumed != 0) {
        return kNeg(-consumed);
    }
    if (is_user) {
        auto checked_ptr = checked(reinterpret_cast<uint64_t *>(buffer));
        if (!checked_ptr.valid()) {
            return kNeg(time::kPosixErrFault);
        }
        checked_ptr.write(expiries, 0);
    } else {
        if (buffer == nullptr) {
            return kNeg(time::kPosixErrFault);
        }
        // NOLINTNEXTLINE(performance-no-int-to-ptr)
        *reinterpret_cast<uint64_t *>(buffer) = expiries;
    }
    return 8;
}

} // namespace kernel
