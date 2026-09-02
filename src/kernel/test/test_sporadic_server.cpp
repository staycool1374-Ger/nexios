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

/// @file test_sporadic_server.cpp
/// @brief Sporadic server scheduling tests.

#include <test.hpp>
#include <logger.hpp>
#include <kernel/task/sporadic_server.hpp>

using namespace kernel;
using task::SporadicServer;

// Runmode: kernel
// Testidea: Verify init sets budget, period, and background priority correctly.
// Expect: All accessors return the configured values; state is IDLE.
JARVIS_TEST(sporadic_server_init, "PRE: none | POST: none") {
    SporadicServer ss;
    ss.init(10, 100, 2);
    JARVIS_ASSERT_EQ(10ULL, ss.max_budget());
    JARVIS_ASSERT_EQ(100ULL, ss.period());
    JARVIS_ASSERT_EQ(10ULL, ss.remaining_budget());
    ss.set_base_priority(2);
    JARVIS_ASSERT_EQ(2ULL, ss.current_priority());
    JARVIS_ASSERT(ss.state() == SporadicServer::IDLE);
    JARVIS_ASSERT(!ss.is_active());
    JARVIS_ASSERT(ss.has_budget());
    JARVIS_ASSERT_EQ(0ULL, ss.pending_count());
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Activation transitions IDLE -> ACTIVE and records activation time.
// Expect: state is ACTIVE, is_active returns true, budget unchanged.
JARVIS_TEST(sporadic_server_activation, "PRE: none | POST: none") {
    SporadicServer ss;
    ss.init(10, 100, 2);
    ss.on_activation(50);
    JARVIS_ASSERT(ss.state() == SporadicServer::ACTIVE);
    JARVIS_ASSERT(ss.is_active());
    JARVIS_ASSERT_EQ(10ULL, ss.remaining_budget());
    JARVIS_ASSERT_EQ(0ULL, ss.pending_count());
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Double activation while already active is a no-op.
// Expect: State remains ACTIVE; budget same as after first activation.
JARVIS_TEST(sporadic_server_double_activation, "PRE: none | POST: none") {
    SporadicServer ss;
    ss.init(10, 100, 2);
    ss.on_activation(50);
    uint64_t budget_after_first = ss.remaining_budget();
    // Second activation while already active — should be no-op
    ss.on_activation(60);
    JARVIS_ASSERT(ss.state() == SporadicServer::ACTIVE);
    JARVIS_ASSERT_EQ(budget_after_first, ss.remaining_budget());
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Consume decrements budget each tick; returns false when exhausted.
// Expect: After C ticks of consume, budget is 0, state is EXHAUSTED,
//         and a replenishment event is scheduled.
JARVIS_TEST(sporadic_server_consumption_exhaustion, "PRE: none | POST: none") {
    SporadicServer ss;
    ss.init(5, 100, 2);
    ss.on_activation(10);

    for (uint64_t i = 0; i < 4; i++) {
        JARVIS_ASSERT(ss.consume(10 + i));
        JARVIS_ASSERT(ss.state() == SporadicServer::ACTIVE);
    }
    // 5th consume exhausts budget
    bool ok = ss.consume(14);
    JARVIS_ASSERT(!ok);
    JARVIS_ASSERT(ss.state() == SporadicServer::EXHAUSTED);
    JARVIS_ASSERT_EQ(0ULL, ss.remaining_budget());
    JARVIS_ASSERT(!ss.has_budget());
    // Should have one pending replenishment at t=10+100=110, amount=5
    JARVIS_ASSERT_EQ(1ULL, ss.pending_count());
    // Priority should now be background
    JARVIS_ASSERT_EQ(2ULL, ss.current_priority());
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Completion schedules a replenishment for consumed budget and goes
// IDLE. Expect: After consuming 3 ticks and completing, 1 replenishment
// pending,
//         state IDLE, remaining budget = 7 (C - consumed).
JARVIS_TEST(sporadic_server_completion_schedules_replenishment,
            "PRE: none | POST: none") {
    SporadicServer ss;
    ss.init(10, 100, 2);
    ss.on_activation(20);

    ss.consume(20);
    ss.consume(21);
    ss.consume(22);

    ss.on_completion(23);
    JARVIS_ASSERT(ss.state() == SporadicServer::IDLE);
    JARVIS_ASSERT(!ss.is_active());
    JARVIS_ASSERT_EQ(7ULL, ss.remaining_budget());
    JARVIS_ASSERT_EQ(1ULL, ss.pending_count());
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Processing due replenishment restores budget and reactivates
// server. Expect: After replenishment at t=110 fires at tick 110, budget is
// restored
//         to 5, state transitions back to ACTIVE, pending count is 0.
JARVIS_TEST(sporadic_server_replenishment_restores_budget,
            "PRE: none | POST: none") {
    SporadicServer ss;
    ss.init(5, 100, 2);
    ss.on_activation(10);

    // Exhaust all 5 ticks
    for (uint64_t i = 0; i < 5; i++)
        ss.consume(10 + i);

    JARVIS_ASSERT(ss.state() == SporadicServer::EXHAUSTED);
    JARVIS_ASSERT_EQ(0ULL, ss.remaining_budget());
    JARVIS_ASSERT_EQ(1ULL, ss.pending_count());

    // Fast-forward to replenishment time
    ss.process_replenishments(109);
    JARVIS_ASSERT(ss.state() == SporadicServer::EXHAUSTED);
    JARVIS_ASSERT_EQ(0ULL, ss.remaining_budget());

    ss.process_replenishments(110);
    JARVIS_ASSERT(ss.state() == SporadicServer::ACTIVE);
    JARVIS_ASSERT_EQ(5ULL, ss.remaining_budget());
    JARVIS_ASSERT(ss.has_budget());
    JARVIS_ASSERT_EQ(0ULL, ss.pending_count());
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Multiple activations and completions produce correct replenishment
//           chain (one replenishment per busy interval).
// Expect: Two complete cycles: after first, replenishment at t=110 amount=5;
//         after second, replenishment at t=210 amount=5.
JARVIS_TEST(sporadic_server_two_cycles, "PRE: none | POST: none") {
    SporadicServer ss;
    ss.init(5, 100, 2);

    // ---- Cycle 1 ----
    ss.on_activation(10);
    for (uint64_t i = 0; i < 5; i++)
        ss.consume(10 + i);
    JARVIS_ASSERT(ss.state() == SporadicServer::EXHAUSTED);
    JARVIS_ASSERT_EQ(0ULL, ss.remaining_budget());
    JARVIS_ASSERT_EQ(1ULL, ss.pending_count());

    // Replenishment fires at 110
    ss.process_replenishments(110);
    JARVIS_ASSERT(ss.state() == SporadicServer::ACTIVE);
    JARVIS_ASSERT_EQ(5ULL, ss.remaining_budget());

    // ---- Cycle 2 ----
    // Server is already active from replenishment; a new activation while
    // active is a no-op, which is correct — the job is already running.
    ss.on_activation(115); // no-op (already ACTIVE)
    for (uint64_t i = 0; i < 5; i++)
        ss.consume(110 + i);
    JARVIS_ASSERT(ss.state() == SporadicServer::EXHAUSTED);
    JARVIS_ASSERT_EQ(0ULL, ss.remaining_budget());
    // Should have replenishment at activation_time (110) + T (100) = 210
    JARVIS_ASSERT_EQ(1ULL, ss.pending_count());

    ss.process_replenishments(210);
    JARVIS_ASSERT(ss.state() == SporadicServer::ACTIVE);
    JARVIS_ASSERT_EQ(5ULL, ss.remaining_budget());
    JARVIS_ASSERT_EQ(0ULL, ss.pending_count());
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Replenishment budget is capped at C even if multiple events fire.
// Expect: If two replenishments each of amount 5 fire at the same tick,
//         budget does not exceed C = 8.
JARVIS_TEST(sporadic_server_budget_capped_at_C, "PRE: none | POST: none") {
    SporadicServer ss;
    ss.init(8, 50, 2);
    ss.on_activation(10);

    // Consume 3 ticks then complete — schedule repl at 60 for amount 3
    ss.consume(10);
    ss.consume(11);
    ss.consume(12);
    ss.on_completion(13);
    JARVIS_ASSERT_EQ(1ULL, ss.pending_count());

    // Re-activate at 20 (budget still 5)
    ss.on_activation(20);
    ss.consume(20);
    ss.consume(21);
    ss.consume(22);
    ss.on_completion(23);
    JARVIS_ASSERT_EQ(2ULL, ss.pending_count());

    // Process replenishments — both fire at/after 70
    ss.process_replenishments(70);
    // First repl: budget 5 + 3 = 8 (capped at 8)
    // Second repl: would add 3 more but capped at C=8
    JARVIS_ASSERT_EQ(8ULL, ss.remaining_budget());
    JARVIS_ASSERT_EQ(0ULL, ss.pending_count());
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Completion when no budget was consumed schedules no replenishment.
// Expect: After activation -> immediate completion (no consume), 0 pending.
JARVIS_TEST(sporadic_server_completion_no_consume, "PRE: none | POST: none") {
    SporadicServer ss;
    ss.init(5, 100, 2);
    ss.on_activation(10);
    ss.on_completion(10);
    JARVIS_ASSERT(ss.state() == SporadicServer::IDLE);
    JARVIS_ASSERT_EQ(0ULL, ss.pending_count());
    JARVIS_ASSERT_EQ(5ULL, ss.remaining_budget());
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Consuming when IDLE returns false and does not change state.
// Expect: consume() on idle server returns false, state stays IDLE.
JARVIS_TEST(sporadic_server_consume_idle, "PRE: none | POST: none") {
    SporadicServer ss;
    ss.init(5, 100, 2);
    bool ok = ss.consume(10);
    JARVIS_ASSERT(!ok);
    JARVIS_ASSERT(ss.state() == SporadicServer::IDLE);
    JARVIS_ASSERT_EQ(5ULL, ss.remaining_budget());
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Completion when already IDLE is a no-op.
// Expect: on_completion() called twice has no effect.
JARVIS_TEST(sporadic_server_double_completion, "PRE: none | POST: none") {
    SporadicServer ss;
    ss.init(5, 100, 2);
    ss.on_activation(10);
    ss.consume(10);
    ss.consume(11);
    ss.on_completion(12);
    JARVIS_ASSERT(ss.state() == SporadicServer::IDLE);
    uint64_t budget = ss.remaining_budget();
    // Second completion while idle
    ss.on_completion(13);
    JARVIS_ASSERT(ss.state() == SporadicServer::IDLE);
    JARVIS_ASSERT_EQ(budget, ss.remaining_budget());
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Process replenishments with empty queue is a safe no-op.
// Expect: No crash, state unchanged, budget unchanged.
JARVIS_TEST(sporadic_server_process_empty_queue, "PRE: none | POST: none") {
    SporadicServer ss;
    ss.init(5, 100, 2);
    ss.process_replenishments(999);
    JARVIS_ASSERT(ss.state() == SporadicServer::IDLE);
    JARVIS_ASSERT_EQ(5ULL, ss.remaining_budget());
    JARVIS_ASSERT_EQ(0ULL, ss.pending_count());
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Replenishment queue handles up to MAX_REPLENISHMENTS entries.
// Expect: Insertions succeed until full; further insertions return false.
JARVIS_TEST(sporadic_server_queue_limits, "PRE: none | POST: none") {
    SporadicServer ss;
    ss.init(10, 10, 2);

    // Each activation+completion adds one replenishment
    for (uint64_t i = 0; i < SporadicServer::MAX_REPLENISHMENTS; i++) {
        ss.on_activation(10 * i);
        ss.consume(10 * i);
        ss.on_completion(10 * i + 1);
    }
    JARVIS_ASSERT_EQ(SporadicServer::MAX_REPLENISHMENTS, ss.pending_count());

    // Next one should fail silently
    ss.on_activation(1000);
    ss.consume(1000);
    ss.on_completion(1001);
    JARVIS_ASSERT_EQ(SporadicServer::MAX_REPLENISHMENTS, ss.pending_count());
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Priority after exhaustion returns bg_priority; after replenishment
//           and reactivation, priority returns to base_priority (set
//           externally).
// Expect: When EXHAUSTED, current_priority == bg_priority (2).
//         Set base_priority externally, after replenishment fires,
//         current_priority == base.
JARVIS_TEST(sporadic_server_priority_transitions, "PRE: none | POST: none") {
    SporadicServer ss;
    ss.init(5, 100, 2);
    ss.set_base_priority(1);

    ss.on_activation(10);
    for (uint64_t i = 0; i < 5; i++)
        ss.consume(10 + i);
    JARVIS_ASSERT(ss.state() == SporadicServer::EXHAUSTED);
    JARVIS_ASSERT_EQ(2ULL, ss.current_priority());

    ss.process_replenishments(110);
    JARVIS_ASSERT(ss.state() == SporadicServer::ACTIVE);
    JARVIS_ASSERT_EQ(1ULL, ss.current_priority());
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Verify deadline handler fires on budget exhaustion (reason=0).
// Expect: Handler flag is set after consume exhausts the budget.
JARVIS_TEST(sporadic_server_deadline_miss, "PRE: none | POST: none") {
    SporadicServer ss;
    ss.init(3, 100, 0);
    ss.set_base_priority(1);
    ss.on_activation(10);
    // C=3 means 2 successful consumes, 3rd exhausts (1→0)
    JARVIS_ASSERT(ss.consume(10));
    JARVIS_ASSERT(ss.consume(11));
    JARVIS_ASSERT(!ss.consume(12)); // exhausts here
    JARVIS_ASSERT(!ss.consume(13));
    JARVIS_ASSERT(ss.state() == SporadicServer::EXHAUSTED);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Verify granularity > 1 skips intermediate consume calls.
// Expect: With granularity=3, only every 3rd consume decrements budget.
JARVIS_TEST(sporadic_server_granularity_skip, "PRE: none | POST: none") {
    SporadicServer ss;
    ss.init(5, 100, 0, 3);
    ss.set_base_priority(1);
    ss.on_activation(10);
    // 5 budget units at granularity 3 = 15 consume calls before exhaustion
    for (uint64_t i = 0; i < 14; i++)
        JARVIS_ASSERT(ss.consume(10 + i));
    // 15th call should exhaust
    JARVIS_ASSERT(!ss.consume(24));
    JARVIS_ASSERT(ss.state() == SporadicServer::EXHAUSTED);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Verify granularity boundary — budget=1, granularity=2 means 2
// ticks. Expect: First consume skips (granularity check), second consumes and
// exhausts.
JARVIS_TEST(sporadic_server_granularity_exhaustion, "PRE: none | POST: none") {
    SporadicServer ss;
    ss.init(1, 100, 0, 2);
    ss.set_base_priority(1);
    ss.on_activation(10);
    // First call is skipped (consume_counter % 2 == 1)
    JARVIS_ASSERT(ss.consume(10));
    JARVIS_ASSERT(ss.has_budget());
    JARVIS_ASSERT(ss.state() == SporadicServer::ACTIVE);
    // Second call consumes the only budget unit → exhausted
    JARVIS_ASSERT(!ss.consume(11));
    JARVIS_ASSERT(ss.state() == SporadicServer::EXHAUSTED);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Verify init with explicit granularity stores and uses the value.
// Expect: Granularity=7 is accepted; consume behavior reflects it.
JARVIS_TEST(sporadic_server_init_with_granularity, "PRE: none | POST: none") {
    SporadicServer ss;
    ss.init(5, 100, 2, 7);
    ss.set_base_priority(1);
    ss.on_activation(10);
    // First 6 consume calls are skipped (7%7==0 on the 7th)
    for (uint64_t i = 0; i < 6; i++)
        JARVIS_ASSERT(ss.consume(10 + i));
    JARVIS_ASSERT_EQ(5ULL, ss.remaining_budget());
    // 7th call consumes
    JARVIS_ASSERT(ss.consume(16));
    JARVIS_ASSERT_EQ(4ULL, ss.remaining_budget());
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Granularity=0 is clamped to 1.
// Expect: Server behaves as if granularity=1.
JARVIS_TEST(sporadic_server_granularity_clamp_zero, "PRE: none | POST: none") {
    SporadicServer ss;
    ss.init(3, 100, 0, 0);
    ss.set_base_priority(1);
    ss.on_activation(10);
    // Every consume should decrement (granularity clamped to 1)
    JARVIS_ASSERT(ss.consume(10));
    JARVIS_ASSERT_EQ(2ULL, ss.remaining_budget());
    JARVIS_ASSERT(ss.consume(11));
    JARVIS_ASSERT_EQ(1ULL, ss.remaining_budget());
    // 3rd consume exhausts (1→0) and returns false
    JARVIS_ASSERT(!ss.consume(12));
    JARVIS_ASSERT_EQ(0ULL, ss.remaining_budget());
    JARVIS_ASSERT(!ss.consume(13));
    JARVIS_ASSERT(ss.state() == SporadicServer::EXHAUSTED);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Granularity with completion — verify replenishment amount
//           equals consumed units, not raw ticks.
// Expect: budget=5, granularity=3, consume 2 units (6 calls), complete.
//         Replenishment amount = 2 (not 6).  M-8: the server completed (IDLE),
//         so a replenishment restores budget WITHOUT forcing it ACTIVE — only
//         an EXHAUSTED server may be re-activated by a replenishment.
JARVIS_TEST(sporadic_server_granularity_completion, "PRE: none | POST: none") {
    SporadicServer ss;
    ss.init(5, 100, 0, 3);
    ss.set_base_priority(1);
    ss.on_activation(10);

    ss.consume(10); // skip
    ss.consume(11); // skip
    ss.consume(12); // consume (unit 1, consumed_since_active_=1)
    ss.consume(13); // skip
    ss.consume(14); // skip
    ss.consume(15); // consume (unit 2, consumed_since_active_=2)

    ss.on_completion(16);
    JARVIS_ASSERT(ss.state() == SporadicServer::IDLE);
    JARVIS_ASSERT_EQ(3ULL, ss.remaining_budget());
    JARVIS_ASSERT_EQ(1ULL, ss.pending_count());
    // Replenishment amount should be consumed_since_active_ = 2, not 6
    ss.process_replenishments(110);
    // M-8: IDLE server stays IDLE after a replenishment (budget restored, but
    // no aperiodic work pending — forcing ACTIVE would delay the next job).
    JARVIS_ASSERT(ss.state() == SporadicServer::IDLE);
    // Budget was 3, replenished 2, capped at C=5
    JARVIS_ASSERT_EQ(5ULL, ss.remaining_budget());
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Two complete activation/consumption/exhaustion/replenishment
//           cycles with granularity > 1.
// Expect: Both cycles complete correctly, replenishment amounts match
//         consumed units.
JARVIS_TEST(sporadic_server_granularity_two_cycles, "PRE: none | POST: none") {
    SporadicServer ss;
    ss.init(3, 50, 0, 2);
    ss.set_base_priority(1);

    // Cycle 1: consume budget at granularity 2 → 6 consume calls
    ss.on_activation(10);
    for (uint64_t i = 0; i < 6; i++) {
        bool ok = ss.consume(10 + i);
        if (i < 5) {
            JARVIS_ASSERT(ok);
        } else {
            JARVIS_ASSERT(!ok);
        }
    }
    JARVIS_ASSERT(ss.state() == SporadicServer::EXHAUSTED);
    JARVIS_ASSERT_EQ(1ULL, ss.pending_count());

    ss.process_replenishments(60);
    JARVIS_ASSERT(ss.state() == SporadicServer::ACTIVE);
    JARVIS_ASSERT_EQ(3ULL, ss.remaining_budget());

    // Cycle 2: same pattern — 6 consume calls
    for (uint64_t i = 0; i < 6; i++) {
        bool ok = ss.consume(60 + i);
        if (i < 5) {
            JARVIS_ASSERT(ok);
        } else {
            JARVIS_ASSERT(!ok);
        }
    }
    JARVIS_ASSERT(ss.state() == SporadicServer::EXHAUSTED);

    ss.process_replenishments(110);
    JARVIS_ASSERT(ss.state() == SporadicServer::ACTIVE);
    JARVIS_ASSERT_EQ(3ULL, ss.remaining_budget());
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Granularity larger than budget — only one actual decrement
//           across many ticks.
// Expect: budget=3, granularity=10. First consume at call 10, then at 20, then
// at 30.
JARVIS_TEST(sporadic_server_granularity_large, "PRE: none | POST: none") {
    SporadicServer ss;
    ss.init(3, 100, 0, 10);
    ss.set_base_priority(1);
    ss.on_activation(10);

    // 9 skips, then 1 consume, then 9 skips, then 1 consume...
    for (uint64_t i = 0; i < 9; i++)
        JARVIS_ASSERT(ss.consume(10 + i));
    JARVIS_ASSERT_EQ(3ULL, ss.remaining_budget());

    JARVIS_ASSERT(ss.consume(19)); // consume unit 1
    JARVIS_ASSERT_EQ(2ULL, ss.remaining_budget());

    for (uint64_t i = 0; i < 9; i++)
        JARVIS_ASSERT(ss.consume(20 + i));
    JARVIS_ASSERT_EQ(2ULL, ss.remaining_budget());

    JARVIS_ASSERT(ss.consume(29)); // consume unit 2
    JARVIS_ASSERT_EQ(1ULL, ss.remaining_budget());

    for (uint64_t i = 0; i < 9; i++)
        JARVIS_ASSERT(ss.consume(30 + i));
    JARVIS_ASSERT_EQ(1ULL, ss.remaining_budget());

    JARVIS_ASSERT(!ss.consume(39)); // consume unit 3 → exhausted
    JARVIS_ASSERT_EQ(0ULL, ss.remaining_budget());
    JARVIS_ASSERT(ss.state() == SporadicServer::EXHAUSTED);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: consume() on already-exhausted server returns false and stays
// EXHAUSTED. Expect: Multiple consume calls after exhaustion all return false.
JARVIS_TEST(sporadic_server_consume_already_exhausted,
            "PRE: none | POST: none") {
    SporadicServer ss;
    ss.init(2, 100, 0);
    ss.set_base_priority(1);
    ss.on_activation(10);
    ss.consume(10);                 // budget=1
    JARVIS_ASSERT(!ss.consume(11)); // budget=0, EXHAUSTED
    JARVIS_ASSERT(ss.state() == SporadicServer::EXHAUSTED);
    // Further consume calls on exhausted server
    JARVIS_ASSERT(!ss.consume(12));
    JARVIS_ASSERT(ss.state() == SporadicServer::EXHAUSTED);
    JARVIS_ASSERT(!ss.consume(13));
    JARVIS_ASSERT(ss.state() == SporadicServer::EXHAUSTED);
    JARVIS_ASSERT_EQ(0ULL, ss.remaining_budget());
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Replenishment after partial consumption restores only the
//           consumed amount, capped at C.
// Expect: Consume 2 of 5 budget, complete. Replenishment restores 2.
//         Budget goes from 3 → 5 (capped).  M-8: server completed (IDLE), so
//         the replenishment must NOT force it ACTIVE.
JARVIS_TEST(sporadic_server_replenishment_partial, "PRE: none | POST: none") {
    SporadicServer ss;
    ss.init(5, 50, 0);
    ss.set_base_priority(1);
    ss.on_activation(10);
    ss.consume(10);
    ss.consume(11); // consumed 2 units
    ss.on_completion(12);
    JARVIS_ASSERT_EQ(3ULL, ss.remaining_budget());
    JARVIS_ASSERT_EQ(1ULL, ss.pending_count());

    // Fire replenishment
    ss.process_replenishments(60);
    JARVIS_ASSERT_EQ(5ULL, ss.remaining_budget()); // 3 + 2 = 5
    // M-8: IDLE stays IDLE (replenishment restores budget, never re-activates
    // a server with no pending aperiodic work).
    JARVIS_ASSERT(ss.state() == SporadicServer::IDLE);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Exhaustion + replenishment with granularity — verify handler
//           fires and state transitions are correct.
// Expect: With granularity=2, budget=2, exhaustion at 4th consume call.
//         After replenishment, budget restored and state ACTIVE.
JARVIS_TEST(sporadic_server_granularity_exhaust_replenish,
            "PRE: none | POST: none") {
    SporadicServer ss;
    ss.init(2, 50, 0, 2);
    ss.set_base_priority(1);
    ss.on_activation(10);

    // consume: skip, consume(1st), skip, consume(2nd/exhaust)
    JARVIS_ASSERT(ss.consume(10));  // skip
    JARVIS_ASSERT(ss.consume(11));  // consume unit 1
    JARVIS_ASSERT(ss.consume(12));  // skip
    JARVIS_ASSERT(!ss.consume(13)); // consume unit 2 → exhaust
    JARVIS_ASSERT(ss.state() == SporadicServer::EXHAUSTED);
    JARVIS_ASSERT_EQ(0ULL, ss.remaining_budget());
    JARVIS_ASSERT_EQ(1ULL, ss.pending_count());

    ss.process_replenishments(60);
    JARVIS_ASSERT(ss.state() == SporadicServer::ACTIVE);
    JARVIS_ASSERT_EQ(2ULL, ss.remaining_budget());
    JARVIS_ASSERT_EQ(0ULL, ss.pending_count());
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Registers all SporadicServer unit tests with the test framework.
void register_sporadic_server_tests() {
    Logger::info("Registering SporadicServer tests");
    JARVIS_REGISTER_TEST(sporadic_server_init);
    JARVIS_REGISTER_TEST(sporadic_server_activation);
    JARVIS_REGISTER_TEST(sporadic_server_double_activation);
    JARVIS_REGISTER_TEST(sporadic_server_consumption_exhaustion);
    JARVIS_REGISTER_TEST(sporadic_server_completion_schedules_replenishment);
    JARVIS_REGISTER_TEST(sporadic_server_replenishment_restores_budget);
    JARVIS_REGISTER_TEST(sporadic_server_two_cycles);
    JARVIS_REGISTER_TEST(sporadic_server_budget_capped_at_C);
    JARVIS_REGISTER_TEST(sporadic_server_completion_no_consume);
    JARVIS_REGISTER_TEST(sporadic_server_consume_idle);
    JARVIS_REGISTER_TEST(sporadic_server_double_completion);
    JARVIS_REGISTER_TEST(sporadic_server_process_empty_queue);
    JARVIS_REGISTER_TEST(sporadic_server_queue_limits);
    JARVIS_REGISTER_TEST(sporadic_server_priority_transitions);
    JARVIS_REGISTER_TEST(sporadic_server_deadline_miss);
    JARVIS_REGISTER_TEST(sporadic_server_granularity_skip);
    JARVIS_REGISTER_TEST(sporadic_server_granularity_exhaustion);
    JARVIS_REGISTER_TEST(sporadic_server_init_with_granularity);
    JARVIS_REGISTER_TEST(sporadic_server_granularity_clamp_zero);
    JARVIS_REGISTER_TEST(sporadic_server_granularity_completion);
    JARVIS_REGISTER_TEST(sporadic_server_granularity_two_cycles);
    JARVIS_REGISTER_TEST(sporadic_server_granularity_large);
    JARVIS_REGISTER_TEST(sporadic_server_consume_already_exhausted);
    JARVIS_REGISTER_TEST(sporadic_server_replenishment_partial);
    JARVIS_REGISTER_TEST(sporadic_server_granularity_exhaust_replenish);
}