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

/// @file timer_wheel.hpp
/// @brief Per-CPU O(1) event-timer wheel (issue #17, v0.4.7).
///
/// Static slot-table queue powering bounded sleeps, driver timeouts,
/// deadline release queues, and watchdog pre-timeouts. Pure callee of
/// the system tick: never programs timers, never reads the tick source
/// (the caller passes now_ns), never blocks or reschedules. Callbacks
/// run in tick context and must only set flags / mark wake objects —
/// deferred wake application belongs to task context (#18 producers).

#pragma once

#include <types.hpp>
#include <kernel/nexios_config.h>
#include <kernel/sync/spinlock.hpp>

namespace kernel {
namespace time {

/// @brief Per-CPU event-timer wheel with generation-tagged handles.
class TimerWheel {
  public:
    /// @brief Timer slots per CPU (static bound; scan is O(1)-amortized).
    static constexpr uint32_t kMaxTimersPerCpu = 64;
    /// @brief Max expiries fired per on_tick call (RT bound, TlbShootdown
    /// precedent). Overflow stays armed and fires on subsequent ticks.
    static constexpr uint32_t kMaxExpirePerTick = 8;
    /// @brief Free-list terminator / invalid slot marker.
    static constexpr uint32_t kInvalidSlot = 0xFFFFFFFFu;
    /// @brief Generation value that is never issued (fail-closed stale).
    static constexpr uint32_t kInvalidGeneration = 0u;
    /// @brief Expiry callback: runs in tick context, must be non-blocking
    /// and must not reschedule, block, or allocate.
    using ExpireCallback = void (*)(void*);

    /// @brief Opaque arm receipt; validate generation on every use.
    struct Handle {
        uint8_t cpu;
        uint8_t slot;
        uint32_t generation;
    };
    static_assert(CONFIG_MAX_CPUS <= 255, "Handle.cpu width");
    static_assert(kMaxTimersPerCpu <= 256, "Handle.slot width");

    /// @brief Arm a one-shot expiry (O(1)).
    /// Fail-closed: false on invalid cpu, null callback/context-out,
    /// or a full wheel (caller keeps its fallback; no eviction).
    /// @param cpu Owning CPU index; must be 0 (BSP). Non-BSP CPUs are
    /// rejected: production never services their wheels (AP dispatch-only).
    /// @param expiry_ns Absolute expiry in monotonic ns (#16 clock).
    /// @param callback Expiry callback (non-null).
    /// @param context Opaque callback context (carries owner identity).
    /// @param out Receipt handle (non-null).
    /// @return true when armed.
    static bool arm(uint32_t cpu, uint64_t expiry_ns,
                    ExpireCallback callback, void* context,
                    Handle* out) noexcept;
    /// @brief Cancel an armed expiry (O(1)).
    /// Stale handles (wrong generation, disarmed slot) fail closed.
    /// @param handle Receipt from arm().
    /// @return true when an armed entry was cancelled.
    static bool cancel(const Handle& handle) noexcept;
    /// @brief Tick hook: fire due expiries for one CPU (bounded).
    /// ISR-safe: try-lock, skip on contention (fires next tick instead).
    /// Fires at most kMaxExpirePerTick earliest-due callbacks; the lock
    /// is released before callbacks run (re-entrant arm/cancel safe).
    /// @param now_ns Current monotonic ns (caller-provided, #16 clock).
    /// @param cpu CPU index to service.
    /// @return Number of callbacks fired.
    static uint32_t on_tick(uint64_t now_ns, uint32_t cpu) noexcept;
    /// @brief Armed entry count for one CPU (introspection/test only).
    /// @param cpu CPU index.
    /// @return Live count (0 on invalid cpu).
    static uint32_t live_count(uint32_t cpu) noexcept;
    /// @brief Disarm everything, invalidate all handles (test isolation).
    /// Bumps every generation so pre-reset handles fail closed.
    static void snapshot_reset() noexcept;

  private:
    struct Slot {
        bool armed;
        uint32_t generation;
        uint64_t expiry_ns;
        ExpireCallback callback;
        void* context;
        uint32_t next_free;
    };
    struct CpuWheel {
        Slot slots[kMaxTimersPerCpu];
        uint32_t free_head;
        uint32_t live_count;
    };

    /// @brief One-time free-list build (call with lock held).
    static void ensure_init_locked() noexcept;

    static sync::SpinLock lock_;
    static CpuWheel wheels_[CONFIG_MAX_CPUS];
    static bool initialized_;
};

} // namespace time
} // namespace kernel
