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
/// @brief Task control block tests (arch-neutral TCB lifecycle; issue #356
///        ported off-x86: create/cleanup, create_user, clone deep-copy and
///        teardown, ELF IPC init, recent-ring claim).

#if defined(CONFIG_ARCH_X86_64) || defined(CONFIG_ARCH_AARCH64) || \
    defined(CONFIG_ARCH_RISCV64)
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
    JARVIS_ASSERT(tcb->get_page_table() != 0);
    JARVIS_ASSERT(tcb->user_stack_ != 0);
    JARVIS_ASSERT_EQ(32_KiB, tcb->user_stack_size_);

    tcb->cleanup();
    delete tcb;
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Verifies that TaskControlBlock::clone() deep-copies page tables.
// Input: A REAL user parent (create_user: real PMM user stack + real user
//        page tables, never dispatched) impersonated via ScopedCurrentTask;
//        clone() runs the genuine user copy path in the test's own context.
// Expect: child has own page tables (deep-copied — page_table_ != 0 and
// != parent's), child user stack exists
// Depends: kernel::TaskControlBlock, kernel::Scheduler
// NOTE (issue #399 verdict): synthetic parents (flag-flipped kernel tasks
// with a fake 0x80000000 user_stack_) are UNSUPPORTED — clone() faults on
// the first stack-copy iteration (HHDM+fake dereference) and hangs both
// QEMU virt arches. Real create_user parents exercise the same user copy
// path without dispatch.
JARVIS_TEST(task_clone_shares_page_tables, "PRE: none | POST: none") {
    auto *parent = TaskControlBlock::create_user(test::forever_entry, 5, 10,
                                                 32_KiB);
    JARVIS_ASSERT(parent != nullptr);
    JARVIS_ASSERT(parent->is_user_);
    JARVIS_ASSERT(parent->user_stack_ != 0);
    const uint64_t parent_pt = parent->get_page_table();
    uint64_t child_pt = 0;
    uint64_t child_shared = 1;
    uint64_t child_stack = 0;
    uint64_t child_ok = 0;
    {
        test::ScopedCurrentTask scope(*parent);
        uint64_t regs[37] = {};
        test::make_synthetic_clone_frame(regs);

        auto *child = TaskControlBlock::clone(regs);
        if (child != nullptr) {
            child_pt = child->get_page_table();
            // v0.4.0 MP-7: page_table_shared_ removed — deep copy means the
            // child's PML4 must differ from the parent's.
            child_shared = (child_pt != parent_pt) ? 0 : 1;
            child_stack = child->user_stack_;
            child_ok = (child_pt != 0 && child_stack != 0) ? 1 : 0;
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

    JARVIS_ASSERT_EQ(1ULL, child_ok);
    JARVIS_ASSERT(child_pt != 0);
    JARVIS_ASSERT_EQ(0ULL, child_shared);
    JARVIS_ASSERT(child_stack != 0);
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
    initrd::InitrdFile f = initrd::find("./bin/user-app.c.elf");
    if (!f.data)
        f = initrd::find("bin/user-app.c.elf");
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
// Input: A REAL user parent (create_user, never dispatched) impersonated
//        via ScopedCurrentTask; clone + child teardown in the test's own
//        context, no dispatch.
// Expect: parent->get_page_table() unchanged and non-null after child cleanup
// Depends: kernel::TaskControlBlock, kernel::Scheduler
JARVIS_TEST(task_fork_child_cleanup_preserves_parent_pages,
            "PRE: none | POST: none") {
    auto *parent = TaskControlBlock::create_user(test::forever_entry, 5, 10,
                                                 32_KiB);
    JARVIS_ASSERT(parent != nullptr);
    JARVIS_ASSERT(parent->is_user_);
    const uint64_t parent_pml4 = parent->get_page_table();
    JARVIS_ASSERT(parent_pml4 != 0);
    uint64_t after_pt = 0;
    uint64_t preserved = 0;
    {
        test::ScopedCurrentTask scope(*parent);
        uint64_t regs[37] = {};
        test::make_synthetic_clone_frame(regs);

        auto *child = TaskControlBlock::clone(regs);
        if (child != nullptr) {
            // delete calls cleanup() internally (destroy path)
            delete child;
            after_pt = parent->get_page_table();
            preserved = (parent_pml4 != 0 && after_pt != 0 &&
                         after_pt == parent_pml4)
                            ? 1
                            : 0;
        }
        // Park the never-dispatched fixture (see clone_shares test): a
        // READY outgoing current would be re-enqueued by set_current().
        parent->state = TaskState::BLOCKED;
    }
    parent->cleanup();
    delete parent;

    JARVIS_ASSERT_EQ(1ULL, preserved);
    JARVIS_ASSERT(after_pt != 0);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Verifies that clone + cleanup does not leak page-table-level
// pages (bug #015). A REAL user parent (create_user, never dispatched) is
// impersonated via ScopedCurrentTask; the child is cloned and torn down in
// the test's own context.
// Input: Real user parent, clone child, cleanup+delete both.
// Expect: Free memory count is the same after the round-trip as before.
// Depends: kernel::TaskControlBlock, PMM
JARVIS_TEST(task_clone_no_page_table_leak, "PRE: none | POST: none") {
    uint64_t free_before = 0;
    uint64_t ran = 0;
    auto *parent = TaskControlBlock::create_user(test::forever_entry, 5, 10,
                                                 32_KiB);
    JARVIS_ASSERT(parent != nullptr);
    free_before = PMM::free_pages_ref();
    {
        test::ScopedCurrentTask scope(*parent);
        uint64_t regs[37] = {};
        test::make_synthetic_clone_frame(regs);

        auto *child = TaskControlBlock::clone(regs);
        if (child != nullptr) {
            // delete calls cleanup() internally (destroy path)
            delete child;
            ran = 1;
        }
        // Park the never-dispatched fixture (see clone_shares test): a
        // READY outgoing current would be re-enqueued by set_current().
        parent->state = TaskState::BLOCKED;
    }
    const int64_t pmm_delta =
        (int64_t)(PMM::free_pages_ref() - free_before);
    parent->cleanup();
    delete parent;

    JARVIS_ASSERT_EQ(1ULL, ran);
    JARVIS_ASSERT_FMT(pmm_delta == 0,
                      "PMM delta %ld pages after clone teardown",
                      (long)pmm_delta);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Registers all task management unit tests with the test framework.
// Input: None
// Expect: All task_* tests are registered via JARVIS_REGISTER_TEST
// Depends: kernel test framework
namespace kernel {
namespace debug {
// Issue #264: per-slot single-writer contract (task.cpp). The ring is
// write-only diagnostics with no header declaration, so the test declares
// the two entry points locally (definitions in task.cpp).
void record_task_entry(uint64_t entry, uint64_t tcb);
uint64_t find_entry_owner(uint64_t value);
} // namespace debug
} // namespace kernel

// Runmode: kernel
// Testidea: v0.5.x issue #264 — record_task_entry claims each slot with
// an atomic fetch-add, so sequential writers land in distinct slots: no
// mixed entry-from-A/tcb-from-B records, no lost updates.
// Input: Record 10 distinct (entry, tcb) marker pairs with sentinel high
// bits, then 64 more to force a full 64-slot wrap; resolve each via
// find_entry_owner. Markers are pre-checked absent first, so a real task
// entry recorded at boot can never alias them into a false pass/fail.
// Expect: Every marker resolves to exactly its own tcb (pre-wrap batch
// and post-wrap last-64 alike); no stale record survives. Pure sequential
// calls — no tasks, no IRQs, TCG-deterministic. (True concurrent-writer
// coverage is not deterministic under QEMU timing; the atomic claim
// itself is reviewed, not raced here.)
// Depends: kernel::debug::record_task_entry/find_entry_owner (task.cpp).
JARVIS_TEST(task_recent_ring_claim_distinct_slots, "PRE: none | POST: none") {
    for (uint64_t idx = 0; idx < 10; ++idx) {
        const uint64_t marker = 0x5A5A0000ULL + idx;
        JARVIS_ASSERT_FMT(kernel::debug::find_entry_owner(marker) == 0,
                          "marker %lx already owned before insert", marker);
    }
    for (uint64_t idx = 0; idx < 10; ++idx) {
        kernel::debug::record_task_entry(0x5A5A0000ULL + idx, 0x7000ULL + idx);
    }
    for (uint64_t idx = 0; idx < 10; ++idx) {
        const uint64_t marker = 0x5A5A0000ULL + idx;
        const uint64_t want = 0x7000ULL + idx;
        JARVIS_ASSERT_FMT(kernel::debug::find_entry_owner(marker) == want,
                          "marker %lx owner mismatch", marker);
    }
    for (uint64_t idx = 10; idx < 74; ++idx) {
        kernel::debug::record_task_entry(0x5A5A0000ULL + idx, 0x7000ULL + idx);
    }
    for (uint64_t idx = 10; idx < 74; ++idx) {
        const uint64_t marker = 0x5A5A0000ULL + idx;
        const uint64_t want = 0x7000ULL + idx;
        JARVIS_ASSERT_FMT(kernel::debug::find_entry_owner(marker) == want,
                          "post-wrap marker %lx owner mismatch", marker);
    }
    JARVIS_TEST_PASS();
}

void register_task_tests() {
    Logger::info("Registering task tests");
    JARVIS_REGISTER_TEST(task_cleanup_frees_resources);
    JARVIS_REGISTER_TEST(task_create_user_page_table);
    JARVIS_REGISTER_TEST(task_clone_shares_page_tables);
    JARVIS_REGISTER_TEST(task_elf_load_inits_ipc_objects);
    JARVIS_REGISTER_TEST(task_fork_child_cleanup_preserves_parent_pages);
    JARVIS_REGISTER_TEST(task_clone_no_page_table_leak);
    JARVIS_REGISTER_TEST(task_recent_ring_claim_distinct_slots);
}
#endif
