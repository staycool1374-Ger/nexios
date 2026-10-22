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
/// @brief Process (parent/child hierarchy) tests. Bookkeeping tests are
/// arch-neutral (issue #356); clone tests use REAL create_user parents
/// impersonated via ScopedCurrentTask (no dispatch) per the #399 verdict —
/// synthetic flag-flipped parents are unsupported and hang QEMU virt.

#if defined(CONFIG_ARCH_X86_64) || defined(CONFIG_ARCH_AARCH64) || \
    defined(CONFIG_ARCH_RISCV64)
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
// list. A REAL user parent (create_user, never dispatched) is impersonated
// via ScopedCurrentTask; clone() runs in the test's own context.
// Input: Real user parent fixture; clone via impersonated current task.
// Expect: parent->num_children == 1, child is in parent's list, child->parent_id ==
// parent->id Depends: test, scheduler, task
JARVIS_TEST(process_clone_adds_child, "PRE: none | POST: none") {
    auto *parent = TaskControlBlock::create_user(test::forever_entry, 5, 10,
                                                 32_KiB);
    JARVIS_ASSERT(parent != nullptr);
    JARVIS_ASSERT(parent->is_user_);
    uint64_t child_num = 0;
    uint64_t child_id = 0;
    uint64_t found = 0;
    {
        test::ScopedCurrentTask scope(*parent);
        uint64_t regs[37] = {};
        test::make_synthetic_clone_frame(regs);

        auto *child = TaskControlBlock::clone(regs);
        if (child != nullptr) {
            child_num = parent->num_children;
            child_id = child->id;
            found = (parent->find_child(child->id) == child) ? 1 : 0;

            // The child is never add_task'd — cleanup + delete only (its
            // resources are freed by cleanup()).
            child->cleanup();
            delete child;
        }
        // Park the never-dispatched fixture BEFORE scope exit: set_current()
        // re-enqueues a READY/RUNNING outgoing current, which would dispatch
        // this fixture on the next tick (then cleanup would free a live
        // task). BLOCKED outgoing currents are left alone.
        parent->state = TaskState::BLOCKED;
    }
    parent->cleanup();
    delete parent;

    JARVIS_ASSERT_EQ(1ULL, child_num);
    JARVIS_ASSERT(child_id != 0);
    JARVIS_ASSERT_EQ(1ULL, found);
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
    // Pinned FIXED (issue #19): fixtures must not dispatch mid-body
    // (pre-EDF prio 5 never preempted the harness); an EDF dispatch would
    // terminate them and corrupt the list asserts below.
    JARVIS_ASSERT(Scheduler::set_sched_policy(*parent, SchedPolicy::FIXED));
    Scheduler::add_task(*parent);

    auto *child = TaskControlBlock::create([]() {}, 5, 10);
    JARVIS_ASSERT(child != nullptr);
    JARVIS_ASSERT(Scheduler::set_sched_policy(*child, SchedPolicy::FIXED));

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
// Testidea: v0.4.0 MP-7 — driven-cookbook fork isolation: a real user
// parent (create_user, never dispatched) impersonated via ScopedCurrentTask
// clones a child; the child owns a DIFFERENT PML4 and a DIFFERENT data
// leaf (deep copy); tearing the child down leaves the parent's mapping
// intact.
// Input: Real user parent with a USER page mapped at PROBE_VA; clone via
// impersonated current task, verify table/leaf independence, clean up.
// Expect: child->get_page_table() != parent->get_page_table(); child leaf != parent
// leaf; after child teardown the parent's leaf still resolves to the original
// frame.
// Depends: test, scheduler, task, VMM, PMM
JARVIS_TEST(process_clone_child_table_independent, "PRE: none | POST: none") {
    constexpr uint64_t PROBE_VA = 0x20000000;

    auto *parent = TaskControlBlock::create_user(test::forever_entry, 5, 10,
                                                 32_KiB);
    JARVIS_ASSERT(parent != nullptr);
    JARVIS_ASSERT(parent->is_user_);
    uint64_t phys = PMM::alloc_user_page();
    JARVIS_ASSERT(phys != 0);
    VMM::map_page_in_pml4(PROBE_VA, phys, true, false, parent->get_page_table());

    uint64_t pt_diff = 0;
    uint64_t leaf_diff = 0;
    uint64_t parent_leaf = 0;
    uint64_t parent_ok = 0;
    uint64_t ran = 0;
    {
        test::ScopedCurrentTask scope(*parent);
        uint64_t regs[37] = {};
        test::make_synthetic_clone_frame(regs);

        auto *child = TaskControlBlock::clone(regs);
        if (child != nullptr) {
            uint64_t child_leaf = VMM::virt_to_phys_in_pml4(
                PROBE_VA, child->get_page_table());
            parent_leaf = VMM::virt_to_phys_in_pml4(PROBE_VA,
                                                    parent->get_page_table());
            pt_diff = (child->get_page_table() != parent->get_page_table())
                          ? 1
                          : 0;
            leaf_diff =
                (child_leaf != 0 && child_leaf != parent_leaf) ? 1 : 0;

            child->cleanup();
            delete child;

            // Parent mapping intact after child teardown.
            parent_ok = (VMM::virt_to_phys_in_pml4(PROBE_VA,
                                                  parent->get_page_table()) ==
                         parent_leaf)
                            ? 1
                            : 0;
            ran = 1;
        }
        // Park the never-dispatched fixture (see clone_adds_child): a READY
        // outgoing current would be re-enqueued by set_current().
        parent->state = TaskState::BLOCKED;
    }
    parent->cleanup();
    delete parent;

    JARVIS_ASSERT_EQ(1ULL, ran);
    JARVIS_ASSERT_EQ(1ULL, pt_diff);
    JARVIS_ASSERT_EQ(1ULL, leaf_diff);
    JARVIS_ASSERT(parent_leaf == phys);
    JARVIS_ASSERT_EQ(1ULL, parent_ok);
    // The parent's cleanup reclaimed the USER-owned leaf via free_user_pages.
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: v0.4.0 MP-7 — driven-cookbook teardown: clone + child cleanup
// returns the PMM to its baseline (no page-table leaks).
// Input: Real user parent (create_user, never dispatched) with a USER page;
// clone via impersonated current task + clean the child up.
// Expect: PMM::free_pages_ref() delta == 0 across the child cycle.
// Depends: test, scheduler, task, VMM, PMM
JARVIS_TEST(process_clone_teardown_zero_delta, "PRE: none | POST: none") {
    constexpr uint64_t PROBE_VA = 0x20001000;

    auto *parent = TaskControlBlock::create_user(test::forever_entry, 5, 10,
                                                 32_KiB);
    JARVIS_ASSERT(parent != nullptr);
    JARVIS_ASSERT(parent->is_user_);
    uint64_t phys = PMM::alloc_user_page();
    JARVIS_ASSERT(phys != 0);
    VMM::map_page_in_pml4(PROBE_VA, phys, true, false, parent->get_page_table());

    const uint64_t free_before = PMM::free_pages_ref();
    uint64_t ran = 0;
    {
        test::ScopedCurrentTask scope(*parent);
        uint64_t regs[37] = {};
        test::make_synthetic_clone_frame(regs);

            auto *child = TaskControlBlock::clone(regs);
            if (child != nullptr) {
                child->cleanup();
                delete child;
                ran = 1;
            }
        // Park the never-dispatched fixture (see clone_adds_child): a READY
        // outgoing current would be re-enqueued by set_current().
        parent->state = TaskState::BLOCKED;
    }
    const int64_t pmm_delta =
        (int64_t)(PMM::free_pages_ref() - free_before);
    parent->cleanup();
    delete parent;

    JARVIS_ASSERT_EQ(1ULL, ran);
    JARVIS_ASSERT_FMT(pmm_delta == 0,
                      "PMM delta %ld pages after clone teardown cycle",
                      (long)pmm_delta);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: v0.4.0 MP-1 — a kernel task owns a private kernel-half PML4:
// non-zero, distinct from the boot kernel PML4, kernel entries copied by
// value, user half empty — except riscv64 L0[0], which is the shared
// kernel identity/MMIO window by design (clone_kernel_pml4 deep-copies
// the subtree, U=0; #205/#221).
// Input: TaskControlBlock::create().
// Expect: page_table_ != 0 != kernel PML4; user entries zero (riscv64:
//         entry 0 present-but-U==0, rest zero); kernel entries match;
//         is_user_ == false.
// Depends: test, task, VMM
JARVIS_TEST(process_kernel_half_private, "PRE: none | POST: none") {
    auto *t = TaskControlBlock::create([]() {}, 5, 10);
    JARVIS_ASSERT(t != nullptr);
    auto cleanup = ScopeGuard([&]() {
        t->cleanup();
        delete t;
    });
    JARVIS_ASSERT(t->is_user_ == false);
    JARVIS_ASSERT(t->get_page_table() != 0);
    JARVIS_ASSERT(t->get_page_table() != VMM::get_kernel_pml4());

    auto *priv = reinterpret_cast<uint64_t *>(arch::HHDM_OFFSET +
                                              (t->get_page_table() & ~0xFFFULL));
    auto *kern = reinterpret_cast<uint64_t *>(
        arch::HHDM_OFFSET + (VMM::get_kernel_pml4() & ~0xFFFULL));
#if defined(CONFIG_ARCH_X86_64) || defined(CONFIG_ARCH_AARCH64)
    for (size_t i = 0; i < arch::PML4_USER_COUNT; ++i) {
        JARVIS_ASSERT(priv[i] == 0);
    }
#else
    // riscv64 L0[0]: shared kernel identity/MMIO window (present, never
    // user-accessible); the rest of the user half is empty. Sv39 PTE U
    // bit is 1<<4 (mirrors private VMM::PAGE_USER; vmm.hpp riscv64 block).
    constexpr uint64_t kSv39PteUser = 1ULL << 4;
    JARVIS_ASSERT(priv[0] != 0);
    JARVIS_ASSERT((priv[0] & kSv39PteUser) == 0);
    for (size_t i = 1; i < arch::PML4_USER_COUNT; ++i) {
        JARVIS_ASSERT(priv[i] == 0);
    }
#endif
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
// Input: Real user parent (create_user, never dispatched) creates a pipe,
// is impersonated via ScopedCurrentTask, clones a child (fd table copied
// with vnode_ref_inc), then both ends are closed in parent + child.
// Expect: pipe closes cleanly exactly once, zero ResourceTracker delta.
JARVIS_TEST(process_clone_pipe_fd_refcount, "PRE: vfsd, iocd | POST: none") {
    auto *parent = TaskControlBlock::create_user(test::forever_entry, 5, 10,
                                                 32_KiB);
    JARVIS_ASSERT(parent != nullptr);
    JARVIS_ASSERT(parent->is_user_);
    int ret = -1;
    {
        test::ScopedCurrentTask scope(*parent);
        int fds[2];
        ret = vfs::create_pipe(fds);
        if (ret == 0) {
            uint64_t regs[37] = {};
            test::make_synthetic_clone_frame(regs);

            // clone() copies the fd_table (with vnode_ref_inc on each used
            // vnode).  The child is never add_task'd; cleanup + delete frees
            // its copied fds (each decs the pipe vnode -> pb release).
            auto *child = TaskControlBlock::clone(regs);
            if (child != nullptr) {
                // Child closes its copy of both pipe ends.
                child->fd_table.free(fds[0]);
                child->fd_table.free(fds[1]);
                child->cleanup();
                delete child;

                // Parent closes its ends; the pb refcount (read+write ends,
                // both copied) must now hit zero exactly once -> dispose.
                parent->fd_table.free(fds[0]);
                parent->fd_table.free(fds[1]);
            } else {
                ret = -2;
            }
        }
        // Park the never-dispatched fixture (see clone_adds_child): a READY
        // outgoing current would be re-enqueued by set_current().
        parent->state = TaskState::BLOCKED;
    }
    parent->cleanup();
    delete parent;

    JARVIS_ASSERT_EQ(0, ret);
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
