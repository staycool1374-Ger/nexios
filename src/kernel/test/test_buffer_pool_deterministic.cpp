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

#include <test.hpp>
#include <logger.hpp>
#include <scope_guard.hpp>
#include <kernel/test/task_ptr.hpp>
#include <kernel/ipc/buffer_pool.hpp>
#include <kernel/ipc/ipc.hpp>
#include <kernel/task/task.hpp>
#include <kernel/task/scheduler.hpp>
#include <kernel/arch/irq_guard.hpp>
#include <kernel/memory/pmm.hpp>
#include <kernel/memory/vmm.hpp>

using namespace kernel;

JARVIS_TEST(buffer_pool_deterministic_preallocated_pool,
            "PRE: none | POST: none") {
    JARVIS_ASSERT(BufferPool::POOL_PAGES == CONFIG_BUFFER_POOL_PAGES);
    JARVIS_ASSERT(BufferPool::POOL_PAGES > 0);
    JARVIS_ASSERT(BufferPool::MAX_BUFFERS == 1024);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Pre-allocated buffer pool fulfills alloc/free without
// requiring dynamic PMM allocation during the call.  Verified by
// snapshot isolation (ResourceTracker catches any PMM leak across
// tests).  This test validates that alloc returns a handle pointing
// to a pre-initialised entry with valid phys_addr and mapped_va.
// Input: Create user task, allocate one buffer, inspect entry.
// Expect: Valid handle, non-zero phys_addr, correct mapped_va.
// Depends: BufferPool, TaskControlBlock
JARVIS_TEST(buffer_pool_deterministic_no_dynamic_alloc,
            "PRE: none | POST: none") {
    SimpleTaskPtr task(TaskControlBlock::create_user([]() {}, 5, 10, 32_KiB));
    JARVIS_ASSERT(task != nullptr);
    uint64_t handle = BufferPool::alloc(*task, 0x100000000);
    JARVIS_ASSERT(handle != 0);
    uint32_t idx = static_cast<uint32_t>(handle & 0xFFFFFFFFULL);
    JARVIS_ASSERT(BufferPool::entries[idx].phys_addr != 0);
    JARVIS_ASSERT(BufferPool::entries[idx].mapped_va == 0x100000000);
    JARVIS_ASSERT(BufferPool::free(*task, handle));
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Exhaust buffer pool at MAX_BUFFERS, then verify overflow
// returns 0.  Properly cleans up all allocated buffers afterward.
// Input: Allocate MAX_BUFFERS buffers at distinct VAs, then one more.
// Expect: The first MAX_BUFFERS succeed; the overflow returns 0.
// Depends: BufferPool, TaskControlBlock
JARVIS_TEST(buffer_pool_deterministic_exhaustion_returns_zero,
            "PRE: none | POST: none") {
    SimpleTaskPtr task(TaskControlBlock::create_user([]() {}, 5, 10, 32_KiB));
    JARVIS_ASSERT(task != nullptr);
    uint64_t va = 0x200000000;

    uint64_t handles[BufferPool::MAX_BUFFERS];
    uint64_t n_alloced = 0;
    for (; n_alloced < BufferPool::MAX_BUFFERS; ++n_alloced) {
        handles[n_alloced] = BufferPool::alloc(*task, va + n_alloced * arch::PAGE_SIZE);
        if (handles[n_alloced] == 0)
            break;
    }
    JARVIS_ASSERT_FMT(n_alloced == BufferPool::MAX_BUFFERS,
                      "Only allocated %lu of %u buffers before exhaustion",
                      n_alloced, BufferPool::MAX_BUFFERS);

    uint64_t overflow = BufferPool::alloc(*task, va + BufferPool::MAX_BUFFERS * arch::PAGE_SIZE);
    JARVIS_ASSERT_EQ(0ULL, overflow);

    for (uint64_t i = 0; i < n_alloced; ++i) {
        uint32_t idx = static_cast<uint32_t>(handles[i] & 0xFFFFFFFFULL);
        uint32_t gen = BufferPool::entries[idx].generation;
        uint64_t h = (static_cast<uint64_t>(gen) << 32) | idx;
        JARVIS_ASSERT(BufferPool::free(*task, h));
    }
    JARVIS_TEST_PASS();
}

JARVIS_TEST(buffer_pool_deterministic_zero_copy_transfer,
            "PRE: none | POST: none") {
    auto *sender = TaskControlBlock::create_user([]() {}, 5, 10, 32_KiB);
    auto *receiver = TaskControlBlock::create_user([]() {}, 5, 10, 32_KiB);
    JARVIS_ASSERT(sender != nullptr && receiver != nullptr);
    auto cleanup = ScopeGuard([&]() {
        sender->cleanup();
        delete sender;
        receiver->cleanup();
        delete receiver;
    });
    uint64_t va = 0x300000000;
    uint64_t handle = BufferPool::alloc(*sender, va);
    JARVIS_ASSERT(handle != 0);
    JARVIS_ASSERT(BufferPool::transfer(handle, *sender, *receiver));
    JARVIS_ASSERT(BufferPool::entries[handle & 0xFFFFFFFF].owner_task ==
                  static_cast<uint32_t>(receiver->id));

    // Security check: after transfer, sender's original VA must be unmapped.
    uint64_t sender_va_phys = VMM::virt_to_phys_in_pml4(va, sender->page_table_);
    JARVIS_ASSERT_FMT(sender_va_phys == 0,
                      "Sender still owns VA 0x%lx (phys 0x%lx) after transfer",
                      va, sender_va_phys);

    // Buffer VAs must be >= 0x100000000 (memory.md §5.3): 0x40000000
    // collides with kUserYieldStubVa (task.cpp) and orphans the stub page.
    uint64_t recv_va = 0x400000000;
    JARVIS_ASSERT(BufferPool::map(*receiver, handle, recv_va));
    JARVIS_ASSERT(BufferPool::free(*receiver, handle));
    JARVIS_TEST_PASS();
}

JARVIS_TEST(buffer_pool_deterministic_inline_payload,
            "PRE: none | POST: none") {
    Message msg{};
    msg.sender_id = 1;
    msg.type = 42;
    msg.priority = 5;
    JARVIS_ASSERT(msg.sender_id == 1);
    JARVIS_ASSERT(msg.type == 42);
    JARVIS_ASSERT(msg.priority == 5);
    JARVIS_ASSERT(msg.data_size == 0);
    JARVIS_ASSERT(msg.buf_handle == 0);
    static_assert(sizeof(msg.data) == CONFIG_IPC_MAX_MSG_SIZE,
                  "Message payload must be inline (no heap pointer)");
    for (size_t i = 0; i < IPC_MAX_MSG_SIZE; ++i)
        msg.data[i] = static_cast<uint8_t>(i & 0xFF);
    for (size_t i = 0; i < IPC_MAX_MSG_SIZE; ++i)
        JARVIS_ASSERT(msg.data[i] == static_cast<uint8_t>(i & 0xFF));
    JARVIS_TEST_PASS();
}

JARVIS_TEST(buffer_pool_deterministic_pool_config_consistent,
            "PRE: none | POST: none") {
    JARVIS_ASSERT(BufferPool::POOL_PAGES == CONFIG_BUFFER_POOL_PAGES);
    JARVIS_ASSERT(BufferPool::MAX_BUFFERS > 0);
    JARVIS_ASSERT(BufferPool::BUFFER_SIZE == arch::PAGE_SIZE);
    JARVIS_TEST_PASS();
}

void register_buffer_pool_deterministic_tests() {
    Logger::info("Registering buffer pool deterministic tests");
    JARVIS_REGISTER_TEST(buffer_pool_deterministic_preallocated_pool);
    JARVIS_REGISTER_TEST(buffer_pool_deterministic_no_dynamic_alloc);
    JARVIS_REGISTER_TEST(buffer_pool_deterministic_exhaustion_returns_zero);
    JARVIS_REGISTER_TEST(buffer_pool_deterministic_zero_copy_transfer);
    JARVIS_REGISTER_TEST(buffer_pool_deterministic_inline_payload);
    JARVIS_REGISTER_TEST(buffer_pool_deterministic_pool_config_consistent);
}
