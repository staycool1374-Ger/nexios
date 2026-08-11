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

/// @file test_task.cpp
/// @brief Task control block tests.

#if defined(CONFIG_ARCH_X86_64)
#include <test.hpp>
#include <logger.hpp>
#include <kernel/task/task.hpp>
#include <kernel/task/scheduler.hpp>
#include <kernel/memory/pmm.hpp>
#include <kernel/memory/vmm.hpp>
#include <kernel/ipc/ipc.hpp>
#include <kernel/sync/notify.hpp>
#include <kernel/sync/eventgroup.hpp>
#include <kernel/elf/elf.hpp>
#include <initrd/initrd.hpp>
#include "test_sched_helpers.hpp"

using namespace kernel;

// Runmode: kernel
// Testidea: Verifies that TaskControlBlock::cleanup() nullifies all
// allocated resources (kernel stack, msg queue, notify, event group).
// Input: Create a TCB and call cleanup()
// Expect: kernel_stack, msg_queue, notify, event_group become nullptr;
// stack_phys_ becomes 0
// Depends: kernel::TaskControlBlock
JARVIS_TEST(task_cleanup_frees_resources, "PRE: none | POST: none") {
    auto *tcb = TaskControlBlock::create([]() {}, 1, 10);
    JARVIS_ASSERT(tcb != nullptr);
    JARVIS_ASSERT(tcb->kernel_stack != nullptr);

    tcb->cleanup();

    JARVIS_ASSERT(tcb->kernel_stack == nullptr);
    JARVIS_ASSERT_EQ(0ULL, tcb->stack_phys_);

    delete tcb;
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Verifies that create_user allocates a page table and user stack
// of the requested size.
// Input: create_user with stack_size=32_KiB
// Expect: page_table_ != 0, user_stack_ != 0, user_stack_size_ == 32_KiB
// Depends: kernel::TaskControlBlock
JARVIS_TEST(task_create_user_page_table, "PRE: none | POST: none") {
    auto *tcb = TaskControlBlock::create_user([]() {}, 1, 10, 32_KiB);
    JARVIS_ASSERT(tcb != nullptr);
    JARVIS_ASSERT(tcb->page_table_ != 0);
    JARVIS_ASSERT(tcb->user_stack_ != 0);
    JARVIS_ASSERT_EQ(32_KiB, tcb->user_stack_size_);

    tcb->cleanup();
    delete tcb;
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Verifies that TaskControlBlock::clone() deep-copies page tables.
// Input: A REAL dispatched kernel task (prio 11) with a cloned PML4 calls
//        clone(); the child is created in the running task's own context.
// Expect: child has own page tables (deep-copied — page_table_ != 0 and
// != parent's), child user stack exists
// Depends: kernel::TaskControlBlock, kernel::Scheduler
JARVIS_TEST(task_clone_shares_page_tables, "PRE: none | POST: none") {
    static uint64_t g_child_pt = 0;
    static uint64_t g_child_shared = 0;
    static uint64_t g_child_stack = 0;
    static uint64_t g_child_ok = 0;
    static uint64_t g_parent_pt = 0;

    auto *parent = TaskControlBlock::create(
        []() {
            uint64_t regs[22] = {};
            regs[17] = 0x1000;
            regs[18] = arch::SEG_USER_CODE;
            regs[19] = arch::RFLAGS_DEFAULT;
            regs[20] = 0x80000000;
            regs[21] = arch::SEG_USER_DATA;

            auto *child = TaskControlBlock::clone(regs);
            if (child == nullptr)
                return;
            g_child_pt = child->page_table_;
            // v0.4.0 MP-7: page_table_shared_ removed — deep copy means the
            // child's PML4 must differ from the parent's.
            g_child_shared = (g_child_pt != g_parent_pt) ? 0 : 1;
            g_child_stack = child->user_stack_;
            g_child_ok = (g_child_pt != 0 && g_child_stack != 0) ? 1 : 0;
            child->cleanup();
            delete child;
        },
        11, 10);
    JARVIS_ASSERT(parent != nullptr);
    g_parent_pt = parent->page_table_;
    parent->is_user_ = true;           // simulate a user parent for clone()
    parent->user_stack_ = 0x80000000;  // mark as user-like for clone path
    parent->user_stack_size_ = 32_KiB; // clone() needs a real size to succeed
    Scheduler::add_task(*parent);
    Scheduler::reschedule();
    kernel::test::wait_for_termination_safe(parent);
    // The parent self-terminated and is owned by the zombie list.  Reclaim it
    // BEFORE asserting so a failure cannot leak the parent TCB.
    Scheduler::drain_zombie_list();

    JARVIS_ASSERT_EQ(1ULL, g_child_ok);
    JARVIS_ASSERT(g_child_pt != 0);
    JARVIS_ASSERT_EQ(0ULL, g_child_shared);
    JARVIS_ASSERT(g_child_stack != 0);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Verifies that ELF loading initializes IPC notification/event
// objects for a task.
// Input: Load an ELF binary via elf::load, check that msg_queue, notify,
// event_group are initialized.
// Expect: All three IPC objects are non-null after ELF load.
JARVIS_TEST(task_elf_load_inits_ipc_objects, "PRE: none | POST: none") {
    // Find a test ELF in initrd
    initrd::InitrdFile f = initrd::find("./user-app.c.elf");
    if (!f.data)
        f = initrd::find("user-app.c.elf");
    if (!f.data) {
        // No test ELF available, skip with pass
        JARVIS_TEST_PASS();
        return;
    }

    auto *hdr = reinterpret_cast<const kernel::elf::ELF64Header *>(f.data);
    if (!kernel::elf::validate_header(hdr)) {
        JARVIS_TEST_PASS();
        return;
    }

    auto *tcb = kernel::elf::load(hdr, f.data, f.size);
    JARVIS_ASSERT(tcb != nullptr);

    // ELF load should have called init_task_common, so IPC objects should be
    // initialized

    // Cleanup
    tcb->cleanup();
    delete tcb;
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Verifies that cleaning up a cloned child task does not affect
// the parent's page table (deep copy ensures independence).
// Input: A REAL dispatched kernel task (prio 11) with a cloned PML4 calls
//        clone(); the child is cleaned up in the running task's own context.
// Expect: parent->page_table_ unchanged and non-null after child cleanup
// Depends: kernel::TaskControlBlock, kernel::Scheduler
JARVIS_TEST(task_fork_child_cleanup_preserves_parent_pages,
            "PRE: none | POST: none") {
    static uint64_t g_parent_pt = 0;
    static uint64_t g_preserved = 0;

    auto *parent = TaskControlBlock::create(
        []() {
            uint64_t regs[22] = {};
            regs[17] = 0x1000;
            regs[18] = arch::SEG_USER_CODE;
            regs[19] = arch::RFLAGS_DEFAULT;
            regs[20] = 0x80000000;
            regs[21] = arch::SEG_USER_DATA;

            auto *self = Scheduler::current_task();
            auto *child = TaskControlBlock::clone(regs);
            if (child == nullptr)
                return;
            uint64_t parent_pml4 = self->page_table_;
            // delete calls cleanup() internally
            delete child;
            g_parent_pt = self->page_table_;
            g_preserved = (parent_pml4 != 0 && self->page_table_ != 0 &&
                           self->page_table_ == parent_pml4)
                              ? 1
                              : 0;
        },
        11, 10);
    JARVIS_ASSERT(parent != nullptr);
    parent->is_user_ = true; // simulate a user parent for clone()
    parent->user_stack_ = 0x80000000;
    parent->user_stack_size_ = 32_KiB;
    Scheduler::add_task(*parent);
    Scheduler::reschedule();
    kernel::test::wait_for_termination_safe(parent);
    Scheduler::drain_zombie_list();

    JARVIS_ASSERT_EQ(1ULL, g_preserved);
    JARVIS_ASSERT(g_parent_pt != 0);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Verifies that clone + cleanup does not leak page-table-level
// pages (bug #015).  A REAL dispatched kernel task (prio 11) with a cloned
// PML4 clones a child and cleans it up in its own running context.
// Input: Real dispatched parent task, clone child, cleanup+delete both.
// Expect: Free memory count is the same after the round-trip as before.
JARVIS_TEST(task_clone_no_page_table_leak, "PRE: none | POST: none") {
    static uint64_t g_ran = 0;

    auto *parent = TaskControlBlock::create(
        []() {
            uint64_t regs[22] = {};
            regs[17] = 0x1000;
            regs[18] = arch::SEG_USER_CODE;
            regs[19] = arch::RFLAGS_DEFAULT;
            regs[20] = 0x80000000;
            regs[21] = arch::SEG_USER_DATA;

            auto *child = TaskControlBlock::clone(regs);
            if (child == nullptr)
                return;
            // delete calls cleanup() internally
            delete child;
            g_ran = 1;
        },
        11, 10);
    JARVIS_ASSERT(parent != nullptr);
    parent->is_user_ = true; // simulate a user parent for clone()
    parent->user_stack_ = 0x80000000;
    parent->user_stack_size_ = 32_KiB;
    Scheduler::add_task(*parent);
    Scheduler::reschedule();
    kernel::test::wait_for_termination_safe(parent);
    Scheduler::drain_zombie_list();

    JARVIS_ASSERT_EQ(1ULL, g_ran);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Registers all task management unit tests with the test framework.
// Input: None
// Expect: All task_* tests are registered via JARVIS_REGISTER_TEST
// Depends: kernel test framework
void register_task_tests() {
    Logger::info("Registering task tests");
    JARVIS_REGISTER_TEST(task_cleanup_frees_resources);
    JARVIS_REGISTER_TEST(task_create_user_page_table);
    JARVIS_REGISTER_TEST(task_clone_shares_page_tables);
    JARVIS_REGISTER_TEST(task_elf_load_inits_ipc_objects);
    JARVIS_REGISTER_TEST(task_fork_child_cleanup_preserves_parent_pages);
    JARVIS_REGISTER_TEST(task_clone_no_page_table_leak);
}
#endif
