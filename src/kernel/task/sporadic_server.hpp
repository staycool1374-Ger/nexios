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

/// @file sporadic_server.hpp
/// @brief Sporadic Server budget management for aperiodic daemons (vfsd, iocd).
/// Implements POSIX.1b Sporadic Server replenishment (IEEE Std 1003.1-2008)
/// with O(1) tick-driven replenishment processing and a bounded circular queue.

#pragma once

#include <types.hpp>
#include <kernel/nexios_config.h>
#include <kernel/memory/refcounted.hpp>

namespace kernel {
namespace task {

/// @brief Per-instance Sporadic Server controller.
///
/// Tracks execution budget C and replenishment period T for a single
/// aperiodic server thread (e.g., vfsd, iocd).  The caller invokes:
///   - on_activation() when an aperiodic event arrives while idle,
///   - consume() on every timer tick the server executes,
///   - on_completion() when the server goes idle before budget exhaustion,
///   - process_replenishments() once per timer tick.
///
/// All times are in integer ticks.  No libc, no FP, no dynamic allocation.
class SporadicServer : public RefCounted {
  public:
    /// @brief Maximum number of simultaneously pending replenishments.
    /// Sufficient for all practical cases: at most one replenishment per
    /// period, and the queue drains in T ticks.
    static constexpr uint64_t MAX_REPLENISHMENTS = 8;

    /// @brief A single scheduled replenishment event.
    struct Replenishment {
        uint64_t time;   ///< System tick when this replenishment fires.
        uint64_t amount; ///< Budget (ticks) to restore.
    };

    /// @brief Server execution state.
    enum State : uint8_t {
        IDLE = 0,      ///< No pending work; budget is fully available.
        ACTIVE = 1,    ///< Currently executing aperiodic work.
        EXHAUSTED = 2, ///< Budget hit zero; server runs at background priority.
    };

    /// @brief Initialises the server parameters.
    /// @param budget_c           Execution budget C (in ticks).
    /// @param period_t           Replenishment period T (in ticks, must be >
    /// 0).
    /// @param bg_prio            Priority level when budget is exhausted (lower
    /// = less urgent).
    /// @param budget_granularity Ticks per budget unit (default:
    /// CONFIG_SPORADIC_SERVER_BUDGET_GRANULARITY).
    void init(uint64_t budget_c, uint64_t period_t, uint64_t bg_prio,
              uint64_t budget_granularity =
                  CONFIG_SPORADIC_SERVER_BUDGET_GRANULARITY) noexcept;

    // ---- Event interface ----

    /// @brief Signals that a new aperiodic job has arrived.
    /// @param now  Current system tick.
    /// Records activation_time if the server was idle.  No effect if already
    /// active or exhausted (the server is already handling the current job).
    void on_activation(uint64_t now) noexcept;

    /// @brief Signals that the server goes idle (all aperiodic work done).
    /// @param now  Current system tick.
    /// Schedules a replenishment for budget consumed since the last activation.
    /// No effect if the server was idle.
    void on_completion(uint64_t now) noexcept;

    // ---- Tick-driven interface ----

    /// @brief Consumes one tick of execution budget.
    /// @param now  Current system tick.
    /// @return true if budget remains > 0; false if exhausted (caller should
    ///         demote priority).
    bool consume(uint64_t now) noexcept;

    /// @brief Processes all due replenishments at the given tick.
    /// @param now  Current system tick.
    /// Restores budget for every Replenishment whose time <= now and
    /// transitions the server back to ACTIVE if budget becomes positive.
    void process_replenishments(uint64_t now) noexcept;

    // ---- Queries ----

    /// @return true if the server has any remaining budget (> 0).
    bool has_budget() const noexcept {
        return budget_remaining_ > 0;
    }

    /// @return Current effective priority (background if exhausted, normal
    /// otherwise).
    uint64_t current_priority() const noexcept {
        return (state_ == EXHAUSTED) ? bg_priority_ : base_priority_;
    }

    /// @return Remaining execution budget in ticks.
    uint64_t remaining_budget() const noexcept {
        return budget_remaining_;
    }

    /// @return C (maximum budget per period).
    uint64_t max_budget() const noexcept {
        return budget_c_;
    }

    /// @return T (replenishment period).
    uint64_t period() const noexcept {
        return period_t_;
    }

    /// @return Current server state.
    State state() const noexcept {
        return state_;
    }

    /// @return true if server is currently executing aperiodic work.
    bool is_active() const noexcept {
        return state_ == ACTIVE;
    }

    /// @return Number of pending replenishment events.
    uint64_t pending_count() const noexcept {
        return replenishment_count_;
    }

    /// @brief Sets the normal scheduling priority for this server.
    /// Call this after init() to assign the server's base priority level.
    void set_base_priority(uint64_t prio) noexcept {
        base_priority_ = prio;
    }

    /// @brief Returns the configured background priority.
    uint64_t bg_priority() const noexcept {
        return bg_priority_;
    }

  private:
    // ---- Configuration ----
    uint64_t budget_c_;           /// C  (maximum budget per period, ticks)
    uint64_t period_t_;           /// T  (replenishment period, ticks)
    uint64_t base_priority_;      ///  Normal scheduling priority.
    uint64_t bg_priority_;        ///  Background priority when exhausted.
    uint64_t budget_granularity_; ///  Ticks per budget unit.

    // ---- Dynamic state ----
    uint64_t budget_remaining_;      ///< Remaining budget for current period.
    uint64_t consumed_since_active_; ///< Budget consumed since last activation.
    uint64_t activation_time_; ///< Tick of most recent idle->active transition.
    uint64_t consume_counter_; ///< Tick counter for granularity skip.
    State state_;

    // ---- Replenishment queue (circular buffer) ----
    Replenishment replenishments_[MAX_REPLENISHMENTS];
    uint64_t replenishment_head_;  ///< Dequeue from head.
    uint64_t replenishment_tail_;  ///< Enqueue at tail.
    uint64_t replenishment_count_; ///< Number of valid entries.

    /// @brief Inserts a replenishment event into the queue.
    /// @param time   Tick at which to replenish.
    /// @param amount Budget amount to restore.
    /// @return true if inserted, false if queue full.
    bool schedule_replenishment(uint64_t time, uint64_t amount) noexcept;

    /// @brief Removes and returns the earliest replenishment.
    /// @return The head Replenishment.  Undefined if queue is empty.
    Replenishment pop_replenishment() noexcept;
};

static_assert(sizeof(SporadicServer::Replenishment) <= 16,
              "SporadicServer::Replenishment must fit in 16 bytes");

#if CONFIG_SPORADIC_SERVER_DEADLINE_HOOK
/// @brief Weak callback invoked on a SporadicServer deadline event.
/// @param ss     The server that triggered the event.
/// @param reason 0 = budget exhausted in consume(); 1 = replenishment after
///               exhaustion in process_replenishments().
/// The default implementation is a no-op. Override to implement deadline-miss
/// logging, task demotion, or panic. Called from ISR context — must not block.
__attribute__((weak)) void
sporadic_server_deadline_handler(SporadicServer *ss, uint64_t reason) noexcept;
#endif

} // namespace task
} // namespace kernel