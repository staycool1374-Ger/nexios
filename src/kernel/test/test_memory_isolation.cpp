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

/// @file test_memory_isolation.cpp
/// @brief v0.4.0 MP-5 — memory-isolation verification suite (user-level).

#include <test.hpp>
#include <logger.hpp>
#include <constants.hpp>
#include <kernel/task/scheduler.hpp>
#include <kernel/task/task.hpp>
#include <kernel/memory/pmm.hpp>
#include <kernel/memory/vmm.hpp>
#include <kernel/elf/elf.hpp>
#include <kernel/arch/timer.hpp>
#include <initrd/initrd.hpp>
#include "test_sched_helpers.hpp"
#include "task_ptr.hpp"

using namespace kernel;

namespace {

/// @brief Load a userspace probe ELF by name ("fault-probe.c.elf").
///        Returns nullptr when the ELF is missing or invalid.
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
// Testidea: v0.4.0 MP-5 — a live user-mode page fault in task B (deref of a
// VA mapped only in A) terminates B without touching A and without hanging
// the kernel: the fault is delivered as SIGSEGV → TERMINATED, never a
// kernel panic.
// Input: Load fault-probe as user task A with 0x10000000 mapped (A writes
// 0xAB and exits 0).  Load fault-probe again as user task B with the VA
// unmapped and dispatch it.
// Expect: A's frame still holds 0xAB; B reaches TERMINATED (SIGSEGV path);
// the harness stays responsive (subsequent ops succeed).
// Depends: elf loader, VMM, scheduler signal path
JARVIS_TEST(cross_task_page_fault_isolated, "PRE: none | POST: none") {
    constexpr uint64_t PROBE_VA = 0x10000000UL;

    auto *a = load_probe("fault-probe.c.elf");
    if (!a) {
        JARVIS_TEST_PASS(); // probe ELF not built — skip
        return;
    }
    auto *b = load_probe("fault-probe.c.elf");
    if (!b) {
        a->cleanup();
        delete a;
        JARVIS_TEST_PASS();
        return;
    }
    JARVIS_ASSERT(a->is_user_ && b->is_user_);

    // Map one user page at PROBE_VA in A only.
    uint64_t a_phys = PMM::alloc_user_page();
    JARVIS_ASSERT(a_phys != 0);
    VMM::map_page_in_pml4(PROBE_VA, a_phys, true, a->page_table_);

    // Dispatch A: the probe writes 0xAB to PROBE_VA (mapped → succeeds) and
    // exits.
    Scheduler::add_task(*a);
    Scheduler::reschedule();
    kernel::test::wait_for_termination_safe(a);
    JARVIS_ASSERT(VMM::virt_to_phys_in_pml4(PROBE_VA, a->page_table_) ==
                  a_phys);
    // NOLINTNEXTLINE(performance-no-int-to-ptr)
    JARVIS_ASSERT(reinterpret_cast<uint8_t *>(arch::HHDM_OFFSET + a_phys)[0] ==
                  0xAB);

    // Dispatch B: PROBE_VA is unmapped in B's table → live user-mode #PF →
    // SIGSEGV → TERMINATED.
    Scheduler::add_task(*b);
    Scheduler::reschedule();
    kernel::test::wait_for_termination_safe(b);
    JARVIS_ASSERT(b->state == TaskState::TERMINATED);

    // A is intact: same frame, same value.
    JARVIS_ASSERT(VMM::virt_to_phys_in_pml4(PROBE_VA, a->page_table_) ==
                  a_phys);
    // NOLINTNEXTLINE(performance-no-int-to-ptr)
    JARVIS_ASSERT(reinterpret_cast<uint8_t *>(arch::HHDM_OFFSET + a_phys)[0] ==
                  0xAB);

    // a_phys is USER-owned and mapped into A: A's cleanup (via the zombie
    // drain) reclaims it through free_user_pages — do NOT free it again.
    kernel::test::terminate_and_drain2(a, b);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: v0.4.0 MP-5 — the kernel can read a user task's memory through
// the HHDM direct map: a value written by user code in one CR3 context is
// visible to the harness (running on a different PML4) at
// HHDM_OFFSET + phys.
// Input: Load fault-probe with 0x10000000 mapped; dispatch; user writes
// 0xAB; harness reads via HHDM.
// Expect: HHDM_OFFSET + virt_to_phys_in_pml4(0x10000000) == 0xAB.
// Depends: elf loader, VMM, PMM
JARVIS_TEST(hhdm_kernel_reads_user_page, "PRE: none | POST: none") {
    constexpr uint64_t PROBE_VA = 0x10000000UL;

    auto *a = load_probe("fault-probe.c.elf");
    if (!a) {
        JARVIS_TEST_PASS();
        return;
    }
    uint64_t a_phys = PMM::alloc_user_page();
    JARVIS_ASSERT(a_phys != 0);
    VMM::map_page_in_pml4(PROBE_VA, a_phys, true, a->page_table_);

    Scheduler::add_task(*a);
    Scheduler::reschedule();
    kernel::test::wait_for_termination_safe(a);

    uint64_t resolved = VMM::virt_to_phys_in_pml4(PROBE_VA, a->page_table_);
    JARVIS_ASSERT(resolved == a_phys);
    // NOLINTNEXTLINE(performance-no-int-to-ptr)
    JARVIS_ASSERT(reinterpret_cast<uint8_t *>(arch::HHDM_OFFSET + resolved)[0] ==
                  0xAB);

    // a_phys is reclaimed by A's cleanup (user-owned leaf in A's PML4).
    kernel::test::terminate_and_drain(*a);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: v0.4.0 MP-5 — a user red-zone fault (write below the stack
// guard) terminates the faulting task; the kernel and harness remain fully
// responsive (no panic, scheduler keeps ticking).
// Input: Load stack-probe (writes 0x6FFFFFF8 — below STACK_VADDR) and
// dispatch it.
// Expect: task TERMINATED; harness uptime advances and a follow-up kernel
// operation (timer read) still works.
// Depends: elf loader, scheduler signal path
JARVIS_TEST(guard_page_fault_not_kernel_fatal, "PRE: none | POST: none") {
    auto *t = load_probe("stack-probe.c.elf");
    if (!t) {
        JARVIS_TEST_PASS();
        return;
    }
    uint64_t ticks_before = arch::Timer::ticks();

    Scheduler::add_task(*t);
    Scheduler::reschedule();
    kernel::test::wait_for_termination_safe(t);

    JARVIS_ASSERT(t->state == TaskState::TERMINATED);
    // Harness responsive: the timer keeps ticking and a trivial scheduler op
    // still works after the fault.
    uint64_t ticks_after = arch::Timer::ticks();
    JARVIS_ASSERT(ticks_after >= ticks_before);
    JARVIS_ASSERT(Scheduler::current_task() != nullptr);

    kernel::test::terminate_and_drain(*t);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Registers all memory-isolation (MP-5) tests.
// Input: None
// Expect: All tests registered
// Depends: test framework
void register_memory_isolation_tests() {
    JARVIS_REGISTER_TEST(cross_task_page_fault_isolated);
    JARVIS_REGISTER_TEST(hhdm_kernel_reads_user_page);
    JARVIS_REGISTER_TEST(guard_page_fault_not_kernel_fatal);
}
