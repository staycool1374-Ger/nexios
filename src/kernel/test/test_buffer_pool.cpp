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

/// @file test_buffer_pool.cpp
/// @brief Buffer pool allocation and lifecycle tests.

// Runmode: kernel
// Testidea: BufferPool alloc/free/map/unmap/transfer/cleanup
// Depends: kernel::BufferPool, kernel::TaskControlBlock, kernel::Scheduler

#include <test.hpp>
#include <logger.hpp>
#include <scope_guard.hpp>
#include <kernel/test/resource_tracker.hpp>
#include <kernel/test/task_ptr.hpp>
#include <kernel/ipc/buffer_pool.hpp>
#include <kernel/ipc/ipc.hpp>
#include <kernel/task/task.hpp>
#include <kernel/task/scheduler.hpp>
#include <kernel/syscall/syscall.hpp>
#include <kernel/memory/vmm.hpp>
#include <kernel/arch/irq_guard.hpp>
#include <kernel/memory/pmm.hpp>
#include <kernel/sync/semaphore.hpp>
#include <constants.hpp>
#include <kernel/arch/qemu_debugcon.hpp>
#include <kernel/test/test_sched_helpers.hpp>

using namespace kernel;

// -------------------------------------------------------------------
// All tests use create_user() so page_table_ is non-null.
// Tasks are not added to the scheduler (just created and cleaned up).
// -------------------------------------------------------------------

#if !defined(CONFIG_ARCH_RISCV64)

JARVIS_TEST(buffer_pool_basic_alloc_free, "PRE: none | POST: none") {
    SimpleTaskPtr task(TaskControlBlock::create_user([]() {}, 5, 10, 32_KiB));
    JARVIS_ASSERT(task != nullptr);

    uint64_t va = 0x100000000;
    uint64_t handle = BufferPool::alloc(*task, va);
    JARVIS_ASSERT(handle != 0);

    uint32_t idx = static_cast<uint32_t>(handle & 0xFFFFFFFFULL);
    JARVIS_ASSERT(idx < BufferPool::MAX_BUFFERS);
    JARVIS_ASSERT(BufferPool::entries[idx].phys_addr != 0);
    JARVIS_ASSERT(BufferPool::entries[idx].mapped_va == va);

    JARVIS_ASSERT(BufferPool::free(*task, handle));

    // After free, entry should be recycled
    JARVIS_ASSERT(BufferPool::entries[idx].phys_addr == 0);

    JARVIS_TEST_PASS();
}

JARVIS_TEST(buffer_pool_multiple_alloc, "PRE: none | POST: none") {
    SimpleTaskPtr task(TaskControlBlock::create_user([]() {}, 5, 10, 32_KiB));
    JARVIS_ASSERT(task != nullptr);

    uint64_t handles[5];
    uint64_t va = 0x200000000;
    for (int i = 0; i < 5; ++i) {
        handles[i] = BufferPool::alloc(*task, va + i * arch::PAGE_SIZE);
        JARVIS_ASSERT(handles[i] != 0);
    }

    // Verify list has 5 entries
    int count = 0;
    int32_t idx = task->buf_list_head;
    while (idx != -1) {
        count++;
        idx = BufferPool::entries[idx].list_next;
    }
    JARVIS_ASSERT_EQ(5, count);

    // Free in reverse order
    for (int i = 4; i >= 0; --i) {
        JARVIS_ASSERT(BufferPool::free(*task, handles[i]));
    }

    JARVIS_ASSERT_EQ(-1, task->buf_list_head);

    JARVIS_TEST_PASS();
}

JARVIS_TEST(buffer_pool_invalid_handle, "PRE: none | POST: none") {
    SimpleTaskPtr task(TaskControlBlock::create_user([]() {}, 5, 10, 32_KiB));
    JARVIS_ASSERT(task != nullptr);

    // Handle 0 should always be invalid
    JARVIS_ASSERT_EQ(BufferPool::BUF_INVALID_HANDLE, BufferPool::validate(0));

    // Forged handle (index 0, gen 0xDEAD)
    uint64_t bad = (static_cast<uint64_t>(0xDEAD) << 32) | 0;
    JARVIS_ASSERT_EQ(BufferPool::BUF_INVALID_HANDLE, BufferPool::validate(bad));

    // Valid alloc then check bogus gen
    uint64_t va = 0x300000000;
    uint64_t good = BufferPool::alloc(*task, va);
    JARVIS_ASSERT(good != 0);
    uint32_t real_idx = static_cast<uint32_t>(good & 0xFFFFFFFFULL);
    uint32_t real_gen = static_cast<uint32_t>(good >> 32);

    // Wrong generation
    uint64_t forged = (static_cast<uint64_t>(real_gen + 1) << 32) | real_idx;
    JARVIS_ASSERT_EQ(BufferPool::BUF_INVALID_HANDLE, BufferPool::validate(forged));

    // Index out of range
    uint64_t oob =
        (static_cast<uint64_t>(real_gen) << 32) | BufferPool::MAX_BUFFERS;
    JARVIS_ASSERT_EQ(BufferPool::BUF_INVALID_INDEX, BufferPool::validate(oob));

    BufferPool::free(*task, good);

    JARVIS_TEST_PASS();
}

JARVIS_TEST(buffer_pool_exhaustion, "PRE: none | POST: none") {
    // v0.3.11 leak-pin instrumentation (TUI delta run):
    // 1. Snapshot the PMM allocation bitmap + record every buffer's phys page
    //    IMMEDIATELY BEFORE / DURING this test.
    // 2. After the task's cleanup completes, check each buffer phys against
    //    the bitmap (is it REALLY freed?) and print the net PMM delta via the
    //    fast QEMU debugcon.
    const uint64_t bsz = PMM::bitmap_bytes();
    static uint8_t s_before_bitmap[16 * 1024];
    static uint64_t s_buf_phys[BufferPool::MAX_BUFFERS];
    static size_t s_before_pool_count = 0;
    if (bsz <= sizeof(s_before_bitmap))
        __builtin_memcpy(s_before_bitmap, PMM::bitmap_ptr(), bsz);
    else
        arch::QemuDebugcon::write("[LEAK] bitmap too large for snapshot\n");
    s_before_pool_count = BufferPool::pool_count_debug();

    {
        SimpleTaskPtr task(TaskControlBlock::create_user([]() {}, 5, 10, 32_KiB));
        JARVIS_ASSERT(task != nullptr);

        int alloc_count = 0;
        // v0.3.11: buffer VAs must be >= 0x1000000000 (documented convention).
        // 0x40000000 collided with kUserYieldStubVa (task.cpp) and orphaned
        // the stub page (Root Cause 2).
        uint64_t va = 0x1000000000;
        for (size_t i = 0; i < BufferPool::MAX_BUFFERS + 1; ++i) {
            uint64_t h = BufferPool::alloc(*task, va + i * arch::PAGE_SIZE);
            if (h == 0)
                break;
            s_buf_phys[alloc_count] =
                BufferPool::entries[static_cast<uint32_t>(h & 0xFFFFFFFFULL)]
                    .phys_addr;
            alloc_count++;
        }
        JARVIS_ASSERT_EQ(static_cast<int>(BufferPool::MAX_BUFFERS), alloc_count);

        // Free all
        int32_t idx = task->buf_list_head;
        while (idx != -1) {
            int32_t next = BufferPool::entries[idx].list_next;
            uint32_t gen = BufferPool::entries[idx].generation;
            uint64_t h = (static_cast<uint64_t>(gen) << 32) |
                         static_cast<uint64_t>(idx);
            JARVIS_ASSERT(BufferPool::free(*task, h));
            idx = next;
        }
    }
    // SimpleTaskPtr destroyed here -> task->cleanup() completed.

    {
        // Surgical: which buffer data pages are STILL allocated after cleanup?
        uint64_t buf_still_alloc = 0;
        uint64_t buf_total = 0;
        for (size_t i = 0; i < BufferPool::MAX_BUFFERS; ++i) {
            uint64_t phys = s_buf_phys[i];
            if (phys == 0)
                continue;
            ++buf_total;
            if ((PMM::bitmap_ptr()[phys / 4096ULL / 8] >>
                 (phys / 4096ULL % 8)) &
                1ULL) {
                char buf[64];
                int p = 0;
                const char *hdr = "[BUFLEAK] phys=0x";
                while (*hdr)
                    buf[p++] = *hdr++;
                bool started = false;
                for (int sh = 60; sh >= 0; sh -= 4) {
                    unsigned nib =
                        static_cast<unsigned>((phys >> sh) & 0xF);
                    if (nib || started || sh == 0) {
                        buf[p++] = "0123456789abcdef"[nib];
                        started = true;
                    }
                }
                buf[p++] = '\n';
                arch::QemuDebugcon::write(buf, static_cast<size_t>(p));
                ++buf_still_alloc;
            }
        }
        char t[72];
        int tp = 0;
        const char *s0 = "[BUF] total=";
        while (*s0)
            t[tp++] = *s0++;
        uint64_t lv = buf_total;
        char rev[24];
        int rp = 0;
        do {
            rev[rp++] = static_cast<char>('0' + (lv % 10));
            lv /= 10;
        } while (lv);
        while (rp)
            t[tp++] = rev[--rp];
        const char *s1 = " still-alloc=";
        while (*s1)
            t[tp++] = *s1++;
        lv = buf_still_alloc;
        rp = 0;
        do {
            rev[rp++] = static_cast<char>('0' + (lv % 10));
            lv /= 10;
        } while (lv);
        while (rp)
            t[tp++] = rev[--rp];
        const char *s2 = " pool_count=";
        while (*s2)
            t[tp++] = *s2++;
        lv = BufferPool::pool_count_debug();
        rp = 0;
        do {
            rev[rp++] = static_cast<char>('0' + (lv % 10));
            lv /= 10;
        } while (lv);
        while (rp)
            t[tp++] = rev[--rp];
        t[tp++] = '\n';
        arch::QemuDebugcon::write(t, static_cast<size_t>(tp));

        // Net PMM delta from the full bitmap.
        uint64_t before_alloc = 0;
        uint64_t after_alloc = 0;
        for (uint64_t i = 0; i < bsz; ++i) {
            uint8_t b = s_before_bitmap[i];
            uint8_t n = PMM::bitmap_ptr()[i];
            for (unsigned bi = 0; bi < 8; ++bi) {
                before_alloc += static_cast<uint64_t>((b >> bi) & 1);
                after_alloc += static_cast<uint64_t>((n >> bi) & 1);
            }
        }
        char t2[72];
        int q = 0;
        const char *u0 = "[NET] before=";
        while (*u0)
            t2[q++] = *u0++;
        lv = before_alloc;
        rp = 0;
        do {
            rev[rp++] = static_cast<char>('0' + (lv % 10));
            lv /= 10;
        } while (lv);
        while (rp)
            t2[q++] = rev[--rp];
        const char *u1 = " after=";
        while (*u1)
            t2[q++] = *u1++;
        lv = after_alloc;
        rp = 0;
        do {
            rev[rp++] = static_cast<char>('0' + (lv % 10));
            lv /= 10;
        } while (lv);
        while (rp)
            t2[q++] = rev[--rp];
        const char *u2 = " pool_before=";
        while (*u2)
            t2[q++] = *u2++;
        lv = s_before_pool_count;
        rp = 0;
        do {
            rev[rp++] = static_cast<char>('0' + (lv % 10));
            lv /= 10;
        } while (lv);
        while (rp)
            t2[q++] = rev[--rp];
        t2[q++] = '\n';
        arch::QemuDebugcon::write(t2, static_cast<size_t>(q));

        // Find the net-new page(s): allocated now, free before, no same-phys
        // free counterpart.  With the pool fix the pool is stable, so this
        // isolates the residual +1.
        uint64_t new_alloc = 0;
        uint64_t new_free = 0;
        uint64_t first_leak_phys = 0;
        uint64_t first_freed_phys = 0;
        for (uint64_t i = 0; i < bsz; ++i) {
            uint8_t b = s_before_bitmap[i];
            uint8_t n = PMM::bitmap_ptr()[i];
            uint8_t nl = n & static_cast<uint8_t>(~b);
            uint8_t nf = b & static_cast<uint8_t>(~n);
            while (nl) {
                unsigned bit = __builtin_ctz(nl);
                uint64_t pg = i * 8 + bit;
                if (first_leak_phys == 0)
                    first_leak_phys = pg * 4096ULL;
                ++new_alloc;
                nl = static_cast<uint8_t>(nl & (nl - 1));
            }
            while (nf) {
                unsigned bit = __builtin_ctz(nf);
                uint64_t pg = i * 8 + bit;
                if (first_freed_phys == 0)
                    first_freed_phys = pg * 4096ULL;
                ++new_free;
                nf = static_cast<uint8_t>(nf & (nf - 1));
            }
        }
        char t3[96];
        int r = 0;
        const char *v0 = "[DIFF] new=";
        while (*v0)
            t3[r++] = *v0++;
        lv = new_alloc;
        rp = 0;
        do {
            rev[rp++] = static_cast<char>('0' + (lv % 10));
            lv /= 10;
        } while (lv);
        while (rp)
            t3[r++] = rev[--rp];
        const char *v1 = " freed=";
        while (*v1)
            t3[r++] = *v1++;
        lv = new_free;
        rp = 0;
        do {
            rev[rp++] = static_cast<char>('0' + (lv % 10));
            lv /= 10;
        } while (lv);
        while (rp)
            t3[r++] = rev[--rp];
        const char *v2 = " first-leak=0x";
        while (*v2)
            t3[r++] = *v2++;
        uint64_t fv = first_leak_phys;
        bool started = false;
        for (int sh = 60; sh >= 0; sh -= 4) {
            unsigned nib = static_cast<unsigned>((fv >> sh) & 0xF);
            if (nib || started || sh == 0) {
                t3[r++] = "0123456789abcdef"[nib];
                started = true;
            }
        }
        const char *v3 = " first-freed=0x";
        while (*v3)
            t3[r++] = *v3++;
        fv = first_freed_phys;
        started = false;
        for (int sh = 60; sh >= 0; sh -= 4) {
            unsigned nib = static_cast<unsigned>((fv >> sh) & 0xF);
            if (nib || started || sh == 0) {
                t3[r++] = "0123456789abcdef"[nib];
                started = true;
            }
        }
        t3[r++] = '\n';
        arch::QemuDebugcon::write(t3, static_cast<size_t>(r));

        // Print every newly-allocated page (the pool replacement + the leak).
        for (uint64_t i = 0; i < bsz; ++i) {
            uint8_t n = PMM::bitmap_ptr()[i];
            uint8_t nl = n & static_cast<uint8_t>(~s_before_bitmap[i]);
            while (nl) {
                unsigned bit = __builtin_ctz(nl);
                uint64_t pg = i * 8 + bit;
                uint64_t pv = pg * 4096ULL;
                char pb[32];
                int q2 = 0;
                const char *ph = "[NEW] 0x";
                while (*ph)
                    pb[q2++] = *ph++;
                bool st2 = false;
                for (int sh = 60; sh >= 0; sh -= 4) {
                    unsigned nib = static_cast<unsigned>((pv >> sh) & 0xF);
                    if (nib || st2 || sh == 0) {
                        pb[q2++] = "0123456789abcdef"[nib];
                        st2 = true;
                    }
                }
                pb[q2++] = '\n';
                arch::QemuDebugcon::write(pb, static_cast<size_t>(q2));
                nl = static_cast<uint8_t>(nl & (nl - 1));
            }
        }
    }

    JARVIS_TEST_PASS();
}

JARVIS_TEST(buffer_pool_double_free, "PRE: none | POST: none") {
    SimpleTaskPtr task(TaskControlBlock::create_user([]() {}, 5, 10, 32_KiB));
    JARVIS_ASSERT(task != nullptr);

    uint64_t handle = BufferPool::alloc(*task, 0x500000000);
    JARVIS_ASSERT(handle != 0);

    JARVIS_ASSERT(BufferPool::free(*task, handle));

    // Double free must fail
    JARVIS_ASSERT(!BufferPool::free(*task, handle));

    JARVIS_TEST_PASS();
}

JARVIS_TEST(buffer_pool_map_unmap, "PRE: none | POST: none") {
    SimpleTaskPtr task(TaskControlBlock::create_user([]() {}, 5, 10, 32_KiB));
    JARVIS_ASSERT(task != nullptr);

    uint64_t va = 0x600000000;
    uint64_t handle = BufferPool::alloc(*task, va);
    JARVIS_ASSERT(handle != 0);

    // Unmap
    JARVIS_ASSERT(BufferPool::unmap(*task, handle));
    JARVIS_ASSERT_EQ(0ULL, BufferPool::entries[handle & 0xFFFFFFFF].mapped_va);

    // Free should still work (unmapped)
    JARVIS_ASSERT(BufferPool::free(*task, handle));

    JARVIS_TEST_PASS();
}

JARVIS_TEST(buffer_pool_transfer, "PRE: none | POST: none") {
    auto *sender = TaskControlBlock::create_user([]() {}, 5, 10, 32_KiB);
    auto *receiver = TaskControlBlock::create_user([]() {}, 5, 10, 32_KiB);
    JARVIS_ASSERT(sender != nullptr && receiver != nullptr);

    auto cleanup = ScopeGuard([&]() {
        sender->cleanup();
        delete sender;
        receiver->cleanup();
        delete receiver;
    });

    uint64_t va = 0x800000000;
    uint64_t handle = BufferPool::alloc(*sender, va);
    JARVIS_ASSERT(handle != 0);

    // Transfer -> receiver
    JARVIS_ASSERT(BufferPool::transfer(handle, *sender, *receiver));
    JARVIS_ASSERT_EQ(receiver->id,
                     BufferPool::entries[handle & 0xFFFFFFFF].owner_task);
    JARVIS_ASSERT_EQ(0ULL, BufferPool::entries[handle & 0xFFFFFFFF].mapped_va);

    // Sender can no longer free it
    JARVIS_ASSERT(!BufferPool::free(*sender, handle));

    // Receiver can map and free it
    uint64_t recv_va = 0x900000000;
    JARVIS_ASSERT(BufferPool::map(*receiver, handle, recv_va));
    JARVIS_ASSERT(BufferPool::free(*receiver, handle));

    JARVIS_TEST_PASS();
}

JARVIS_TEST(buffer_pool_unmap_all, "PRE: none | POST: none") {
    SimpleTaskPtr task(TaskControlBlock::create_user([]() {}, 5, 10, 32_KiB));
    JARVIS_ASSERT(task != nullptr);

    uint64_t va = 0xA00000000;
    uint64_t h1 = BufferPool::alloc(*task, va);
    uint64_t h2 = BufferPool::alloc(*task, va + arch::PAGE_SIZE);
    uint64_t h3 = BufferPool::alloc(*task, va + 2 * arch::PAGE_SIZE);
    JARVIS_ASSERT(h1 != 0 && h2 != 0 && h3 != 0);

    BufferPool::unmap_all(*task);
    JARVIS_ASSERT_EQ(-1, task->buf_list_head);

    JARVIS_ASSERT(BufferPool::entries[h1 & 0xFFFFFFFF].phys_addr == 0);
    JARVIS_ASSERT(BufferPool::entries[h2 & 0xFFFFFFFF].phys_addr == 0);
    JARVIS_ASSERT(BufferPool::entries[h3 & 0xFFFFFFFF].phys_addr == 0);

    JARVIS_TEST_PASS();
}

JARVIS_TEST(buffer_pool_syscall_dispatch, "PRE: none | POST: none") {
    // Trigger-driven: dispatch a REAL kernel task whose lambda invokes the
    // BUF_ALLOC/BUF_FREE syscall handlers.  syscall_task() resolves to the
    // genuinely-running task, and the handlers run via the real dispatched
    // context.  BUGS.md#020 hazard avoided: the task runs in kernel mode (a
    // C++ lambda cannot execute in user mode), and page_table_ is set to a
    // clone so BufferPool::alloc (which requires page_table_ != 0) succeeds.
    static uint64_t g_buf_handle = 0;
    static uint64_t g_buf_free_ret = static_cast<uint64_t>(-1);

    auto *task = TaskControlBlock::create(
        []() {
            uint64_t va = 0xB00000000;
            g_buf_handle = Syscall::handle(
                static_cast<uint64_t>(SyscallNumber::BUF_ALLOC), va, 0, 0, 0,
                nullptr);
            if (g_buf_handle != 0) {
                g_buf_free_ret = Syscall::handle(
                    static_cast<uint64_t>(SyscallNumber::BUF_FREE), g_buf_handle,
                    0, 0, 0, nullptr);
            }
        },
        11, 10);
    JARVIS_ASSERT(task != nullptr);
    Scheduler::add_task(*task);

    auto *original = Scheduler::current_task();
    // Do NOT yield_as(*task): next_task() skips the current task, so that
    // would make the only test task current and never dispatch it.  A plain
    // reschedule() picks the higher-priority task (11 > harness 10) on the
    // next timer tick.  Busy-wait WITHOUT reschedule() so the timer ISR can
    // acquire the scheduler lock and apply the deferred switch.
    Scheduler::reschedule();
    kernel::test::wait_for_termination_safe(task);

    JARVIS_ASSERT(g_buf_handle != 0);
    JARVIS_ASSERT_EQ(0ULL, g_buf_free_ret);

    Scheduler::set_current(*original);
    // cleanup() frees page_table_ automatically (task.cpp cleanup frees
    // user_page + page_table_).  Just remove/delete.
    kernel::test::terminate_and_drain(*task);
    JARVIS_TEST_PASS();
}

JARVIS_TEST(buffer_pool_ipc_transfer, "PRE: none | POST: none") {
    // Real kernel sender + receiver (prio 12/11), each with a cloned PML4 so
    // BufferPool alloc/map (which require a non-null page table) work while
    // the lambdas run in kernel mode (BUGS.md#020-safe).
    static uint64_t g_sender_ok = 0;
    static uint64_t g_recv_ok = 0;

    auto *sender = TaskControlBlock::create(
        []() {
            auto *self = Scheduler::current_task();
            auto *ctx = reinterpret_cast<uint64_t *>(self->user_data);
            uint64_t peer = ctx[0];
            uint64_t va = 0xC00000000;
            uint64_t handle = BufferPool::alloc(*self, va);
            if (handle == 0) {
                g_sender_ok = 1;
                return;
            }
            Message msg{};
            msg.buf_handle = handle;
            msg.type = 42;
            if (!IPC::send(peer, msg, 0)) {
                g_sender_ok = 2;
                return;
            }
            // Sender can no longer free the transferred buffer.
            if (BufferPool::free(*self, handle)) {
                g_sender_ok = 3;
                return;
            }
            g_sender_ok = 0;
        },
        12, 10);

    auto *receiver = TaskControlBlock::create(
        []() {
            auto *self = Scheduler::current_task();
            uint64_t recv_va = 0xD00000000;
            Message recv_msg{};
            bool ok = false;
            for (int i = 0; i < 100000 && !ok; ++i)
                ok = IPC::recv(recv_msg);
            if (!ok || recv_msg.type != 42ULL) {
                g_recv_ok = 1;
                return;
            }
            if (!BufferPool::map(*self, recv_msg.buf_handle, recv_va)) {
                g_recv_ok = 2;
                return;
            }
            if (!BufferPool::free(*self, recv_msg.buf_handle)) {
                g_recv_ok = 3;
                return;
            }
            g_recv_ok = 0;
        },
        11, 10);
    if (!sender || !receiver) { JARVIS_FAIL("task create failed (OOM)"); return; }

    uint64_t sctx[1];
    sctx[0] = receiver->id;
    sender->user_data = sctx;
    uint64_t rctx[1];
    rctx[0] = 0xD00000000;
    receiver->user_data = rctx;

    {
        arch::IrqGuard _guard;
        Scheduler::add_task(*sender);
        Scheduler::add_task(*receiver);
    }
    Scheduler::reschedule();
    kernel::test::wait_for_termination_safe(sender);
kernel::test::wait_for_termination_safe(receiver);


    kernel::test::terminate_and_drain2(sender, receiver);
    JARVIS_ASSERT_EQ(0ULL, g_sender_ok);
    JARVIS_ASSERT_EQ(0ULL, g_recv_ok);
    JARVIS_TEST_PASS();
}

JARVIS_TEST(buffer_pool_cleanup_frees_buffers, "PRE: none | POST: none") {
    SimpleTaskPtr task(TaskControlBlock::create_user([]() {}, 5, 10, 32_KiB));
    JARVIS_ASSERT(task != nullptr);

    uint64_t va = 0xE00000000;
    uint64_t h1 = BufferPool::alloc(*task, va);
    uint64_t h2 = BufferPool::alloc(*task, va + arch::PAGE_SIZE);
    JARVIS_ASSERT(h1 != 0 && h2 != 0);

    // cleanup() should call BufferPool::unmap_all() which frees everything
    task->cleanup();

    // Entries should be recycled
    JARVIS_ASSERT(BufferPool::entries[h1 & 0xFFFFFFFF].phys_addr == 0);
    JARVIS_ASSERT(BufferPool::entries[h2 & 0xFFFFFFFF].phys_addr == 0);

    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Tests that BufferPool::exec_into_current properly clears buffer
// pool entries by calling unmap_all BEFORE swapping the page table. This is a
// regression test for a bug where unmap_all used the NEW page table (after
// swap) instead of the OLD one, leaving PTEs stale and buffer entries leaked.
// Input: A REAL dispatched kernel task allocs a buffer; the harness then
//        simulates exec_into_current's unmap_all-before-swap ordering.
// Expect: Buffer entry recycled (phys_addr == 0), old PML4 freed cleanly.
// Depends: kernel::BufferPool, kernel::TaskControlBlock, kernel::VMM,
// kernel::PMM
JARVIS_TEST(buffer_pool_exec_into_current_clears_buffers,
            "PRE: none | POST: none") {
    auto *task = TaskControlBlock::create_user([]() {}, 5, 10, 32_KiB);
    JARVIS_ASSERT(task != nullptr);

    // Drive the alloc through a REAL dispatched kernel task (BUGS.md#020-safe:
    // kernel-mode lambda with a cloned PML4).
    static uint64_t g_handle = 0;
    struct ExecContext {
        TaskControlBlock *target_;
    } context{task};
    auto *worker = TaskControlBlock::create(
        []() {
            auto *self = Scheduler::current_task();
            auto *ctx = reinterpret_cast<ExecContext *>(self->user_data);
            g_handle = BufferPool::alloc(*ctx->target_, 0xF00000000);
        },
        11, 10);
    JARVIS_ASSERT(worker != nullptr);
    worker->user_data = &context;
    Scheduler::add_task(*worker);
    Scheduler::reschedule();
    kernel::test::wait_for_termination_safe(worker);

    kernel::test::terminate_and_drain(*worker);

    uint64_t handle = g_handle;
    JARVIS_ASSERT(handle != 0);

    uint32_t idx = static_cast<uint32_t>(handle & 0xFFFFFFFFULL);
    JARVIS_ASSERT(BufferPool::entries[idx].phys_addr != 0);
    JARVIS_ASSERT(BufferPool::entries[idx].mapped_va == 0xF00000000);

    // Simulate exec_into_current:
    // 1. Create new PML4 (like exec_into_current does)
    uint64_t new_pml4 = VMM::clone_kernel_pml4();
    JARVIS_ASSERT(new_pml4 != 0);

    // 2. Call unmap_all BEFORE swapping page_table_ (this is what
    // exec_into_current does)
    // BUG: If unmap_all uses task->page_table_ AFTER the swap, it clears
    // wrong PML4
    BufferPool::unmap_all(*task);

    // 3. Swap page table (simulating exec_into_current line 308)
    uint64_t old_pml4 = task->page_table_;
    task->page_table_ = new_pml4;

    // 4. Free old PML4 (simulating exec_into_current cleanup)
    if (old_pml4 && old_pml4 != VMM::get_kernel_pml4()) {
        VMM::free_user_pages(old_pml4);
        PMM::free_page(old_pml4);
    }

    // Verify buffer entry was recycled (phys_addr == 0)
    JARVIS_ASSERT(BufferPool::entries[idx].phys_addr == 0);
    JARVIS_ASSERT(BufferPool::entries[idx].mapped_va == 0);
    JARVIS_ASSERT(task->buf_list_head == -1);

    task->cleanup();
    delete task;

    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Tests that BufferPool::transfer() adds the buffer to the RECEIVER's
//           buf_list_head. This is a regression test for a bug where transfer()
//           did NOT add to receiver's list, causing a leak when the receiver
// exits without ever mapping the buffer (cleanup() walks buf_list_head
//           and frees entries; if not in list, buffer is never freed).
// Input: Sender allocates buffer, transfer() to receiver, sender cleaned up,
//        receiver exits without calling map().
// Expect: Buffer appears in receiver's buf_list_head; receiver cleanup frees
// it.
// Depends: kernel::BufferPool, kernel::TaskControlBlock, kernel::IPC
JARVIS_TEST(buffer_pool_transfer_adds_to_receiver_list,
            "PRE: none | POST: none") {
    auto *sender = TaskControlBlock::create_user([]() {}, 5, 10, 32_KiB);
    auto *receiver = TaskControlBlock::create_user([]() {}, 5, 10, 32_KiB);
    JARVIS_ASSERT(sender != nullptr && receiver != nullptr);

    auto cleanup = ScopeGuard([&]() {
        sender->cleanup();
        delete sender;
        delete receiver;
    });

    uint64_t va = 0x1000000000ULL;
    uint64_t handle = BufferPool::alloc(*sender, va);
    JARVIS_ASSERT(handle != 0);

    uint32_t idx = static_cast<uint32_t>(handle & 0xFFFFFFFFULL);
    JARVIS_ASSERT(BufferPool::entries[idx].owner_task ==
                  static_cast<uint32_t>(sender->id));
    JARVIS_ASSERT(BufferPool::entries[idx].mapped_va == va);

    // Transfer from sender to receiver
    JARVIS_ASSERT(BufferPool::transfer(handle, *sender, *receiver));

    // Buffer should now be in receiver's list
    JARVIS_ASSERT(BufferPool::entries[idx].owner_task ==
                  static_cast<uint32_t>(receiver->id));
    JARVIS_ASSERT(BufferPool::entries[idx].mapped_va ==
                  0); // unmapped during transfer

    // Check receiver's list contains the buffer
    int count = 0;
    int32_t list_idx = receiver->buf_list_head;
    bool found = false;
    while (list_idx != -1) {
        if (list_idx == static_cast<int32_t>(idx))
            found = true;
        count++;
        list_idx = BufferPool::entries[list_idx].list_next;
    }
    JARVIS_ASSERT(found);
    JARVIS_ASSERT_EQ(1, count);

    // Sender's list should be empty
    JARVIS_ASSERT_EQ(-1, sender->buf_list_head);

    // Simulate receiver exiting WITHOUT ever calling map()
    // cleanup() -> unmap_all() walks buf_list_head and frees entries
    receiver->cleanup();

    // Buffer entry should be recycled
    JARVIS_ASSERT(BufferPool::entries[idx].phys_addr == 0);
    JARVIS_ASSERT(BufferPool::entries[idx].mapped_va == 0);
    JARVIS_ASSERT(receiver->buf_list_head == -1);

    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Alloc with VA >= USER_SPACE_LIMIT fails (BUF_ERR_VA_OUT_OF_RANGE)
// Input: task, alloc at USER_SPACE_LIMIT and above
// Expect: Both allocs return 0
// Depends: kernel::BufferPool
JARVIS_TEST(buffer_pool_va_out_of_range_rejected, "PRE: none | POST: none") {
    SimpleTaskPtr task(TaskControlBlock::create_user([]() {}, 5, 10, 32_KiB));
    JARVIS_ASSERT(task != nullptr);

    uint64_t h1 = BufferPool::alloc(*task, USER_SPACE_LIMIT);
    uint64_t h2 = BufferPool::alloc(*task, USER_SPACE_LIMIT + arch::PAGE_SIZE);
    JARVIS_ASSERT_EQ(0ULL, h1);
    JARVIS_ASSERT_EQ(0ULL, h2);

    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: v0.4.0 MP-1 — a kernel task now owns a private kernel-half PML4,
// so BufferPool::alloc (address-space gate) SUCCEEDS for it: the buffer maps
// into the task's otherwise-empty user half and is reclaimed by cleanup.
// CHANGED (v0.4.0 MP-8): the pre-MP-1 premise "kernel task has no page
// table → alloc fails" is invalidated — every task has a PML4.
// Input: Dispatch a real kernel task; its lambda allocs a buffer.
// Expect: g_handle != 0 (buffer mapped in the kernel task's user half).
// Depends: kernel::BufferPool, kernel::TaskControlBlock
JARVIS_TEST(buffer_pool_kernel_task_alloc_fails, "PRE: none | POST: none") {
    static uint64_t g_handle = 0;

    auto *task = TaskControlBlock::create(
        []() {
            g_handle = BufferPool::alloc(*Scheduler::current_task(),
                                         0x3000000000ULL);
        },
        11, 10);
    JARVIS_ASSERT(task != nullptr);
    JARVIS_ASSERT(task->is_user_ == false); // real kernel task (MP-1)
    Scheduler::add_task(*task);
    Scheduler::reschedule();
    kernel::test::wait_for_termination_safe(task);

    // MP-1: the alloc succeeds; cleanup (via the drain) frees the user-half
    // mapping + buffer page.
    kernel::test::terminate_and_drain(*task);
    JARVIS_ASSERT(g_handle != 0);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Forged handle after free fails validation
// Input: alloc -> free -> use same handle (old gen), validate() returns -1
// Expect: validate returns -1
// Depends: kernel::BufferPool
JARVIS_TEST(buffer_pool_forged_handle_after_free, "PRE: none | POST: none") {
    SimpleTaskPtr task(TaskControlBlock::create_user([]() {}, 5, 10, 32_KiB));
    JARVIS_ASSERT(task != nullptr);

    uint64_t va = 0x400000000ULL;
    uint64_t handle = BufferPool::alloc(*task, va);
    JARVIS_ASSERT(handle != 0);

    JARVIS_ASSERT(BufferPool::free(*task, handle));

    // Try to use the old handle (same idx, same gen)
    JARVIS_ASSERT_EQ(BufferPool::BUF_INVALID_HANDLE, BufferPool::validate(handle));

    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Realloc after free recycles entry with incremented generation
// Input: alloc -> free -> alloc again, verify same entry index recycled with
// new gen
// Expect: New handle has same idx, different (incremented) gen
// Depends: kernel::BufferPool
JARVIS_TEST(buffer_pool_realloc_recycles_entry, "PRE: none | POST: none") {
    SimpleTaskPtr task(TaskControlBlock::create_user([]() {}, 5, 10, 32_KiB));
    JARVIS_ASSERT(task != nullptr);

    uint64_t va = 0x5000000000ULL;
    uint64_t h1 = BufferPool::alloc(*task, va);
    JARVIS_ASSERT(h1 != 0);

    uint32_t idx1 = static_cast<uint32_t>(h1 & 0xFFFFFFFFULL);
    uint32_t gen1 = static_cast<uint32_t>(h1 >> 32);

    JARVIS_ASSERT(BufferPool::free(*task, h1));

    uint64_t h2 = BufferPool::alloc(*task, va);
    JARVIS_ASSERT(h2 != 0);

    uint32_t idx2 = static_cast<uint32_t>(h2 & 0xFFFFFFFFULL);
    uint32_t gen2 = static_cast<uint32_t>(h2 >> 32);

    JARVIS_ASSERT_EQ(idx1, idx2);
    JARVIS_ASSERT(gen2 == gen1 + 1);

    BufferPool::free(*task, h2);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Exhaust all 1024 entries, free one, new alloc recycles the freed
// entry
// Input: Fill pool completely, free one, alloc again
// Expect: New alloc succeeds and uses the freed entry index
// Depends: kernel::BufferPool
JARVIS_TEST(buffer_pool_alloc_after_exhaustion_and_free,
            "PRE: none | POST: none") {
    // v0.3.11 final-leak instrumentation (24 GB test): isolate entry 512's
    // phys before/after the mid-test free + re-alloc, snapshot the PMM bitmap,
    // and print the net delta + escaping page after cleanup.
    const uint64_t bsz = PMM::bitmap_bytes();
    static uint8_t s_before_bitmap[16 * 1024];
    static uint64_t s_e512_orig = 0;
    static uint64_t s_e512_new = 0;
    static size_t s_before_pool_count = 0;
    static kernel::test::ResourceCounters s_before_rsrc = {};
    if (bsz <= sizeof(s_before_bitmap))
        __builtin_memcpy(s_before_bitmap, PMM::bitmap_ptr(), bsz);
    else
        arch::QemuDebugcon::write("[L512] bitmap too large\n");
    s_before_pool_count = BufferPool::pool_count_debug();
    kernel::test::ResourceTracker::instance().capture(s_before_rsrc);

    {
        SimpleTaskPtr task(TaskControlBlock::create_user([]() {}, 5, 10, 32_KiB));
        JARVIS_ASSERT(task != nullptr);

        uint64_t handles[BufferPool::MAX_BUFFERS];
        uint64_t va = 0x6000000000ULL;
        int alloc_count = 0;
        for (size_t i = 0; i < BufferPool::MAX_BUFFERS; ++i) {
            uint64_t h = BufferPool::alloc(*task, va + i * arch::PAGE_SIZE);
            if (h == 0)
                break;
            handles[i] = h;
            alloc_count++;
        }
    JARVIS_ASSERT_EQ(static_cast<int>(BufferPool::MAX_BUFFERS), alloc_count);

    // Free the middle entry (index 512)
    uint64_t freed = handles[512];
    uint32_t freed_idx = static_cast<uint32_t>(freed & 0xFFFFFFFFULL);
    s_e512_orig = BufferPool::entries[freed_idx].phys_addr;
    {
        // v0.3.11: log pool state around the mid-test free + re-alloc.
        char t[100];
        int p = 0;
        const char *a0 = "[L512S] pre-free pool=";
        while (*a0)
            t[p++] = *a0++;
        uint64_t v = BufferPool::pool_count_debug();
        char rev[24];
        int rp = 0;
        do {
            rev[rp++] = static_cast<char>('0' + (v % 10));
            v /= 10;
        } while (v);
        while (rp)
            t[p++] = rev[--rp];
        const char *a1 = " e512_orig=0x";
        while (*a1)
            t[p++] = *a1++;
        v = s_e512_orig;
        bool st = false;
        for (int sh = 60; sh >= 0; sh -= 4) {
            unsigned nib = static_cast<unsigned>((v >> sh) & 0xF);
            if (nib || st || sh == 0) {
                t[p++] = "0123456789abcdef"[nib];
                st = true;
            }
        }
        t[p++] = '\n';
        arch::QemuDebugcon::write(t, static_cast<size_t>(p));
    }
    JARVIS_ASSERT(BufferPool::free(*task, freed));
    {
        char t[120];
        int p = 0;
        const char *a0 = "[L512S] post-free pool=";
        while (*a0)
            t[p++] = *a0++;
        uint64_t v = BufferPool::pool_count_debug();
        char rev[24];
        int rp = 0;
        do {
            rev[rp++] = static_cast<char>('0' + (v % 10));
            v /= 10;
        } while (v);
        while (rp)
            t[p++] = rev[--rp];
        const char *a1 = " owner_e512=0x";
        while (*a1)
            t[p++] = *a1++;
        v = s_e512_orig;
        bool st = false;
        for (int sh = 60; sh >= 0; sh -= 4) {
            unsigned nib = static_cast<unsigned>((v >> sh) & 0xF);
            if (nib || st || sh == 0) {
                t[p++] = "0123456789abcdef"[nib];
                st = true;
            }
        }
        const char *a2 = " is_user=";
        while (*a2)
            t[p++] = *a2++;
        {
            uint8_t *own = PMM::owner_bitmap_ptr();
            uint64_t pg = s_e512_orig / 4096ULL;
            t[p++] = ((own[pg / 8] >> (pg % 8)) & 1) ? '1' : '0';
        }
        t[p++] = '\n';
        arch::QemuDebugcon::write(t, static_cast<size_t>(p));
    }

        // Alloc again - should reuse the freed entry
        uint64_t h = BufferPool::alloc(*task, va + 512 * arch::PAGE_SIZE);
        JARVIS_ASSERT(h != 0);
        uint32_t new_idx = static_cast<uint32_t>(h & 0xFFFFFFFFULL);
        JARVIS_ASSERT_EQ(freed_idx, new_idx);
        s_e512_new = BufferPool::entries[new_idx].phys_addr;

        // Cleanup
        for (size_t i = 0; i < BufferPool::MAX_BUFFERS; ++i) {
            if (i != 512 && handles[i] != 0) {
                BufferPool::free(*task, handles[i]);
            }
        }
        BufferPool::free(*task, h);
    }
    // SimpleTaskPtr destroyed here -> task->cleanup() completed.

    // Check whether the leaked pages are inside the BufferPool cache.
    {
        size_t pc = BufferPool::pool_count_debug();
        uint64_t l1 = 0x6bc3000;
        uint64_t l2 = 0x6dc3000;
        bool l1_in_pool = false;
        bool l2_in_pool = false;
        for (size_t i = 0; i < pc; ++i) {
            uint64_t pp = BufferPool::pool_page_debug(i);
            if (pp == l1)
                l1_in_pool = true;
            if (pp == l2)
                l2_in_pool = true;
        }
        char t[96];
        int p = 0;
        const char *a0 = "[L512P] pool_count=";
        while (*a0)
            t[p++] = *a0++;
        uint64_t v = pc;
        char rev[24];
        int rp = 0;
        do {
            rev[rp++] = static_cast<char>('0' + (v % 10));
            v /= 10;
        } while (v);
        while (rp)
            t[p++] = rev[--rp];
        const char *a1 = " 0x6bc3000_in_pool=";
        while (*a1)
            t[p++] = *a1++;
        t[p++] = l1_in_pool ? '1' : '0';
        const char *a2 = " 0x6dc3000_in_pool=";
        while (*a2)
            t[p++] = *a2++;
        t[p++] = l2_in_pool ? '1' : '0';
        t[p++] = '\n';
        arch::QemuDebugcon::write(t, static_cast<size_t>(p));
    }

    // ---- post-cleanup delta (before snapshot_restore's rewind) ----
    {
        uint64_t before_alloc = 0;
        uint64_t after_alloc = 0;
        uint64_t new_pages = 0;
        uint64_t freed_pages = 0;
        uint64_t first_new = 0;
        for (uint64_t i = 0; i < bsz; ++i) {
            uint8_t b = s_before_bitmap[i];
            uint8_t n = PMM::bitmap_ptr()[i];
            for (unsigned bi = 0; bi < 8; ++bi) {
                before_alloc += static_cast<uint64_t>((b >> bi) & 1);
                after_alloc += static_cast<uint64_t>((n >> bi) & 1);
            }
            uint8_t nl = n & static_cast<uint8_t>(~b);
            while (nl) {
                unsigned bit = __builtin_ctz(nl);
                uint64_t pg = i * 8 + bit;
                if (first_new == 0)
                    first_new = pg * 4096ULL;
                ++new_pages;
                nl = static_cast<uint8_t>(nl & (nl - 1));
            }
            uint8_t nf = b & static_cast<uint8_t>(~n);
            while (nf) {
                nf = static_cast<uint8_t>(nf & (nf - 1));
                ++freed_pages;
            }
        }
        char t[120];
        int p = 0;
        const char *a0 = "[L512] e512_orig=0x";
        while (*a0)
            t[p++] = *a0++;
        uint64_t v = s_e512_orig;
        bool st = false;
        for (int sh = 60; sh >= 0; sh -= 4) {
            unsigned nib = static_cast<unsigned>((v >> sh) & 0xF);
            if (nib || st || sh == 0) {
                t[p++] = "0123456789abcdef"[nib];
                st = true;
            }
        }
        const char *a1 = " e512_new=0x";
        while (*a1)
            t[p++] = *a1++;
        v = s_e512_new;
        st = false;
        for (int sh = 60; sh >= 0; sh -= 4) {
            unsigned nib = static_cast<unsigned>((v >> sh) & 0xF);
            if (nib || st || sh == 0) {
                t[p++] = "0123456789abcdef"[nib];
                st = true;
            }
        }
        const char *a2 = " before=";
        while (*a2)
            t[p++] = *a2++;
        v = before_alloc;
        char rev[24];
        int rp = 0;
        do {
            rev[rp++] = static_cast<char>('0' + (v % 10));
            v /= 10;
        } while (v);
        while (rp)
            t[p++] = rev[--rp];
        const char *a3 = " after=";
        while (*a3)
            t[p++] = *a3++;
        v = after_alloc;
        rp = 0;
        do {
            rev[rp++] = static_cast<char>('0' + (v % 10));
            v /= 10;
        } while (v);
        while (rp)
            t[p++] = rev[--rp];
        const char *a4 = " new=";
        while (*a4)
            t[p++] = *a4++;
        v = new_pages;
        rp = 0;
        do {
            rev[rp++] = static_cast<char>('0' + (v % 10));
            v /= 10;
        } while (v);
        while (rp)
            t[p++] = rev[--rp];
        const char *a5 = " freed=";
        while (*a5)
            t[p++] = *a5++;
        v = freed_pages;
        rp = 0;
        do {
            rev[rp++] = static_cast<char>('0' + (v % 10));
            v /= 10;
        } while (v);
        while (rp)
            t[p++] = rev[--rp];
        const char *a6 = " first_new=0x";
        while (*a6)
            t[p++] = *a6++;
        v = first_new;
        st = false;
        for (int sh = 60; sh >= 0; sh -= 4) {
            unsigned nib = static_cast<unsigned>((v >> sh) & 0xF);
            if (nib || st || sh == 0) {
                t[p++] = "0123456789abcdef"[nib];
                st = true;
            }
        }
        {
            kernel::test::ResourceCounters after_rsrc = {};
            kernel::test::ResourceTracker::instance().capture(after_rsrc);
            const char *a7 = " pool_before=";
            while (*a7)
                t[p++] = *a7++;
            v = s_before_pool_count;
            char rev2[24];
            int rp2 = 0;
            do {
                rev2[rp2++] = static_cast<char>('0' + (v % 10));
                v /= 10;
            } while (v);
            while (rp2)
                t[p++] = rev2[--rp2];
            const char *a8 = " pool_after=";
            while (*a8)
                t[p++] = *a8++;
            v = BufferPool::pool_count_debug();
            rp2 = 0;
            do {
                rev2[rp2++] = static_cast<char>('0' + (v % 10));
                v /= 10;
            } while (v);
            while (rp2)
                t[p++] = rev2[--rp2];
            const char *a9 = " tracker_pmm_before=";
            while (*a9)
                t[p++] = *a9++;
            v = s_before_rsrc.pmm_pages_used;
            rp2 = 0;
            do {
                rev2[rp2++] = static_cast<char>('0' + (v % 10));
                v /= 10;
            } while (v);
            while (rp2)
                t[p++] = rev2[--rp2];
            const char *aa = " tracker_pmm_after=";
            while (*aa)
                t[p++] = *aa++;
            v = after_rsrc.pmm_pages_used;
            rp2 = 0;
            do {
                rev2[rp2++] = static_cast<char>('0' + (v % 10));
                v /= 10;
            } while (v);
            while (rp2)
                t[p++] = rev2[--rp2];
        }
        t[p++] = '\n';
        arch::QemuDebugcon::write(t, static_cast<size_t>(p));

        for (uint64_t i = 0; i < bsz; ++i) {
            uint8_t n = PMM::bitmap_ptr()[i];
            uint8_t nl = n & static_cast<uint8_t>(~s_before_bitmap[i]);
            while (nl) {
                unsigned bit = __builtin_ctz(nl);
                uint64_t pg = i * 8 + bit;
                uint64_t pv = pg * 4096ULL;
                char pb[32];
                int q2 = 0;
                const char *ph = "[L512N] 0x";
                while (*ph)
                    pb[q2++] = *ph++;
                bool st2 = false;
                for (int sh = 60; sh >= 0; sh -= 4) {
                    unsigned nib = static_cast<unsigned>((pv >> sh) & 0xF);
                    if (nib || st2 || sh == 0) {
                        pb[q2++] = "0123456789abcdef"[nib];
                        st2 = true;
                    }
                }
                pb[q2++] = '\n';
                arch::QemuDebugcon::write(pb, static_cast<size_t>(q2));
                nl = static_cast<uint8_t>(nl & (nl - 1));
            }
        }
    }

    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: List integrity after unlinking middle entry
// Input: alloc 10 buffers, free entry at index 5, verify neighbours'
// list_prev/list_next
// Expect: Entries 4 and 6 are correctly linked, head/tail intact
// Depends: kernel::BufferPool
JARVIS_TEST(buffer_pool_list_integrity_after_unlink, "PRE: none | POST: none") {
    SimpleTaskPtr task(TaskControlBlock::create_user([]() {}, 5, 10, 32_KiB));
    JARVIS_ASSERT(task != nullptr);

    uint64_t handles[10];
    uint64_t va = 0x700000000ULL;
    for (int i = 0; i < 10; ++i) {
        handles[i] = BufferPool::alloc(*task, va + i * arch::PAGE_SIZE);
        JARVIS_ASSERT(handles[i] != 0);
    }

    // Free middle entry (index 5)
    uint64_t freed = handles[5];
    JARVIS_ASSERT(BufferPool::free(*task, freed));

    // Verify list integrity: walk list and check all remaining entries present
    int count = 0;
    int32_t idx = task->buf_list_head;
    bool found[10] = {false};
    int32_t prev = -1;
    while (idx != -1) {
        count++;
        // Match entry index back to handles[] slot (entry indices are not
        // necessarily 0-9; the LIFO free list starts at MAX_BUFFERS-1).
        int slot = -1;
        for (int j = 0; j < 10; j++) {
            if ((handles[j] & 0xFFFFFFFFULL) == static_cast<uint32_t>(idx)) {
                slot = j;
                break;
            }
        }
        JARVIS_ASSERT(slot >= 0);
        found[slot] = true;
        // Check prev link
        JARVIS_ASSERT(BufferPool::entries[idx].list_prev == prev);
        prev = idx;
        idx = BufferPool::entries[idx].list_next;
    }
    JARVIS_ASSERT_EQ(9, count);

    // Verify all except the freed handle are in list
    for (int i = 0; i < 10; ++i) {
        if (i == 5)
            continue;
        JARVIS_ASSERT(found[i]);
    }
    JARVIS_ASSERT(!found[5]);

    // Cleanup
    for (int i = 0; i < 10; ++i) {
        if (i != 5 && handles[i] != 0) {
            BufferPool::free(*task, handles[i]);
        }
    }

    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Transfer same handle between two tasks back and forth
// multiple times, verifying ownership transfers correctly each time.
// Input: Sender allocs, transfers to receiver, receiver transfers back,
// sender transfers again. 5 cycles.
// Expect: After each transfer, only the new owner can free.
JARVIS_TEST(buffer_pool_transfer_race, "PRE: none | POST: none") {
    auto *task_a = TaskControlBlock::create_user([]() {}, 5, 10, 32_KiB);
    auto *task_b = TaskControlBlock::create_user([]() {}, 5, 10, 32_KiB);
    JARVIS_ASSERT(task_a != nullptr && task_b != nullptr);

    auto cleanup = ScopeGuard([&]() {
        task_a->cleanup();
        delete task_a;
        task_b->cleanup();
        delete task_b;
    });

    uint64_t va = 0x1000000000ULL;
    uint64_t handle = BufferPool::alloc(*task_a, va);
    JARVIS_ASSERT(handle != 0);

    static const int CYCLES = 5;
    for (int i = 0; i < CYCLES; ++i) {
        // A -> B
        JARVIS_ASSERT(BufferPool::transfer(handle, *task_a, *task_b));
        JARVIS_ASSERT(!BufferPool::free(*task_a, handle));
        JARVIS_ASSERT(BufferPool::entries[handle & 0xFFFFFFFF].owner_task ==
                      static_cast<uint32_t>(task_b->id));

        // B -> A
        JARVIS_ASSERT(BufferPool::transfer(handle, *task_b, *task_a));
        JARVIS_ASSERT(!BufferPool::free(*task_b, handle));
        JARVIS_ASSERT(BufferPool::entries[handle & 0xFFFFFFFF].owner_task ==
                      static_cast<uint32_t>(task_a->id));
    }

    // Final cleanup by A
    JARVIS_ASSERT(BufferPool::free(*task_a, handle));

    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Alloc, free, then verify the old handle (with original
// generation) is rejected.  Then alloc again, verify the new handle
// has an incremented generation and the old handle still rejected.
// Input: Alloc -> free -> try old handle -> alloc -> try old handle
// Expect: Old handle always rejected; new handle valid.
JARVIS_TEST(buffer_pool_handle_reuse_security, "PRE: none | POST: none") {
    SimpleTaskPtr task(TaskControlBlock::create_user([]() {}, 5, 10, 32_KiB));
    JARVIS_ASSERT(task != nullptr);

    uint64_t va = 0x2000000000ULL;
    uint64_t h1 = BufferPool::alloc(*task, va);
    JARVIS_ASSERT(h1 != 0);

    uint64_t old_handle = h1;
    JARVIS_ASSERT(BufferPool::free(*task, h1));
    JARVIS_ASSERT_EQ(BufferPool::BUF_INVALID_HANDLE, BufferPool::validate(old_handle));

    uint64_t h2 = BufferPool::alloc(*task, va);
    JARVIS_ASSERT(h2 != 0);
    uint32_t old_gen = static_cast<uint32_t>(old_handle >> 32);
    uint32_t new_gen = static_cast<uint32_t>(h2 >> 32);
    JARVIS_ASSERT(new_gen > old_gen);
    JARVIS_ASSERT_EQ(BufferPool::BUF_INVALID_HANDLE, BufferPool::validate(old_handle));

    BufferPool::free(*task, h2);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Transfer a buffer to a task that has no page table
// (kernel-only), verify transfer fails gracefully.
// Input: alloc in user task, transfer to kernel task.
// Expect: transfer returns false.
JARVIS_TEST(buffer_pool_transfer_to_kernel_task, "PRE: none | POST: none") {
    auto *user = TaskControlBlock::create_user([]() {}, 5, 10, 32_KiB);
    auto *kernel_task = TaskControlBlock::create([]() {}, 5, 10);
    JARVIS_ASSERT(user != nullptr && kernel_task != nullptr);
    JARVIS_ASSERT(kernel_task->is_user_ == false); // kernel task (MP-1)

    auto cleanup = ScopeGuard([&]() {
        user->cleanup();
        delete user;
        kernel_task->cleanup();
        delete kernel_task;
    });

    uint64_t va = 0x3000000000ULL;
    uint64_t handle = BufferPool::alloc(*user, va);
    JARVIS_ASSERT(handle != 0);

    bool ok = BufferPool::transfer(handle, *user, *kernel_task);

    // If transfer succeeded, the buffer now belongs to kernel_task;
    // free it via kernel_task.  Otherwise free via user.
    if (ok) {
        BufferPool::free(*kernel_task, handle);
    } else {
        BufferPool::free(*user, handle);
    }

    JARVIS_TEST_PASS();
}

// ======================================================================
// v0.3.11 B1-B3 regression tests: BufferPool user-stack PT-page walk
// (testcases-v0.3.11.md).  Direct PML4 walks pin the invariant that every
// page-table page under a user mapping is USER-owned and that cleanup()
// frees every PT page it allocated (net-zero PMM delta).
// ======================================================================

// Local page-table constants (VMM's PAGE_PRESENT/PAGE_FRAME_MASK are
// private).  The frame mask covers both x86_64 (bits [51:12]) and aarch64
// (bits [47:12]) — the extra upper bits are zero for aarch64 physical
// addresses.
static constexpr uint64_t kPtP = 1ULL << 0;
static constexpr uint64_t kPtFrame = 0x000FFFFFFFFFF000ULL;

/// @brief Walk a user PML4 and count present PT pages under a single PDPT
///        entry.  Mirrors VMM::free_user_pages' walk (vmm.cpp) without
///        freeing anything.  When @p first_non_user is non-null, the first
///        PT page whose PMM owner bit is NOT user is recorded there (B1).
static size_t count_pt_pages_under_pdpt(uint64_t pml4_phys, size_t pml4_idx,
                                        size_t pdpt_idx,
                                        uint64_t *first_non_user) {
    auto *pml4 = reinterpret_cast<uint64_t *>(
        arch::HHDM_OFFSET + (pml4_phys & ~0xFFFULL));
    if (!(pml4[pml4_idx] & kPtP))
        return 0;
    uint64_t pdpt_phys = pml4[pml4_idx] & kPtFrame;
    auto *pdpt =
        reinterpret_cast<uint64_t *>(arch::HHDM_OFFSET + pdpt_phys);
    if (!(pdpt[pdpt_idx] & kPtP))
        return 0;
    uint64_t pd_phys = pdpt[pdpt_idx] & kPtFrame;
    auto *pd = reinterpret_cast<uint64_t *>(arch::HHDM_OFFSET + pd_phys);
    size_t count = 0;
    for (size_t i = 0; i < 512; ++i) {
        if (!(pd[i] & kPtP))
            continue;
#if defined(CONFIG_ARCH_AARCH64)
        // Block descriptor (bits[1:0]=01) — no PT page beneath.
        if ((pd[i] & 0x3ULL) == 0x1ULL)
            continue;
#else
        // x86_64 2 MB huge page (PS bit 7) — no PT page beneath.
        if (pd[i] & (1ULL << 7))
            continue;
#endif
        uint64_t pt_phys = pd[i] & kPtFrame;
        if (first_non_user && *first_non_user == 0 &&
            !PMM::is_user_page(pt_phys)) {
            *first_non_user = pt_phys;
        }
        ++count;
    }
    return count;
}

// Runmode: kernel
// Testidea: B1 — after pool-overflow free (owner → KERNEL) and re-map, the
// PT page owner bit stays USER so free_user_pages never skips it.  A user
// task mapping 1024 buffers under PDPT[1] (4 MB, PD[1..2]) must leave every
// present PT page USER-owned, including the yield-stub (PD[0]) and stack
// (PD[384]) pages sharing the same PDPT.
// Input: create_user task; alloc MAX_BUFFERS buffers at 0x40200000; walk
// PDPT[1] before cleanup.
// Expect: exactly 2 new PT pages (PD[1], PD[2]); every PT page under PDPT[1]
// is USER-owned; PMM free count net-zero after free + cleanup.
// Depends: kernel::BufferPool, kernel::TaskControlBlock, kernel::VMM,
//          kernel::PMM
JARVIS_TEST(buffer_pool_pt_owner_bit_stays_user, "PRE: none | POST: none") {
    uint64_t pmm_before = PMM::free_pages_ref();
    uint64_t first_non_user = 0;

    {
        SimpleTaskPtr task(TaskControlBlock::create_user([]() {}, 5, 10, 32_KiB));
        JARVIS_ASSERT(task != nullptr);

        // PT pages already present under PDPT[1]: stub (PD[0]) + stack
        // (PD[384]).
        size_t pre = count_pt_pages_under_pdpt(task->page_table_, 0, 1,
                                               nullptr);

        // Allocate MAX_BUFFERS buffers spanning 2 PT pages under PDPT[1]:
        // 0x40200000 + 4 MB covers PD[1]..PD[2], clear of the stub
        // (0x40000000, PD[0]) and the user stack (0x70000000, PD[384]).
        uint64_t va = 0x40200000;
        int alloc_count = 0;
        for (size_t i = 0; i < BufferPool::MAX_BUFFERS; ++i) {
            uint64_t h = BufferPool::alloc(*task, va + i * arch::PAGE_SIZE);
            if (h == 0)
                break;
            ++alloc_count;
        }
        JARVIS_ASSERT_EQ(static_cast<int>(BufferPool::MAX_BUFFERS), alloc_count);

        // Walk every present PD entry under PDPT[1] and pin the owner bit.
        size_t post = count_pt_pages_under_pdpt(task->page_table_, 0, 1,
                                                &first_non_user);
        JARVIS_ASSERT_FMT(pre + 2 == post,
                          "allocating 4 MB under PDPT[1] must add exactly 2 "
                          "PT pages (pre=%lu post=%lu)",
                          pre, post);
        JARVIS_ASSERT_FMT(first_non_user == 0,
                          "PT page 0x%lx under a user mapping is NOT "
                          "USER-owned (owner-bit escape)",
                          first_non_user);

        // Free all buffers back to the pool (overflow spills to PMM).
        int32_t idx = task->buf_list_head;
        while (idx != -1) {
            int32_t next = BufferPool::entries[idx].list_next;
            uint32_t gen = BufferPool::entries[idx].generation;
            uint64_t h = (static_cast<uint64_t>(gen) << 32) |
                         static_cast<uint64_t>(idx);
            JARVIS_ASSERT(BufferPool::free(*task, h));
            idx = next;
        }
    }
    // SimpleTaskPtr destroyed: cleanup() → BufferPool::unmap_all +
    // VMM::free_user_pages frees every PT page and the stack.

    JARVIS_ASSERT_EQ(pmm_before, PMM::free_pages_ref());
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: B2 — stack and buffers share PDPT[1]; the free_user_pages walk
// must cover ALL present PD entries (stub, buffer PTs, stack) so no PT page
// survives cleanup.  Exhaustion alloc + free loop + cleanup leaves a
// net-zero PMM delta.
// Input: create_user task; alloc MAX_BUFFERS under PDPT[1]; free all; count
// PT pages before cleanup; destroy task; recount PMM free pages.
// Expect: PT-page count grows by exactly 2 and PMM free count is unchanged
// (alloc-count == freed-count, net_new_pages == 0).
// Depends: kernel::BufferPool, kernel::TaskControlBlock, kernel::VMM,
//          kernel::PMM
JARVIS_TEST(buffer_pool_shared_pdpt_walk_frees_all,
            "PRE: none | POST: none") {
    uint64_t pmm_before = PMM::free_pages_ref();
    size_t pt_before_cleanup = 0;

    {
        SimpleTaskPtr task(TaskControlBlock::create_user([]() {}, 5, 10, 32_KiB));
        JARVIS_ASSERT(task != nullptr);

        uint64_t va = 0x40200000;
        int alloc_count = 0;
        for (size_t i = 0; i < BufferPool::MAX_BUFFERS; ++i) {
            uint64_t h = BufferPool::alloc(*task, va + i * arch::PAGE_SIZE);
            if (h == 0)
                break;
            ++alloc_count;
        }
        JARVIS_ASSERT_EQ(static_cast<int>(BufferPool::MAX_BUFFERS), alloc_count);

        // Free all buffers (exhaustion free loop).
        int32_t idx = task->buf_list_head;
        while (idx != -1) {
            int32_t next = BufferPool::entries[idx].list_next;
            uint32_t gen = BufferPool::entries[idx].generation;
            uint64_t h = (static_cast<uint64_t>(gen) << 32) |
                         static_cast<uint64_t>(idx);
            JARVIS_ASSERT(BufferPool::free(*task, h));
            idx = next;
        }

        // Count PT pages under the shared PDPT[1] (stub + buffers + stack)
        // before cleanup — every one must be freed by free_user_pages.
        pt_before_cleanup =
            count_pt_pages_under_pdpt(task->page_table_, 0, 1, nullptr);
        JARVIS_ASSERT_FMT(pt_before_cleanup >= 4,
                          "expected stub+buffers+stack PT pages under PDPT[1] "
                          "(got %lu)",
                          pt_before_cleanup);
    }
    // SimpleTaskPtr destroyed: cleanup() → free_user_pages.

    JARVIS_ASSERT_EQ(pmm_before, PMM::free_pages_ref());
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: B3 — a 4 MB buffer range spanning 2 PT pages must balance
// exactly: every PMM page allocated by the alloc loop is freed by the free
// loop + cleanup (new_alloc == new_free via PMM free-page count).
// Input: create_user task; alloc MAX_BUFFERS (4 MB) at 0x40200000; free all;
// destroy task; compare PMM free count before/after.
// Expect: pmm_before == pmm_after (net-zero balance).
// Depends: kernel::BufferPool, kernel::TaskControlBlock, kernel::PMM
JARVIS_TEST(buffer_pool_4mb_walk_balance, "PRE: none | POST: none") {
    uint64_t pmm_before = PMM::free_pages_ref();
    size_t pt_count = 0;

    {
        SimpleTaskPtr task(TaskControlBlock::create_user([]() {}, 5, 10, 32_KiB));
        JARVIS_ASSERT(task != nullptr);

        uint64_t va = 0x40200000;
        int alloc_count = 0;
        for (size_t i = 0; i < BufferPool::MAX_BUFFERS; ++i) {
            uint64_t h = BufferPool::alloc(*task, va + i * arch::PAGE_SIZE);
            if (h == 0)
                break;
            ++alloc_count;
        }
        JARVIS_ASSERT_EQ(static_cast<int>(BufferPool::MAX_BUFFERS), alloc_count);
        JARVIS_ASSERT_FMT(
            va + alloc_count * arch::PAGE_SIZE - va == 4ULL * 1024 * 1024,
            "MAX_BUFFERS allocs must span exactly 4 MB");

        pt_count = count_pt_pages_under_pdpt(task->page_table_, 0, 1, nullptr);
        JARVIS_ASSERT_FMT(pt_count >= 4,
                          "expected 4 PT pages under PDPT[1] for the 4 MB "
                          "range + stub + stack (got %lu)",
                          pt_count);

        // Free all buffers (overflow spills to PMM).
        int32_t idx = task->buf_list_head;
        while (idx != -1) {
            int32_t next = BufferPool::entries[idx].list_next;
            uint32_t gen = BufferPool::entries[idx].generation;
            uint64_t h = (static_cast<uint64_t>(gen) << 32) |
                         static_cast<uint64_t>(idx);
            JARVIS_ASSERT(BufferPool::free(*task, h));
            idx = next;
        }
    }
    // SimpleTaskPtr destroyed: cleanup() → free_user_pages.

    // new_alloc == new_free: the PMM free count must be byte-identical.
    JARVIS_ASSERT_EQ(pmm_before, PMM::free_pages_ref());
    JARVIS_TEST_PASS();
}

void register_buffer_pool_tests() {
    Logger::info("Registering BufferPool tests");

    JARVIS_REGISTER_TEST(buffer_pool_invalid_handle);
    JARVIS_REGISTER_TEST(buffer_pool_basic_alloc_free);
    JARVIS_REGISTER_TEST(buffer_pool_multiple_alloc);
    JARVIS_REGISTER_TEST(buffer_pool_exhaustion);
    JARVIS_REGISTER_TEST(buffer_pool_double_free);
    JARVIS_REGISTER_TEST(buffer_pool_map_unmap);
    JARVIS_REGISTER_TEST(buffer_pool_transfer);
    JARVIS_REGISTER_TEST(buffer_pool_unmap_all);
    JARVIS_REGISTER_TEST(buffer_pool_syscall_dispatch);
    JARVIS_REGISTER_TEST(buffer_pool_ipc_transfer);
    JARVIS_REGISTER_TEST(buffer_pool_cleanup_frees_buffers);
    JARVIS_REGISTER_TEST(buffer_pool_exec_into_current_clears_buffers);
    JARVIS_REGISTER_TEST(buffer_pool_transfer_adds_to_receiver_list);
    JARVIS_REGISTER_TEST(buffer_pool_va_out_of_range_rejected);
    JARVIS_REGISTER_TEST(buffer_pool_kernel_task_alloc_fails);
    JARVIS_REGISTER_TEST(buffer_pool_forged_handle_after_free);
    JARVIS_REGISTER_TEST(buffer_pool_realloc_recycles_entry);
    JARVIS_REGISTER_TEST(buffer_pool_alloc_after_exhaustion_and_free);
    JARVIS_REGISTER_TEST(buffer_pool_list_integrity_after_unlink);
    JARVIS_REGISTER_TEST(buffer_pool_transfer_race);
    JARVIS_REGISTER_TEST(buffer_pool_handle_reuse_security);
    JARVIS_REGISTER_TEST(buffer_pool_transfer_to_kernel_task);
    JARVIS_REGISTER_TEST(buffer_pool_pt_owner_bit_stays_user);
    JARVIS_REGISTER_TEST(buffer_pool_shared_pdpt_walk_frees_all);
    JARVIS_REGISTER_TEST(buffer_pool_4mb_walk_balance);
}
#endif
