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

/// @file posix_time.hpp
/// @brief POSIX time/timer surface over the HRT clock + event-timer wheel
/// (issue #76, v0.5.0). CLOCK_MONOTONIC/CLOCK_REALTIME, wheel-armed bounded
/// nanosleep, one-shot/periodic posix timers, and timerfd expiry counters.
///
/// Locking: one leaf IrqSpinLockGuard-ordered SpinLock. Wheel callbacks run
/// with the wheel lock released and may run under the outer scheduler_lock_
/// (on_tick path), so the registry lock is scheduler_lock_->posix_lock
/// ordered: task-context paths must NEVER acquire scheduler services while
/// holding it (§11.1 — release before BLOCKED + dequeue + reschedule).

#pragma once

#include <types.hpp>
#include <kernel/nexios_config.h>
#include <kernel/sync/spinlock.hpp>
#include <kernel/task/task.hpp>
#include <kernel/time/timer_wheel.hpp>

namespace kernel {
namespace time {

/// @brief POSIX clock IDs (Linux values).
enum class PosixClock : uint64_t {
    REALTIME = 0,
    MONOTONIC = 1,
};

/// @brief Kernel-local timespec (sign-extended fields, range-checked).
struct TimespecU {
    int64_t sec;
    int64_t nsec;
};

/// @brief Kernel-local itimerspec (value + interval).
struct ItimerspecU {
    TimespecU value;
    TimespecU interval;
};

/// @brief TIMER_CREATE multiplex operations.
enum class PosixTimerOp : uint64_t {
    CREATE = 0,
    SETTIME = 1,
    GETTIME = 2,
    DELETE = 3,
};

/// @brief Reachable errno values (Linux numbers, mirrored in src/libc/errno.h).
constexpr int64_t kPosixErrInvalid = 22;   // EINVAL
constexpr int64_t kPosixErrFault = 14;     // EFAULT
constexpr int64_t kPosixErrAgain = 11;     // EAGAIN
constexpr int64_t kPosixErrIntr = 4;       // EINTR
constexpr int64_t kPosixErrBadFd = 9;      // EBADF
constexpr int64_t kPosixErrNoSys = 38;     // ENOSYS

/// @brief POSIX time/timer registries + clock sources (issue #76).
class PosixTime {
  public:
    /// @brief Static timer slots (bounded; table-full fails closed).
    static constexpr uint32_t kMaxPosixTimers = 32;
    /// @brief Static timerfd slots (bounded; table-full fails closed).
    static constexpr uint32_t kMaxTimerfds = 32;
    /// @brief Timer ID layout: (generation << 8) | index.
    static constexpr uint32_t kTimerIdIndexBits = 8;

    /// @brief Saturating 64-bit add (absurd values clamp, never wrap).
    static uint64_t sat_add(uint64_t left, uint64_t right) noexcept;
    /// @brief Timespec to ns with range check (tv_nsec in [0, 1e9)).
    /// @return True + out ns on success; false on range violation.
    static bool timespec_to_ns(const TimespecU &spec,
                               uint64_t &out_ns) noexcept;
    /// @brief Ns to saturating timespec split.
    static TimespecU ns_to_timespec(uint64_t total_ns) noexcept;

    /// @brief Latch the REALTIME boot anchor (idempotent, first wins).
    /// @param epoch_sec Wall seconds at latch time (RTC read).
    static void boot_latch(uint64_t epoch_sec) noexcept;
    /// @brief MONOTONIC ns (HRT clock, never RTC-dependent after boot).
    static uint64_t clock_monotonic_ns() noexcept;
    /// @brief REALTIME ns (anchor + monotonic delta, saturating).
    static uint64_t clock_realtime_ns() noexcept;
    /// @brief Test hook: reset the anchor latch (test isolation only).
    static void anchor_reset_for_test() noexcept;

    /// @brief Create a timer slot owned by (owner_id, owner_gen).
    /// @return 0 + out timer id, or negative -errno (EINVAL/AGAIN).
    static int64_t timer_create(uint64_t clock_id, uint64_t owner_id,
                                uint32_t owner_gen, uint64_t &out_id) noexcept;
    /// @brief Arm/disarm/re-arm a timer (zero value = disarm).
    /// @return 0 or negative -errno (EINVAL/AGAIN on wheel-full).
    static int64_t timer_settime(uint64_t timer_id, const ItimerspecU &spec,
                                 uint64_t owner_id,
                                 uint32_t owner_gen) noexcept;
    /// @brief Read remaining + period of a timer.
    /// @return 0 or negative -errno (EINVAL on stale/foreign id).
    static int64_t timer_gettime(uint64_t timer_id, ItimerspecU &out_spec,
                                 uint64_t owner_id,
                                 uint32_t owner_gen) noexcept;
    /// @brief Delete a timer (cancel arm, free slot, track_remove).
    /// @return 0 or negative -errno (EINVAL on stale/foreign id).
    static int64_t timer_delete(uint64_t timer_id, uint64_t owner_id,
                                uint32_t owner_gen) noexcept;

    /// @brief Allocate a timerfd slot owned by (owner_id, owner_gen).
    /// @return Slot index >= 0, or negative -errno (EINVAL/AGAIN).
    static int64_t timerfd_slot_alloc(uint64_t clock_id, uint64_t owner_id,
                                      uint32_t owner_gen) noexcept;
    /// @brief Owner-validated generation peek for vnode tagging.
    /// @return Live generation, or 0 when slot unused/foreign.
    static uint32_t timerfd_slot_gen_for_owner(uint32_t index,
                                               uint64_t owner_id,
                                               uint32_t owner_gen) noexcept;
    /// @brief Arm/disarm a timerfd slot (zero value = disarm).
    /// @return 0 or negative -errno.
    static int64_t timerfd_settime(uint32_t index, uint32_t slot_gen,
                                   const ItimerspecU &spec, uint64_t owner_id,
                                   uint32_t owner_gen) noexcept;
    /// @brief Read-and-clear the expiry counter.
    /// @return 0 + out count, -EAGAIN when empty, -EBADF when stale.
    static int64_t timerfd_consume(uint32_t index, uint32_t slot_gen,
                                   uint64_t owner_id, uint32_t owner_gen,
                                   uint64_t &out_count) noexcept;
    /// @brief Register a BLOCKED waiter on a timerfd slot.
    /// @return True when armed; false when slot stale (caller fails EBADF).
    static bool timerfd_waiter_arm(uint32_t index, uint32_t slot_gen,
                                   TaskControlBlock &task) noexcept;
    /// @brief Clear the waiter registration (wake observed or cancelled).
    static void timerfd_waiter_clear(uint32_t index, uint32_t slot_gen,
                                     const TaskControlBlock &task) noexcept;
    /// @brief Free a timerfd slot (close path; wakes any waiter with EBADF).
    static void timerfd_slot_free(uint32_t index, uint32_t slot_gen,
                                  uint64_t owner_id,
                                  uint32_t owner_gen) noexcept;

    /// @brief Arm a bounded nanosleep on a TCB (caller holds no locks).
    /// @return True when armed (or zero budget, which never arms).
    static bool sleep_arm(TaskControlBlock &task,
                          uint64_t budget_ns) noexcept;
    /// @brief Cancel a nanosleep arm (idempotent, fail-closed stale).
    static void sleep_cancel(TaskControlBlock &task) noexcept;

    /// @brief Death-drain: disarm sleep + free owned timers/timerfds +
    /// clear waiter registrations of a dying task (cleanup path, §12.3).
    static void drain_owner(TaskControlBlock &task) noexcept;

    /// @brief Scheduler re-apply: wake sleep-expired + timerfd-ready
    /// BLOCKED tasks (level-triggered; call under scheduler_lock_).
    /// @param sched_cpu Servicing CPU index.
    static void reapply_wakes(uint32_t sched_cpu) noexcept;

    /// @brief Wheel callback: nanosleep expiry (flag-store only).
    static void sleep_fire(void *context) noexcept;
    /// @brief Wheel callback: posix timer expiry (+ periodic re-arm).
    static void timer_fire(void *context) noexcept;
    /// @brief Wheel callback: timerfd expiry (count + waiter flag).
    static void timerfd_fire(void *context) noexcept;

    /// @brief Disarm everything, invalidate handles, zero counters
    /// (test isolation; bumps all generations fail-closed).
    static void snapshot_reset() noexcept;

  private:
    struct TimerSlot {
        bool used;
        uint32_t gen;
        uint64_t owner_id;
        uint32_t owner_gen;
        bool armed;
        bool periodic;
        uint64_t period_ns;
        uint64_t expiry_ns;
        uint64_t expiries;
        time::TimerWheel::Handle wheel_h;
    };
    struct TimerFdSlot {
        bool used;
        uint32_t gen;
        uint64_t owner_id;
        uint32_t owner_gen;
        bool armed;
        bool periodic;
        uint64_t period_ns;
        uint64_t expiry_ns;
        uint64_t expiries;
        time::TimerWheel::Handle wheel_h;
        TaskControlBlock *waiter;
        uint32_t waiter_gen;
        bool waiter_armed;
        bool waiter_done;
        bool waiter_dead;
    };

    static bool decode_timer_id(uint64_t timer_id, uint32_t &out_index,
                                uint32_t &out_gen) noexcept;
    static uint64_t encode_timer_id(uint32_t index, uint32_t gen) noexcept;
    static uint32_t bump_gen(uint32_t old_gen) noexcept;
    static void free_timer_slot_locked(uint32_t index) noexcept;
    static void free_timerfd_slot_locked(uint32_t index) noexcept;

    static sync::SpinLock lock_;
    static TimerSlot timers_[kMaxPosixTimers];
    static TimerFdSlot timerfds_[kMaxTimerfds];
    static bool anchor_latched_;
    static uint64_t anchor_ns_;
    static uint64_t mono_at_anchor_ns_;
};

} // namespace time
} // namespace kernel
