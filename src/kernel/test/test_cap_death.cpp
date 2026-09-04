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

/// @file test_cap_death.cpp
/// @brief Asynchronous task-death notification tests (issue #105 Part B).
/// Verifies the DeathNotify registry + SYS_DEATH_WATCH/RECV/UNWATCH: exactly-
/// once latch on death, crash-reason (signal) encoding, multi-task fan-in,
/// registration authority + fail-closed exhaustion, supervisor-death drain,
/// and non-blocking recv.  ResourceTracker death_watches must be zero-delta.
///
/// IMPORTANT: a helper task's id is captured BEFORE the task is terminated —
/// drain_zombie_list() frees the TCB, so reading t->id after death is a
/// use-after-free (garbage id).  All asserts use the captured id.

#include <test.hpp>
#include <logger.hpp>
#include <kernel/ipc/death_notify.hpp>
#include <kernel/syscall/syscall.hpp>
#include <kernel/task/scheduler.hpp>
#include <kernel/task/task.hpp>
#include <kernel/test/resource_tracker.hpp>
#include <kernel/arch/io.hpp>
#include <kernel/arch/irq_guard.hpp>
#include <lib/signal.hpp>
#include "test_sched_helpers.hpp"

using namespace kernel;

namespace {

/// @brief Dispatches a syscall by number (Syscall::handle takes uint64_t).
uint64_t death_syscall(uint64_t number, uint64_t a0, uint64_t a1, uint64_t a2,
                       uint64_t a3, uint64_t *regs = nullptr) {
    return Syscall::handle(number, a0, a1, a2, a3, regs);
}

/// @brief Creates a task that parks forever (self-terminates only when the
///        harness kills it), so the harness can drive the death path
///        deterministically.
/// @param[out] out_id Receives the task's id BEFORE it can die.
///
/// Priority note: HIGHER number = HIGHER priority (PriorityMap bitmap).  The
/// harness runs at priority 10 and is only non-preemptible while RUNNING; a
/// parked helper with priority > 10 that gets scheduled on a timer tick never
/// yields and starves the harness.  These helpers never need to RUN — they
/// only need to exist in id_table_ for watch()/terminate() — so they are
/// created at a LOW priority (2, below the harness) and aperiodic (period 0)
/// to stay out of the deadline list.
TaskControlBlock *make_parked(uint64_t /*unused_prio*/, uint64_t *out_id = nullptr) {
    auto *t = TaskControlBlock::create(
        []() {
            // Park — the harness terminates us to drive the death path.
            for (;;)
                kernel::Scheduler::reschedule();
        },
        2, 0);
    if (!t)
        return nullptr;
    if (out_id)
        *out_id = t->id;
    Scheduler::add_task(*t);
    return t;
}

} // namespace

// Runmode: kernel
// Testidea: A supervisor registers a watch on a live helper; the helper exits
// with a known code; the supervisor drains exactly one record.
// Input: Helper task parks; supervisor (the harness) registers a watch; the
//        helper is terminated with exit code 42; recv returns the record.
// Expect: dead_id == helper id, exit_code == 42, no SIGNAL flag, slot freed.
// Depends: DeathNotify, SYS_DEATH_WATCH/RECV, cleanup() hook
JARVIS_TEST(death_watch_consume_roundtrip, "PRE: none | POST: none") {
    auto *cur = Scheduler::current_task();
    JARVIS_ASSERT(cur != nullptr);
    uint64_t hid = 0;
    auto *h = make_parked(21, &hid);
    JARVIS_ASSERT(h != nullptr);
    JARVIS_ASSERT(hid != 0);
    Scheduler::reschedule();

    size_t base = ipc::DeathNotify::live_count();
    uint64_t r = death_syscall(
        static_cast<uint64_t>(SyscallNumber::DEATH_WATCH), hid, 0, 0, 0);
    JARVIS_ASSERT_EQ(0ULL, r);
    JARVIS_ASSERT(ipc::DeathNotify::is_watching(*cur, hid));

    Scheduler::terminate(*h, 42);
    Scheduler::drain_zombie_list();

    ipc::DeathRecord rec{};
    int got = ipc::DeathNotify::recv(*cur, rec);
    JARVIS_ASSERT_EQ(1, got);
    JARVIS_ASSERT_EQ(hid, rec.dead_id);
    JARVIS_ASSERT_EQ(42ULL, rec.exit_code);
    JARVIS_ASSERT_EQ(0ULL, rec.flags & ipc::DEATH_FLAG_SIGNAL);
    JARVIS_ASSERT_EQ(base, ipc::DeathNotify::live_count());
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: A crash death (signal-terminated task) carries the signal value in
// exit_code and sets DEATH_FLAG_SIGNAL.
// Input: Watch a helper; terminate it with a negative (signal) exit code.
// Expect: exit_code is the sign-extended SIGSEGV, SIGNAL flag set.
// Depends: exit_code high-bit encoding, DEATH_FLAG_SIGNAL
JARVIS_TEST(death_watch_crash_reason, "PRE: none | POST: none") {
    auto *cur = Scheduler::current_task();
    JARVIS_ASSERT(cur != nullptr);
    uint64_t hid = 0;
    auto *h = make_parked(22, &hid);
    JARVIS_ASSERT(h != nullptr);
    JARVIS_ASSERT(hid != 0);
    Scheduler::reschedule();

    size_t base = ipc::DeathNotify::live_count();
    uint64_t r = death_syscall(
        static_cast<uint64_t>(SyscallNumber::DEATH_WATCH), hid, 0, 0, 0);
    JARVIS_ASSERT_EQ(0ULL, r);

    uint64_t sigcode = static_cast<uint64_t>(
        -static_cast<int64_t>(static_cast<int>(Signal::SIGSEGV)));
    Scheduler::terminate(*h, sigcode);
    Scheduler::drain_zombie_list();

    ipc::DeathRecord rec{};
    int got = ipc::DeathNotify::recv(*cur, rec);
    JARVIS_ASSERT_EQ(1, got);
    JARVIS_ASSERT_EQ(hid, rec.dead_id);
    JARVIS_ASSERT_EQ(sigcode, rec.exit_code);
    JARVIS_ASSERT(rec.flags & ipc::DEATH_FLAG_SIGNAL);
    JARVIS_ASSERT_EQ(base, ipc::DeathNotify::live_count());
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: One supervisor watches three tasks; all die; exactly three records
// are drained in slot order; the registry returns to baseline.
// Input: Register 3 watches; terminate all 3 helpers; drain 3 records.
// Expect: 3 records with distinct dead ids; live_count back to baseline.
// Depends: multi-watch fan-in, recv scan order
JARVIS_TEST(death_watch_multi_fan_in, "PRE: none | POST: none") {
    auto *cur = Scheduler::current_task();
    JARVIS_ASSERT(cur != nullptr);
    uint64_t id1 = 0, id2 = 0, id3 = 0;
    auto *h1 = make_parked(31, &id1);
    auto *h2 = make_parked(32, &id2);
    auto *h3 = make_parked(33, &id3);
    JARVIS_ASSERT(h1 && h2 && h3);
    JARVIS_ASSERT(id1 && id2 && id3);
    Scheduler::reschedule();

    size_t base = ipc::DeathNotify::live_count();
    JARVIS_ASSERT_EQ(0ULL, death_syscall(
        static_cast<uint64_t>(SyscallNumber::DEATH_WATCH), id1, 0, 0, 0));
    JARVIS_ASSERT_EQ(0ULL, death_syscall(
        static_cast<uint64_t>(SyscallNumber::DEATH_WATCH), id2, 0, 0, 0));
    JARVIS_ASSERT_EQ(0ULL, death_syscall(
        static_cast<uint64_t>(SyscallNumber::DEATH_WATCH), id3, 0, 0, 0));
    JARVIS_ASSERT_EQ(base + 3, ipc::DeathNotify::live_count());

    Scheduler::terminate(*h1, 1);
    Scheduler::terminate(*h2, 2);
    Scheduler::terminate(*h3, 3);
    Scheduler::drain_zombie_list();

    ipc::DeathRecord rec{};
    uint64_t seen = 0;
    for (int i = 0; i < 3; ++i) {
        int got = ipc::DeathNotify::recv(*cur, rec);
        JARVIS_ASSERT_EQ(1, got);
        seen |= (1ULL << (rec.dead_id % 64));
    }
    JARVIS_ASSERT(seen & (1ULL << (id1 % 64)));
    JARVIS_ASSERT(seen & (1ULL << (id2 % 64)));
    JARVIS_ASSERT(seen & (1ULL << (id3 % 64)));
    JARVIS_ASSERT_EQ(base, ipc::DeathNotify::live_count());
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: A watch on a task that already exited/reaped is rejected.
// Input: Terminate + reap a helper; then attempt to watch it.
// Expect: watch fails; no slot created; registry baseline.
// Depends: authority + liveness validation in watch()
JARVIS_TEST(death_watch_after_death_rejected, "PRE: none | POST: none") {
    auto *cur = Scheduler::current_task();
    JARVIS_ASSERT(cur != nullptr);
    uint64_t hid = 0;
    auto *h = make_parked(41, &hid);
    JARVIS_ASSERT(h != nullptr);
    JARVIS_ASSERT(hid != 0);
    Scheduler::reschedule();
    Scheduler::terminate(*h, 0);
    Scheduler::drain_zombie_list();

    size_t base = ipc::DeathNotify::live_count();
    uint64_t r = death_syscall(
        static_cast<uint64_t>(SyscallNumber::DEATH_WATCH), hid, 0, 0, 0);
    JARVIS_ASSERT_EQ(static_cast<uint64_t>(-1), r);
    JARVIS_ASSERT_EQ(base, ipc::DeathNotify::live_count());
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: A supervisor holding ACTIVE watches and PENDING records dies; its
// slots are freed by drain_task; no poke lands in a freed Notify.
// Input: A dedicated supervisor task S watches two helpers; the helpers die
//        (records PENDING for S); S then dies; its cleanup drains the slots.
// Expect: live_count returns to baseline; no crash; nothing remains pending.
// Depends: drain_task in cleanup(), poke safety
JARVIS_TEST(death_watch_supervisor_death_drains, "PRE: none | POST: none") {
    auto *cur = Scheduler::current_task();
    JARVIS_ASSERT(cur != nullptr);
    // Baseline captured BEFORE any watch exists — the supervisor's two
    // registrations must be measured against this.
    size_t base = ipc::DeathNotify::live_count();

    // The helpers S will watch.
    uint64_t id1 = 0, id2 = 0;
    auto *h1 = make_parked(51, &id1);
    auto *h2 = make_parked(52, &id2);
    JARVIS_ASSERT(h1 && h2);
    JARVIS_ASSERT(id1 && id2);

    // The supervisor task S: registers watches on the two helpers (as their
    // supervisor), then parks.  We keep the harness itself alive throughout.
    static uint64_t g_watch1 = 0;
    static uint64_t g_watch2 = 0;
    static uint64_t g_done = 0;
    __atomic_store_n(&g_watch1, id1, __ATOMIC_RELEASE);
    __atomic_store_n(&g_watch2, id2, __ATOMIC_RELEASE);
    __atomic_store_n(&g_done, 0, __ATOMIC_RELEASE);
    auto *sup = TaskControlBlock::create(
        []() {
            uint64_t w1 = __atomic_load_n(&g_watch1, __ATOMIC_ACQUIRE);
            uint64_t w2 = __atomic_load_n(&g_watch2, __ATOMIC_ACQUIRE);
            uint64_t r1 = death_syscall(
                static_cast<uint64_t>(SyscallNumber::DEATH_WATCH), w1, 0, 0,
                0);
            uint64_t r2 = death_syscall(
                static_cast<uint64_t>(SyscallNumber::DEATH_WATCH), w2, 0, 0,
                0);
            __atomic_store_n(&g_done, (r1 == 0 && r2 == 0) ? 1U : 0U,
                             __ATOMIC_RELEASE);
            for (;;)
                kernel::Scheduler::reschedule();
        },
        53, 10);
    JARVIS_ASSERT(sup != nullptr);
    Scheduler::add_task(*sup);
    Scheduler::reschedule();

    // Drive until S has registered both watches (bounded).
    for (int i = 0; i < 10000 && __atomic_load_n(&g_done, __ATOMIC_ACQUIRE) == 0;
         ++i) {
        __atomic_store_n(&scheduler_need_resched, true, __ATOMIC_RELEASE);
        arch::hlt();
    }
    JARVIS_ASSERT_EQ(1U, __atomic_load_n(&g_done, __ATOMIC_ACQUIRE));
    JARVIS_ASSERT_EQ(base + 2, ipc::DeathNotify::live_count());

    // Kill the helpers: records become PENDING for S.
    Scheduler::terminate(*h1, 1);
    Scheduler::terminate(*h2, 2);
    Scheduler::drain_zombie_list();
    JARVIS_ASSERT_EQ(base + 2, ipc::DeathNotify::live_count()); // PENDING

    // S dies: its cleanup() drains the two PENDING records.  Use
    // terminate_and_drain (safe: skips already-dead, checks magic first).
    kernel::test::terminate_and_drain(*sup);
    JARVIS_ASSERT_EQ(base, ipc::DeathNotify::live_count());
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: The registry fails closed when exhausted, and a freed slot admits
// a new watch.
// Input: Fill all CONFIG_CAP_MAX_DEATH_WATCHES slots with distinct helpers;
//        the next watch fails; consume one record; a new watch succeeds.
// Expect: exhaustion fails closed; consumption frees a slot.
// Depends: bounded registry, fail-closed watch()
JARVIS_TEST(death_watch_registry_full_fails_closed, "PRE: none | POST: none") {
    auto *cur = Scheduler::current_task();
    JARVIS_ASSERT(cur != nullptr);

    constexpr size_t kMax = CONFIG_CAP_MAX_DEATH_WATCHES;
    uint64_t ids[kMax];
    TaskControlBlock *helpers[kMax];
    size_t base = ipc::DeathNotify::live_count();
    for (size_t i = 0; i < kMax; ++i) {
        helpers[i] = make_parked(60 + static_cast<int>(i), &ids[i]);
        JARVIS_ASSERT(helpers[i] != nullptr);
        JARVIS_ASSERT(ids[i] != 0);
    }
    Scheduler::reschedule();

    for (size_t i = 0; i < kMax; ++i) {
        JARVIS_ASSERT_EQ(0ULL,
                         death_syscall(
                             static_cast<uint64_t>(SyscallNumber::DEATH_WATCH),
                             ids[i], 0, 0, 0));
    }
    JARVIS_ASSERT_EQ(base + kMax, ipc::DeathNotify::live_count());

    // Exhausted: one more watch fails closed.
    uint64_t extra_id = 0;
    auto *extra = make_parked(80, &extra_id);
    JARVIS_ASSERT(extra != nullptr);
    uint64_t r = death_syscall(
        static_cast<uint64_t>(SyscallNumber::DEATH_WATCH), extra_id, 0, 0, 0);
    JARVIS_ASSERT_EQ(static_cast<uint64_t>(-1), r);
    JARVIS_ASSERT_EQ(base + kMax, ipc::DeathNotify::live_count());

    // Consume one record -> a slot frees -> a new watch succeeds.
    Scheduler::terminate(*helpers[0], 0);
    Scheduler::drain_zombie_list();
    ipc::DeathRecord rec{};
    JARVIS_ASSERT_EQ(1, ipc::DeathNotify::recv(*cur, rec));
    JARVIS_ASSERT_EQ(base + kMax - 1, ipc::DeathNotify::live_count());

    r = death_syscall(static_cast<uint64_t>(SyscallNumber::DEATH_WATCH),
                      extra_id, 0, 0, 0);
    JARVIS_ASSERT_EQ(0ULL, r);
    JARVIS_ASSERT_EQ(base + kMax, ipc::DeathNotify::live_count());

    // Teardown: kill the remaining helpers (safe, idempotent).
    for (size_t i = 0; i < kMax; ++i)
        kernel::test::terminate_if_live(helpers[i]);
    kernel::test::terminate_if_live(extra);
    Scheduler::drain_zombie_list();
    // Consume the remaining PENDING records so the ResourceTracker
    // death_watches counter is zero-delta at the test boundary: the leak
    // check in snapshot_restore runs BEFORE the registry snapshot_reset, so
    // leftover records would fail the check once death_watches is verified.
    ipc::DeathRecord drain{};
    while (ipc::DeathNotify::recv(*cur, drain) == 1) {
    }
    JARVIS_ASSERT_EQ(base, ipc::DeathNotify::live_count());
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: A single death yields exactly one record, even after repeated
// zombie drains.
// Input: Watch a helper; terminate it; drain the zombie list repeatedly; then
//        drain records.
// Expect: exactly one record; slot freed after consume.
// Depends: exactly-once latch (cleanup() single funnel)
JARVIS_TEST(death_watch_exactly_once, "PRE: none | POST: none") {
    auto *cur = Scheduler::current_task();
    JARVIS_ASSERT(cur != nullptr);
    uint64_t hid = 0;
    auto *h = make_parked(71, &hid);
    JARVIS_ASSERT(h != nullptr);
    JARVIS_ASSERT(hid != 0);
    Scheduler::reschedule();

    size_t base = ipc::DeathNotify::live_count();
    JARVIS_ASSERT_EQ(0ULL, death_syscall(
        static_cast<uint64_t>(SyscallNumber::DEATH_WATCH), hid, 0, 0, 0));
    Scheduler::terminate(*h, 7);
    Scheduler::drain_zombie_list();
    Scheduler::drain_zombie_list();

    ipc::DeathRecord rec{};
    JARVIS_ASSERT_EQ(1, ipc::DeathNotify::recv(*cur, rec));
    JARVIS_ASSERT_EQ(hid, rec.dead_id);
    JARVIS_ASSERT_EQ(0, ipc::DeathNotify::recv(*cur, rec)); // no duplicate
    JARVIS_ASSERT_EQ(base, ipc::DeathNotify::live_count());
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: SYS_DEATH_UNWATCH before the watched task dies suppresses the
// notification entirely.
// Input: Watch a helper; unwatch it; terminate the helper.
// Expect: no record; registry baseline.
// Depends: SYS_DEATH_UNWATCH
JARVIS_TEST(death_watch_unwatch_before_death, "PRE: none | POST: none") {
    auto *cur = Scheduler::current_task();
    JARVIS_ASSERT(cur != nullptr);
    uint64_t hid = 0;
    auto *h = make_parked(82, &hid);
    JARVIS_ASSERT(h != nullptr);
    JARVIS_ASSERT(hid != 0);
    Scheduler::reschedule();

    size_t base = ipc::DeathNotify::live_count();
    JARVIS_ASSERT_EQ(0ULL, death_syscall(
        static_cast<uint64_t>(SyscallNumber::DEATH_WATCH), hid, 0, 0, 0));
    JARVIS_ASSERT_EQ(base + 1, ipc::DeathNotify::live_count());
    JARVIS_ASSERT_EQ(0ULL, death_syscall(
        static_cast<uint64_t>(SyscallNumber::DEATH_UNWATCH), hid, 0, 0, 0));
    JARVIS_ASSERT_EQ(base, ipc::DeathNotify::live_count());

    Scheduler::terminate(*h, 9);
    Scheduler::drain_zombie_list();
    ipc::DeathRecord rec{};
    JARVIS_ASSERT_EQ(0, ipc::DeathNotify::recv(*cur, rec));
    JARVIS_ASSERT_EQ(base, ipc::DeathNotify::live_count());
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: SYS_DEATH_RECV with nothing pending returns 0 immediately (the
// supervisor never wedges).
// Input: Call recv with no watches/pending records.
// Expect: 0, registry unchanged.
// Depends: non-blocking recv
JARVIS_TEST(death_recv_nonblocking_none, "PRE: none | POST: none") {
    auto *cur = Scheduler::current_task();
    JARVIS_ASSERT(cur != nullptr);
    size_t base = ipc::DeathNotify::live_count();
    ipc::DeathRecord rec{};
    int got = ipc::DeathNotify::recv(*cur, rec);
    JARVIS_ASSERT_EQ(0, got);
    JARVIS_ASSERT_EQ(base, ipc::DeathNotify::live_count());
    JARVIS_TEST_PASS();
}

void register_cap_death_tests() {
    Logger::info("Registering cap_death tests");
    JARVIS_REGISTER_TEST(death_watch_consume_roundtrip);
    JARVIS_REGISTER_TEST(death_watch_crash_reason);
    JARVIS_REGISTER_TEST(death_watch_multi_fan_in);
    JARVIS_REGISTER_TEST(death_watch_after_death_rejected);
    JARVIS_REGISTER_TEST(death_watch_supervisor_death_drains);
    JARVIS_REGISTER_TEST(death_watch_registry_full_fails_closed);
    JARVIS_REGISTER_TEST(death_watch_exactly_once);
    JARVIS_REGISTER_TEST(death_watch_unwatch_before_death);
    JARVIS_REGISTER_TEST(death_recv_nonblocking_none);
}