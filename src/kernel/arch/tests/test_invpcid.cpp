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

/// @file test_invpcid.cpp
/// @brief INVPCID selective-invalidation tests (issue #157; stubs from
///        issue #85, module 14).  TLB hit/miss is unobservable without
///        cycle timing (QEMU-flaky by construction), so tests pin the
///        exact descriptor encoding, the dispatch routing (new flush
///        counters — deterministic on any CPU), and structural outcomes
///        (walks, no-fault re-touch).  The 3-line emit asm itself is
///        review- and audit-verified.

#if defined(CONFIG_ARCH_X86_64)
#include <test.hpp>
#include <logger.hpp>
#include <scope_guard.hpp>
#include <kernel/memory/pmm.hpp>
#include <kernel/memory/vmm.hpp>
#include <kernel/arch/page_table.hpp>
#include <kernel/arch/x86_64/hal/cpuid_impl.hpp>
#include <kernel/arch/x86_64/hal/io_impl.hpp>
#include <kernel/arch/x86_64/hal/pcid.hpp>
#include <kernel/arch/x86_64/hal/pcid.hpp>

using namespace kernel;

namespace {

// Allocate a kernel-owned scratch page, zeroed (test-local helper).
uint64_t alloc_zero_page() {
    uint64_t phys = PMM::alloc_page();
    if (phys == 0)
        return 0;
    // NOLINTNEXTLINE(performance-no-int-to-ptr)
    auto *virt = reinterpret_cast<uint64_t *>(arch::HHDM_OFFSET +
                                              (phys & ~0xFFFULL));
    __builtin_memset(virt, 0, arch::PAGE_SIZE);
    return phys;
}

} // namespace

// Runmode: kernel
// Testidea: INVPCID individual-address mode drops one page only.
// Input: Descriptor unit checks + two mapped scratch pages, unmap one
//        via the VMM path.
// Expect: Descriptor words exact (pcid masked, addr page-masked);
//         cleared VA walks unmapped, sibling intact and re-touchable;
//         single_va counter +1 (routing proof on any CPU).
// Depends: invpcid_build_desc, VMM unmap selective path (issue #157)
JARVIS_TEST(invpcid_single_page, "PRE: none | POST: none") {
    auto desc = arch::invpcid_build_desc(
        arch::InvpcidType::SINGLE_ADDRESS, 0x12345ULL, 0xABCDEF123ULL);
    JARVIS_ASSERT(desc.pcid == 0x345ULL);
    JARVIS_ASSERT(desc.addr == (0xABCDEF123ULL & ~0xFFFULL));

    constexpr uint64_t k_va_a = 0x10000000ULL;
    constexpr uint64_t k_va_b = 0x10001000ULL;
    uint64_t mem_before = PMM::free_memory();
    uint64_t pml4 = alloc_zero_page();
    uint64_t pa = PMM::alloc_user_page();
    uint64_t pb = PMM::alloc_user_page();
    JARVIS_ASSERT(pml4 != 0 && pa != 0 && pb != 0);
    VMM::map_page_in_pml4(k_va_a, pa, true, false, pml4);
    VMM::map_page_in_pml4(k_va_b, pb, true, false, pml4);
    JARVIS_ASSERT(VMM::virt_to_phys_in_pml4(k_va_a, pml4) == pa);
    JARVIS_ASSERT(VMM::virt_to_phys_in_pml4(k_va_b, pml4) == pb);

    arch::TlbFlushStats before = arch::tlb_flush_stats();
    VMM::unmap_page_in_pml4(k_va_a, pml4);
    arch::TlbFlushStats after = arch::tlb_flush_stats();
    JARVIS_ASSERT(after.single_va == before.single_va + 1);
    JARVIS_ASSERT(VMM::virt_to_phys_in_pml4(k_va_a, pml4) == 0);
    JARVIS_ASSERT(VMM::virt_to_phys_in_pml4(k_va_b, pml4) == pb);
    // Re-touch the sibling through its stable mapping (no fault).
    // NOLINTNEXTLINE(performance-no-int-to-ptr)
    auto *sib = reinterpret_cast<uint64_t *>(arch::HHDM_OFFSET + pb);
    sib[0] = 0xA5A5A5A5A5A5A5A5ULL;
    JARVIS_ASSERT(sib[0] == 0xA5A5A5A5A5A5A5A5ULL);

    // The unmapped page is orphaned (free_user_pages only reclaims
    // still-mapped pages) — free it explicitly before teardown.
    PMM::free_page(pa);
    VMM::free_user_pages(pml4);
    PMM::free_page(pml4);
    JARVIS_ASSERT(PMM::free_memory() == mem_before);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: INVPCID single-context mode drops one PCID's entries.
// Input: Descriptor unit checks + a forced PCID through the purge
//        path (no dispatch while forced — same containment as #156).
// Expect: Descriptor words exact; purge bumps the branch-correct
//         counter (invpcid vs fallback per hardware); freed ID
//         reallocates cleanly.
// Depends: invpcid_build_desc, tlb_purge_context (issue #157)
JARVIS_TEST(invpcid_single_context, "PRE: none | POST: none") {
    auto desc = arch::invpcid_build_desc(
        arch::InvpcidType::SINGLE_CONTEXT, 0x1ABCULL, 0xDEADULL);
    JARVIS_ASSERT(desc.pcid == 0xABCULL);
    JARVIS_ASSERT(desc.addr == (0xDEADULL & ~0xFFFULL));

    arch::pcid_test_force();
    auto force_off = ScopeGuard([]() { arch::pcid_test_reset(); });
    uint64_t id = arch::pcid_alloc();
    JARVIS_ASSERT(id != 0);
    arch::TlbFlushStats before = arch::tlb_flush_stats();
    arch::tlb_purge_context(static_cast<uint16_t>(id));
    arch::TlbFlushStats after = arch::tlb_flush_stats();
    if (arch::has_invpcid()) {
        JARVIS_ASSERT(after.ctx_invpcid == before.ctx_invpcid + 1);
    } else {
        JARVIS_ASSERT(after.ctx_fallback == before.ctx_fallback + 1);
    }
    arch::pcid_free(static_cast<uint16_t>(id));
    uint64_t id2 = arch::pcid_alloc();
    JARVIS_ASSERT(id2 != 0);
    arch::pcid_free(static_cast<uint16_t>(id2));
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: INVPCID all-context mode drops everything incl. globals.
// Input: Descriptor unit checks + a live purge_all call.
// Expect: Descriptor words exact; purge bumps the branch-correct
//         counter; a subsequent map/unmap cycle still works (no wedge).
// Depends: invpcid_build_desc, tlb_purge_all (issue #157)
JARVIS_TEST(invpcid_all_context, "PRE: none | POST: none") {
    auto desc = arch::invpcid_build_desc(
        arch::InvpcidType::ALL_INCL_GLOBAL, 0xFFULL, 0x12345ULL);
    JARVIS_ASSERT(desc.pcid == 0xFFULL);
    JARVIS_ASSERT(desc.addr == (0x12345ULL & ~0xFFFULL));

    arch::TlbFlushStats before = arch::tlb_flush_stats();
    arch::tlb_purge_all();
    arch::TlbFlushStats after = arch::tlb_flush_stats();
    if (arch::has_invpcid()) {
        JARVIS_ASSERT(after.all_invpcid == before.all_invpcid + 1);
    } else {
        JARVIS_ASSERT(after.all_fallback == before.all_fallback + 1);
    }

    constexpr uint64_t k_va = 0x10002000ULL;
    uint64_t mem_before = PMM::free_memory();
    uint64_t pml4 = alloc_zero_page();
    uint64_t data = PMM::alloc_user_page();
    JARVIS_ASSERT(pml4 != 0 && data != 0);
    VMM::map_page_in_pml4(k_va, data, true, false, pml4);
    JARVIS_ASSERT(VMM::virt_to_phys_in_pml4(k_va, pml4) == data);
    VMM::unmap_page_in_pml4(k_va, pml4);
    JARVIS_ASSERT(VMM::virt_to_phys_in_pml4(k_va, pml4) == 0);
    PMM::free_page(data);
    VMM::free_user_pages(pml4);
    PMM::free_page(pml4);
    JARVIS_ASSERT(PMM::free_memory() == mem_before);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: INVPCID on a never-mapped address is a safe no-op.
// Input: Unmap a VA with no tables behind it; builder edge cases.
// Expect: No fault; builder masks low addr bits; a subsequent valid
//         map/unmap cycle works (harness sane).
// Depends: VMM unmap fail-safe behavior (issue #157)
JARVIS_TEST(invpcid_nonexistent_safe, "PRE: none | POST: none") {
    auto desc = arch::invpcid_build_desc(
        arch::InvpcidType::SINGLE_ADDRESS, 7, 0xFFFFFFFFFFFFFFFFULL);
    JARVIS_ASSERT(desc.pcid == 7);
    JARVIS_ASSERT(desc.addr == 0xFFFFFFFFFFFFF000ULL);

    constexpr uint64_t k_va = 0x10003000ULL;
    uint64_t mem_before = PMM::free_memory();
    uint64_t pml4 = alloc_zero_page();
    JARVIS_ASSERT(pml4 != 0);
    VMM::unmap_page_in_pml4(k_va, pml4);
    uint64_t data = PMM::alloc_user_page();
    JARVIS_ASSERT(data != 0);
    VMM::map_page_in_pml4(k_va, data, true, false, pml4);
    JARVIS_ASSERT(VMM::virt_to_phys_in_pml4(k_va, pml4) == data);
    VMM::unmap_page_in_pml4(k_va, pml4);
    JARVIS_ASSERT(VMM::virt_to_phys_in_pml4(k_va, pml4) == 0);
    PMM::free_page(data);
    VMM::free_user_pages(pml4);
    PMM::free_page(pml4);
    JARVIS_ASSERT(PMM::free_memory() == mem_before);
    JARVIS_TEST_PASS();
}

void register_invpcid_tests() {
    Logger::info("Registering invpcid tests");
    JARVIS_REGISTER_TEST(invpcid_single_page);
    JARVIS_REGISTER_TEST(invpcid_single_context);
    JARVIS_REGISTER_TEST(invpcid_all_context);
    JARVIS_REGISTER_TEST(invpcid_nonexistent_safe);
}
#endif  // CONFIG_ARCH_X86_64
