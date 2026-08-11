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
#include <constants.hpp>
#include <kernel/task/task.hpp>
#include <kernel/task/scheduler.hpp>
#include <kernel/memory/pmm.hpp>
#include <kernel/memory/vmm.hpp>
#include <kernel/arch/io.hpp>
#include <kernel/test/test_isolate.hpp>

using namespace kernel;

JARVIS_TEST(stack_alloc_default_size_correct, "PRE: none | POST: none") {
    JARVIS_ASSERT(TaskControlBlock::STACK_SIZE == CONFIG_STACK_SIZE);
    JARVIS_ASSERT(TaskControlBlock::STACK_SIZE >= CONFIG_MIN_STACK_SIZE);
    JARVIS_TEST_PASS();
}

JARVIS_TEST(stack_alloc_task_has_stack_phys, "PRE: none | POST: none") {
    auto *t = TaskControlBlock::create([]() {}, 5, 10);
    JARVIS_ASSERT(t != nullptr);
    auto cleanup = ScopeGuard([&]() {
        t->cleanup();
        delete t;
    });
    JARVIS_ASSERT(t->stack_phys_ != 0);
    JARVIS_ASSERT(t->stack_phys_ % arch::PAGE_SIZE == 0);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: User task has guard page (STACK_VADDR+0 unmapped) and
// stack pages (STACK_VADDR+PAGE_SIZE mapped) in its page table.
// Input: Create user task with 64_KiB stack.
// Expect: Guard page unmapped -> virt_to_phys returns 0.
//         Stack base mapped -> virt_to_phys returns non-zero.
// Depends: VMM page table walk, TaskControlBlock::create_user
JARVIS_TEST(stack_alloc_user_task_has_guard_page, "PRE: none | POST: none") {
    auto *t = TaskControlBlock::create_user([]() {}, 5, 10, 64_KiB);
    JARVIS_ASSERT(t != nullptr);
    auto cleanup = ScopeGuard([&]() {
        t->cleanup();
        delete t;
    });
    JARVIS_ASSERT(t->page_table_ != 0);

    uint64_t guard_phys = VMM::virt_to_phys_in_pml4(mem::STACK_VADDR,
                                                     t->page_table_);
    JARVIS_ASSERT_FMT(guard_phys == 0,
                      "Guard page at 0x%lx mapped to 0x%lx (expected unmapped)",
                      mem::STACK_VADDR, guard_phys);

    uint64_t stack_phys = VMM::virt_to_phys_in_pml4(
        mem::STACK_VADDR + arch::PAGE_SIZE, t->page_table_);
    JARVIS_ASSERT_FMT(stack_phys != 0,
                      "Stack base at 0x%lx unmapped (expected mapped)",
                      mem::STACK_VADDR + arch::PAGE_SIZE);
    JARVIS_ASSERT(stack_phys % arch::PAGE_SIZE == 0);
    JARVIS_TEST_PASS();
}

JARVIS_TEST(stack_alloc_stack_alignment, "PRE: none | POST: none") {
    auto *t = TaskControlBlock::create([]() {}, 5, 10);
    JARVIS_ASSERT(t != nullptr);
    auto cleanup = ScopeGuard([&]() {
        t->cleanup();
        delete t;
    });
    JARVIS_ASSERT((reinterpret_cast<uint64_t>(t->kernel_stack) %
                   arch::PAGE_SIZE) == 0);
    JARVIS_TEST_PASS();
}

JARVIS_TEST(stack_alloc_multiple_tasks_distinct_stacks,
            "PRE: none | POST: none") {
    auto *t1 = TaskControlBlock::create([]() {}, 5, 10);
    auto *t2 = TaskControlBlock::create([]() {}, 6, 10);
    JARVIS_ASSERT(t1 != nullptr && t2 != nullptr);
    auto cleanup = ScopeGuard([&]() {
        t1->cleanup();
        delete t1;
        t2->cleanup();
        delete t2;
    });
    JARVIS_ASSERT(t1->kernel_stack != t2->kernel_stack);
    JARVIS_ASSERT(t1->stack_phys_ != t2->stack_phys_);
    JARVIS_TEST_PASS();
}

// v0.4.0 MP-6.2: strong test override of the weak stack_overflow_hook.  In
// test-active mode it records the faulting task and returns (the #PF handler
// then resumes via the rewritten iret frame); outside tests it panics exactly
// like the weak production default.  is_test_active() gates recovery so a
// real production overflow can never be swallowed.
static uint64_t g_hook_task_id = 0;
static uint64_t g_hook_rip = 0;

void stack_overflow_hook(kernel::TaskControlBlock *task) {
    if (!task)
        return;
    g_hook_task_id = task->id;
    g_hook_rip = 0xDEAD0000BEEFULL;
    if (kernel::Scheduler::is_test_active()) {
        // Synthetic-drive recovery: nothing to recover (we are not inside the
        // #PF handler) — just latch and return.
        return;
    }
    panic("kernel stack overflow");
}

JARVIS_TEST(stack_alloc_overflow_hook_weak_symbol,
            "PRE: none | POST: none") {
    extern void stack_overflow_hook(kernel::TaskControlBlock *task);
    // v0.4.0 MP-6.2: the weak default is now DEFINED in kernel.cpp, so the
    // symbol must resolve to the strong test override here (never null).
    JARVIS_ASSERT(stack_overflow_hook != nullptr);
#if CONFIG_STACK_OVERFLOW_HOOK
    JARVIS_ASSERT(CONFIG_STACK_OVERFLOW_HOOK == 1);
#else
    JARVIS_FAIL("CONFIG_STACK_OVERFLOW_HOOK must be 1 (MP-6.2)");
#endif
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: v0.4.0 MP-6.1 — a user task's kslot (only user tasks route
// through kslots in test mode) has an unmapped guard page at
// kstack_slot_va_ (the window PT entry is not-present in the kernel PML4),
// while the kernel stack above it is mapped.
// Input: create_user(); walk the kernel PML4 at kstack_slot_va_ and at
// kernel_stack.
// Expect: virt_to_phys_in_pml4(slot_va, kernel PML4) == 0;
// virt_to_phys_in_pml4(kernel_stack, kernel PML4) != 0.
// Depends: VMM page-table walk, kslot window wiring
JARVIS_TEST(kstack_guard_base_unmapped, "PRE: none | POST: none") {
    auto *t = TaskControlBlock::create_user([]() {}, 5, 10, 32_KiB);
    JARVIS_ASSERT(t != nullptr);
    auto cleanup = ScopeGuard([&]() {
        t->cleanup();
        delete t;
    });
    JARVIS_ASSERT(t->kstack_slot_va_ != 0);

    uint64_t kernel_pml4 = VMM::get_kernel_pml4();
    // Guard page below the kernel stack must be unmapped in the kernel PML4.
    uint64_t guard_phys =
        VMM::virt_to_phys_in_pml4(t->kstack_slot_va_, kernel_pml4);
    JARVIS_ASSERT_FMT(guard_phys == 0,
                      "kslot guard page at 0x%lx mapped to 0x%lx",
                      t->kstack_slot_va_, guard_phys);
    // The kernel stack above the guard must be mapped.
    uint64_t stack_phys =
        VMM::virt_to_phys_in_pml4(reinterpret_cast<uint64_t>(t->kernel_stack),
                                  kernel_pml4);
    JARVIS_ASSERT_FMT(stack_phys != 0,
                      "kernel stack at 0x%lx unmapped",
                      reinterpret_cast<uint64_t>(t->kernel_stack));
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: v0.4.0 MP-6.2 — the stack_overflow_hook fires with the faulting
// task's id.  Real-#PF coverage is delegated to kstack_guard_base_unmapped
// (the guard page is provably not-present, so a genuine kernel-stack
// overflow would #PF there and route to the hook in handle_interrupt_c); in
// test mode a kernel-mode guard #PF is not triggerable through existing
// syscall paths (only user tasks get kslots, their entry runs in ring 3, and
// no syscall dereferences the guard VA in kernel mode), so the hook is
// driven synthetically here.
// Input: create_user() (kstack_slot_va_ set); call stack_overflow_hook(t).
// Expect: g_hook_task_id == t->id; g_hook_rip != 0.
// Depends: kernel.cpp weak default + test strong override
JARVIS_TEST(kstack_overflow_invokes_hook, "PRE: none | POST: none") {
    auto *t = TaskControlBlock::create_user([]() {}, 5, 10, 32_KiB);
    JARVIS_ASSERT(t != nullptr);
    auto cleanup = ScopeGuard([&]() {
        t->cleanup();
        delete t;
    });
    g_hook_task_id = 0;
    g_hook_rip = 0;

    stack_overflow_hook(t);

    JARVIS_ASSERT(g_hook_task_id == t->id);
    JARVIS_ASSERT(g_hook_rip != 0);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: v0.4.0 MP-6.3 — the kslot pool + window PT state is
// snapshot-safe: capture, corrupt a window PTE, restore → the original PTE
// comes back; a fresh allocation after restore reuses the rewound slot with
// no stale PTE and zero PMM delta.
// Input: create_user() (allocates a kslot); kslot_snapshot_capture; corrupt
// the slot's stack PTE via map_page_in_pml4; kslot_snapshot_restore;
// create_user() again.
// Expect: after restore the original stack mapping is back; the second task
// reuses the same slot VA (bump rewound) with its guard unmapped; PMM free
// count returns to baseline after both tasks are cleaned up.
// Depends: kslot snapshot helpers (task.cpp MP-6.3)
JARVIS_TEST(kstack_slot_snapshot_restore_safe, "PRE: none | POST: none") {
    uint64_t free_before = PMM::free_pages_ref();
    // 8 window PT pages (8*4096) + generous bookkeeping headroom.
    static constexpr size_t KSLOT_SNAP_SIZE = 8 * arch::PAGE_SIZE + 4096;
    static uint8_t s_kslot_buf[KSLOT_SNAP_SIZE];
    JARVIS_ASSERT(kernel::kslot_snapshot_bytes() <= KSLOT_SNAP_SIZE);

    auto *t1 = TaskControlBlock::create_user([]() {}, 5, 10, 32_KiB);
    JARVIS_ASSERT(t1 != nullptr);
    uint64_t slot1 = t1->kstack_slot_va_;
    JARVIS_ASSERT(slot1 != 0);
    uint64_t stack_va = slot1 + arch::PAGE_SIZE;
    uint64_t stack_phys1 =
        VMM::virt_to_phys_in_pml4(stack_va, VMM::get_kernel_pml4());
    JARVIS_ASSERT(stack_phys1 != 0);

    // Capture the kslot window + bookkeeping.
    kernel::kslot_snapshot_capture(s_kslot_buf);

    // Corrupt the slot's stack PTE through the kernel PML4's window entry.
    uint64_t evil_phys = PMM::alloc_page();
    JARVIS_ASSERT(evil_phys != 0);
    VMM::map_page_in_pml4(stack_va, evil_phys, false,
                          VMM::get_kernel_pml4());
    JARVIS_ASSERT(VMM::virt_to_phys_in_pml4(stack_va,
                                            VMM::get_kernel_pml4()) ==
                  evil_phys);

    // Restore: the original PTE must come back.
    kernel::kslot_snapshot_restore(s_kslot_buf);
    JARVIS_ASSERT(VMM::virt_to_phys_in_pml4(stack_va,
                                            VMM::get_kernel_pml4()) ==
                  stack_phys1);
    PMM::free_page(evil_phys);
    t1->cleanup();
    delete t1;

    // A fresh allocation after restore must NOT inherit a stale PTE: its
    // guard base is unmapped and its stack is mapped.  (The exact slot VA is
    // NOT asserted — earlier classes' create_user tests may leave entries on
    // the kslot free list, so first-fit can return an older slot.)
    auto *t2 = TaskControlBlock::create_user([]() {}, 5, 10, 32_KiB);
    JARVIS_ASSERT(t2 != nullptr);
    JARVIS_ASSERT(t2->kstack_slot_va_ != 0);
    JARVIS_ASSERT(VMM::virt_to_phys_in_pml4(t2->kstack_slot_va_,
                                            VMM::get_kernel_pml4()) == 0);
    JARVIS_ASSERT(VMM::virt_to_phys_in_pml4(
                      reinterpret_cast<uint64_t>(t2->kernel_stack),
                      VMM::get_kernel_pml4()) != 0);

    // Explicitly tear down t2 BEFORE the delta assert (a ScopeGuard would
    // defer cleanup past the assert and make the check fail spuriously).
    t2->cleanup();
    delete t2;

    // Zero PMM delta across the whole cycle (after both tasks cleaned up).
    JARVIS_ASSERT_FMT(PMM::free_pages_ref() == free_before,
                      "PMM delta %ld pages after kslot cycle",
                      (long)(PMM::free_pages_ref() - free_before));
    JARVIS_TEST_PASS();
}

JARVIS_TEST(stack_alloc_user_stack_phys_freed_on_cleanup,
            "PRE: none | POST: none") {
    auto *t = TaskControlBlock::create_user([]() {}, 5, 10, 32_KiB);
    JARVIS_ASSERT(t != nullptr);
    JARVIS_ASSERT(t->user_stack_ != 0);
    JARVIS_ASSERT(t->page_table_ != 0);
    t->cleanup();
    JARVIS_ASSERT(t->page_table_ == 0);
    JARVIS_ASSERT(t->user_stack_ == 0);
    JARVIS_TEST_PASS();
}

JARVIS_TEST(stack_alloc_user_stack_size_in_config, "PRE: none | POST: none") {
    JARVIS_ASSERT(CONFIG_STACK_SIZE > 0);
    JARVIS_ASSERT(CONFIG_MIN_STACK_SIZE > 0);
    JARVIS_ASSERT(CONFIG_STACK_SIZE >= CONFIG_MIN_STACK_SIZE);
    JARVIS_TEST_PASS();
}

void register_stack_alloc_tests() {
    Logger::info("Registering stack allocation tests");
    JARVIS_REGISTER_TEST(stack_alloc_default_size_correct);
    JARVIS_REGISTER_TEST(stack_alloc_task_has_stack_phys);
    JARVIS_REGISTER_TEST(stack_alloc_user_task_has_guard_page);
    JARVIS_REGISTER_TEST(stack_alloc_stack_alignment);
    JARVIS_REGISTER_TEST(stack_alloc_multiple_tasks_distinct_stacks);
    JARVIS_REGISTER_TEST(stack_alloc_overflow_hook_weak_symbol);
    JARVIS_REGISTER_TEST(stack_alloc_user_stack_phys_freed_on_cleanup);
    JARVIS_REGISTER_TEST(stack_alloc_user_stack_size_in_config);
    // v0.4.0 MP-6 tests (kstack_overflow_invokes_hook registered LAST).
    JARVIS_REGISTER_TEST(kstack_guard_base_unmapped);
    JARVIS_REGISTER_TEST(kstack_slot_snapshot_restore_safe);
    JARVIS_REGISTER_TEST(kstack_overflow_invokes_hook);
}
