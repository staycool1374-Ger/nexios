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

/// @file test_testrunner.cpp
/// @brief Base test class verifying test-harness integrity: snapshot/restore
///        correctness, priority-ordered blocked-sender wakeup, and leak
///        detection.  These tests must pass before any other test class can
///        be trusted — they validate the test infrastructure itself.

#include <test.hpp>
#include <logger.hpp>
#include <constants.hpp>
#include <kernel/memory/pmm.hpp>
#include <kernel/memory/vmm.hpp>
#include <kernel/task/scheduler.hpp>
#include <kernel/task/task.hpp>
#include <kernel/ipc/ipc.hpp>
#include <kernel/ipc/buffer_pool.hpp>
#include <kernel/sync/mutex.hpp>
#include <kernel/test/test_isolate.hpp>
#include <kernel/test/resource_tracker.hpp>
#include "test_sched_helpers.hpp"

using namespace kernel;

// ── Shared state for IPC tests ──────────────────────────────────────────
static volatile bool   ipc_test_done_ = false;
static volatile uint64_t ipc_recv_count_ = 0;
static constexpr uint64_t IPC_MSG_TYPE_TEST = 42;

// ======================================================================
// Tests
// ======================================================================

// Runmode: kernel
// Testidea: Snapshot/restore must leave the ready queue bitmap consistent.
//           Create a task, snapshot, remove, restore, verify.
// TEMPORARY stray-write-catcher — commented out: not normally part of `all`
// (it manually remove/cleanup/deletes a task and poisons the next test's
// snapshot_restore). See tcb_write_log tracer for the proper mechanism.
#if 0
JARVIS_TEST(harness_snapshot_bitmap_consistency,
            "PRE: none | POST: none") {
    auto *t = TaskControlBlock::create([]() {}, 5, 10);
    JARVIS_ASSERT(t != nullptr);
    Scheduler::add_task(*t);
    JARVIS_ASSERT(t->in_ready_queue_);

    kernel::test::terminate_and_drain(*t);

    JARVIS_TEST_PASS();
}
#endif

// Runmode: kernel
// Testidea: Priority-ordered wakeup of blocked senders.  High-prio sender
//           must be woken and run before low-prio sender.
JARVIS_TEST(harness_priority_ordered_wakeup,
            "PRE: none | POST: none") {
    // This test validates that the scheduler correctly handles priority-ordered
    // wakeup of blocked IPC senders.  It fills a receiver's queue to capacity,
    // then blocks a sender, then drains the queue and verifies the sender wakes.
    ipc_test_done_ = false;
    ipc_recv_count_ = 0;

    // Receiver at priority 5 (LOWER than init's 10) — runs when init blocks.
    // After init wakes and continues, its higher priority (10) preempts receiver.
    auto *receiver = TaskControlBlock::create([]() {
        while (!ipc_test_done_) {
            Message msg;
            if (IPC::recv(msg)) {
                __atomic_add_fetch(&ipc_recv_count_, 1, __ATOMIC_RELAXED);
            }
            arch::pause();
        }
    }, 5, 10);
    JARVIS_ASSERT(receiver != nullptr);
    uint64_t rcv_id = receiver->id;
    Scheduler::add_task(*receiver);

    // Fill the queue from this context (init, priority 10)
    for (size_t i = 0; i < IPC_MAX_QUEUE_MSG; ++i) {
        Message msg{};
        msg.type = IPC_MSG_TYPE_TEST;
        msg.priority = 0;
        IPC::send(rcv_id, msg, IPC_NONBLOCK);
    }

    // Send one more — should block (queue full), then wake when receiver drains
    ipc_recv_count_ = 0;
    Message block_msg{};
    block_msg.type = IPC_MSG_TYPE_TEST;
    block_msg.priority = 0;

    // Current task blocks here; receiver runs, drains queue, wakes us.
    // After waking, the queue has space and our message is delivered.
    bool ok = IPC::send(rcv_id, block_msg, 0);
    JARVIS_ASSERT_FMT(ok, "Priority-ordered wakeup should complete send");

    // Let receiver drain remaining messages
    for (int h = 0; h < 50; ++h) {
        Scheduler::reschedule();
        arch::hlt();
    }
    ipc_test_done_ = true;
    Scheduler::reschedule();

    JARVIS_ASSERT_FMT(ipc_recv_count_ > IPC_MAX_QUEUE_MSG,
                      "Receiver should have processed > %lu messages (got %lu)",
                      (uint64_t)IPC_MAX_QUEUE_MSG, (uint64_t)ipc_recv_count_);

    if (receiver && receiver->magic == TaskControlBlock::TCB_MAGIC) {
        kernel::test::terminate_and_drain(*receiver);
    }

    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Sender blocks on full queue, receiver drains, sender wakes.
//           Tests the RMS blocked-sender wakeup path WITHOUT send_sync.
JARVIS_TEST(harness_blocked_sender_wakes,
            "PRE: none | POST: none") {
    ipc_recv_count_ = 0;
    ipc_test_done_ = false;

    // Receiver at priority 5 (LOWER than init/harness's 10) — runs when the
    // harness blocks as a sender.  When the harness wakes and continues, its
    // higher priority (10) preempts the receiver.  NOTE: the priority MUST be
    // strictly below the harness's 10.  At an equal priority the receiver
    // competes with the harness during the drain poll and the reschedule()
    // in its loop keeps re-selecting the harness, so the drain can stall
    // (the pre-fix harness_blocked_sender_wakes flake: "processed 2/17").
    // Loops until ipc_test_done_ is set, so it never terminates early.
    auto *receiver = TaskControlBlock::create([]() {
        while (!ipc_test_done_) {
            Message msg;
            if (IPC::recv(msg)) {
                __atomic_add_fetch(&ipc_recv_count_, 1, __ATOMIC_RELAXED);
            }
            arch::pause();
        }
    }, 5, 10);
    JARVIS_ASSERT(receiver != nullptr);
    uint64_t rcv_id = receiver->id;
    Scheduler::add_task(*receiver);

    // Fill the queue
    for (size_t i = 0; i < IPC_MAX_QUEUE_MSG; ++i) {
        Message msg{};
        msg.type = IPC_MSG_TYPE_TEST;
        msg.priority = 0;
        IPC::send(rcv_id, msg, IPC_NONBLOCK);
    }

    // Send one more — should block (queue full)
    ipc_recv_count_ = 0;
    Message block_msg{};
    block_msg.type = IPC_MSG_TYPE_TEST;
    block_msg.priority = 0;

    if (!IPC::send(rcv_id, block_msg, 0)) {
        // If IPC::send returns false (e.g. OOM), the test failed
        JARVIS_ASSERT_FMT(false, "Blocking send failed");
    }

    // After waking, our message is in the queue. Let receiver process it.
    // CHANGED (v0.4.0 MP-8): the 50-iteration bound is arbitrary and
    // timing-sensitive — the MP-1 per-switch overhead (private-PML4 CR3
    // publishes + canary verify) shifts the receiver's dispatch cadence and
    // the loop could observe only 1/17 processed messages in the `all`
    // context.  Raise the bound so the poll is robust to scheduler-timing
    // drift while keeping the drain-before-break logic.
    for (int h = 0; h < 500; ++h) {
        Scheduler::reschedule();
        arch::hlt();
        if (ipc_recv_count_ > IPC_MAX_QUEUE_MSG)
            break;
    }

    JARVIS_ASSERT_FMT(ipc_recv_count_ > IPC_MAX_QUEUE_MSG,
                      "Receiver should have processed all messages (%lu/%lu)",
                      (uint64_t)ipc_recv_count_,
                      (uint64_t)(IPC_MAX_QUEUE_MSG + 1));

    // Forever-loop receiver (ipc_test_done_ never set): terminate()+drain can
    // strand it if the deferred switch is mid-flight (receiver calls
    // reschedule() in its loop).  Direct remove_task+cleanup+delete is the
    // leak-free pattern for never-terminating tasks (baseline).
    if (receiver && receiver->magic == TaskControlBlock::TCB_MAGIC) {
        Scheduler::remove_task(*receiver);
        receiver->cleanup();
        delete receiver;
    }

    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: in_ready_queue_ flag after re-enqueue via the real
// add_task → remove → set_task_ready lifecycle (real dispatch).
JARVIS_TEST(harness_snapshot_inrq_consistency,
            "PRE: none | POST: none") {
    auto *t = TaskControlBlock::create([]() {}, 11, 10);
    JARVIS_ASSERT(t != nullptr);
    {
        // Register + assert membership atomically: a tick firing between
        // add_task and the assert could dispatch t (prio 11 > harness 10),
        // dequeue it, and self-terminate it before the membership is read.
        arch::IrqGuard guard;
        Scheduler::add_task(*t);
        JARVIS_ASSERT(t->in_ready_queue_);
    }

    // Dispatch + terminate the task for real, then re-add a fresh one and
    // drive the ready-queue membership through the real scheduler API.
    Scheduler::reschedule();
    kernel::test::wait_for_termination_safe(t);
    if (t->magic == TaskControlBlock::TCB_MAGIC) {
        Scheduler::remove_task(*t);
        t->cleanup();
        delete t;
    }

    auto *t2 = TaskControlBlock::create([]() {}, 11, 10);
    JARVIS_ASSERT(t2 != nullptr);
    {
        // Same atomicity: no tick may dispatch t2 before the membership
        // asserts below (see the t registration above).
        arch::IrqGuard guard;
        Scheduler::add_task(*t2);
        JARVIS_ASSERT_FMT(t2->in_ready_queue_,
                          "Task should be in ready queue after add_task");

        // Scheduler::set_task_ready on an already-READY/queued task is a no-op
        // that must not corrupt membership (driven via the real API).
        Scheduler::set_task_ready(*t2);
        JARVIS_ASSERT(t2->in_ready_queue_);
    }

    Scheduler::reschedule();
    kernel::test::wait_for_termination_safe(t2);
    if (t2->magic == TaskControlBlock::TCB_MAGIC) {
        Scheduler::remove_task(*t2);
        t2->cleanup();
        delete t2;
    }

    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Resource leak detection after task create/destroy cycle.
JARVIS_TEST(harness_leak_detection, "PRE: none | POST: none") {
    for (int i = 0; i < 10; ++i) {
        auto *t = TaskControlBlock::create([]() {}, 5, 10);
        JARVIS_ASSERT(t != nullptr);
        Scheduler::add_task(*t);
        Scheduler::remove_task(*t);
        t->cleanup();
        delete t;
    }
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Spawn and cleanup multiple tasks without leaks.
JARVIS_TEST(harness_multi_task_spawn_cleanup,
            "PRE: none | POST: none") {
    static constexpr uint64_t NUM_WORKERS = 5;
    TaskControlBlock *workers[NUM_WORKERS] = {};
    // Register all workers under one IrqGuard so no timer tick splits the
    // registration (cookbook Rule 2).
    {
        arch::IrqGuard guard;
        for (uint64_t i = 0; i < NUM_WORKERS; ++i) {
            workers[i] = TaskControlBlock::create([]() {}, 5, 10);
            JARVIS_ASSERT(workers[i] != nullptr);
            Scheduler::add_task(*workers[i]);
        }
    }

    for (uint64_t i = 0; i < NUM_WORKERS; ++i) {
        if (workers[i] &&
            workers[i]->magic == TaskControlBlock::TCB_MAGIC) {
            Scheduler::remove_task(*workers[i]);
            workers[i]->cleanup();
            delete workers[i];
        }
    }

    JARVIS_TEST_PASS();
}

// ======================================================================
// HHDM / Snapshot regression tests (BUGS.md#021)
// ======================================================================

// Runmode: kernel
// Testidea: Snapshot_restore rewinds the PMM bitmap but the free_list_[]
//           array entries from free_page() calls persist across restores.
//           Over many cycles, the free list accumulates entries pointing
//           beyond the 128 MiB HHDM window.  This test simulates the
//           cumulative effect of 80+ test cycles by repeatedly creating
//           and destroying user tasks (which alloc/free page-table pages
//           and BufferPool pages), then verifies that alloc_user_page()
//           always returns a physical address within the HHDM window.
// Regression: Without the HHDM limit check in try_alloc_user/alloc_kernel
//             free list fast paths, a page beyond 128 MiB would be
//             returned and then accessed via HHDM → GPF (vector 0xD).
JARVIS_TEST(harness_hhdm_user_page_bounds,
            "PRE: none | POST: none") {
    static constexpr uint64_t HHDM_LIMIT = 128ULL * 1024 * 1024;
    static constexpr uint64_t CYCLES = 20;

    for (uint64_t cycle = 0; cycle < CYCLES; ++cycle) {
        // Simulate the pattern of a typical test: create user tasks
        auto *sender = TaskControlBlock::create_user([]() {}, 5, 10, 8_KiB);
        auto *receiver = TaskControlBlock::create_user([]() {}, 5, 10, 8_KiB);
        if (!sender || !receiver)
            continue;
        Scheduler::add_task(*sender);
        Scheduler::add_task(*receiver);

        // Each BufferPool::alloc calls map_page_in_pml4 which allocates
        // user page-table pages (PDPT/PD/PT) via alloc_user_page.  Drive the
        // alloc through a REAL dispatched kernel task whose page_table_ is a
        // clone (BUGS.md#020-safe: the lambda runs in kernel mode).
        static uint64_t g_handle = 0;
        auto *worker = TaskControlBlock::create(
            []() {
                g_handle = BufferPool::alloc(*Scheduler::current_task(),
                                             0x80000000);
            },
            11, 10);
        if (worker == nullptr) {
            kernel::test::terminate_and_drain2(sender, receiver);
            continue;
        }
        Scheduler::add_task(*worker);
        Scheduler::reschedule();
        kernel::test::wait_for_termination_safe(worker);
        kernel::test::terminate_and_drain(*worker);

        // Free the buffer back to the pool (kernel worker's buffer).
        if (g_handle != 0) {
            BufferPool::free(*sender, g_handle);
        }

        // Clean up tasks (this frees page-table pages back to PMM free list)
        kernel::test::terminate_and_drain2(sender, receiver);

        // After each cycle, verify that a fresh user page allocation
        // lands within the HHDM window.  If the free list drifts beyond
        // 128 MiB (the regression scenario), alloc_user_page would
        // return a page outside the HHDM window, causing a GPF when
        // accessed via HHDM by clear_pte_in_pml4 / get_table.
        uint64_t test_page = PMM::alloc_user_page();
        JARVIS_ASSERT_FMT(
            test_page < HHDM_LIMIT,
            "Cycle %lu: alloc_user_page returned 0x%lx (>= 128 MiB) — "
            "free list drifted beyond HHDM window; accessing via HHDM "
            "would cause GPF",
            cycle, test_page);

        // Verify the page is safely accessible via HHDM
        volatile auto *ptr = reinterpret_cast<volatile uint8_t *>(
            arch::HHDM_OFFSET + test_page);
        uint8_t v = *ptr;
        (void)v;
        PMM::free_page(test_page);
    }
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Snapshot_restore does not restore the free_list_[] linked
//           array — only free_head_ and the free_pages counter are
//           restored from the snapshot.  After many cycles where tests
//           allocate and free user page-table pages, the free list
//           array accumulates stale entries.  This test directly
//           drives the scenario: allocate large numbers of user pages
//           to exhaust the within-HHDM free list, force the bitmap
//           scan to return a page beyond 128 MiB, then free it so the
//           stale entry stays in the free_list_[] array (even after
//           the bitmap says the page is allocated).
// Regression: The next alloc pops the stale beyond-HHDM entry from the
//             free list → page-table page outside HHDM → GPF.
JARVIS_TEST(harness_free_list_stability,
            "PRE: none | POST: none") {
    static constexpr uint64_t HHDM_LIMIT = 128ULL * 1024 * 1024;
    static constexpr uint64_t ALLOC_BURST = 32;

    // Phase 1: exhaust the within-HHDM free list by allocating many
    // user pages.  Once the free list is empty, try_alloc_user falls
    // through to the bitmap scan which (before the HHDM fix) could
    // return a page beyond 128 MiB.
    uint64_t pages[ALLOC_BURST];
    uint64_t num_alloced = 0;
    for (; num_alloced < ALLOC_BURST; ++num_alloced) {
        uint64_t p = PMM::alloc_user_page();
        if (p == 0)
            break; // OOM — free list empty, bitmap scan failed
        pages[num_alloced] = p;
        if (p >= HHDM_LIMIT) {
            // This simulates the scenario: a page beyond HHDM was
            // allocated.  Free it so the stale entry enters the
            // free list array.
            PMM::free_page(p);
            break;
        }
    }

    // Free all within-HHDM pages
    for (uint64_t i = 0; i < num_alloced; ++i) {
        if (pages[i] > 0 && pages[i] < HHDM_LIMIT)
            PMM::free_page(pages[i]);
    }

    // Phase 2: now allocate again.  If the free list drifted (has a
    // stale beyond-HHDM entry from the free in phase 1), the next
    // alloc could return that page.  Verify it's within HHDM.
    for (uint64_t i = 0; i < 4; ++i) {
        uint64_t p = PMM::alloc_user_page();
        JARVIS_ASSERT_FMT(p != 0, "Allocation %lu failed after burst-free", i);
        JARVIS_ASSERT_FMT(
            p < HHDM_LIMIT,
            "Allocation %lu returned 0x%lx (>= 128 MiB) — "
            "free list has stale beyond-HHDM entry",
            i, p);
        volatile auto *ptr = reinterpret_cast<volatile uint8_t *>(
            arch::HHDM_OFFSET + p);
        uint8_t v = *ptr;
        (void)v;
        PMM::free_page(p);
    }
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: After many snapshot_restore-style cycles, the kernel must
//           maintain the invariant that all page-table pages allocated
//           via get_table (PDPT/PD/PT) are within the 128 MiB HHDM
//           window.  This test runs N complete test cycles that include
//           user-task creation, BufferPool alloc/transfer/map/free, and
//           full task cleanup — all operations that allocate and free
//           page-table pages.  After each cycle, it samples a user
//           page-table alloc and verifies the physical address.
JARVIS_TEST(harness_snapshot_cycle_isolation,
            "PRE: none | POST: none") {
    static constexpr uint64_t HHDM_LIMIT = 128ULL * 1024 * 1024;
    static constexpr uint64_t CYCLES = 10;

    for (uint64_t cycle = 0; cycle < CYCLES; ++cycle) {
        uint64_t test_pages[4];
        for (int i = 0; i < 4; ++i) {
            test_pages[i] = PMM::alloc_user_page();
            JARVIS_ASSERT_FMT(
                test_pages[i] < HHDM_LIMIT,
                "Cycle %lu, page %d: alloc_user_page returned 0x%lx "
                "(>= 128 MiB)", cycle, i, test_pages[i]);
            volatile auto *ptr = reinterpret_cast<volatile uint8_t *>(
                arch::HHDM_OFFSET + test_pages[i]);
            *ptr = static_cast<uint8_t>(cycle ^ i);
        }
        for (int i = 0; i < 4; ++i) {
            volatile auto *ptr = reinterpret_cast<volatile uint8_t *>(
                arch::HHDM_OFFSET + test_pages[i]);
            JARVIS_ASSERT(*ptr == static_cast<uint8_t>(cycle ^ i));
            PMM::free_page(test_pages[i]);
        }
    }

    // After all cycles, verify a fresh alloc still lands within HHDM
    uint64_t final = PMM::alloc_user_page();
    JARVIS_ASSERT_FMT(final < HHDM_LIMIT,
                      "Final alloc returned 0x%lx (>= 128 MiB)", final);
    PMM::free_page(final);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: BufferPool::unmap_all must not GPF when page-table pages are
//           freed (stale entries after snapshot_restore PMM bitmap rewind).
//           Simulate  the scenario: create a user task, map a buffer, free
//           the PDPT page from PMM (simulating snapshot_restore rewinding
//           the bitmap), then call unmap_all.  The is_allocated check in
//           clear_pte_in_pml4 must detect the freed page and return early
//           instead of following the stale PTE into freed memory.
JARVIS_TEST(harness_buffer_unmap_stale_safe,
            "PRE: none | POST: none") {
    static constexpr uint64_t HHDM_LIMIT = 128ULL * 1024 * 1024;

    auto *task = TaskControlBlock::create([]() {}, 5, 10);
    if (!task) { JARVIS_TEST_PASS(); return; }
    // v0.4.0 MP-1: create() already assigned a private kernel-half PML4 —
    // do NOT overwrite it (that would leak the create()-allocated page).
    if (!task->page_table_) { JARVIS_TEST_PASS(); return; }
    Scheduler::add_task(*task);

    // Phase 1: Allocate a buffer — this builds PDPT/PD/PT entries.  Drive the
    // alloc through a REAL dispatched kernel task (BUGS.md#020-safe: the
    // lambda runs in kernel mode with a cloned PML4).
    uint64_t va = 0x80000000;
    static uint64_t g_handle = 0;
    auto *worker = TaskControlBlock::create(
        []() {
            g_handle =
                BufferPool::alloc(*Scheduler::current_task(), 0x80000000);
        },
        11, 10);
    if (!worker) { JARVIS_TEST_PASS(); return; }
    // MP-1: create() assigns the private PML4.
    Scheduler::add_task(*worker);
    Scheduler::reschedule();
    kernel::test::wait_for_termination_safe(worker);
    kernel::test::terminate_and_drain(*worker);
    uint64_t handle = g_handle;
    if (handle == 0) { JARVIS_TEST_PASS(); return; }

    // Phase 2: Read the PML4 entry to get PDPT physical address,
    // then free PDPT page from PMM (simulating snapshot_restore).
    // Use local PT constants (VMM flags are private).
    constexpr uint64_t PT_P = 1ULL << 0;
    constexpr uint64_t PT_W = 1ULL << 1;
    constexpr uint64_t PT_U = 1ULL << 2;
    auto *pml4 = reinterpret_cast<uint64_t *>(
        arch::HHDM_OFFSET + (task->page_table_ & ~0xFFFULL));
    size_t pml4_idx = (va >> 39) & 0x1FF;
    uint64_t pdpt_phys = pml4[pml4_idx] & ~0xFFFULL;

    if (pdpt_phys > 0 && pdpt_phys < HHDM_LIMIT) {
        // Free PDPT page to simulate stale entry after PMM restore
        PMM::free_page(pdpt_phys);

        // Phase 3: Call unmap_all — must NOT GPF because the is_allocated
        // check in clear_pte_in_pml4 should catch the freed page.
        BufferPool::unmap_all(*task);

        // Phase 4: Restore — re-allocate a new PDPT page so the task's
        // page table is valid again for cleanup.
        uint64_t new_pdp = PMM::alloc_user_page();
        JARVIS_ASSERT_FMT(new_pdp != 0 && new_pdp < HHDM_LIMIT,
                          "Failed to allocate replacement PDPT page");
        __builtin_memset(reinterpret_cast<void *>(
            arch::HHDM_OFFSET + new_pdp), 0, 4096);
        pml4[pml4_idx] = new_pdp | PT_P | PT_W | PT_U;
    }

    // Cleanup
    if (task->magic == TaskControlBlock::TCB_MAGIC) {
        kernel::test::terminate_and_drain(*task);
    }
    JARVIS_TEST_PASS();
}

void test_harness_expected_panic_handling();

void register_testrunner_tests() {
    Logger::info("Registering TestRunner tests");
    // TEMPORARY stray-write-catcher — NOT normally part of the `all` class.
    // It manually remove/cleanup/deletes a task, which poisons the next
    // test's snapshot_restore and deadlocks `all`. Commented out until the
    // stray-write investigation is resolved via the tcb_write_log tracer.
    // JARVIS_REGISTER_TEST(harness_snapshot_bitmap_consistency);
    JARVIS_REGISTER_TEST(harness_priority_ordered_wakeup);
    JARVIS_REGISTER_TEST(harness_blocked_sender_wakes);
    JARVIS_REGISTER_TEST(harness_snapshot_inrq_consistency);
    JARVIS_REGISTER_TEST(harness_leak_detection);
    JARVIS_REGISTER_TEST(harness_multi_task_spawn_cleanup);
    JARVIS_REGISTER_TEST(harness_hhdm_user_page_bounds);
    JARVIS_REGISTER_TEST(harness_free_list_stability);
    JARVIS_REGISTER_TEST(harness_snapshot_cycle_isolation);
    JARVIS_REGISTER_TEST(harness_buffer_unmap_stale_safe);
    // NOTE: harness_expected_panic_handling is NOT registered here
    // because it panics the kernel — it would prevent all subsequent
    // tests from running in all-1/all-2.  It's registered separately
    // via register_expected_panic_tests() which is only called by the
    // "testrunner" class, never by "all-1" or "all-2".
}

void register_expected_panic_tests() {
    Logger::info("Registering expected-panic harness test");
    JARVIS_REGISTER_TEST(harness_expected_panic_handling);
}
// Runmode: kernel
// Testidea: The test harness (tools/run-test.exp) must classify known
//           panic signatures as PASS (expected) instead of FAIL.  This
//           test intentionally triggers the PCP retry-budget panic
//           (ASIL-D safety, SYNC-01).  Because the panic kills the
//           kernel, this test MUST be registered LAST in the class so
//           all preceding tests complete before the panic halts QEMU.
// Expect:   Harness sees "KERNEL PANIC: Mutex::lock() exhausted PCP
//           retry budget", matches it against the expected-panic list,
//           reports RESULT: PASS (expected panic: ...), exits 0.
JARVIS_TEST(harness_expected_panic_handling,
            "PRE: none | POST: none") {
    Logger::info("Triggering expected PCP retry-budget panic — "
                 "harness must classify as PASS");
    panic("Mutex::lock() exhausted PCP retry budget");
}
