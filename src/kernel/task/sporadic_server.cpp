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

/// @file sporadic_server.cpp
/// @brief Implementation of the Sporadic Server replenishment mechanism.

#include <kernel/task/sporadic_server.hpp>
#include <kernel/memory/mempool.hpp>
#include <kernel/task/scheduler.hpp>
#include <assert.hpp>

namespace kernel {
namespace task {

static constexpr uint64_t SPRIO_INVALID = ~0ULL;

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
void SporadicServer::init(uint64_t budget_c, uint64_t period_t,
                          uint64_t bg_prio,
                          uint64_t budget_granularity) noexcept {
    budget_c_ = budget_c;
    period_t_ = period_t;
    base_priority_ = SPRIO_INVALID; // caller should set externally
    bg_priority_ = bg_prio;
    budget_granularity_ = (budget_granularity > 0) ? budget_granularity : 1;
    budget_remaining_ = budget_c;
    consumed_since_active_ = 0;
    activation_time_ = 0;
    consume_counter_ = 0;
    state_ = IDLE;
    replenishment_head_ = 0;
    replenishment_tail_ = 0;
    replenishment_count_ = 0;
    coalesce_count_ = 0;
}

void SporadicServer::on_activation(uint64_t now) noexcept {
    if (state_ != IDLE)
        return;

    // Server was idle — a new aperiodic job has arrived.
    state_ = ACTIVE;
    activation_time_ = now;
    consumed_since_active_ = 0;

    // budget_remaining_ may be > 0 if prior replenishments have already
    // restored some budget; we do NOT reset it here because that would
    // violate the Sporadic Server budget accounting.  The server simply
    // runs with whatever budget it currently has, and the replenishment
    // at activation_time + T will restore whatever it consumes now.
}

void SporadicServer::on_completion(uint64_t now) noexcept {
    (void)now;
    if (state_ != ACTIVE)
        return;

    // Server goes idle — schedule a replenishment for what was consumed.
    if (consumed_since_active_ > 0) {
        schedule_replenishment(activation_time_ + period_t_,
                               consumed_since_active_);
    }

    // If the server completed with remaining budget, that budget is
    // preserved for the next activation (it does NOT roll over in the
    // standard Sporadic Server model — the replenishment event will
    // restore only the consumed amount, not the full C).
    //
    // However, once we go idle, the next activation will create a new
    // replenishment event, so unused budget is effectively consumed
    // first and the replenishment covers any deficit.
    state_ = IDLE;
    consumed_since_active_ = 0;
}

bool SporadicServer::consume(uint64_t now) noexcept {
    (void)now;
    if (state_ == IDLE)
        return false;

    if (budget_remaining_ == 0) {
        // Budget already exhausted; should not normally be called.
        state_ = EXHAUSTED;
        return false;
    }

    // Budget granularity: only consume every Nth tick.
    if (budget_granularity_ > 1) {
        consume_counter_++;
        if ((consume_counter_ % budget_granularity_) != 0)
            return true;
    }

    budget_remaining_--;
    consumed_since_active_++;

    if (budget_remaining_ == 0) {
        // Budget exhausted — schedule replenishment and drop to background.
        if (consumed_since_active_ > 0) {
            schedule_replenishment(activation_time_ + period_t_,
                                   consumed_since_active_);
        }
        state_ = EXHAUSTED;
#if CONFIG_SPORADIC_SERVER_DEADLINE_HOOK
        sporadic_server_deadline_handler(this, 0);
#endif
        return false;
    }

    return true;
}

void SporadicServer::process_replenishments(uint64_t now) noexcept {
    while (replenishment_count_ > 0) {
        Replenishment r = replenishments_[replenishment_head_];
        if (r.time > now)
            break;

        pop_replenishment();

        // Restore budget, capped at C (can't exceed max budget).
        uint64_t new_budget = budget_remaining_ + r.amount;
        if (new_budget > budget_c_)
            new_budget = budget_c_;
        budget_remaining_ = new_budget;

        // If budget is now positive and the server was EXHAUSTED, return to
        // normal priority (the caller checks current_priority()).
        if (new_budget > 0 && state_ == EXHAUSTED) {
            // M-8 (audit-task-sync): only an EXHAUSTED server may be forced
            // ACTIVE by a replenishment.  An IDLE server must stay IDLE — a
            // forced ACTIVE blocks the next on_activation() (which requires
            // IDLE), delaying the next aperiodic job.
            state_ = ACTIVE;
#if CONFIG_SPORADIC_SERVER_DEADLINE_HOOK
            sporadic_server_deadline_handler(this, 1);
#endif
        }
    }
}

// ---- Private helpers ----

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
void SporadicServer::schedule_replenishment(uint64_t time,
                                            uint64_t amount) noexcept {
    if (replenishment_count_ == MAX_REPLENISHMENTS) {
        // C-3 (audit-task-sync-v0.4.2): ring full — do NOT silently drop the
        // consumed budget.  Merge the amount into the newest pending entry,
        // deferring its restore to the later of the two times (conservative
        // late restore).  The consumed budget is thus preserved, at worst
        // restored one period late instead of being lost permanently.
        Replenishment &newest =
            replenishments_[(replenishment_tail_ + MAX_REPLENISHMENTS - 1) %
                            MAX_REPLENISHMENTS];
        newest.amount += amount;
        if (newest.amount > budget_c_)
            newest.amount = budget_c_;
        if (time > newest.time)
            newest.time = time;
        ++coalesce_count_;
        return;
    }

    replenishments_[replenishment_tail_].time = time;
    replenishments_[replenishment_tail_].amount = amount;

    replenishment_tail_ = (replenishment_tail_ + 1) % MAX_REPLENISHMENTS;
    replenishment_count_++;
}

SporadicServer::Replenishment SporadicServer::pop_replenishment() noexcept {
    Replenishment r = replenishments_[replenishment_head_];
    replenishment_head_ = (replenishment_head_ + 1) % MAX_REPLENISHMENTS;
    replenishment_count_--;
    return r;
}

#if CONFIG_SPORADIC_SERVER_DEADLINE_HOOK
__attribute__((weak)) void
sporadic_server_deadline_handler(SporadicServer * /*ss*/,
                                 uint64_t /*reason*/) noexcept {
    // default no-op — user code may override
}
#endif

/// @brief Final teardown on the last release (KernelObject::dispose).
///        Moves the sporadic-task-count decrement here from the TCB teardown
///        hook so a future shared SporadicServer fires the accounting on the
///        correct last-release event.  Stack-allocated test instances are not
///        pool-backed and are never freed.
void SporadicServer::dispose() noexcept {
    if (!is_pool_backed()) {
#if defined(CONFIG_DEBUG)
        ENSURE(false && "dispose() on a non-pool-backed SporadicServer");
#endif
        return;
    }
    Scheduler::dec_sporadic_count();
    MemPool::free(this);
}

} // namespace task
} // namespace kernel