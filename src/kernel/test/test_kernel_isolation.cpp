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

/// @file test_kernel_isolation.cpp
/// @brief v0.4.0 MP-1 — private kernel-half page tables per kernel task.

#include <test.hpp>
#include <logger.hpp>
#include <scope_guard.hpp>
#include <constants.hpp>
#include <kernel/nexios_config.h>
#include <kernel/task/task.hpp>
#include <kernel/task/scheduler.hpp>
#include <kernel/memory/pmm.hpp>
#include <kernel/memory/vmm.hpp>
#include <kernel/arch/io.hpp>
#include <kernel/arch/page_table.hpp>
#include <kernel/arch/irq_guard.hpp>
#include <kernel/test/test_sched_helpers.hpp>

using namespace kernel;

namespace {

constexpr uint64_t PRIV_BASE = CONFIG_KERNEL_PRIV_DATA_BASE;

/// @brief Walk A's PML4 at CONFIG_KERNEL_PRIV_DATA_BASE and free the
///        kernel-owned table pages (PDPT/PD/PT on x86_64, L1/L2 on Sv39)
///        plus the leaf data page.  free_user_pages() only walks the user
///        half (0..255), so the kernel-half private window must be torn
///        down manually. Boot asserts the window slot absent, so tables
///        below it are test-exclusive on every arch.
void free_priv_window(uint64_t pml4_phys) {
#if defined(CONFIG_ARCH_RISCV64)
    // Sv39 branch (issue #540): the canonical window decodes to L0[320]
    // (same slot number as x86 by design), L1[0], L2[0]. Mirrors the x86
    // chain teardown level for level; the L0[320] subtree is
    // test-exclusive exactly like the x86 PDPT chain.
    constexpr uint64_t kV = 1ULL << 0;
    auto *l0 = reinterpret_cast<uint64_t *>(arch::HHDM_OFFSET +
                                            (pml4_phys & ~0xFFFULL));
    size_t l0_idx = (PRIV_BASE >> 30) & 0x1FF;
    if (!(l0[l0_idx] & kV))
        return;
    uint64_t l1_phys = VMM::sv39_pte_phys(l0[l0_idx]);
    auto *l1 = reinterpret_cast<uint64_t *>(arch::HHDM_OFFSET + l1_phys);
    size_t l1_idx = (PRIV_BASE >> 21) & 0x1FF;
    if (!(l1[l1_idx] & kV)) {
        PMM::free_page(l1_phys);
        l0[l0_idx] = 0;
        return;
    }
    uint64_t l2_phys = VMM::sv39_pte_phys(l1[l1_idx]);
    auto *l2 = reinterpret_cast<uint64_t *>(arch::HHDM_OFFSET + l2_phys);
    size_t l2_idx = (PRIV_BASE >> 12) & 0x1FF;
    // Reclaim through the canonical translation (page-aligned) instead of
    // a raw mask (Sv39 PTEs carry PPN<<10 — raw masking decodes 4x too
    // low; same class as the #536 helper bug).
    if ((l2[l2_idx] & kV) != 0) {
        uint64_t leaf_phys =
            VMM::virt_to_phys_in_pml4(PRIV_BASE, pml4_phys);
        if (leaf_phys != 0)
            PMM::free_page(leaf_phys);
    }
    PMM::free_page(l2_phys);
    PMM::free_page(l1_phys);
    l1[l1_idx] = 0;
    l0[l0_idx] = 0;
    return;
#else
    constexpr uint64_t P = 1ULL << 0;
    auto *pml4 = reinterpret_cast<uint64_t *>(arch::HHDM_OFFSET +
                                              (pml4_phys & ~0xFFFULL));
    size_t pml4_idx = (PRIV_BASE >> 39) & 0x1FF;
    if (!(pml4[pml4_idx] & P))
        return;
    uint64_t pdpt_phys = pml4[pml4_idx] & ~0xFFFULL;
    auto *pdpt = reinterpret_cast<uint64_t *>(arch::HHDM_OFFSET + pdpt_phys);
    size_t pdpt_idx = (PRIV_BASE >> 30) & 0x1FF;
    if (!(pdpt[pdpt_idx] & P)) {
        PMM::free_page(pdpt_phys);
        pml4[pml4_idx] = 0;
        return;
    }
    uint64_t pd_phys = pdpt[pdpt_idx] & ~0xFFFULL;
    auto *pd = reinterpret_cast<uint64_t *>(arch::HHDM_OFFSET + pd_phys);
    size_t pd_idx = (PRIV_BASE >> 21) & 0x1FF;
    if (!(pd[pd_idx] & P)) {
        PMM::free_page(pd_phys);
        PMM::free_page(pdpt_phys);
        pdpt[pdpt_idx] = 0;
        pml4[pml4_idx] = 0;
        return;
    }
    uint64_t pt_phys = pd[pd_idx] & ~0xFFFULL;
    auto *pt = reinterpret_cast<uint64_t *>(arch::HHDM_OFFSET + pt_phys);
    size_t pt_idx = (PRIV_BASE >> 12) & 0x1FF;
    // Issue #363 option-B: the leaf may carry NX (x86_64 bit 63) or
    // UXN/PXN (aarch64) now — raw `& ~0xFFF` keeps those bits and frees a
    // bogus address (production PAGE_FRAME_MASK strips them; see vmm.hpp).
    // Reclaim through the canonical translation instead (PRIV_BASE is
    // page-aligned, so no offset is added).
    if ((pt[pt_idx] & P) != 0) {
        uint64_t leaf_phys =
            VMM::virt_to_phys_in_pml4(PRIV_BASE, pml4_phys);
        if (leaf_phys != 0)
            PMM::free_page(leaf_phys);
    }
    PMM::free_page(pt_phys);
    PMM::free_page(pd_phys);
    PMM::free_page(pdpt_phys);
    pd[pd_idx] = 0;
    pdpt[pdpt_idx] = 0;
    pml4[pml4_idx] = 0;
#endif
}

} // namespace

// Runmode: kernel
// Testidea: v0.4.0 MP-1 — every kernel task owns a private PML4 whose kernel
// half (>= PML4_KERNEL_START) mirrors the boot kernel PML4 (kernel
// text/data/bss + HHDM + kslot window) and whose user half is empty.
// Input: Create a kernel task via TaskControlBlock::create().
// Expect: page_table_ != 0; entries [PML4_KERNEL_START, 512) match the kernel
// PML4 by value (HHDM entry 256 present); entries [0, PML4_USER_COUNT) are
// all zero.
// Depends: kernel::task::TaskControlBlock, kernel::memory::VMM
JARVIS_TEST(kernel_priv_pml4_clone_kernel_half_present,
            "PRE: none | POST: none") {
    auto *t = TaskControlBlock::create([]() {}, 5, 10);
    JARVIS_ASSERT(t != nullptr);
    // Issue #542 (birth/death symmetry): track_task_add moved to the TCB
    // birth funnels, so pure create/cleanup nets zero with no registration.
    // (Supersedes the #541 option-A register_task workaround, reverted —
    // keeping it would mask a future accounting regression.)
    auto cleanup = ScopeGuard([&]() {
        t->cleanup();
        delete t;
    });

    JARVIS_ASSERT(t->get_page_table() != 0);
    JARVIS_ASSERT(t->get_page_table() != VMM::get_kernel_pml4());

    auto *priv = reinterpret_cast<uint64_t *>(arch::HHDM_OFFSET +
                                              (t->get_page_table() & ~0xFFFULL));
    auto *kern = reinterpret_cast<uint64_t *>(
        arch::HHDM_OFFSET + (VMM::get_kernel_pml4() & ~0xFFFULL));

    // Zero user entries — a kernel task must not expose any user mapping.
    // riscv64 carve-out: the clone deliberately inherits the kernel-owned
    // low-half MMIO/identity blocks by value (issue #387 — U=0 preserved,
    // never a fork failure) and re-allocates table pages fresh, so neither
    // zero-half nor pointer-equality holds portably; the zero premise is
    // skipped on this port (slot-shape pinning would overfit the clone
    // allocator — see the canonical-PRIV follow-up).
    for (size_t i = 0; i < arch::PML4_USER_COUNT; ++i) {
#if !defined(CONFIG_ARCH_RISCV64)
        JARVIS_ASSERT_FMT(priv[i] == 0,
                          "kernel task user entry %u = 0x%lx", (unsigned)i,
                          priv[i]);
#else
        (void)priv[i];
#endif
    }

    // Kernel half mirrors the boot kernel PML4 by value.
    for (size_t i = arch::PML4_KERNEL_START; i < arch::PML4_ENTRIES; ++i) {
        JARVIS_ASSERT_FMT(priv[i] == kern[i],
                          "kernel entry %u differs: priv=0x%lx kern=0x%lx",
                          (unsigned)i, priv[i], kern[i]);
    }

    // HHDM direct map present (PML4 entry 256 on x86_64). riscv64 carve-out:
    // the HHDM RAM window lives at L0[258], not slot 256, so the slot
    // number is x86-shaped; the mirror loop above already pins slot-by-slot
    // equality (including slot 256), which is the portable property.
#if !defined(CONFIG_ARCH_RISCV64)
    JARVIS_ASSERT((priv[arch::PML4_KERNEL_START] & 1ULL) != 0);
#else
    (void)priv;
#endif
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: v0.4.0 MP-1 — two kernel tasks own distinct private PML4s; a
// page mapped in task A's kernel-half private-data window is invisible in
// task B's table (not-present ≡ #PF proof, walk-based — no live kernel #PF,
// which would panic in the #PF handler).
// Input: Create kernel tasks A and B.  Map one kernel page at
// CONFIG_KERNEL_PRIV_DATA_BASE in A via map_page_in_pml4(user=false).
// A's dispatched lambda writes 0xAA through the private VA.
// Expect: readback via HHDM == 0xAA; virt_to_phys_in_pml4(PRIV_BASE, A) ==
// phys; virt_to_phys_in_pml4(PRIV_BASE, B) == 0; zero user entries in both.
// Depends: kernel::task::TaskControlBlock, kernel::memory::VMM, PMM
JARVIS_TEST(kernel_priv_cross_task_data_isolation,
            "PRE: none | POST: none") {
    // riscv64 uses the canonical window (issue #540: PRIV_BASE
    // 0xFFFFFFD000000000, Sv39 slot 320); the free_priv_window helper
    // carries the matching Sv39 walk.
    static uint64_t g_written = 0;
    static uint64_t g_cr3_in_a = 0;

    auto *a = TaskControlBlock::create(
        []() {
            g_cr3_in_a = arch::read_cr3();
            // NOLINTNEXTLINE(performance-no-int-to-ptr)
            reinterpret_cast<volatile uint8_t *>(PRIV_BASE)[0] = 0xAA;
            g_written = 1;
        },
        11, 10);
    auto *b = TaskControlBlock::create([]() {}, 12, 10);
    JARVIS_ASSERT(a != nullptr && b != nullptr);

    auto cleanup_a = ScopeGuard([&]() {
        free_priv_window(a->get_page_table());
        a->cleanup();
        delete a;
    });
    auto cleanup_b = ScopeGuard([&]() {
        b->cleanup();
        delete b;
    });

    // Distinct private PML4s, none aliasing the boot kernel PML4.
    JARVIS_ASSERT(a->get_page_table() != 0);
    JARVIS_ASSERT(b->get_page_table() != 0);
    JARVIS_ASSERT(a->get_page_table() != b->get_page_table());
    JARVIS_ASSERT(a->get_page_table() != VMM::get_kernel_pml4());
    JARVIS_ASSERT(b->get_page_table() != VMM::get_kernel_pml4());

    // Map one private page in A's kernel-half private-data window.
    uint64_t priv_phys = PMM::alloc_page();
    JARVIS_ASSERT(priv_phys != 0);
    // NOLINTNEXTLINE(performance-no-int-to-ptr)
    reinterpret_cast<uint8_t *>(arch::HHDM_OFFSET + priv_phys)[0] = 0x00;
    VMM::map_page_in_pml4(PRIV_BASE, priv_phys, false, false, a->get_page_table());

    // Dispatch A: the lambda writes 0xAA through the private VA (with A's
    // PML4 active as CR3, see kernel_priv_cr3_switch_on_dispatch).
    Scheduler::add_task(*a);
    Scheduler::reschedule();
    kernel::test::wait_for_termination_safe(a);
    JARVIS_ASSERT(g_written == 1);
    JARVIS_ASSERT(g_cr3_in_a == a->get_page_table());

    // A's mapping resolves; the frame holds what A wrote.
    uint64_t resolved = VMM::virt_to_phys_in_pml4(PRIV_BASE, a->get_page_table());
    JARVIS_ASSERT(resolved == priv_phys);
    // NOLINTNEXTLINE(performance-no-int-to-ptr)
    JARVIS_ASSERT(reinterpret_cast<uint8_t *>(arch::HHDM_OFFSET + priv_phys)[0] == 0xAA);

    // B's table: PRIV_BASE is not-present.  A live kernel-mode dereference of
    // PRIV_BASE under B's PML4 would #PF and panic in the kernel handler
    // (kernel.cpp CPU-EXCEPTION path), so the walk result of 0 is the
    // equivalent proof: hardware translation would fail with a page fault.
    JARVIS_ASSERT(VMM::virt_to_phys_in_pml4(PRIV_BASE, b->get_page_table()) == 0);

    // Both tasks expose zero user entries (riscv64: same inherit
    // carve-out as above — skipped, see the canonical-PRIV follow-up).
    auto *pml4_a = reinterpret_cast<uint64_t *>(arch::HHDM_OFFSET +
                                                (a->get_page_table() & ~0xFFFULL));
    auto *pml4_b = reinterpret_cast<uint64_t *>(arch::HHDM_OFFSET +
                                                (b->get_page_table() & ~0xFFFULL));
#if !defined(CONFIG_ARCH_RISCV64)
    for (size_t i = 0; i < arch::PML4_USER_COUNT; ++i) {
        JARVIS_ASSERT(pml4_a[i] == 0);
        JARVIS_ASSERT(pml4_b[i] == 0);
    }
#else
    (void)pml4_a;
    (void)pml4_b;
#endif
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: v0.4.0 MP-1 — the deferred-switch CR3 publish path switches CR3
// to the dispatched task's private PML4: read_cr3() inside a dispatched
// kernel task's lambda equals that task's page_table_.  The harness itself
// runs on the boot kernel PML4 (its CR3 is only switched away on dispatch).
// Input: Create kernel task A with a lambda recording read_cr3(); dispatch.
// Expect: g_cr3_in_a == a->get_page_table(); harness read_cr3() ==
// VMM::get_kernel_pml4().
// Depends: kernel::task::Scheduler, kernel::memory::VMM
JARVIS_TEST(kernel_priv_cr3_switch_on_dispatch, "PRE: none | POST: none") {
    static uint64_t g_cr3 = 0;

    auto *a = TaskControlBlock::create(
        []() { g_cr3 = arch::read_cr3(); }, 11, 10);
    JARVIS_ASSERT(a != nullptr);
    auto cleanup = ScopeGuard([&]() {
        a->cleanup();
        delete a;
    });

    Scheduler::add_task(*a);
    Scheduler::reschedule();
    kernel::test::wait_for_termination_safe(a);

    JARVIS_ASSERT(g_cr3 != 0);
    JARVIS_ASSERT(g_cr3 == a->get_page_table());

    // Harness (test body): the active CR3 is EITHER the boot kernel PML4 or
    // the harness's own private kernel-half PML4 (MP-1.2 gives every kernel
    // task one) — both carry identical kernel halves, so kernel code is
    // correct in either.  The essential invariant is that the dispatched
    // task's PML4 was switched in, proven by the g_cr3 assertion above.
    uint64_t harness_cr3 = arch::read_cr3();
    auto *harness = Scheduler::get_harness_task();
    JARVIS_ASSERT(harness_cr3 == VMM::get_kernel_pml4() ||
                  (harness && harness->get_page_table() == harness_cr3));
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: v0.4.0 MP-1.4/MP-7.5 — creating, dispatching, terminating and
// draining a kernel task returns the PMM to its baseline: the private PML4
// page and the stack pages are all reclaimed by cleanup().
// Input: Record PMM::free_pages_ref(); create a self-terminating kernel task;
// dispatch; drain zombies.
// Expect: PMM free-pages count returns to the baseline (zero delta).
// Depends: kernel::task::Scheduler, kernel::memory::PMM
JARVIS_TEST(kernel_priv_teardown_frees_pml4_stack,
            "PRE: none | POST: none") {
    uint64_t free_before = PMM::free_pages_ref();

    auto *t = TaskControlBlock::create([]() {}, 11, 10);
    JARVIS_ASSERT(t != nullptr);
    JARVIS_ASSERT(t->get_page_table() != 0);
    Scheduler::add_task(*t);
    Scheduler::reschedule();
    kernel::test::wait_for_termination_safe(t);
    Scheduler::drain_zombie_list();

    JARVIS_ASSERT_FMT(PMM::free_pages_ref() == free_before,
                      "PMM delta %ld pages after kernel task teardown",
                      (long)(PMM::free_pages_ref() - free_before));
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Issue #199 — the private-window teardown clears the PML4 slot,
// unmaps the VA, and reclaims every page (zero PMM delta).
// Input: Create task A, map PRIV_BASE in A->get_page_table(), teardown under
// IrqGuard, verify slot/translation/delta
// Expect: pml4 slot == 0, virt_to_phys == 0, free_pages_ref unchanged
// Depends: kernel::VMM, kernel::PMM, free_priv_window
JARVIS_TEST(kernel_priv_teardown_clears_pml4_slot_and_reclaims,
            "PRE: none | POST: none") {
    uint64_t free_before = PMM::free_pages_ref();

    auto *task_a = TaskControlBlock::create([]() {}, 11, 10);
    JARVIS_ASSERT(task_a != nullptr);
    JARVIS_ASSERT(task_a->get_page_table() != 0);
    JARVIS_ASSERT(task_a->get_page_table() != VMM::get_kernel_pml4());

    uint64_t priv_phys = PMM::alloc_page();
    JARVIS_ASSERT(priv_phys != 0);
    VMM::map_page_in_pml4(PRIV_BASE, priv_phys, false, false, task_a->get_page_table());
    JARVIS_ASSERT_FMT(
        VMM::virt_to_phys_in_pml4(PRIV_BASE, task_a->get_page_table()) == priv_phys,
        "PRIV_BASE must resolve after map");

    {
        // Private-tree surgery is still IRQ-critical: a timer tick between
        // the slot clear and the TLB flush could observe the torn window.
        // NOTE: free_priv_window already reclaims the leaf data page.
        arch::IrqGuard teardown_guard;
        free_priv_window(task_a->get_page_table());
        arch::ArchPageTable::tlb_flush(PRIV_BASE);
    }

    JARVIS_ASSERT_FMT(
        VMM::virt_to_phys_in_pml4(PRIV_BASE, task_a->get_page_table()) == 0,
        "PRIV_BASE must not resolve after teardown");
    // NOLINTNEXTLINE(performance-no-int-to-ptr)
    auto *pml4 = reinterpret_cast<uint64_t *>(
        arch::HHDM_OFFSET + (task_a->get_page_table() & ~0xFFFULL));
#if defined(CONFIG_ARCH_RISCV64)
    // Sv39 root index (issue #540: canonical window names slot 320, the
    // same slot number as the x86 shift below — static-asserted equal).
    constexpr size_t PRIV_SLOT = (CONFIG_KERNEL_PRIV_DATA_BASE >> 30) & 0x1FF;
#else
    constexpr size_t PRIV_SLOT = (CONFIG_KERNEL_PRIV_DATA_BASE >> 39) & 0x1FF;
#endif
    static_assert(PRIV_SLOT == 320,
                  "PRIV window must live in slot 320 on every arch");
    JARVIS_ASSERT_FMT(pml4[PRIV_SLOT] == 0, "PML4 priv slot must be clear");

    task_a->cleanup();
    delete task_a;

    JARVIS_ASSERT_FMT(PMM::free_pages_ref() == free_before,
                      "PMM delta %ld pages after priv-window teardown",
                      (long)(PMM::free_pages_ref() - free_before));
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Issue #199 — tearing down an already-torn private window is a
// safe no-op (early-return on absent entries, no double free).
// Input: Map PRIV_BASE in a fresh task, teardown twice
// Expect: Second teardown changes nothing; PMM delta zero
// Depends: free_priv_window early-return chain
JARVIS_TEST(kernel_priv_teardown_double_free_safe,
            "PRE: none | POST: none") {
    uint64_t free_before = PMM::free_pages_ref();

    auto *task_a = TaskControlBlock::create([]() {}, 11, 10);
    JARVIS_ASSERT(task_a != nullptr);

    uint64_t priv_phys = PMM::alloc_page();
    JARVIS_ASSERT(priv_phys != 0);
    VMM::map_page_in_pml4(PRIV_BASE, priv_phys, false, false, task_a->get_page_table());
    {
        // NOTE: free_priv_window already reclaims the leaf data page.
        arch::IrqGuard teardown_guard;
        free_priv_window(task_a->get_page_table());
        arch::ArchPageTable::tlb_flush(PRIV_BASE);
    }

    uint64_t free_after_first = PMM::free_pages_ref();
    {
        arch::IrqGuard second_guard;
        free_priv_window(task_a->get_page_table());
        arch::ArchPageTable::tlb_flush(PRIV_BASE);
    }
    JARVIS_ASSERT_FMT(
        VMM::virt_to_phys_in_pml4(PRIV_BASE, task_a->get_page_table()) == 0,
        "PRIV_BASE must stay unmapped after double teardown");
    JARVIS_ASSERT_FMT(PMM::free_pages_ref() == free_after_first,
                      "second teardown must not free anything");

    task_a->cleanup();
    delete task_a;

    JARVIS_ASSERT_FMT(PMM::free_pages_ref() == free_before,
                      "PMM delta %ld pages after double teardown",
                      (long)(PMM::free_pages_ref() - free_before));
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Registers all kernel-isolation (MP-1) tests.
// Input: None
// Expect: All tests registered
// Depends: test framework
void register_kernel_isolation_tests() {
    JARVIS_REGISTER_TEST(kernel_priv_pml4_clone_kernel_half_present);
    JARVIS_REGISTER_TEST(kernel_priv_cross_task_data_isolation);
    JARVIS_REGISTER_TEST(kernel_priv_cr3_switch_on_dispatch);
    JARVIS_REGISTER_TEST(kernel_priv_teardown_frees_pml4_stack);
    JARVIS_REGISTER_TEST(kernel_priv_teardown_clears_pml4_slot_and_reclaims);
    JARVIS_REGISTER_TEST(kernel_priv_teardown_double_free_safe);
}
