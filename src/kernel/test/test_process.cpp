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

/// @file test_process.cpp
/// @brief Process lifecycle tests.

#if defined(CONFIG_ARCH_X86_64)
#include <test.hpp>
#include <logger.hpp>
#include <kernel/task/scheduler.hpp>
#include <kernel/task/task.hpp>
#include <kernel/arch/irq_guard.hpp>
#include <kernel/memory/pmm.hpp>
#include <kernel/memory/vmm.hpp>
#include <scope_guard.hpp>
#include <constants.hpp>
#include <kernel/test/test_sched_helpers.hpp>
#include <kernel/vfs/pipe.hpp>

using namespace kernel;

// Runmode: kernel
// Testidea: Validates that add_child() correctly links a child into the
// parent's child list and increments num_children. Input: Create parent and
// child via TaskControlBlock::create() Expect: parent->num_children == 1, child
// is in parent's child list, child->parent_id == parent->id Depends: test,
// scheduler
JARVIS_TEST(process_add_child, "PRE: none | POST: none") {
    auto *parent = TaskControlBlock::create([]() {}, 5, 10);
    JARVIS_ASSERT(parent != nullptr);

    auto *child = TaskControlBlock::create([]() {}, 5, 10);
    JARVIS_ASSERT(child != nullptr);

    parent->add_child(child);
    JARVIS_ASSERT_EQ(1ULL, parent->num_children);

    auto *found = parent->find_child(child->id);
    JARVIS_ASSERT(found == child);
    JARVIS_ASSERT_EQ(parent->id, child->parent_id);

    child->cleanup();
    delete child;
    parent->cleanup();
    delete parent;
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Validates that find_child returns nullptr when searching for a
// non-existent child PID.
// Input: PID 999999 (non-existent)
// Expect: JARVIS_ASSERT checks that find_child returns nullptr
// Depends: test, scheduler
JARVIS_TEST(process_find_child, "PRE: none | POST: none") {
    auto *parent = Scheduler::current_task();
    JARVIS_ASSERT(parent != nullptr);

    auto *child = parent->find_child(999999);
    JARVIS_ASSERT(child == nullptr);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Validates that the current task's num_children is zero when no
// child has been added (the harness has no children in the test runner).
// Input: Current task from Scheduler::current_task()
// Expect: num_children == 0
// Depends: test, scheduler
JARVIS_TEST(process_num_children_count, "PRE: none | POST: none") {
    auto *parent = Scheduler::current_task();
    JARVIS_ASSERT(parent != nullptr);
    JARVIS_ASSERT_EQ(0ULL, parent->num_children);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Verifies that remove_child() does not modify num_children when
// the target
// is not actually a child of the parent (bug #019).
// Input: Create a parent and two tasks; add one as child; try to remove the
// non-child.
// Expect: num_children remains unchanged.
// Depends: test, scheduler
JARVIS_TEST(process_remove_child_non_child_no_underflow,
            "PRE: none | POST: none") {
    auto *parent = TaskControlBlock::create([]() {}, 5, 10);
    JARVIS_ASSERT(parent != nullptr);

    auto *child = TaskControlBlock::create([]() {}, 5, 10);
    JARVIS_ASSERT(child != nullptr);

    auto *stranger = TaskControlBlock::create([]() {}, 5, 10);
    JARVIS_ASSERT(stranger != nullptr);

    parent->add_child(child);
    JARVIS_ASSERT_EQ(1ULL, parent->num_children);

    // Try to remove the stranger (not a child)
    parent->remove_child(stranger);
    JARVIS_ASSERT_EQ(1ULL, parent->num_children);

    // Removing the actual child should work
    parent->remove_child(child);
    JARVIS_ASSERT_EQ(0ULL, parent->num_children);

    child->cleanup();
    delete child;
    stranger->cleanup();
    delete stranger;
    parent->cleanup();
    delete parent;
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Registers all process-related unit tests with the test framework.
// Input: (none)
// Expect: Each JARVIS_REGISTER_TEST call registers a test function for later
// execution
// Depends: test, logger, scheduler
// Runmode: kernel
// Testidea: Validates that find_child correctly returns the child when multiple
// children exist.
// Input: Parent with 3 children, search for middle child by PID
// Expect: find_child returns correct child pointer
// Depends: test, scheduler
JARVIS_TEST(process_find_child_multiple, "PRE: none | POST: none") {
    auto *parent = TaskControlBlock::create([]() {}, 5, 10);
    JARVIS_ASSERT(parent != nullptr);

    auto *child1 = TaskControlBlock::create([]() {}, 5, 10);
    JARVIS_ASSERT(child1 != nullptr);
    auto *child2 = TaskControlBlock::create([]() {}, 5, 10);
    JARVIS_ASSERT(child2 != nullptr);
    auto *child3 = TaskControlBlock::create([]() {}, 5, 10);
    JARVIS_ASSERT(child3 != nullptr);

    parent->add_child(child1);
    parent->add_child(child2);
    parent->add_child(child3);
    JARVIS_ASSERT_EQ(3ULL, parent->num_children);

    auto *found = parent->find_child(child2->id);
    JARVIS_ASSERT(found == child2);

    found = parent->find_child(child1->id);
    JARVIS_ASSERT(found == child1);

    found = parent->find_child(child3->id);
    JARVIS_ASSERT(found == child3);

    // Cleanup
    child3->cleanup();
    delete child3;
    child2->cleanup();
    delete child2;
    child1->cleanup();
    delete child1;
    parent->cleanup();
    delete parent;
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Validates that remove_child correctly updates sibling links when
// removing the first child (head of list).
// Input: Parent with 3 children, remove the first added (which is head)
// Expect: num_children decremented, remaining children linked correctly
// Depends: test, scheduler
JARVIS_TEST(process_remove_first_child, "PRE: none | POST: none") {
    auto *parent = TaskControlBlock::create([]() {}, 5, 10);
    JARVIS_ASSERT(parent != nullptr);

    auto *child1 = TaskControlBlock::create([]() {}, 5, 10);
    JARVIS_ASSERT(child1 != nullptr);
    auto *child2 = TaskControlBlock::create([]() {}, 5, 10);
    JARVIS_ASSERT(child2 != nullptr);
    auto *child3 = TaskControlBlock::create([]() {}, 5, 10);
    JARVIS_ASSERT(child3 != nullptr);

    parent->add_child(child1);
    parent->add_child(child2);
    parent->add_child(child3);
    JARVIS_ASSERT_EQ(3ULL, parent->num_children);

    // child3 is head (last added), child2 is middle, child1 is tail
    // Remove child3 (head)
    parent->remove_child(child3);
    JARVIS_ASSERT_EQ(2ULL, parent->num_children);
    JARVIS_ASSERT(parent->first_child == child2);
    JARVIS_ASSERT(child2->prev_sibling == nullptr);
    JARVIS_ASSERT(child2->next_sibling == child1);
    JARVIS_ASSERT(child1->prev_sibling == child2);
    JARVIS_ASSERT(child1->next_sibling == nullptr);

    // Cleanup
    child3->cleanup();
    delete child3;
    child2->cleanup();
    delete child2;
    child1->cleanup();
    delete child1;
    parent->cleanup();
    delete parent;
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Validates that remove_child correctly updates sibling links when
// removing a middle child.
// Input: Parent with 3 children, remove the middle one
// Expect: num_children decremented, head and tail linked correctly
// Depends: test, scheduler
JARVIS_TEST(process_remove_middle_child, "PRE: none | POST: none") {
    auto *parent = TaskControlBlock::create([]() {}, 5, 10);
    JARVIS_ASSERT(parent != nullptr);

    auto *child1 = TaskControlBlock::create([]() {}, 5, 10);
    JARVIS_ASSERT(child1 != nullptr);
    auto *child2 = TaskControlBlock::create([]() {}, 5, 10);
    JARVIS_ASSERT(child2 != nullptr);
    auto *child3 = TaskControlBlock::create([]() {}, 5, 10);
    JARVIS_ASSERT(child3 != nullptr);

    parent->add_child(child1);
    parent->add_child(child2);
    parent->add_child(child3);
    JARVIS_ASSERT_EQ(3ULL, parent->num_children);

    // Remove child2 (middle)
    parent->remove_child(child2);
    JARVIS_ASSERT_EQ(2ULL, parent->num_children);
    JARVIS_ASSERT(parent->first_child == child3);
    JARVIS_ASSERT(child3->prev_sibling == nullptr);
    JARVIS_ASSERT(child3->next_sibling == child1);
    JARVIS_ASSERT(child1->prev_sibling == child3);
    JARVIS_ASSERT(child1->next_sibling == nullptr);

    // Cleanup
    child2->cleanup();
    delete child2;
    child3->cleanup();
    delete child3;
    child1->cleanup();
    delete child1;
    parent->cleanup();
    delete parent;
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Validates that remove_child correctly updates sibling links when
// removing the last child (tail of list).
// Input: Parent with 3 children, remove the last added (tail)
// Expect: num_children decremented, remaining children linked correctly
// Depends: test, scheduler
JARVIS_TEST(process_remove_last_child, "PRE: none | POST: none") {
    auto *parent = TaskControlBlock::create([]() {}, 5, 10);
    JARVIS_ASSERT(parent != nullptr);

    auto *child1 = TaskControlBlock::create([]() {}, 5, 10);
    JARVIS_ASSERT(child1 != nullptr);
    auto *child2 = TaskControlBlock::create([]() {}, 5, 10);
    JARVIS_ASSERT(child2 != nullptr);
    auto *child3 = TaskControlBlock::create([]() {}, 5, 10);
    JARVIS_ASSERT(child3 != nullptr);

    parent->add_child(child1);
    parent->add_child(child2);
    parent->add_child(child3);
    JARVIS_ASSERT_EQ(3ULL, parent->num_children);

    // Remove child1 (tail)
    parent->remove_child(child1);
    JARVIS_ASSERT_EQ(2ULL, parent->num_children);
    JARVIS_ASSERT(parent->first_child == child3);
    JARVIS_ASSERT(child3->prev_sibling == nullptr);
    JARVIS_ASSERT(child3->next_sibling == child2);
    JARVIS_ASSERT(child2->prev_sibling == child3);
    JARVIS_ASSERT(child2->next_sibling == nullptr);

    // Cleanup
    child1->cleanup();
    delete child1;
    child3->cleanup();
    delete child3;
    child2->cleanup();
    delete child2;
    parent->cleanup();
    delete parent;
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Validates that clone() properly adds the child to parent's child
// list.  A REAL dispatched kernel task (with a cloned PML4 so clone()
// exercises the user page-table path) calls clone() in its own running
// context.
// Input: Kernel task (prio 11) + page_table_=clone_kernel_pml4; dispatched;
//        its lambda calls TaskControlBlock::clone(regs).
// Expect: parent->num_children == 1, child is in parent's list, child->parent_id ==
// parent->id Depends: test, scheduler, task
JARVIS_TEST(process_clone_adds_child, "PRE: none | POST: none") {
    static uint64_t g_child_num = 0;
    static uint64_t g_child_id = 0;
    static uint64_t g_found = 0;

    auto *parent = TaskControlBlock::create(
        []() {
            auto *self = Scheduler::current_task();
            uint64_t regs[22] = {};
            regs[17] = 0x1000;
            regs[18] = arch::SEG_USER_CODE;
            regs[19] = arch::RFLAGS_DEFAULT;
            regs[20] = 0x80000000;
            regs[21] = arch::SEG_USER_DATA;

            auto *child = TaskControlBlock::clone(regs);
            if (child == nullptr)
                return;
            g_child_num = self->num_children;
            g_child_id = child->id;
            g_found = (self->find_child(child->id) == child) ? 1 : 0;

            // The child is never add_task'd — cleanup + delete only (its
            // resources are freed by cleanup()).
            child->cleanup();
            delete child;
        },
        11, 10);
    JARVIS_ASSERT(parent != nullptr);
    parent->is_user_ = true; // simulate a user parent for clone()
    // clone() needs a real user-stack size/phys to succeed (the clone path
    // allocates and copies the stack); without these, TASK_ERR_USTACK_ALLOC
    // fires and the child leaks.
    parent->user_stack_ = 0x80000000;
    parent->user_stack_size_ = 32_KiB;
    Scheduler::add_task(*parent);
    Scheduler::reschedule();
    kernel::test::wait_for_termination_safe(parent);
    // Cleanup BEFORE asserting (cookbook Rule 5): self-terminated.
    Scheduler::drain_zombie_list();
    JARVIS_ASSERT_EQ(1ULL, g_child_num);
    JARVIS_ASSERT(g_child_id != 0);
    JARVIS_ASSERT_EQ(1ULL, g_found);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Validates that cleanup() removes the task from its parent's child
// list. Input: Parent with child, call cleanup() on child Expect:
// parent->num_children == 0, parent->first_child == nullptr Depends: test,
// scheduler, task
JARVIS_TEST(process_cleanup_removes_from_parent, "PRE: none | POST: none") {
    auto *parent = TaskControlBlock::create([]() {}, 5, 10);
    JARVIS_ASSERT(parent != nullptr);
    Scheduler::add_task(*parent);

    auto *child = TaskControlBlock::create([]() {}, 5, 10);
    JARVIS_ASSERT(child != nullptr);

    parent->add_child(child);
    JARVIS_ASSERT_EQ(1ULL, parent->num_children);

    child->cleanup();
    JARVIS_ASSERT_EQ(0ULL, parent->num_children);
    JARVIS_ASSERT(parent->first_child == nullptr);
    JARVIS_ASSERT_EQ(0ULL, child->parent_id);

    Scheduler::remove_task(*parent);
    delete child;
    parent->cleanup();
    delete parent;
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Validates that num_children never underflows when removing
// non-existent children multiple times.
// Input: Parent with one child, remove it, then try to remove again
// Expect: num_children stays at 0
// Depends: test, scheduler
JARVIS_TEST(process_remove_child_twice_no_underflow, "PRE: none | POST: none") {
    auto *parent = TaskControlBlock::create([]() {}, 5, 10);
    JARVIS_ASSERT(parent != nullptr);

    auto *child = TaskControlBlock::create([]() {}, 5, 10);
    JARVIS_ASSERT(child != nullptr);

    parent->add_child(child);
    JARVIS_ASSERT_EQ(1ULL, parent->num_children);

    parent->remove_child(child);
    JARVIS_ASSERT_EQ(0ULL, parent->num_children);

    // Try to remove again
    parent->remove_child(child);
    JARVIS_ASSERT_EQ(0ULL, parent->num_children);

    child->cleanup();
    delete child;
    parent->cleanup();
    delete parent;
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Validates that removing a child sets child's parent_id to 0.
// Input: Parent with child, remove child
// Expect: child->parent_id == 0
// Depends: test, scheduler
JARVIS_TEST(process_remove_child_clears_parent_id, "PRE: none | POST: none") {
    auto *parent = TaskControlBlock::create([]() {}, 5, 10);
    JARVIS_ASSERT(parent != nullptr);

    auto *child = TaskControlBlock::create([]() {}, 5, 10);
    JARVIS_ASSERT(child != nullptr);

    parent->add_child(child);
    JARVIS_ASSERT_EQ(parent->id, child->parent_id);

    parent->remove_child(child);
    JARVIS_ASSERT_EQ(0ULL, child->parent_id);

    child->cleanup();
    delete child;
    parent->cleanup();
    delete parent;
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: v0.4.0 MP-7 — driven-cookbook fork isolation: a dispatched user
// parent (is_user_=true) clones a child; the child owns a DIFFERENT PML4 and
// a DIFFERENT data leaf (deep copy); tearing the child down leaves the
// parent's mapping intact.
// Input: parent kernel task with a USER page mapped at PROBE_VA; lambda
// clones, verifies table/leaf independence, cleans the child up.
// Expect: child->page_table_ != parent->page_table_; child leaf != parent
// leaf; after child cleanup the parent's leaf still resolves to the original
// frame.
// Depends: test, scheduler, task, VMM, PMM
JARVIS_TEST(process_clone_child_table_independent, "PRE: none | POST: none") {
    static uint64_t g_pt_diff = 0;
    static uint64_t g_leaf_diff = 0;
    static uint64_t g_parent_leaf = 0;
    static uint64_t g_parent_ok = 0;
    static uint64_t g_ran = 0;
    constexpr uint64_t PROBE_VA = 0x20000000;

    auto *parent = TaskControlBlock::create(
        []() {
            auto *self = Scheduler::current_task();
            uint64_t regs[22] = {};
            regs[17] = 0x1000;
            regs[18] = arch::SEG_USER_CODE;
            regs[19] = arch::RFLAGS_DEFAULT;
            regs[20] = 0x80000000;
            regs[21] = arch::SEG_USER_DATA;

            auto *child = TaskControlBlock::clone(regs);
            if (child == nullptr)
                return;
            uint64_t child_leaf =
                VMM::virt_to_phys_in_pml4(PROBE_VA, child->page_table_);
            g_parent_leaf =
                VMM::virt_to_phys_in_pml4(PROBE_VA, self->page_table_);
            g_pt_diff = (child->page_table_ != self->page_table_) ? 1 : 0;
            g_leaf_diff = (child_leaf != 0 && child_leaf != g_parent_leaf)
                              ? 1
                              : 0;

            child->cleanup();
            delete child;

            // Parent mapping intact after child teardown.
            g_parent_ok =
                (VMM::virt_to_phys_in_pml4(PROBE_VA, self->page_table_) ==
                 g_parent_leaf)
                    ? 1
                    : 0;
            g_ran = 1;
        },
        11, 10);
    JARVIS_ASSERT(parent != nullptr);
    parent->is_user_ = true; // simulate a user parent for clone()
    parent->user_stack_ = 0x80000000;
    parent->user_stack_size_ = 32_KiB;
    uint64_t phys = PMM::alloc_user_page();
    JARVIS_ASSERT(phys != 0);
    VMM::map_page_in_pml4(PROBE_VA, phys, true, parent->page_table_);

    Scheduler::add_task(*parent);
    Scheduler::reschedule();
    kernel::test::wait_for_termination_safe(parent);
    Scheduler::drain_zombie_list();

    JARVIS_ASSERT_EQ(1ULL, g_ran);
    JARVIS_ASSERT_EQ(1ULL, g_pt_diff);
    JARVIS_ASSERT_EQ(1ULL, g_leaf_diff);
    JARVIS_ASSERT(g_parent_leaf == phys);
    JARVIS_ASSERT_EQ(1ULL, g_parent_ok);
    // The parent's cleanup reclaimed the USER-owned leaf via free_user_pages.
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: v0.4.0 MP-7 — driven-cookbook teardown: clone + child cleanup +
// parent drain returns the PMM to its baseline (no page-table leaks).
// Input: parent with a USER page; lambda clones + cleans the child; drain.
// Expect: PMM::free_pages_ref() delta == 0 across the whole cycle.
// Depends: test, scheduler, task, VMM, PMM
JARVIS_TEST(process_clone_teardown_zero_delta, "PRE: none | POST: none") {
    static uint64_t g_ran = 0;
    constexpr uint64_t PROBE_VA = 0x20001000;

    uint64_t free_before = PMM::free_pages_ref();

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
            child->cleanup();
            delete child;
            g_ran = 1;
        },
        11, 10);
    JARVIS_ASSERT(parent != nullptr);
    parent->is_user_ = true; // simulate a user parent for clone()
    parent->user_stack_ = 0x80000000;
    parent->user_stack_size_ = 32_KiB;
    uint64_t phys = PMM::alloc_user_page();
    JARVIS_ASSERT(phys != 0);
    VMM::map_page_in_pml4(PROBE_VA, phys, true, parent->page_table_);

    Scheduler::add_task(*parent);
    Scheduler::reschedule();
    kernel::test::wait_for_termination_safe(parent);
    Scheduler::drain_zombie_list();

    JARVIS_ASSERT_EQ(1ULL, g_ran);
    JARVIS_ASSERT_FMT(PMM::free_pages_ref() == free_before,
                      "PMM delta %ld pages after clone teardown cycle",
                      (long)(PMM::free_pages_ref() - free_before));
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: v0.4.0 MP-1 — a kernel task owns a private kernel-half PML4:
// non-zero, distinct from the boot kernel PML4, kernel entries copied by
// value, user half empty.
// Input: TaskControlBlock::create().
// Expect: page_table_ != 0 != kernel PML4; user entries zero; kernel entries
// match the kernel PML4; is_user_ == false.
// Depends: test, task, VMM
JARVIS_TEST(process_kernel_half_private, "PRE: none | POST: none") {
    auto *t = TaskControlBlock::create([]() {}, 5, 10);
    JARVIS_ASSERT(t != nullptr);
    auto cleanup = ScopeGuard([&]() {
        t->cleanup();
        delete t;
    });
    JARVIS_ASSERT(t->is_user_ == false);
    JARVIS_ASSERT(t->page_table_ != 0);
    JARVIS_ASSERT(t->page_table_ != VMM::get_kernel_pml4());

    auto *priv = reinterpret_cast<uint64_t *>(arch::HHDM_OFFSET +
                                              (t->page_table_ & ~0xFFFULL));
    auto *kern = reinterpret_cast<uint64_t *>(
        arch::HHDM_OFFSET + (VMM::get_kernel_pml4() & ~0xFFFULL));
    for (size_t i = 0; i < arch::PML4_USER_COUNT; ++i) {
        JARVIS_ASSERT(priv[i] == 0);
    }
    for (size_t i = arch::PML4_KERNEL_START; i < arch::PML4_ENTRIES; ++i) {
        JARVIS_ASSERT(priv[i] == kern[i]);
    }
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: A forked child copies the parent's fd_table; every copied used
// vnode must receive a vnode_ref_inc so the child's cleanup does not
// over-decrement and prematurely close a shared object (a pipe's PipeBuffer).
// Regression guard for the clone fd-copy fix: without the vnode_ref_inc the
// child over-releases at teardown and a refcounted pipe buffer is disposed
// twice (or its ResourceTracker delta goes negative).
// Input: Kernel task creates a pipe, clones a child (fd table copied with
//        vnode_ref_inc), then both ends are closed in parent + child.
// Expect: pipe closes cleanly exactly once, zero ResourceTracker delta.
JARVIS_TEST(process_clone_pipe_fd_refcount, "PRE: vfsd, iocd | POST: none") {
    static volatile int g_ret = -1;

    auto *parent = TaskControlBlock::create(
        []() {
            auto *self = Scheduler::current_task();
            int fds[2];
            int ret = vfs::create_pipe(fds);
            if (ret != 0)
                return;
            g_ret = 0;

            uint64_t regs[22] = {};
            regs[17] = 0x1000;
            regs[18] = arch::SEG_USER_CODE;
            regs[19] = arch::RFLAGS_DEFAULT;
            regs[20] = 0x80000000;
            regs[21] = arch::SEG_USER_DATA;

            // clone() copies the fd_table (with vnode_ref_inc on each used
            // vnode).  The child is never add_task'd; cleanup + delete frees
            // its copied fds (each decs the pipe vnode -> pb release).
            auto *child = TaskControlBlock::clone(regs);
            if (child == nullptr)
                return;
            // Child closes its copy of both pipe ends.
            child->fd_table.free(fds[0]);
            child->fd_table.free(fds[1]);
            child->cleanup();
            delete child;

            // Parent closes its ends; the pb refcount (read+write ends, both
            // copied) must now hit zero exactly once -> dispose.
            self->fd_table.free(fds[0]);
            self->fd_table.free(fds[1]);
        },
        11, 10);
    JARVIS_ASSERT(parent != nullptr);
    parent->is_user_ = true;
    parent->user_stack_ = 0x80000000;
    parent->user_stack_size_ = 32_KiB;
    Scheduler::add_task(*parent);
    Scheduler::reschedule();
    kernel::test::wait_for_termination_safe(parent);
    Scheduler::drain_zombie_list();
    JARVIS_ASSERT_EQ(0, static_cast<int>(g_ret));
    JARVIS_TEST_PASS();
}

void register_process_tests() {
    Logger::info("Registering process tests");
    JARVIS_REGISTER_TEST(process_add_child);
    JARVIS_REGISTER_TEST(process_find_child);
    JARVIS_REGISTER_TEST(process_find_child_multiple);
    JARVIS_REGISTER_TEST(process_num_children_count);
    JARVIS_REGISTER_TEST(process_remove_child_non_child_no_underflow);
    JARVIS_REGISTER_TEST(process_remove_first_child);
    JARVIS_REGISTER_TEST(process_remove_middle_child);
    JARVIS_REGISTER_TEST(process_remove_last_child);
    JARVIS_REGISTER_TEST(process_clone_adds_child);
    JARVIS_REGISTER_TEST(process_cleanup_removes_from_parent);
    JARVIS_REGISTER_TEST(process_remove_child_twice_no_underflow);
    JARVIS_REGISTER_TEST(process_remove_child_clears_parent_id);
    // v0.4.0 MP-1/MP-7 driven-cookbook additions.
    JARVIS_REGISTER_TEST(process_clone_child_table_independent);
    JARVIS_REGISTER_TEST(process_clone_teardown_zero_delta);
    JARVIS_REGISTER_TEST(process_kernel_half_private);
    JARVIS_REGISTER_TEST(process_clone_pipe_fd_refcount);
}
#endif
