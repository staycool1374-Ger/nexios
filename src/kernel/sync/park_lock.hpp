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

/// @file park_lock.hpp
/// @brief Bounded spin → timed park acquisition (M2, issues #545/#546).
/// Try-first, then a bounded pause-spin, then a timed park (BLOCKED +
/// dequeue + reschedule + sti/hlt, self-wake on the sleep-wheel timeout),
/// then retry; pure spin only when parking is denied or exhausted.
/// Wake-only: no waiter registry, no unlock-side hooks — the existing
/// level-triggered sleep-expiry scan is the sole waker (INV-L3).

#include <types.hpp>
#include <kernel/nexios_config.h>

namespace kernel {
namespace sync {

class SpinLock;

/// @brief Why the last park was denied (diagnostic, sticky until reset).
enum class ParkDenyReason : uint8_t {
    None = 0,     ///< Last acquisition parked (or never attempted).
    Disabled,     ///< CONFIG_LOCK_PARK_ENABLE == 0.
    IsrContext,   ///< isr_nesting_own() != 0 — ISR paths keep pure spin.
    IrqGuarded,   ///< An IrqGuard scope is live — sti/hlt would break it.
    LockHeld,     ///< Caller already holds a spinlock (nesting ⇒ no park).
    NotTask,      ///< No current task (early boot / idle-adjacent path).
    IdleTask,     ///< Idle task never parks (would strand the hart).
    NotRunning,   ///< Current task is not RUNNING (already blocked).
    NoTicks,      ///< CONFIG_TICK_HZ == 0 — a sleep arm could never fire.
    ArmFailed,    ///< PosixTime::sleep_arm failed (wheel full / non-BSP).
    ForeignGuard, ///< Depth owned by a foreign context (diagnostic only:
                  ///< never selects the pure-spin branch when
                  ///< CONFIG_LOCK_PARK_FOREIGN_OWNER_ALLOW == 1).
};

/// @brief INV-L1 park predicate (side-effect free): true iff the calling
///        task context may park on @p lock.  Evaluated before arming.
bool lock_park_allowed(const SpinLock &lock) noexcept;

/// @brief RAII timed-park acquisition.  Fast path is a single try_lock;
///        slow paths park and retry, then re-arm the park (never an
///        unbounded IF=0 spin — that starves the tick that would dispatch
///        a live holder, #545 wedge hunt).
///        INV-L1': every exit path returns with the caller's IRQ-enable
///        state restored.  INV-W1': no unbounded wait runs with interrupts
///        disabled while a live holder exists — the park (timer alive) is
///        the only unbounded wait.
class [[nodiscard]] ParkLockGuard {
  public:
    explicit ParkLockGuard(SpinLock &lock,
                           const char *site = "park") noexcept;
    ~ParkLockGuard() noexcept;

    ParkLockGuard(const ParkLockGuard &) = delete;
    ParkLockGuard &operator=(const ParkLockGuard &) = delete;
    ParkLockGuard(ParkLockGuard &&) = delete;
    ParkLockGuard &operator=(ParkLockGuard &&) = delete;

    /// @brief True when the lock is held (destructor releases it).
    bool owned() const noexcept { return owned_; }

    /// @brief Release the lock early without destroying the guard
    ///        (F1 #545: lets block_sender make BLOCKED-marking and unlock
    ///        one IF=0-atomic unit under a caller-owned IrqGuard — a
    ///        preempted holder can no longer strand the queue lock while
    ///        BLOCKED).  No-op when not owned.  Does not touch the
    ///        entry-IF bookkeeping (no park happened).
    void release() noexcept;

  private:
    SpinLock &lock_;
    bool owned_ = false;
    /// @brief IRQ-enable state at entry (INV-L1'): all exit paths and
    ///        every park-window boundary restore this.
    bool entry_if_ = true;
};

/// @brief Park entries (diagnostic counters, QE assertions).
uint64_t lock_park_count() noexcept;
/// @brief Pure-spin fallbacks: denied parks + exhausted attempts.
uint64_t lock_spin_fallback_count() noexcept;
/// @brief Attempts that ran out of park cycles (then pure-spun).
uint64_t lock_park_exhausted_count() noexcept;
/// @brief Last denial code (ParkDenyReason), sticky until reset.
uint64_t lock_park_denied_reason() noexcept;
/// @brief M2 follow-up (#545 FIRE): bounded denied-spin block escapes
///        (each escape re-arms the bounded block and re-diagnoses).
uint64_t lock_denied_escape_count() noexcept;
/// @brief M2 follow-up (#545 FIRE): park acquisitions admitted on a
///        foreign-owned IrqGuard depth (INV-L4).
uint64_t lock_foreign_depth_count() noexcept;
/// @brief Wedge hunt (#545): park re-arm iterations taken by the exhausted
///        fallback (each re-arm keeps the timer alive; the tick watchdog
///        FIRE line reports this so the amplifier is visible in-row).
uint64_t lock_park_rearm_count() noexcept;
/// @brief Wedge hunt (#545): park attempts that observed an IRQ state
///        different from the guard-entry state (INV-L1' repairs, DEBUG).
uint64_t lock_park_if_clobber_count() noexcept;
/// @brief Test-isolation reset (counters + denial code).
void park_lock_reset_for_test() noexcept;

} // namespace sync
} // namespace kernel
