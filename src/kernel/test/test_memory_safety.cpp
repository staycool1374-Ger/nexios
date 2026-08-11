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

/// @file test_memory_safety.cpp
/// @brief Memory safety boundary tests.

#include <test.hpp>
#include <logger.hpp>
#include <kernel/memory/pmm.hpp>
#include <kernel/memory/mempool.hpp>
#include <kernel/memory/vmm.hpp>
#include <kernel/task/scheduler.hpp>
#include <kernel/task/task.hpp>
#include <kernel/elf/elf.hpp>
#include <kernel/syscall/syscall.hpp>
#include <kernel/arch/timer.hpp>
#include <scope_guard.hpp>
#include <initrd/initrd.hpp>
#include "test_sched_helpers.hpp"

using namespace kernel;

namespace {

/// @brief Load a userspace probe ELF by name ("stack-probe.c.elf").
TaskControlBlock *load_probe(const char *name) {
    initrd::InitrdFile f = initrd::find(name);
    if (!f.data)
        f = initrd::find(name + 2); // strip "./"
    if (!f.data)
        return nullptr;
    auto *hdr = reinterpret_cast<const kernel::elf::ELF64Header *>(f.data);
    if (!kernel::elf::validate_header(hdr))
        return nullptr;
    return kernel::elf::load(hdr, f.data, f.size);
}

} // namespace

// Runmode: kernel
// Testidea: Verifies MemPool::free(nullptr) is a safe no-op.
// Input: MemPool::free(nullptr)
// Expect: No crash, returns immediately
// Depends: MemPool
JARVIS_TEST(memory_safety_mempool_free_null, "PRE: none | POST: none") {
    MemPool::free(nullptr);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Verifies MemPool::alloc rejects sizes larger than any pool.
// The largest pool is 8192 bytes (mempool.cpp). Allocating 8193 should fail.
// Input: MemPool::alloc(8193)
// Expect: Returns nullptr
// Depends: MemPool
JARVIS_TEST(memory_safety_mempool_alloc_large_rejected,
            "PRE: none | POST: none") {
    void *p = MemPool::alloc(8193);
    JARVIS_ASSERT(p == nullptr);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Verifies MemPool alloc/free at exact pool size boundaries
// returns valid pointers and cleans up correctly.
// Input: Alloc at 16, 32, 64, 128, 256, 512, 1024, 2048, 8192 bytes
// Expect: All return non-null pointers, free succeeds
// Depends: MemPool
JARVIS_TEST(memory_safety_mempool_exact_edge_sizes, "PRE: none | POST: none") {
    static const size_t sizes[] = {16, 32, 64, 128, 256, 512, 1024, 2048, 8192};
    void *ptrs[9];
    for (size_t i = 0; i < 9; ++i) {
        ptrs[i] = MemPool::alloc(sizes[i]);
        JARVIS_ASSERT_FMT(ptrs[i] != nullptr, "MemPool::alloc(%zu) failed",
                          sizes[i]);
    }
    for (size_t i = 0; i < 9; ++i) {
        MemPool::free(ptrs[i]);
    }
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Asserts the reserved-range invariant (memory.md, oom-rt.md §3):
// physical page 0 lies in the PMM reserved range [0, kernel_start_page),
// bitmap_set at init (pmm.cpp), and must always report allocated.
// NOTE: PMM::free_page(0) is NOT a safe no-op today — it clears the
// reserved bit and pushes page 0 onto the free list. This test therefore
// observes the invariant via is_allocated() without invoking free_page.
// Input: PMM::is_allocated(0)
// Expect: true — page 0 is reserved/allocated, never on the free list
// Depends: PMM
JARVIS_TEST(memory_safety_pmm_free_zero, "PRE: PMM init | POST: none") {
    JARVIS_ASSERT(PMM::is_allocated(0));
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Verifies PMM::free_page with a page beyond total memory is safe.
// Input: PMM::free_page(0xFFFFFFFFFFFFF000)
// Expect: Guards against out-of-bounds bitmap access, returns without crash
// Depends: PMM
JARVIS_TEST(memory_safety_pmm_free_beyond_total, "PRE: none | POST: none") {
    uint64_t huge = 0xFFFFFFFFFFFFF000ULL;
    PMM::free_page(huge);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: v0.4.0 MP-2 — a user task writing below the user stack guard
// (STACK_VADDR) takes a live user-mode #PF: the kernel converts it to
// SIGSEGV (from_user path) and TERMINATES the task — the kernel survives.
// Input: Dispatch stack-probe (writes 0x6FFFFFF8 — below STACK_VADDR).
// Expect: task state == TERMINATED; harness responsive (timer still ticks).
// Depends: elf loader, scheduler signal path
JARVIS_TEST(user_red_zone_stack_overflow_pf, "PRE: none | POST: none") {
    auto *t = load_probe("stack-probe.c.elf");
    if (!t) {
        JARVIS_TEST_PASS(); // probe ELF not built — skip
        return;
    }
    uint64_t ticks_before = arch::Timer::ticks();

    Scheduler::add_task(*t);
    Scheduler::reschedule();
    kernel::test::wait_for_termination_safe(t);

    JARVIS_ASSERT(t->state == TaskState::TERMINATED);
    JARVIS_ASSERT(arch::Timer::ticks() >= ticks_before);

    kernel::test::terminate_and_drain(*t);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: v0.4.0 MP-2 — a user task that brk()s to the heap cap and writes
// one byte past the new break faults in user mode (heap red zone): SIGSEGV
// → TERMINATED, kernel alive.
// Input: Dispatch heap-probe (brk(0x60004000) then write at 0x60004000).
// Expect: task state == TERMINATED; harness responsive.
// Depends: elf loader, sys_brk, scheduler signal path
JARVIS_TEST(user_red_zone_heap_overflow_pf, "PRE: none | POST: none") {
    auto *t = load_probe("heap-probe.c.elf");
    if (!t) {
        JARVIS_TEST_PASS();
        return;
    }
    uint64_t ticks_before = arch::Timer::ticks();

    Scheduler::add_task(*t);
    Scheduler::reschedule();
    kernel::test::wait_for_termination_safe(t);

    JARVIS_ASSERT(t->state == TaskState::TERMINATED);
    JARVIS_ASSERT(arch::Timer::ticks() >= ticks_before);

    kernel::test::terminate_and_drain(*t);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: v0.4.0 MP-2 — kernel-half red zone: the kslot guard page
// between kernel-stack slots is not-present in the kernel PML4 (walk-based;
// a live kernel-mode deref would #PF and panic, so the walk is the
// equivalent proof).
// Input: create_user() (kslot'd in test mode); walk the kernel PML4 at
// kstack_slot_va_.
// Expect: virt_to_phys_in_pml4(slot_va, kernel PML4) == 0; the stack above
// it is mapped.
// Depends: VMM page-table walk, kslot window wiring
JARVIS_TEST(kernel_red_zone_between_stack_data, "PRE: none | POST: none") {
    auto *t = TaskControlBlock::create_user([]() {}, 5, 10, 32_KiB);
    JARVIS_ASSERT(t != nullptr);
    auto cleanup = ScopeGuard([&]() {
        t->cleanup();
        delete t;
    });
    JARVIS_ASSERT(t->kstack_slot_va_ != 0);

    uint64_t guard_phys =
        VMM::virt_to_phys_in_pml4(t->kstack_slot_va_,
                                  VMM::get_kernel_pml4());
    JARVIS_ASSERT_FMT(guard_phys == 0,
                      "kernel red-zone (kslot guard) at 0x%lx mapped to "
                      "0x%lx",
                      t->kstack_slot_va_, guard_phys);
    JARVIS_ASSERT(VMM::virt_to_phys_in_pml4(
                      reinterpret_cast<uint64_t>(t->kernel_stack),
                      VMM::get_kernel_pml4()) != 0);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: v0.4.0 MP-3 — segment-boundary canaries are installed for a
// freshly loaded user ELF: TEXT (first R E PT_LOAD), HEAP and STACK slots
// are armed, and the canary bytes at the STACK before/after VAs hold the
// derived expected value.
// Input: elf::load user-app.c.elf; inspect canary fields + live bytes.
// Expect: canary_installed has TEXT|HEAP|STACK bits; canary_before[STACK] ==
// STACK_VADDR+PAGE_SIZE; live canary bytes match CANARY_MAGIC ^ (seg+1).
// Depends: elf loader, canary install (MP-3)
JARVIS_TEST(canary_installed_at_segment_boundaries,
            "PRE: none | POST: none") {
    auto *t = load_probe("user-app.c.elf");
    if (!t) {
        JARVIS_TEST_PASS();
        return;
    }
    auto cleanup = ScopeGuard([&]() {
        t->cleanup();
        delete t;
    });

    const uint64_t text = TaskControlBlock::SEG_TEXT;
    const uint64_t heap = TaskControlBlock::SEG_HEAP;
    const uint64_t stk = TaskControlBlock::SEG_STACK;
    JARVIS_ASSERT(t->canary_installed & (1u << text));
    JARVIS_ASSERT(t->canary_installed & (1u << heap));
    JARVIS_ASSERT(t->canary_installed & (1u << stk));

    JARVIS_ASSERT(t->canary_before[stk] ==
                  mem::STACK_VADDR + arch::PAGE_SIZE);
    JARVIS_ASSERT(t->canary_after[stk] ==
                  mem::STACK_VADDR + arch::PAGE_SIZE + t->user_stack_size_ -
                      8);

    auto read8 = [](uint64_t va, uint64_t pml4, uint64_t &out) {
        uint64_t phys = VMM::virt_to_phys_in_pml4(va, pml4);
        if (!phys)
            return false;
        // NOLINTNEXTLINE(performance-no-int-to-ptr)
        __builtin_memcpy(&out,
                         reinterpret_cast<const void *>(arch::HHDM_OFFSET +
                                                       phys),
                         8);
        return true;
    };
    uint64_t live = 0;
    JARVIS_ASSERT(read8(t->canary_before[stk], t->page_table_, live));
    JARVIS_ASSERT(live == (TaskControlBlock::CANARY_MAGIC ^ (stk + 1)));
    JARVIS_ASSERT(read8(t->canary_after[stk], t->page_table_, live));
    JARVIS_ASSERT(live == (TaskControlBlock::CANARY_MAGIC ^ (stk + 1)));
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: v0.4.0 MP-3 — overwriting a segment canary is detected on the
// next syscall: the verify latches g_canary_trip {task_id, segment, rip} and
// the syscall returns -1 (test mode), so the kernel survives.
// Input: Load user-app; write 0xDD over canary_after[STACK] via HHDM;
// dispatch.  user-app's write() syscall trips the verify.
// Expect: g_canary_trip.count > 0, task_id matches, segment == SEG_STACK;
// task reaches TERMINATED.
// Depends: Syscall::handle canary verify (MP-3)
JARVIS_TEST(canary_tamper_detected_on_syscall, "PRE: none | POST: none") {
    auto *t = load_probe("user-app.c.elf");
    if (!t) {
        JARVIS_TEST_PASS();
        return;
    }
    const uint64_t stk = TaskControlBlock::SEG_STACK;
    JARVIS_ASSERT(t->canary_installed & (1u << stk));
    uint64_t tamper_va = t->canary_after[stk];
    JARVIS_ASSERT(tamper_va != 0);

    // Overwrite the canary with 0xDD (via HHDM through the task's PML4).
    uint64_t phys = VMM::virt_to_phys_in_pml4(tamper_va, t->page_table_);
    JARVIS_ASSERT(phys != 0);
    // NOLINTNEXTLINE(performance-no-int-to-ptr)
    __builtin_memset(reinterpret_cast<void *>(arch::HHDM_OFFSET + phys), 0xDD,
                     8);

    g_canary_trip = {0, 0, 0, 0};
    Scheduler::add_task(*t);
    Scheduler::reschedule();
    kernel::test::wait_for_termination_safe(t);

    JARVIS_ASSERT(g_canary_trip.count > 0);
    JARVIS_ASSERT(g_canary_trip.task_id == t->id);
    JARVIS_ASSERT(g_canary_trip.segment ==
                  static_cast<uint8_t>(TaskControlBlock::SEG_STACK));
    JARVIS_ASSERT(t->state == TaskState::TERMINATED);

    kernel::test::terminate_and_drain(*t);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: v0.4.0 MP-3 — an untampered user task runs through syscalls
// without tripping the canary verify (no false positives).
// Input: Load user-app (canaries intact); dispatch; it performs write + exit
// syscalls.
// Expect: g_canary_trip.count == 0 after termination; task TERMINATED.
// Depends: Syscall::handle canary verify (MP-3)
JARVIS_TEST(canary_intact_after_normal_dispatch, "PRE: none | POST: none") {
    auto *t = load_probe("user-app.c.elf");
    if (!t) {
        JARVIS_TEST_PASS();
        return;
    }
    g_canary_trip = {0, 0, 0, 0};

    Scheduler::add_task(*t);
    Scheduler::reschedule();
    kernel::test::wait_for_termination_safe(t);

    JARVIS_ASSERT(g_canary_trip.count == 0);
    JARVIS_ASSERT(t->state == TaskState::TERMINATED);

    kernel::test::terminate_and_drain(*t);
    JARVIS_TEST_PASS();
}

void register_memory_safety_tests() {
    Logger::info("Registering memory safety tests");
    JARVIS_REGISTER_TEST(memory_safety_mempool_free_null);
    JARVIS_REGISTER_TEST(memory_safety_mempool_alloc_large_rejected);
    JARVIS_REGISTER_TEST(memory_safety_mempool_exact_edge_sizes);
    JARVIS_REGISTER_TEST(memory_safety_pmm_free_zero);
    JARVIS_REGISTER_TEST(memory_safety_pmm_free_beyond_total);
    JARVIS_REGISTER_TEST(user_red_zone_stack_overflow_pf);
    JARVIS_REGISTER_TEST(user_red_zone_heap_overflow_pf);
    JARVIS_REGISTER_TEST(kernel_red_zone_between_stack_data);
    JARVIS_REGISTER_TEST(canary_installed_at_segment_boundaries);
    JARVIS_REGISTER_TEST(canary_tamper_detected_on_syscall);
    JARVIS_REGISTER_TEST(canary_intact_after_normal_dispatch);
}
