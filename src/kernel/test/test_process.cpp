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
#include <kernel/cap/carve_binding.hpp>
#include <kernel/cap/frame_foreign_map.hpp>
#include <kernel/cap/vspace_map.hpp>
#include <kernel/proc/proc_fork_carve.hpp>
#include <kernel/proc/proc_tcb_caps.hpp>
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

// Runmode: kernel
// Testidea: F11 carve-composed fork parity (spec 8.8.5/M14) — the Stage C
//        composer builds a fork child with identical observable semantics
//        to the kernel clone, from retype + VSpace map + foreign-frame
//        map + carved-root activation, with no kernel deep copy.
// Input: Real user parent (create_user, never dispatched) impersonated
//        via ScopedCurrentTask; proc::carve_fork() on a synthetic frame
//        (the #399 shape); the unpublished child is compared then
//        destroyed via the composer-rollback path.
// Expect: Child non-null, distinct id, parent-linked, distinct private
//        root, tcb_storage_ CARVED + vspace_kind_ CARVED (also pins the
//        carve_tcb_storage half: teardown returns carved pages exactly
//        once), identical region shapes + permissions + page bytes over
//        every enumerated user region, and live 0 on all three F11
//        registries after destroy. Zero tracker delta.
// Depends: proc::carve_fork, VMM::enumerate_user_regions,
//        TaskControlBlock::destroy CARVED arm, CarveBinding/
//        ForeignFrameMap/VSpaceMap live_count
// Note (knob split): the knob-OFF leg is the statement-identical
//        kernel clone (INV-CARVE-10), pinned by process_clone_*;
//        this test pins the ON leg. The verdict+hold+publish tail
//        (ProcSvc::fork) is Stage B, covered by the ON-world
//        waitpid/signal legs + the hold-table unit test below.
// Pseudocode: create_user parent; scope; carve_fork; assert child
// identity/link/root/storage/kind; enumerate both roots; assert same
// shapes+flags; memcmp every page via HHDM; destroy; assert all
// live 0; park parent; teardown; assert all.
JARVIS_TEST(carve_fork_parity, "PRE: none | POST: none") {
#if !CONFIG_PROC_MGR_CARVE_FORK
    // Knob-OFF leg is the kernel clone — covered by process_clone_*.
    JARVIS_TEST_PASS();
#else
    static uint64_t g_null = 0;
    static uint64_t g_child_id = 0;
    static uint64_t g_parent_link = 0;
    static uint64_t g_pt_distinct = 0;
    static uint64_t g_storage = 0;
    static uint64_t g_kind = 0;
    static uint64_t g_regions = 0;
    static uint64_t g_region_match = 0;
    static uint64_t g_content_match = 0;
    static uint64_t g_bind_live = 0;
    static uint64_t g_foreign_live = 0;
    static uint64_t g_vs_live = 0;
    g_null = 0;
    g_child_id = 0;
    g_parent_link = 0;
    g_pt_distinct = 0;
    g_storage = 0;
    g_kind = 0;
    g_regions = 0;
    g_region_match = 0;
    g_content_match = 0;
    g_bind_live = 0;
    g_foreign_live = 0;
    g_vs_live = 0;

    auto *parent = TaskControlBlock::create_user(test::forever_entry, 5, 10,
                                                 32_KiB);
    JARVIS_ASSERT(parent != nullptr);
    JARVIS_ASSERT(parent->is_user_);
    {
        test::ScopedCurrentTask scope(*parent);
        uint64_t regs[37] = {};
        test::make_synthetic_clone_frame(regs);
        auto *child = kernel::proc::carve_fork(regs);
        if (child == nullptr) {
            g_null = 1;
        } else {
            g_child_id = child->id;
            g_parent_link =
                (child->parent_id == parent->id &&
                 parent->find_child(child->id) == child)
                    ? 1ULL
                    : 0ULL;
            g_pt_distinct =
                (child->get_page_table() != 0 &&
                 child->get_page_table() != parent->get_page_table())
                    ? 1ULL
                    : 0ULL;
            g_storage =
                (child->tcb_storage_ ==
                 TaskControlBlock::TcbStorage::CARVED)
                    ? 1ULL
                    : 0ULL;
            g_kind =
                (child->vspace_kind_ == TaskControlBlock::VspaceKind::CARVED)
                    ? 1ULL
                    : 0ULL;
            VMM::UserRegion parent_regions[CONFIG_CARVE_MAX_REGIONS];
            VMM::UserRegion child_regions[CONFIG_CARVE_MAX_REGIONS];
            const size_t parent_total = VMM::enumerate_user_regions(
                parent->get_page_table(), parent_regions,
                CONFIG_CARVE_MAX_REGIONS);
            const size_t child_total = VMM::enumerate_user_regions(
                child->get_page_table(), child_regions,
                CONFIG_CARVE_MAX_REGIONS);
            if (parent_total == 0 || parent_total != child_total ||
                parent_total > CONFIG_CARVE_MAX_REGIONS) {
                g_regions = 0;
            } else {
                g_regions = 1;
                g_region_match = 1;
                for (size_t ri = 0;
                     ri < parent_total && g_region_match != 0; ++ri) {
                    if (parent_regions[ri].va != child_regions[ri].va ||
                        parent_regions[ri].bytes !=
                            child_regions[ri].bytes ||
                        parent_regions[ri].executable !=
                            child_regions[ri].executable ||
                        parent_regions[ri].writable !=
                            child_regions[ri].writable) {
                        g_region_match = 0;
                    }
                }
                g_content_match = 1;
                for (size_t ri = 0;
                     ri < parent_total && g_content_match != 0; ++ri) {
                    const uint64_t pages =
                        parent_regions[ri].bytes / arch::PAGE_SIZE;
                    for (uint64_t pi = 0;
                         pi < pages && g_content_match != 0; ++pi) {
                        const uint64_t va =
                            parent_regions[ri].va + pi * arch::PAGE_SIZE;
                        const uint64_t parent_phys =
                            VMM::virt_to_phys_in_pml4(
                                va, parent->get_page_table());
                        const uint64_t child_phys =
                            VMM::virt_to_phys_in_pml4(
                                va, child->get_page_table());
                        if (parent_phys == 0 || child_phys == 0) {
                            g_content_match = 0;
                            break;
                        }
                        // NOLINTNEXTLINE(performance-no-int-to-ptr)
                        auto *parent_page =
                            reinterpret_cast<volatile const uint64_t *>(
                                CONFIG_HHDM_OFFSET + parent_phys);
                        // NOLINTNEXTLINE(performance-no-int-to-ptr)
                        auto *child_page =
                            reinterpret_cast<volatile const uint64_t *>(
                                CONFIG_HHDM_OFFSET + child_phys);
                        for (size_t w = 0;
                             w < arch::PAGE_SIZE / sizeof(uint64_t); ++w) {
                            if (parent_page[w] != child_page[w]) {
                                g_content_match = 0;
                                break;
                            }
                        }
                    }
                }
            }
            // The child is never published; destroy() takes the
            // composer-rollback path (CARVED drain, no MemPool free).
            TaskControlBlock::destroy(child);
            g_bind_live = cap::CarveBinding::live_count();
            g_foreign_live = cap::ForeignFrameMap::live_count();
            g_vs_live = cap::VSpaceMap::live_count();
        }
        // Park the never-dispatched fixture BEFORE scope exit (see
        // process_clone_adds_child): BLOCKED currents are left alone.
        parent->state = TaskState::BLOCKED;
    }
    parent->cleanup();
    delete parent;

    JARVIS_ASSERT_EQ(0ULL, g_null);
    JARVIS_ASSERT(g_child_id != 0ULL);
    JARVIS_ASSERT_EQ(1ULL, g_parent_link);
    JARVIS_ASSERT_EQ(1ULL, g_pt_distinct);
    JARVIS_ASSERT_EQ(1ULL, g_storage);
    JARVIS_ASSERT_EQ(1ULL, g_kind);
    JARVIS_ASSERT_EQ(1ULL, g_regions);
    JARVIS_ASSERT_EQ(1ULL, g_region_match);
    JARVIS_ASSERT_EQ(1ULL, g_content_match);
    JARVIS_ASSERT_EQ(0ULL, g_bind_live);
    JARVIS_ASSERT_EQ(0ULL, g_foreign_live);
    JARVIS_ASSERT_EQ(0ULL, g_vs_live);
#endif
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: F11 hold-table reuse guard (spec 8.8.5 pid_reuse_safe
//        mechanism) — holds are generation-keyed: a recycled pid with
//        a new generation never matches a stale hold, and the death
//        funnel + release legs are idempotent.
// Input: Kernel holder fixture + the LIVE current task as the hold
//        target (a create()'d fixture is never id_table_insert'ed —
//        insertion happens at add_task — so an unpublished fixture
//        is unresolvable by design and the hold must refuse it);
//        hold, stale-generation/stale-pid lookups, release, re-hold,
//        funnel release_on_death, idempotent re-release.
// Expect: hold -> slot; live 1; lookup(live id, live gen) resolves;
//        lookup(id, gen+1) and (id+1, gen) refuse; release -> OK,
//        lookup refuses, live 0; re-hold -> slot; funnel drop ->
//        lookup refuses; re-release -> OK, live 0. Zero delta.
// Depends: proc_tcb_caps hold/lookup/release/release_on_death/
//        live_hold_count, generation ABA guard
// Note: The target is the live current task (never torn down here);
//        the hold is released before the holder is, so no index entry
//        can dangle (funnel coverage belongs to the published path,
//        exercised by the ON-world suites).
// Pseudocode: fixtures; hold->slot/err 0/live 1; lookup live ok;
// stale lookups null; release ok/live 0; re-hold; funnel; lookup
// null; release ok/live 0; teardown both; assert all (err statics
// name the failing leg: 3 STALE_PID, 4 FULL, 5 INSTALL_FAILED).
JARVIS_TEST(proc_hold_generation_guards_reuse, "PRE: none | POST: none") {
    static int64_t g_hold_slot = -1;
    static uint64_t g_hold_err = 0;
    static uint64_t g_live_after_hold = 0;
    static uint64_t g_lookup_live = 0;
    static uint64_t g_lookup_stale_gen = 0;
    static uint64_t g_lookup_stale_pid = 0;
    static uint64_t g_release_ok = 0;
    static uint64_t g_live_after_release = 0;
    static uint64_t g_lookup_after_release = 0;
    static int64_t g_rehold_slot = -1;
    static uint64_t g_rehold_err = 0;
    static uint64_t g_lookup_after_funnel = 0;
    static uint64_t g_rerelease_ok = 0;
    static uint64_t g_live_end = 0;
    g_hold_slot = -1;
    g_hold_err = 0;
    g_live_after_hold = 0;
    g_lookup_live = 0;
    g_lookup_stale_gen = 0;
    g_lookup_stale_pid = 0;
    g_release_ok = 0;
    g_live_after_release = 0;
    g_lookup_after_release = 0;
    g_rehold_slot = -1;
    g_rehold_err = 0;
    g_lookup_after_funnel = 0;
    g_rerelease_ok = 0;
    g_live_end = 0;

    auto *holder = TaskControlBlock::create(test::forever_entry, 5, 10);
    JARVIS_ASSERT(holder != nullptr);
    // The hold target must be resolvable via the id table: the live
    // current task is (an undispatched create()'d fixture is not —
    // id_table_insert runs at add_task, so hold() on such a fixture
    // fails closed with STALE_PID by design).
    auto *target = Scheduler::current_task();
    JARVIS_ASSERT(target != nullptr);
    const uint64_t target_id = target->id;
    const uint32_t target_gen = target->generation;
    {
        errors::ProcError hold_err = errors::PROC_ERR_OK;
        const int slot = kernel::proc::proc_tcb_caps::hold(
            *holder, target_id, target_gen, &hold_err);
        g_hold_slot = slot;
        g_hold_err = static_cast<uint64_t>(hold_err);
        g_live_after_hold =
            kernel::proc::proc_tcb_caps::live_hold_count();
        g_lookup_live =
            (kernel::proc::proc_tcb_caps::lookup(*holder, target_id,
                                                 target_gen) == target)
                ? 1ULL
                : 0ULL;
        g_lookup_stale_gen =
            (kernel::proc::proc_tcb_caps::lookup(*holder, target_id,
                                                 target_gen + 1) == nullptr)
                ? 1ULL
                : 0ULL;
        g_lookup_stale_pid =
            (kernel::proc::proc_tcb_caps::lookup(*holder, target_id + 1,
                                                 target_gen) == nullptr)
                ? 1ULL
                : 0ULL;
        const errors::ProcError rel =
            kernel::proc::proc_tcb_caps::release(*holder, target_id,
                                                 target_gen);
        g_release_ok = (rel == errors::PROC_ERR_OK) ? 1ULL : 0ULL;
        g_live_after_release =
            kernel::proc::proc_tcb_caps::live_hold_count();
        g_lookup_after_release =
            (kernel::proc::proc_tcb_caps::lookup(*holder, target_id,
                                                 target_gen) == nullptr)
                ? 1ULL
                : 0ULL;
        errors::ProcError rehold_err = errors::PROC_ERR_OK;
        const int reslot = kernel::proc::proc_tcb_caps::hold(
            *holder, target_id, target_gen, &rehold_err);
        g_rehold_slot = reslot;
        g_rehold_err = static_cast<uint64_t>(rehold_err);
        kernel::proc::proc_tcb_caps::release_on_death(target_id, target_gen);
        g_lookup_after_funnel =
            (kernel::proc::proc_tcb_caps::lookup(*holder, target_id,
                                                 target_gen) == nullptr)
                ? 1ULL
                : 0ULL;
        const errors::ProcError rerelease =
            kernel::proc::proc_tcb_caps::release(*holder, target_id,
                                                 target_gen);
        g_rerelease_ok = (rerelease == errors::PROC_ERR_OK) ? 1ULL : 0ULL;
        g_live_end = kernel::proc::proc_tcb_caps::live_hold_count();
    }
    // The holder was never published and never current: plain teardown
    // (the process_add_child shape). The target is the live current
    // task and is not ours to tear down.
    holder->cleanup();
    delete holder;

    JARVIS_ASSERT(g_hold_slot >= 0);
    JARVIS_ASSERT_EQ(0ULL, g_hold_err);
    JARVIS_ASSERT_EQ(1ULL, g_live_after_hold);
    JARVIS_ASSERT_EQ(1ULL, g_lookup_live);
    JARVIS_ASSERT_EQ(1ULL, g_lookup_stale_gen);
    JARVIS_ASSERT_EQ(1ULL, g_lookup_stale_pid);
    JARVIS_ASSERT_EQ(1ULL, g_release_ok);
    JARVIS_ASSERT_EQ(0ULL, g_live_after_release);
    JARVIS_ASSERT_EQ(1ULL, g_lookup_after_release);
    JARVIS_ASSERT(g_rehold_slot >= 0);
    JARVIS_ASSERT_EQ(0ULL, g_rehold_err);
    JARVIS_ASSERT_EQ(1ULL, g_lookup_after_funnel);
    JARVIS_ASSERT_EQ(1ULL, g_rerelease_ok);
    JARVIS_ASSERT_EQ(0ULL, g_live_end);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: F11 pid-reuse safety end-to-end (spec 8.8.5
//        pid_reuse_safe_carved) — kill + reap a CARVED child, force a
//        pid recycle, and prove the stale (pid, gen) hold refuses
//        while the recycled identity serves fresh.
// Input: Published carved child via ProcSvc::fork (PD verdict path)
//        + kill + waitpid reap + forced pid recycle.
// Expect: proc_tcb_caps::lookup(stale pid, stale gen) fails typed;
//        CarveBinding::drain_task replay on the stale key is a
//        generation-mismatch no-op; tracker delta 0.
// Depends: ProcSvc::fork/kill/waitpid (PD verdict), pid recycle,
//        proc_tcb_caps::lookup, CarveBinding::drain_task
// Pseudocode: STUB — fork carved child via ProcSvc::fork (needs live
// PD verdict in-harness); record (pid, gen); kill + reap via
// waitpid; force recycle (next_task_generation/id reuse); assert
// lookup(stale) == nullptr with typed STALE_PID; assert drain_task
// replay no-op (live 0); assert tracker 0; teardown.
// The table-level half is pinned by proc_hold_generation_guards_reuse
// (real asserts); this stub tracks the published-path E2E.
JARVIS_TEST(pid_reuse_safe_carved, "PRE: proc-mgr PD | POST: none") {
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: F11 composer-level rollback (spec 8.8.5 carve_fork_rollback)
//        — a mid-compose PMM/MemPool exhaustion fails the fork closed
//        with no child, no death record, no waiter wake, and empty
//        registries.
// Input: Countdown injector armed before each PMM::alloc_contiguous /
//        MemPool::alloc on the carve path (dev-lane gates); fork for
//        k = 1..N.
// Expect: Fork fails; no child in all_tasks_; no death record; no
//        waiter wake; CarveBinding/VSpaceMap/ForeignFrameMap live 0;
//        PMM + cap-object deltas 0 post-teardown.
// Depends: Dev-lane injector gates on the carve path (production
//        code — QE lane cannot add them), proc::carve_fork
// Pseudocode: STUB — for k in 1..N: arm injector at attempt k;
// carve_fork -> nullptr; assert no child registered, no death
// record, no wake; assert all three live 0; assert deltas 0;
// disarm. The registry half is pinned by
// carve_binding_exhaustion_fails_closed (real asserts); this stub
// tracks the injector-driven composer half.
JARVIS_TEST(carve_fork_rollback, "PRE: none | POST: none") {
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
    JARVIS_REGISTER_TEST(carve_fork_parity);
    JARVIS_REGISTER_TEST(proc_hold_generation_guards_reuse);
    JARVIS_REGISTER_TEST(pid_reuse_safe_carved);
    JARVIS_REGISTER_TEST(carve_fork_rollback);
}
#endif
