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

/// @file test_atomic.cpp
/// @brief Atomic operation tests.

#include <test.hpp>
#include <logger.hpp>
#include <scope_guard.hpp>
#include <lib/atomic.hpp>
#include <constants.hpp>
#include <kernel/memory/pmm.hpp>
#include <kernel/task/scheduler.hpp>
#include <kernel/task/task.hpp>
#include <kernel/arch/io.hpp>
#include <kernel/test/test_sched_helpers.hpp>

using namespace kernel;

// ============================================================
// Test 1: Basic atomic operations (single-threaded)
// ============================================================

JARVIS_TEST(atomic_load_store, "PRE: none | POST: none") {
    uint32_t val = 42;
    kernel::atomic_store(&val, uint32_t(100));
    JARVIS_ASSERT_EQ(100U, kernel::atomic_load(&val));
    kernel::atomic_store(&val, uint32_t(0));
    JARVIS_ASSERT_EQ(0U, kernel::atomic_load(&val));
    JARVIS_TEST_PASS();
}

JARVIS_TEST(atomic_exchange, "PRE: none | POST: none") {
    uint32_t val = 42;
    uint32_t old = kernel::atomic_exchange(&val, uint32_t(99));
    JARVIS_ASSERT_EQ(42U, old);
    JARVIS_ASSERT_EQ(99U, kernel::atomic_load(&val));
    JARVIS_TEST_PASS();
}

JARVIS_TEST(atomic_compare_exchange, "PRE: none | POST: none") {
    uint32_t val = 42;
    uint32_t expected = 42;
    bool ok = kernel::atomic_compare_exchange(&val, &expected, uint32_t(99));
    JARVIS_ASSERT(ok);
    JARVIS_ASSERT_EQ(99U, kernel::atomic_load(&val));
    expected = 42;
    ok = kernel::atomic_compare_exchange(&val, &expected, uint32_t(0));
    JARVIS_ASSERT(!ok);
    JARVIS_ASSERT_EQ(99U, kernel::atomic_load(&val));
    JARVIS_TEST_PASS();
}

JARVIS_TEST(atomic_fetch_add, "PRE: none | POST: none") {
    uint32_t val = 10;
    uint32_t prev = kernel::atomic_fetch_add(&val, uint32_t(5));
    JARVIS_ASSERT_EQ(10U, prev);
    JARVIS_ASSERT_EQ(15U, kernel::atomic_load(&val));
    prev = kernel::atomic_fetch_add(&val, uint32_t(1));
    JARVIS_ASSERT_EQ(15U, prev);
    JARVIS_ASSERT_EQ(16U, kernel::atomic_load(&val));
    JARVIS_TEST_PASS();
}

JARVIS_TEST(atomic_fetch_sub, "PRE: none | POST: none") {
    uint32_t val = 100;
    uint32_t prev = kernel::atomic_fetch_sub(&val, uint32_t(30));
    JARVIS_ASSERT_EQ(100U, prev);
    JARVIS_ASSERT_EQ(70U, kernel::atomic_load(&val));
    JARVIS_TEST_PASS();
}

JARVIS_TEST(atomic_fetch_and, "PRE: none | POST: none") {
    uint32_t val = 0xFF;
    uint32_t prev = kernel::atomic_fetch_and(&val, uint32_t(0x0F));
    JARVIS_ASSERT_EQ(0xFFU, prev);
    JARVIS_ASSERT_EQ(0x0FU, kernel::atomic_load(&val));
    JARVIS_TEST_PASS();
}

JARVIS_TEST(atomic_fetch_or, "PRE: none | POST: none") {
    uint32_t val = 0xF0;
    uint32_t prev = kernel::atomic_fetch_or(&val, uint32_t(0x0F));
    JARVIS_ASSERT_EQ(0xF0U, prev);
    JARVIS_ASSERT_EQ(0xFFU, kernel::atomic_load(&val));
    JARVIS_TEST_PASS();
}

JARVIS_TEST(atomic_64bit_operations, "PRE: none | POST: none") {
    uint64_t val = 0xAAAAAAAAULL;
    kernel::atomic_store(&val, uint64_t(0xBBBBBBBBBBBBBBBBULL));
    JARVIS_ASSERT_EQ(0xBBBBBBBBBBBBBBBBULL, kernel::atomic_load(&val));
    uint64_t old =
        kernel::atomic_exchange(&val, uint64_t(0xCCCCCCCCCCCCCCCCULL));
    JARVIS_ASSERT_EQ(0xBBBBBBBBBBBBBBBBULL, old);
    JARVIS_ASSERT_EQ(0xCCCCCCCCCCCCCCCCULL, kernel::atomic_load(&val));
    old = kernel::atomic_fetch_add(&val, uint64_t(1));
    JARVIS_ASSERT_EQ(0xCCCCCCCCCCCCCCCCULL, old);
    JARVIS_ASSERT_EQ(0xCCCCCCCCCCCCCCCDULL, kernel::atomic_load(&val));
    JARVIS_TEST_PASS();
}

// ============================================================
// Test 2: Store-buffering litmus test (SB)
// ============================================================
// Classic SB: two tasks each store to one variable and load from the other.
// Under sequential consistency, both seeing 0 is forbidden.
//   Initially: x = 0, y = 0
//   Task 1: x = 1; fence; r1 = y;
//   Task 2: y = 1; fence; r2 = x;
// Forbidden: r1 == 0 && r2 == 0

static volatile uint32_t *g_sb_x;
static volatile uint32_t *g_sb_y;
static uint32_t g_sb_r1;
static uint32_t g_sb_r2;

static void sb_worker_a() {
    kernel::atomic_store(const_cast<volatile uint32_t *>(g_sb_x), uint32_t(1));
    kernel::atomic_fence();
    g_sb_r1 = kernel::atomic_load(const_cast<volatile uint32_t *>(g_sb_y));
}

static void sb_worker_b() {
    kernel::atomic_store(const_cast<volatile uint32_t *>(g_sb_y), uint32_t(1));
    kernel::atomic_fence();
    g_sb_r2 = kernel::atomic_load(const_cast<volatile uint32_t *>(g_sb_x));
}

JARVIS_TEST(atomic_sb_litmus, "PRE: none | POST: none") {
    static constexpr uint64_t SB_ITER = 10;

    uint64_t x_phys = PMM::alloc_page();
    uint64_t y_phys = PMM::alloc_page();
    JARVIS_ASSERT(x_phys != 0 && y_phys != 0);
    auto page_guard = ScopeGuard([&]() {
        PMM::free_page(x_phys);
        PMM::free_page(y_phys);
    });

    g_sb_x = reinterpret_cast<volatile uint32_t *>(arch::HHDM_OFFSET + x_phys);
    g_sb_y = reinterpret_cast<volatile uint32_t *>(arch::HHDM_OFFSET + y_phys);

    uint64_t forbidden_count = 0;

    for (uint64_t iter = 0; iter < SB_ITER; ++iter) {
        if (iter % 5 == 0) {
            Logger::raw_write("[SB] iter ");
            Logger::print_dec(iter + 1);
            Logger::raw_write("/");
            Logger::print_dec(SB_ITER);
            Logger::raw_write("\n");
        }
        *const_cast<volatile uint32_t *>(g_sb_x) = 0;
        *const_cast<volatile uint32_t *>(g_sb_y) = 0;
        kernel::atomic_fence();
        g_sb_r1 = 99;
        g_sb_r2 = 99;

        // Workers must outrank the harness (prio 10) so the ISR epilogue
        // dispatches them via the real ready-queue path.  Prio 11 is the
        // driven-test floor (ipc_blocking uses 11/12) and stays below the
        // vfsd/iocd daemons (20) and the deadline monitor (127).
        auto *task_a = TaskControlBlock::create(sb_worker_a, 11, 10);
        JARVIS_ASSERT(task_a != nullptr);
        auto *task_b = TaskControlBlock::create(sb_worker_b, 11, 10);
        JARVIS_ASSERT(task_b != nullptr);

        // Defensive net for early returns: terminate only tasks still live
        // (terminate() on a self-terminated task double-inserts into the
        // zombie list), then reclaim.  Guard is dismissed right after the
        // drain below — the sentinel asserts that follow may `return` on
        // failure, and a live guard would dereference the freed TCBs.
        auto cleanup = ScopeGuard([&]() {
            if (task_a && task_a->state != TaskState::TERMINATED) {
                Scheduler::terminate(*task_a, 0);
            }
            if (task_b && task_b->state != TaskState::TERMINATED) {
                Scheduler::terminate(*task_b, 0);
            }
            Scheduler::drain_zombie_list();
        });

        {
            arch::IrqGuard guard; // register atomically (cookbook Rule 2)
            Scheduler::add_task(*task_a);
            Scheduler::add_task(*task_b);
        }

        // Real dispatch: 11 > harness 10, so reschedule() + the next timer
        // tick run task_a, then task_b (FIFO).  Each self-terminates via
        // _task_trampoline; terminate() arms the switch to the next
        // highest-priority ready task, returning to the harness last.
        Scheduler::reschedule();
        kernel::test::wait_for_termination_safe(task_a);
        kernel::test::wait_for_termination_safe(task_b);
        // Reclaim BEFORE any assertion (cookbook Rule 5).  Both workers
        // self-terminated — drain only, never terminate() twice.  Dismiss the
        // guard here: the sentinel asserts below `return` on failure, and a
        // live guard would deref the just-freed TCBs (use-after-free).
        Scheduler::drain_zombie_list();
        cleanup.dismiss();

        // Sentinel 99 survives only if a worker body never ran (vacuous-pass
        // regression guard).  TCBs are freed above — assert globals only.
        JARVIS_ASSERT(g_sb_r1 != 99);
        JARVIS_ASSERT(g_sb_r2 != 99);

        if (g_sb_r1 == 0 && g_sb_r2 == 0) {
            ++forbidden_count;
        }
    }

    JARVIS_ASSERT_FMT(
        forbidden_count == 0,
        "SB litmus: forbidden (r1==0 && r2==0) occurred %lu/%lu times",
        forbidden_count, SB_ITER);
    JARVIS_TEST_PASS();
}

// ============================================================
// Test 3: Message-passing litmus test (MP) — inline
// ============================================================
// Under SC, r1 must be 42 after data+flag sync.
// Run producer → consumer inline without scheduler preemption.
// The barriers + atomics are still exercised.

JARVIS_TEST(atomic_mp_litmus, "PRE: none | POST: none") {
    uint64_t flag_phys = PMM::alloc_page();
    uint64_t data_phys = PMM::alloc_page();
    JARVIS_ASSERT(flag_phys != 0 && data_phys != 0);

    auto page_guard = ScopeGuard([&]() {
        PMM::free_page(flag_phys);
        PMM::free_page(data_phys);
    });

    auto *flag =
        reinterpret_cast<volatile uint64_t *>(arch::HHDM_OFFSET + flag_phys);
    auto *data = reinterpret_cast<uint64_t *>(arch::HHDM_OFFSET + data_phys);
    *const_cast<volatile uint64_t *>(flag) = 0;
    *data = 0;
    kernel::atomic_fence();

    // producer
    kernel::atomic_store(data, uint64_t(42));
    kernel::atomic_fence();
    kernel::atomic_store(const_cast<volatile uint64_t *>(flag), uint64_t(1));

    // consumer
    JARVIS_ASSERT(kernel::atomic_load(const_cast<volatile uint64_t *>(flag)) ==
                  1);
    kernel::atomic_fence();
    uint64_t observed = kernel::atomic_load(data);

    JARVIS_ASSERT_EQ(42ULL, observed);

    JARVIS_TEST_PASS();
}

// ============================================================
// Test 4: Atomic increment (bulk, inline)
// ============================================================
// Bulk atomic_fetch_add — run inline, no scheduler concurrency.
// The atomicity of fetch_add is already covered by Test 2;
// this verifies repeated operations don't corrupt.

JARVIS_TEST(atomic_increment_race, "PRE: none | POST: none") {
    uint64_t counter_phys = PMM::alloc_page();
    JARVIS_ASSERT(counter_phys != 0);
    auto page_guard = ScopeGuard([&]() { PMM::free_page(counter_phys); });

    auto *inc_counter =
        reinterpret_cast<uint64_t *>(arch::HHDM_OFFSET + counter_phys);
    *inc_counter = 0;
    kernel::atomic_fence();

    for (uint64_t i = 0; i < 1000; ++i) {
        kernel::atomic_fetch_add(inc_counter, uint64_t(1));
    }

    JARVIS_ASSERT_EQ(1000ULL, *inc_counter);

    JARVIS_TEST_PASS();
}

// ============================================================
// Test 5: Memory fence acquire/release (ping-pong, inline)
// ============================================================
// Ping-pong a turn variable using acquire/release semantics inline.
// No scheduler concurrency; the barriers are still exercised.

JARVIS_TEST(atomic_acquire_release_pingpong, "PRE: none | POST: none") {
    uint64_t turn_phys = PMM::alloc_page();
    uint64_t data1_phys = PMM::alloc_page();
    uint64_t data2_phys = PMM::alloc_page();
    JARVIS_ASSERT(turn_phys != 0 && data1_phys != 0 && data2_phys != 0);

    auto page_guard = ScopeGuard([&]() {
        PMM::free_page(turn_phys);
        PMM::free_page(data1_phys);
        PMM::free_page(data2_phys);
    });

    auto *turn = reinterpret_cast<uint64_t *>(arch::HHDM_OFFSET + turn_phys);
    auto *data1 = reinterpret_cast<uint64_t *>(arch::HHDM_OFFSET + data1_phys);
    auto *data2 = reinterpret_cast<uint64_t *>(arch::HHDM_OFFSET + data2_phys);
    *turn = 0;
    *data1 = 0;
    *data2 = 0;
    kernel::atomic_fence();

    int rounds_a = 0;
    int rounds_b = 0;

    for (int i = 0; i < 20; ++i) {
        // worker A
        JARVIS_ASSERT(kernel::atomic_load(turn) == 0);
        kernel::atomic_acquire_fence();
        *data1 = kernel::atomic_load(data2) + 1;
        kernel::atomic_release_fence();
        kernel::atomic_store(turn, uint64_t(1));
        ++rounds_a;

        // worker B
        JARVIS_ASSERT(kernel::atomic_load(turn) == 1);
        kernel::atomic_acquire_fence();
        *data2 = kernel::atomic_load(data1) + 1;
        kernel::atomic_release_fence();
        kernel::atomic_store(turn, uint64_t(0));
        ++rounds_b;
    }

    JARVIS_ASSERT_EQ(20, rounds_a);
    JARVIS_ASSERT_EQ(20, rounds_b);

    JARVIS_TEST_PASS();
}

// ============================================================
// Registration
// ============================================================

void register_atomic_tests() {
    Logger::info("Registering atomic tests");
    JARVIS_REGISTER_TEST(atomic_load_store);
    JARVIS_REGISTER_TEST(atomic_exchange);
    JARVIS_REGISTER_TEST(atomic_compare_exchange);
    JARVIS_REGISTER_TEST(atomic_fetch_add);
    JARVIS_REGISTER_TEST(atomic_fetch_sub);
    JARVIS_REGISTER_TEST(atomic_fetch_and);
    JARVIS_REGISTER_TEST(atomic_fetch_or);
    JARVIS_REGISTER_TEST(atomic_64bit_operations);
    JARVIS_REGISTER_TEST(atomic_sb_litmus);
    JARVIS_REGISTER_TEST(atomic_mp_litmus);
    JARVIS_REGISTER_TEST(atomic_increment_race);
    JARVIS_REGISTER_TEST(atomic_acquire_release_pingpong);
}
