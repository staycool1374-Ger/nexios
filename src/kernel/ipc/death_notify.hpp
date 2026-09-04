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

/// @file death_notify.hpp
/// @brief Asynchronous task-death notification registry (issue #105 Part B).
/// Lets a user-space supervisor register a watch on a task so it is woken up
/// when that task dies (normal exit or crash) and can then drain the death
/// record, reclaim leaked capabilities, and restart the failed server.  Mirrors
/// the FrameUserMap/MmioUserMap static-bounded-registry pattern.

#pragma once

#include <types.hpp>
#include <kernel/sync/spinlock.hpp>
#include <constants.hpp>

namespace kernel {
struct TaskControlBlock;
}

namespace kernel::ipc {

/// @brief A single task-death record delivered to a supervisor.
struct DeathRecord {
    uint64_t dead_id = 0;  ///< id of the task that died
    uint64_t exit_code = 0; ///< exit code / signal delivered at death
    uint64_t flags = 0;    ///< DEATH_FLAG_* bitmask
};

/// @brief Death record flag: the death was an abnormal termination (a signal /
///        crash) rather than a clean exit.  Encoded from the high bit of the
///        task's exit code (negative exit codes carry the signal value).
constexpr uint64_t DEATH_FLAG_SIGNAL = 1ULL << 0;

/// @brief The wakeup value pulsed into a supervisor's Notify when one of its
///        watched tasks dies.  The authoritative data lives in the registry
///        (drained via SYS_DEATH_RECV); the pulse is a wakeup only, so a single
///        64-bit slot cannot lose a fan-in of multiple deaths.
constexpr uint64_t DEATH_WAKE_PULSE = 0xDEAD0001;

/// @brief Asynchronous task-death notification registry (issue #105 Part B).
/// A static bounded table pairing watched tasks with supervisors.  When a
/// watched task dies, its record is latched in the registry (exactly once, in
/// TaskControlBlock::cleanup()) and the supervisor's Notify is pulsed; the
/// supervisor drains via SYS_DEATH_RECV (never blocks).  No dynamic allocation
/// on RT paths.  Lock order: scheduler_lock_ -> death notify lock (never held
/// across a scheduler call or a Notify poke — supervisors are collected under
/// the lock and poked outside it).
class DeathNotify {
  public:
    /// @brief Registers a death watch.  Authority: the caller may register
    ///        (a) as the supervisor for @p watched_pid, or (b) as the watched
    ///        task designating @p supervisor_pid.  Fails closed when the
    ///        registry is full or the pair is invalid.
    /// @param actor         The calling task.
    /// @param watched_pid   Task to watch.
    /// @param supervisor_pid Task to notify on death; 0 = actor.
    /// @return true when the watch was installed.
    static bool watch(kernel::TaskControlBlock &actor, uint64_t watched_pid,
                      uint64_t supervisor_pid);

    /// @brief Removes every watch the supervisor holds on @p watched_pid
    ///        (ACTIVE or PENDING).
    static void unwatch(kernel::TaskControlBlock &supervisor,
                        uint64_t watched_pid);

    /// @brief Drains the next pending death record for @p supervisor (never
    ///        blocks).
    /// @param[out] out Receives the record on success.
    /// @return 1 when a record was copied, 0 when none is pending.
    static int recv(kernel::TaskControlBlock &supervisor, DeathRecord &out);

    /// @brief Latch the death records for @p dead and poke the supervisors.
    ///        Called exactly once per task death from cleanup().  Never blocks.
    static void on_task_death(kernel::TaskControlBlock &dead);

    /// @brief Frees every slot whose supervisor is @p supervisor (ACTIVE or
    ///        PENDING).  Called from cleanup() so a dying supervisor cannot
    ///        leave stale watches behind.
    static void drain_task(kernel::TaskControlBlock &supervisor);

    /// @brief Clears the whole registry (test isolation snapshot restore).
    static void snapshot_reset();

    /// @brief Number of live (ACTIVE + PENDING) slots (test accessor).
    static size_t live_count();

    /// @brief Whether @p supervisor currently holds a watch on @p watched_pid
    ///        (test accessor).
    static bool is_watching(kernel::TaskControlBlock &supervisor,
                            uint64_t watched_pid);

  private:
    static constexpr size_t kMaxWatches = CONFIG_CAP_MAX_DEATH_WATCHES;
    enum class SlotState : uint8_t { FREE = 0, ACTIVE, PENDING };
    struct Slot {
        SlotState state = SlotState::FREE;
        uint64_t watched_id = 0;     ///< watched task id
        uint32_t watched_gen = 0;    ///< TCB generation at watch time
        uint64_t supervisor_id = 0;  ///< supervisor task id
        uint32_t supervisor_gen = 0; ///< TCB generation at watch time
        DeathRecord record = {};     ///< latched record (PENDING only)
    };
    static Slot s_slots_[kMaxWatches];
    static sync::SpinLock s_lock_;
};

} // namespace kernel::ipc