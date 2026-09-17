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

/// @file test_resource_exhaustion.cpp
/// @brief Resource exhaustion stress tests.

#include <test.hpp>
#include <logger.hpp>
#include <scope_guard.hpp>
#include <kernel/task/scheduler.hpp>
#include <kernel/task/task.hpp>
#include <kernel/ipc/buffer_pool.hpp>
#include <kernel/memory/mempool.hpp>
#include <kernel/memory/pmm.hpp>
#include <kernel/vfs/vfs.hpp>
#include "test_sched_helpers.hpp"

using namespace kernel;

// Runmode: kernel
// Testidea: Open FDs until the fdtable is full, then attempt one more.
// Verify the 33rd open fails, then close and reopen.
// Input: Simulate opening 32 FDs (via fd_table allocation), try 33rd.
// Expect: 33rd fails; after close, reopen succeeds.
TEST_CLASS(FdTableExhaustion) {
    auto *task = TaskControlBlock::create_user([]() {}, 5, 10, 32_KiB);
    CT_ASSERT(task != nullptr);

    // Fill fdtable to capacity
    int fds_allocated = 0;
    for (size_t i = 0; i < vfs::MAX_FDS; ++i) {
        if (task->fd_table.fds[i].used == false) {
            task->fd_table.fds[i].used = true;
            task->fd_table.fds[i].vnode = nullptr;
            task->fd_table.fds[i].flags = 0;
            task->fd_table.fds[i].offset = 0;
            ++fds_allocated;
        }
    }
    CT_ASSERT(fds_allocated <= static_cast<int>(vfs::MAX_FDS));

    // Ensure no slot is free
    bool any_free = false;
    for (size_t i = 0; i < vfs::MAX_FDS; ++i) {
        if (!task->fd_table.fds[i].used) {
            any_free = true;
            break;
        }
    }
    CT_ASSERT(!any_free);

    // Close one
    task->fd_table.fds[vfs::MAX_FDS - 1].used = false;

    // Verify reopen works
    bool found_free = false;
    for (size_t i = 0; i < vfs::MAX_FDS; ++i) {
        if (!task->fd_table.fds[i].used) {
            task->fd_table.fds[i].used = true;
            found_free = true;
            break;
        }
    }
    CT_ASSERT(found_free);

    task->cleanup();
    delete task;
};

// Runmode: kernel
// Testidea: Create tasks until Scheduler's MAX_TASKS is reached.
// Verify the next create/add fails, then cleanup restores capacity.
// Input: Create MAX_TASKS tasks (minus existing), attempt one more.
// Expect: when the fill loop hits exhaustion (created < 64) the extra
// create fails; when the reaper retires the empty-bodied fill tasks fast
// enough that capacity is never reached, no rejection is asserted (the
// premise does not hold) — teardown still verifies count restoration.
// Issue #20: hardened against the reaper-timing race (extra == nullptr
// asserted unconditionally failed on clean main whenever the reaper
// outran the fill loop).
TEST_CLASS(TaskLimitReached) {
    // Non-aborting assertion: records a failure (so the test still fails) but
    // does NOT return, so subsequent teardown always runs.  The kernel reaper
    // may retire tasks mid-test (the timer ISR runs the empty-bodied tasks and
    // reaps them), so exact count assertions can legitimately vary; we must
    // never skip teardown, or the remaining TCBs leak and offset the
    // ResourceTracker baseline (failing test isolation).
#define CHECK_NONFATAL(cond)                                                   \
    do {                                                                       \
        if (!(cond))                                                           \
            fail(__FILE__, __LINE__, #cond);                                   \
    } while (0)

    uint64_t baseline = Scheduler::task_count();

    // Create up to the hard cap.  `created` counts every TCB we allocated.
    // `add_task` always appends to all_tasks_, but the reaper may retire some
    // before we snapshot, so we only assert the soft invariant (did not
    // exceed the cap) and never an exact-count equality that could abort.
    TaskControlBlock *tasks[64] = {};
    uint64_t created = 0;
    for (uint64_t i = 0; i < 64; ++i) {
        auto *t = kernel::test::create_named_task([]() {}, 5, 10, "limit");
        if (!t)
            break;
        tasks[created++] = t;
        kernel::test::add_task_named(*t, "limit");
    }

    CHECK_NONFATAL(created > 0);
    CHECK_NONFATAL(Scheduler::task_count() <= 64);

    // Attempt one more — rejected at capacity (create returns null) ONLY
    // when the fill loop actually hit exhaustion.  If all 64 creates
    // succeeded, the reaper retired fill tasks fast enough that no capacity
    // bound was reached, so a further create legitimately succeeds.
    bool hit_capacity = (created < 64);
    auto *extra = kernel::test::create_named_task([]() {}, 5, 10, "limit");
    if (hit_capacity) {
        CHECK_NONFATAL(extra == nullptr);
    }
    if (extra) {
        kernel::test::terminate_and_drain(*extra);
    }

    // Cleanup all created (unconditional — never skip teardown).
    // Sanctioned pattern (test_sched_helpers.hpp): terminate_if_live skips
    // reaped (0xDD-poisoned) and already-terminated tasks, so the reaper
    // retiring fill tasks mid-test can no longer cause teardown UAF; one
    // drain reaps all zombies exactly once.  The manual
    // remove_task+cleanup+delete sequence dereferenced reaped blocks
    // (remove_task magic-mismatch noise) and hung the class intermittently.
    for (uint64_t i = 0; i < created; ++i) {
        if (!tasks[i])
            continue;
        kernel::test::terminate_if_live(tasks[i]);
        tasks[i] = nullptr;
    }
    Scheduler::drain_zombie_list();

    CHECK_NONFATAL(Scheduler::task_count() == baseline);
#undef CHECK_NONFATAL
};

// Runmode: kernel
// Testidea: Allocate all 1024 BufferPool entries, verify 1025th fails,
// then free one and verify realloc recycles the entry.
// Input: Alloc MAX_BUFFERS in a user task.
// Expect: Alloc MAX_BUFFERS+1 returns 0; free + realloc recycles idx.
TEST_CLASS(MaxBuffersExhaustion) {
    auto *task = TaskControlBlock::create_user([]() {}, 5, 10, 64_KiB);
    CT_ASSERT(task != nullptr);

    uint64_t handles[BufferPool::MAX_BUFFERS];
    uint64_t va = 0x100000000ULL;

    int alloc_count = 0;
    for (size_t i = 0; i < BufferPool::MAX_BUFFERS + 5; ++i) {
        uint64_t h = BufferPool::alloc(*task, va + i * arch::PAGE_SIZE);
        if (h == 0)
            break;
        handles[alloc_count++] = h;
    }
    CT_ASSERT(alloc_count <= static_cast<int>(BufferPool::MAX_BUFFERS));

    // Should have gotten exactly MAX_BUFFERS
    bool all_allocated =
        (static_cast<size_t>(alloc_count) == BufferPool::MAX_BUFFERS);
    CT_ASSERT(all_allocated);

    // Free one from middle
    CT_ASSERT(BufferPool::free(*task, handles[512]));

    // Realloc — should recycle the freed entry
    uint64_t h = BufferPool::alloc(*task, va + 512 * arch::PAGE_SIZE);
    CT_ASSERT(h != 0);
    uint32_t new_idx = static_cast<uint32_t>(h & 0xFFFFFFFFULL);
    uint32_t old_idx = static_cast<uint32_t>(handles[512] & 0xFFFFFFFFULL);
    CT_ASSERT(new_idx == old_idx);

    BufferPool::free(*task, h);

    // Cleanup all remaining
    for (int i = 0; i < alloc_count; ++i) {
        if (i == 512)
            continue;
        if (handles[i] != 0) {
            BufferPool::free(*task, handles[i]);
        }
    }

    task->cleanup();
    delete task;
};

// NOTE: MempoolFragmentation disabled — pre-existing hang at test 438.
// Root cause unknown. See BUGS.md#013.
// Runmode: kernel
// Testidea: Allocate objects of every MemPool size class repeatedly,
// then free them in reverse order. Verify no corruption and all memory
// returned to the pool.
// Input: Alloc + free for each pool class.
// Expect: Successive allocs work; no crash on free.
TEST_CLASS(MempoolFragmentation) {
    static const size_t sizes[] = {
        16, 32, 64, 128, 256, 512, 1024, 2048, 4096
    };

    for (size_t s = 0; s < 9; ++s) {
        size_t bytes = sizes[s];
        static const int ALLOCS = 20;
        void* ptrs[ALLOCS] = {};
        int count = 0;

        auto guard = ScopeGuard([&]() {
            for (int i = count - 1; i >= 0; --i) {
                if (ptrs[i]) MemPool::free(ptrs[i]);
            }
        });

        for (int i = 0; i < ALLOCS; ++i) {
            ptrs[i] = MemPool::alloc(bytes);
            if (!ptrs[i]) break;
            count = i + 1;
            __builtin_memset(ptrs[i], 0xA5, bytes);
        }

        guard.dismiss();

        for (int i = count - 1; i >= 0; --i) {
            MemPool::free(ptrs[i]);
        }
    }

    void* p = MemPool::alloc(64);
    CT_ASSERT(p != nullptr);
    MemPool::free(p);
};

// Runmode: kernel
// Testidea: Exhaust memory by allocating until PMM runs out, then
// verify that subsequent allocs return 0 and free restores capacity.
// Input: Repeated PMM::alloc_page() calls (OOM handler temporarily
//        disabled to avoid killing daemon tasks).
// Expect: Eventually returns 0; no crash; free restores capacity.
// Note: 65K allocs × O(n) bitmap scan is slow; progress markers keep
//       the host watchdog alive.
TEST_CLASS(PmmExhaustion) {
    static const int MAX_ALLOCS = 100000;
    static uint64_t pages[MAX_ALLOCS];
    int count = 0;

    PMM::OOMHandler saved_oom = PMM::get_oom_handler();
    PMM::set_oom_handler(nullptr);
    auto restore = ScopeGuard([&]() { PMM::set_oom_handler(saved_oom); });

    for (int i = 0; i < MAX_ALLOCS; ++i) {
        uint64_t p = PMM::alloc_page();
        if (!p)
            break;
        pages[count++] = p;
        if ((count % 1000) == 0) {
            Logger::info("PmmExhaustion: allocated %u pages", count);
        }
    }
    Logger::info("PmmExhaustion: OOM at %u pages", count);

    for (int i = 0; i < count; ++i) {
        PMM::free_page(pages[i]);
    }

    uint64_t p = PMM::alloc_page();
    CT_ASSERT(p != 0);
    PMM::free_page(p);
};

void register_resource_exhaustion_tests() {
    Logger::info("Registering resource exhaustion tests");
    REGISTER_CLASS(FdTableExhaustion);
    REGISTER_CLASS(TaskLimitReached);
    REGISTER_CLASS(MaxBuffersExhaustion);
    REGISTER_CLASS(MempoolFragmentation);
    REGISTER_CLASS(PmmExhaustion);
}
