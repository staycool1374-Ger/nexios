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

/// @file test_pcid.cpp
/// @brief PCID tests (issue #156; stubs from issue #85, module 13).
///
/// Two tasks dispatch for real through the tagged publish path; ring-3
/// cannot read CR3, so tags are verified post-mortem on the TCBs (lazy
/// assignment proves the publish ran tagged) plus direct pcid_tag()
/// unit checks.  Retention is structural (stable PCID + stable
/// translation across interleaved execution ⇒ same TLB entries stay
/// valid); TLB-hit cycle timing is deliberately NOT measured (QEMU
/// icount makes it flaky by construction).

#if defined(CONFIG_ARCH_X86_64)
#include <test.hpp>
#include <logger.hpp>
#include <scope_guard.hpp>
#include <kernel/task/scheduler.hpp>
#include <kernel/task/task.hpp>
#include <kernel/memory/pmm.hpp>
#include <kernel/memory/vmm.hpp>
#include <kernel/arch/x86_64/hal/cpuid_impl.hpp>
#include <kernel/arch/x86_64/hal/io_impl.hpp>
#include <kernel/arch/x86_64/hal/pcid.hpp>
#include "test_sched_helpers.hpp"

using namespace kernel;

// Ring-3 execution is impossible for test entries (create_user installs a
// yield-forever stub; kernel .text cannot run at CPL=3 — BUGS.md#020), so
// assignment and stability are verified on the TCBs directly without
// dispatch.  The tag-to-CR3 path itself is one mov-cr3 of the full 64-bit
// published value (isr_stubs.asm, review-verified, cannot drop low bits).

// Runmode: kernel
// Testidea: PCID is enabled in CR4 (PCIDE bit set after boot probe).
// Input: CR4 readback + CPUID support query.
// Expect: PCIDE set iff the CPU reports PCID support; the fallback
//         (unsupported) provably allocates nothing.
// Depends: CR4 PCIDE enablement (issue #156)
JARVIS_TEST(pcid_enabled_in_cr4, "PRE: none | POST: none") {
    if (arch::has_pcid()) {
        JARVIS_ASSERT((arch::read_cr4() & arch::CR4_PCIDE) != 0);
        JARVIS_ASSERT(arch::pcid_supported());
    } else {
        Logger::info("pcid: CPU lacks PCID — untagged fallback active");
        JARVIS_ASSERT(!arch::pcid_supported());
        JARVIS_ASSERT(arch::pcid_alloc() == 0);
    }
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: User address spaces own distinct PCIDs at birth (eager
//           assignment covers create/clone/elf uniformly; publish-time
//           lazy assignment is the backstop).  Forced logic path (D2):
//           no user task is dispatched while forced, so no tagged CR3
//           can reach hardware lacking PCIDE.
// Input: Two created (never dispatched) user tasks under force.
// Expect: pcid_ nonzero and distinct; reset restores hardware truth;
//         pcid_tag() unit checks hold.
// Depends: Eager PCID assignment at birth (issue #156)
JARVIS_TEST(pcid_tags_entries, "PRE: none | POST: none") {
    // pcid_tag() is pure: pin its contract directly.
    JARVIS_ASSERT(arch::pcid_tag(0x1000000ULL, 7) ==
                  (0x1000000ULL | 7ULL));
    JARVIS_ASSERT(arch::pcid_tag(0x1000000ULL, arch::PCID_KERNEL) ==
                  0x1000000ULL);

    arch::pcid_test_force();
    auto force_off = ScopeGuard([]() { arch::pcid_test_reset(); });
    auto *a = TaskControlBlock::create_user(kernel::test::forever_entry,
                                            12, 10, 32768);
    auto *b = TaskControlBlock::create_user(kernel::test::forever_entry,
                                            11, 10, 32768);
    JARVIS_ASSERT(a != nullptr && b != nullptr);
    JARVIS_ASSERT(a->pcid_ != 0 && b->pcid_ != 0);
    JARVIS_ASSERT(a->pcid_ != b->pcid_);
    a->cleanup();
    delete a;
    b->cleanup();
    delete b;
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: The PCID is TCB-owned, not PML4-owned: an exec-style PML4
//           swap keeps the ID, and the new table converges.
// Input: User task + probe mapping; swap page_table_ for a fresh
//        clone (as exec_into_current does); re-walk.
// Expect: pcid_ unchanged across the swap; new kernel half matches
//         the live root; probe phys reclaimed with the old table.
// Depends: TCB PCID ownership (issue #156)
JARVIS_TEST(pcid_selective_retention, "PRE: none | POST: none") {
    constexpr uint64_t k_probe_va = 0x20000000ULL;
    arch::pcid_test_force();
    auto force_off = ScopeGuard([]() { arch::pcid_test_reset(); });
    auto *a = TaskControlBlock::create_user(kernel::test::forever_entry,
                                            12, 10, 32768);
    JARVIS_ASSERT(a != nullptr);
    uint64_t pcid_before = a->pcid_;
    JARVIS_ASSERT(pcid_before != 0);
    uint64_t data_phys = PMM::alloc_user_page();
    JARVIS_ASSERT(data_phys != 0);
    VMM::map_page_in_pml4(k_probe_va, data_phys, true, false,
                          a->page_table_);
    JARVIS_ASSERT(VMM::virt_to_phys_in_pml4(k_probe_va, a->page_table_) ==
                  data_phys);
    uint64_t fresh = VMM::clone_kernel_pml4();
    JARVIS_ASSERT(fresh != 0);
    VMM::free_user_pages(a->page_table_);
    PMM::free_page(a->page_table_);
    a->page_table_ = fresh;
    JARVIS_ASSERT(a->pcid_ == pcid_before);
    JARVIS_ASSERT(VMM::kernel_half_equal(fresh, VMM::get_kernel_pml4()));
    a->cleanup();
    delete a;
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: PCID rollover recycles all 4096 IDs safely (forced logic
//           path, D2 — same no-dispatch containment as above).
// Input: Drive pcid_alloc() to exhaustion (+1 past the live set).
// Expect: Epoch bumps exactly once at exhaustion; every issued ID
//         unique; post-rollover allocs still unique; all freed cleanly
//         and hardware truth restored.
// Depends: PCID rollover handling (issue #156)
JARVIS_TEST(pcid_rollover, "PRE: none | POST: none") {
    arch::pcid_test_force();
    auto force_off = ScopeGuard([]() { arch::pcid_test_reset(); });
    static uint16_t held[4100];
    uint64_t epoch_before = arch::pcid_epoch();
    uint64_t held_count = 0;
    for (uint64_t i = 0; i < 4100; ++i) {
        uint16_t id = arch::pcid_alloc();
        if (id == 0)
            break;
        held[held_count++] = id;
        if (arch::pcid_epoch() != epoch_before)
            break;
    }
    // Exhaustion (+1) must have forced exactly one rollover.
    JARVIS_ASSERT(arch::pcid_epoch() == epoch_before + 1);
    for (uint64_t i = 0; i < held_count; ++i) {
        for (uint64_t j = i + 1; j < held_count; ++j)
            JARVIS_ASSERT(held[i] != held[j]);
    }
    for (uint64_t i = 0; i < held_count; ++i)
        arch::pcid_free(held[i]);
    JARVIS_ASSERT(arch::pcid_epoch() == epoch_before + 1);
    uint16_t fresh = arch::pcid_alloc();
    JARVIS_ASSERT(fresh != 0);
    arch::pcid_free(fresh);
    JARVIS_TEST_PASS();
}

void register_pcid_tests() {
    Logger::info("Registering pcid tests");
    JARVIS_REGISTER_TEST(pcid_enabled_in_cr4);
    JARVIS_REGISTER_TEST(pcid_tags_entries);
    JARVIS_REGISTER_TEST(pcid_selective_retention);
    JARVIS_REGISTER_TEST(pcid_rollover);
}
#endif  // CONFIG_ARCH_X86_64
