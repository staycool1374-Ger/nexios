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

/// @file test_park_lock.cpp
/// @brief M2 (issues #545/#546) QE: bounded-park acquisition + lock-order
///        contract tests (core_sync).  Measurement of the ParkLockGuard
///        slow path, the INV-L1 park predicate, the INV-C1 reschedule check,
///        and the report-only rank-order table.  No production code touched.

#include <test.hpp>
#include <logger.hpp>
#include <kernel/sync/spinlock.hpp>
#include <kernel/sync/park_lock.hpp>
#include <kernel/sync/spinlock_order.hpp>
#include <kernel/arch/hal/irq_guard.hpp>
#include <kernel/arch/cpu_context.hpp>
#include <kernel/arch/hal/timer.hpp>
#include <kernel/task/scheduler.hpp>
#include <kernel/task/task.hpp>
#include <kernel/time/posix_time.hpp>
#include "test_sched_helpers.hpp"

using namespace kernel;

// Runmode: kernel
// Testidea: Uncontended ParkLockGuard takes the try_lock fast path.
// Input: Free SpinLock, counters reset.
// Expect: owned, lock_park_count/lock_spin_fallback_count/
//         lock_park_exhausted_count all unchanged.
// Depends: sync::ParkLockGuard, park counters + reset_for_test
JARVIS_TEST(lock_park_fast_path_no_park, "PRE: none | POST: none") {
    sync::park_lock_reset_for_test();
    sync::spinlock_order_reset_for_test();
    {
        sync::SpinLock free_lock;
        sync::ParkLockGuard guard(free_lock, "test_fast");
        JARVIS_ASSERT(guard.owned());
        JARVIS_ASSERT_EQ(0ULL, sync::lock_park_count());
        JARVIS_ASSERT_EQ(0ULL, sync::lock_spin_fallback_count());
        JARVIS_ASSERT_EQ(0ULL, sync::lock_park_exhausted_count());
    }
    sync::park_lock_reset_for_test();
    sync::spinlock_order_reset_for_test();
    JARVIS_TEST_PASS();
}

namespace park_test {

// Queue-ranked rendezvous lock.  A static MessageQueue is impossible here
// (its non-constexpr ctor needs __cxa_atexit, absent freestanding); the
// Queue rank preserves the prescription's intent — the park path under
// test does not consult the rank.
sync::SpinLock g_lock(sync::LockRank::Queue);
volatile bool g_holder_has_lock = false;
volatile bool g_holder_armed = false;
volatile bool g_contender_acquired = false;
volatile uint64_t g_contender_park_delta = 0;

void holder_entry() {
    auto *self = Scheduler::current_task();
    sync::ParkLockGuard hold(g_lock, "test_holder");
    if (!hold.owned())
        return;
    g_holder_has_lock = true;
    // Hold ~10 ms while BLOCKED so the contender can run and park.  The
    // BLOCKED transition with a lock held trips the INV-C1 WARN (warn-only
    // at ASSERT=1) — expected here; it proves the check is live.
    const uint64_t ns =
        10ULL * 1000000000ULL / static_cast<uint64_t>(CONFIG_TICK_HZ);
    if (time::PosixTime::sleep_arm(*self, ns)) {
        g_holder_armed = true;
        {
            arch::IrqGuard irq_guard{};
            Scheduler::dequeue_ready(*self);
            self->state = TaskState::BLOCKED;
        }
        Scheduler::reschedule();
        for (;;) {
            arch::sti();
            arch::hlt();
            arch::cli();
            if (self->state != TaskState::BLOCKED)
                break;
        }
        time::PosixTime::sleep_cancel(*self);
    }
    // Guard dtor releases; entry return terminates the task.
}

void contender_entry() {
    auto *self = Scheduler::current_task();
    (void)self;
    for (uint64_t i = 0; i < 100000ULL && !g_holder_has_lock; ++i) {
        Scheduler::reschedule();
        arch::hlt();
    }
    if (!g_holder_has_lock)
        return;
    const uint64_t park_before = sync::lock_park_count();
    sync::ParkLockGuard guard(g_lock, "test_contender");
    if (guard.owned()) {
        g_contender_acquired = true;
        g_contender_park_delta = sync::lock_park_count() - park_before;
    }
}

} // namespace park_test

// Runmode: kernel
// Testidea: Contended ParkLockGuard parks (not spins) then acquires.
// Input: Holder task holds a Queue-ranked rendezvous lock across a 10 ms
//        wheel sleep; contender ParkLockGuards it (holder releases on wake).
//        (Static MessageQueue is impossible in a freestanding TU — no
//        __cxa_atexit — so a Queue-ranked SpinLock stands in; the park path
//        under test does not consult the rank.)
// Expect: Contender acquires; at suite-entry depth 0 the park path runs
//         (park_count >= 1, fallback/exhausted == 0); at the harness depth
//         1 the denied path runs instead (park == 0, fallback >= 1,
//         exhausted == 0) — the park path is unreachable in-harness while
//         the harness holds its outer IrqGuard (see #545 QE gap note).
//         Both tasks terminate, tracker delta 0.  One INV-C1 WARN from the
//         holder's BLOCK transition is expected (warn-only, proves the
//         check is live).
// Depends: ParkLockGuard park/denied paths, sleep wheel wake,
//         wait_for_termination
JARVIS_TEST(lock_park_taken_under_contention, "PRE: none | POST: none") {
    sync::park_lock_reset_for_test();
    sync::spinlock_order_reset_for_test();
    park_test::g_holder_has_lock = false;
    park_test::g_holder_armed = false;
    park_test::g_contender_acquired = false;
    park_test::g_contender_park_delta = 0;
    const uint64_t d0 = arch::irq_guard_depth_own();
    Logger::info("[PARK-TEST] contention entry depth=%lu", d0);
    auto *holder = TaskControlBlock::create(park_test::holder_entry, 11, 10);
    JARVIS_ASSERT(holder != nullptr);
    JARVIS_ASSERT(Scheduler::set_sched_policy(*holder, SchedPolicy::FIXED));
    auto *contender =
        TaskControlBlock::create(park_test::contender_entry, 12, 10);
    JARVIS_ASSERT(contender != nullptr);
    JARVIS_ASSERT(Scheduler::set_sched_policy(*contender, SchedPolicy::FIXED));
    {
        arch::IrqGuard irq_guard{};
        Scheduler::add_task(*holder);
        Scheduler::add_task(*contender);
    }
    Scheduler::reschedule();
    test::wait_for_termination_safe(holder);
    test::wait_for_termination_safe(contender);
    Logger::info("[PARK-TEST] contention exit depth=%lu",
                 arch::irq_guard_depth_own());
    Logger::info("[PARK-TEST] contention have=%lu armed=%lu got=%lu "
                 "parkdelta=%lu park=%lu fb=%lu exh=%lu",
                 static_cast<uint64_t>(park_test::g_holder_has_lock),
                 static_cast<uint64_t>(park_test::g_holder_armed),
                 static_cast<uint64_t>(park_test::g_contender_acquired),
                 park_test::g_contender_park_delta,
                 sync::lock_park_count(), sync::lock_spin_fallback_count(),
                 sync::lock_park_exhausted_count());
    JARVIS_ASSERT(park_test::g_holder_has_lock);
    JARVIS_ASSERT(park_test::g_contender_acquired);
    JARVIS_ASSERT(d0 <= 1);
    if (d0 == 0) {
        JARVIS_ASSERT(sync::lock_park_count() >= 1);
        JARVIS_ASSERT_EQ(0ULL, sync::lock_spin_fallback_count());
    } else {
        JARVIS_ASSERT_EQ(0ULL, sync::lock_park_count());
        JARVIS_ASSERT(sync::lock_spin_fallback_count() >= 1);
    }
    JARVIS_ASSERT_EQ(0ULL, sync::lock_park_exhausted_count());
    test::terminate_and_drain2(holder, contender);
    sync::park_lock_reset_for_test();
    sync::spinlock_order_reset_for_test();
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: No park under ISR nesting (INV-L1 IsrContext arm).
// Input: isr_nesting_own() raised by one (IF=0 across the mutation so no
//        ISR can observe it), free lock.
// Expect: lock_park_allowed() false; nesting restored; IF unchanged.
// Depends: lock_park_allowed predicate, isr_nesting_own
JARVIS_TEST(lock_park_denied_in_irq, "PRE: none | POST: none") {
    sync::SpinLock probe;
    const uint64_t d0 = arch::irq_guard_depth_own();
    const uint64_t n0 = kernel::isr_nesting_own();
    Logger::info("[PARK-TEST] irq baseline depth=%lu nesting=%lu", d0, n0);
    {
        arch::IrqGuard irq_guard{};
        ++kernel::isr_nesting_own();
        JARVIS_ASSERT(!sync::lock_park_allowed(probe));
        --kernel::isr_nesting_own();
        JARVIS_ASSERT_EQ(n0, kernel::isr_nesting_own());
        JARVIS_ASSERT_EQ(d0 + 1, arch::irq_guard_depth_own());
    }
    JARVIS_ASSERT_EQ(d0, arch::irq_guard_depth_own());
    JARVIS_ASSERT(arch::interrupts_enabled());
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: No park inside an IrqGuard scope (INV-L1 IrqGuarded arm) and no
// park while the caller already holds a spinlock (LockHeld arm).
// Input: Free lock + live IrqGuard; then lock held by self, no guard.
// Expect: lock_park_allowed() false in both; fast-path guard on the free
//         lock inside the guard acquires with counters unchanged; d0 restored.
// Depends: lock_park_allowed predicate, ParkLockGuard fast path
JARVIS_TEST(lock_park_denied_inside_irqguard, "PRE: none | POST: none") {
    sync::park_lock_reset_for_test();
    sync::SpinLock probe;
    const uint64_t d0 = arch::irq_guard_depth_own();
    Logger::info("[PARK-TEST] irqguard baseline depth=%lu", d0);
    {
        arch::IrqGuard irq_guard{};
        JARVIS_ASSERT_EQ(d0 + 1, arch::irq_guard_depth_own());
        JARVIS_ASSERT(!sync::lock_park_allowed(probe));
        // Fast path still acquires a free lock inside the guard — no park
        // is attempted, so no counter moves.
        sync::ParkLockGuard guard(probe, "test_irqguard");
        JARVIS_ASSERT(guard.owned());
    }
    JARVIS_ASSERT_EQ(d0, arch::irq_guard_depth_own());
    JARVIS_ASSERT_EQ(0ULL, sync::lock_park_count());
    JARVIS_ASSERT_EQ(0ULL, sync::lock_spin_fallback_count());
    // LockHeld arm: a self-held spinlock denies parking without a guard.
    sync::SpinLock held;
    held.lock();
    JARVIS_ASSERT(!sync::lock_park_allowed(probe));
    held.unlock();
    // Positive leg is depth-relative: the harness holds one outer IrqGuard
    // across all tests (suite-entry depth 1, logged above), so a bare test
    // task is parkable only at depth 0.  d0 > 1 would mean a leaked guard.
    JARVIS_ASSERT(d0 <= 1);
    JARVIS_ASSERT(sync::lock_park_allowed(probe) == (d0 == 0));
    JARVIS_ASSERT_EQ(0ULL, sync::spinlock_held_by_current_count());
    sync::park_lock_reset_for_test();
    sync::spinlock_order_reset_for_test();
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: INV-C1 reschedule-entry check is warn-only and owner-matched.
// Input: Clean task reschedules (held count 0); then task takes a stack
//        SpinLock, observes held count 1, reschedules (WARN, no panic),
//        releases, observes 0.
// Expect: Positive leg clean; negative leg held==1 across reschedule with
//         no panic/termination; release restores 0; tracker delta 0.
// Depends: spinlock_contract_check_clean, INV-C1 (ASSERT=1 warn-only)
JARVIS_TEST(irq_contract_no_spinlock_at_reschedule, "PRE: none | POST: none") {
    sync::spinlock_order_reset_for_test();
    // Positive leg: clean context reschedules with nothing held.
    JARVIS_ASSERT_EQ(0ULL, sync::spinlock_held_by_current_count());
    JARVIS_ASSERT(sync::spinlock_contract_check_clean("test_positive"));
    Scheduler::reschedule();
    JARVIS_ASSERT_EQ(0ULL, sync::spinlock_held_by_current_count());
    // Negative leg: a held stack lock is observed (held count 1) and the
    // reschedule survives warn-only (ASSERT=1: dmesg SCHED+15, no panic).
    sync::SpinLock probe;
    probe.lock();
    JARVIS_ASSERT_EQ(1ULL, sync::spinlock_held_by_current_count());
    JARVIS_ASSERT(!sync::spinlock_contract_check_clean("test_negative"));
    Scheduler::reschedule();
    JARVIS_ASSERT_EQ(1ULL, sync::spinlock_held_by_current_count());
    probe.unlock();
    JARVIS_ASSERT_EQ(0ULL, sync::spinlock_held_by_current_count());
    sync::spinlock_order_reset_for_test();
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Rank inversion is reported once per (outer, inner) pair.
// Input: Ranked locks Queue(2) then Sched(1) nested (violation);
//        Sched(1)->Queue(2)->Leaf(3) nested (clean).
// Expect: violations[Queue][Sched]==1; all legal-pair counters 0.
// Depends: spinlock_note_acquire order check, violation accessors + reset
JARVIS_TEST(lock_order_violation_reported, "PRE: none | POST: none") {
    sync::spinlock_order_reset_for_test();
    {
        // Illegal: outer Queue(2) taken before inner Sched(1).
        sync::SpinLock outer(sync::LockRank::Queue);
        sync::SpinLock inner(sync::LockRank::Sched);
        outer.lock();
        inner.lock();
        JARVIS_ASSERT_EQ(
            1ULL, sync::spinlock_violation_count(
                      static_cast<uint8_t>(sync::LockRank::Queue),
                      static_cast<uint8_t>(sync::LockRank::Sched)));
        inner.unlock();
        outer.unlock();
    }
    {
        // Legal: strictly increasing Sched(1) -> Queue(2) -> Leaf(3).
        sync::SpinLock sched(sync::LockRank::Sched);
        sync::SpinLock queue(sync::LockRank::Queue);
        sync::SpinLock leaf(sync::LockRank::Leaf);
        sched.lock();
        queue.lock();
        leaf.lock();
        JARVIS_ASSERT_EQ(
            0ULL, sync::spinlock_violation_count(
                      static_cast<uint8_t>(sync::LockRank::Sched),
                      static_cast<uint8_t>(sync::LockRank::Queue)));
        JARVIS_ASSERT_EQ(
            0ULL, sync::spinlock_violation_count(
                      static_cast<uint8_t>(sync::LockRank::Queue),
                      static_cast<uint8_t>(sync::LockRank::Leaf)));
        JARVIS_ASSERT_EQ(
            0ULL, sync::spinlock_violation_count(
                      static_cast<uint8_t>(sync::LockRank::Sched),
                      static_cast<uint8_t>(sync::LockRank::Leaf)));
        leaf.unlock();
        queue.unlock();
        sched.unlock();
    }
    JARVIS_ASSERT_EQ(0ULL, sync::spinlock_held_by_current_count());
    JARVIS_ASSERT_EQ(0ULL, sync::spinlock_unpaired_release_count());
    sync::spinlock_order_reset_for_test();
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Non-LIFO release order does not corrupt the held-lock census.
// Input: Three ranked locks taken, released in a LIFO-breaking order.
// Expect: held_by_current_count() 0 after; unpaired_release 0; no overflow.
// Depends: spinlock_note_release pop-by-address, census + reset
JARVIS_TEST(lock_order_reverse_no_false_release, "PRE: none | POST: none") {
    sync::spinlock_order_reset_for_test();
    {
        // LIFO-breaking release order: pop-by-address must still empty
        // the stack exactly (no false unpaired/overflow).
        sync::SpinLock a(sync::LockRank::Sched);
        sync::SpinLock b(sync::LockRank::Queue);
        sync::SpinLock c(sync::LockRank::Leaf);
        a.lock();
        b.lock();
        c.lock();
        a.unlock();
        c.unlock();
        b.unlock();
    }
    JARVIS_ASSERT_EQ(0ULL, sync::spinlock_held_by_current_count());
    JARVIS_ASSERT_EQ(0ULL, sync::spinlock_unpaired_release_count());
    JARVIS_ASSERT_EQ(0ULL, sync::spinlock_overflow_count());
    sync::spinlock_order_reset_for_test();
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: SpinLock layout guard (M2 rank byte fits padding).
// Input: None (compile-time + runtime shape).
// Expect: sizeof(SpinLock)==16 (holder_ offset 8 is not observable from
//         test context — private member, no accessor; covered by the
//         static_assert in spinlock.hpp, noted here).
// Depends: sync::SpinLock layout
JARVIS_TEST(spinlock_layout_unchanged, "PRE: none | POST: none") {
    JARVIS_ASSERT_EQ(16ULL, static_cast<uint64_t>(sizeof(sync::SpinLock)));
    // holder_ offset 8 is not observable from test context (private member,
    // no accessor); enforced by the static_assert in spinlock.hpp.
    JARVIS_TEST_PASS();
}

void register_park_lock_tests() {
    JARVIS_REGISTER_TEST(lock_park_fast_path_no_park);
    JARVIS_REGISTER_TEST(lock_park_taken_under_contention);
    JARVIS_REGISTER_TEST(lock_park_denied_in_irq);
    JARVIS_REGISTER_TEST(lock_park_denied_inside_irqguard);
    JARVIS_REGISTER_TEST(irq_contract_no_spinlock_at_reschedule);
    JARVIS_REGISTER_TEST(lock_order_violation_reported);
    JARVIS_REGISTER_TEST(lock_order_reverse_no_false_release);
    JARVIS_REGISTER_TEST(spinlock_layout_unchanged);
}
