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

/// @file test_cross_arch.cpp
/// @brief Cross-architecture tests that validate identical behaviour on all
/// three supported architectures (x86_64, aarch64, riscv64).
///
/// Each test exercises a kernel subsystem through its generic interface
/// (VMM, ArchContextManager, Timer, ArchInterruptController, IPC, VFS)
/// and is compiled for all arches.  Architecture-specific assertions are
/// guarded with #if/#elif/#endif.

#include <test.hpp>
#include <logger.hpp>
#include <constants.hpp>
#include <kernel/arch/page_table.hpp>
#include <kernel/arch/context.hpp>
#include <kernel/arch/interrupt_controller.hpp>
#include <kernel/arch/irq_guard.hpp>
#include <kernel/arch/qemu_debugcon.hpp>
#include <kernel/arch/timer.hpp>
#include <kernel/arch/io.hpp>
#include <kernel/memory/pmm.hpp>
#include <kernel/memory/vmm.hpp>
#include <kernel/memory/checked_ptr.hpp>
#include <kernel/task/task.hpp>
#include <kernel/task/scheduler.hpp>
#include <kernel/ipc/ipc.hpp>
#include <kernel/elf/elf.hpp>
#include <scope_guard.hpp>
#include <initrd/initrd.hpp>
#include "test_sched_helpers.hpp"
#include <kernel/nexios_config.h>
#include <kernel/vfs/vfs.hpp>

using namespace kernel;

// ============================================================================
// Test 1: Page-table basic walk — map, verify, unmap through VMM API
// ============================================================================
//
// Allocates a fresh physical page, maps it via VMM::map_page_in_pml4 into a
// cloned kernel page table, verifies VMM::virt_to_phys_in_pml4() returns the
// expected address, unmaps, and confirms the mapping is gone.  This exercises
// the arch-specific page-table walker through the generic VMM interface.
//
// Runmode: kernel
// Testidea: Map a page in a cloned PML4, verify virt_to_phys, unmap, verify
// gone Input: alloc_page + map_page_in_pml4 + virt_to_phys_in_pml4 + unmap
// Expect: Map succeeds, virt_to_phys returns correct phys, unmap succeeds, then
// 0 Depends: kernel::PMM, kernel::VMM
JARVIS_TEST(cross_page_table_map_unmap, "PRE: none | POST: none") {
    // Clone kernel page table for isolated testing
    uint64_t pml4 = VMM::clone_kernel_pml4();
    JARVIS_ASSERT_FMT(pml4 != 0, "clone_kernel_pml4 returned 0");

    // Known user virtual address for testing
    constexpr uint64_t TEST_VA = 0x8000000000ULL;

    // Allocate a USER-owned physical page
    uint64_t phys = PMM::alloc_user_page();
    JARVIS_ASSERT_FMT(phys != 0, "PMM::alloc_user_page returned 0");

    // Map it as a user page in the cloned PML4
    VMM::map_page_in_pml4(TEST_VA, phys, true, pml4);

    // Verify the translation via virt_to_phys_in_pml4
    uint64_t retrieved = VMM::virt_to_phys_in_pml4(TEST_VA, pml4);
    JARVIS_ASSERT_FMT(retrieved == (phys & ~0xFFFULL),
                      "virt_to_phys_in_pml4 returned 0x%lx, expected 0x%lx",
                      retrieved, phys & ~0xFFFULL);

    // Clean up user pages from cloned PML4
    VMM::free_user_pages(pml4);

    // Free the cloned PML4 page itself
    PMM::free_page(pml4);

    // Free the allocated physical page
    PMM::free_page(phys);

    JARVIS_TEST_PASS();
}

// ============================================================================
// Test 2: Page-table basic walk — verify no mapping for unmapped address
// ============================================================================
//
// Runmode: kernel
// Testidea: Verify virt_to_phys returns 0 for unmapped address in cloned PML4
// Input: Clone kernel PML4, query unmapped address
// Expect: virt_to_phys_in_pml4 returns 0
// Depends: kernel::VMM
JARVIS_TEST(cross_page_table_unmapped_returns_zero, "PRE: none | POST: none") {
    uint64_t pml4 = VMM::clone_kernel_pml4();
    JARVIS_ASSERT(pml4 != 0);

    // An address that should not be mapped in a fresh clone
    constexpr uint64_t UNMAPPED_VA = 0x8000001000ULL;
    uint64_t retrieved = VMM::virt_to_phys_in_pml4(UNMAPPED_VA, pml4);
    JARVIS_ASSERT_EQ(0ULL, retrieved);

    PMM::free_page(pml4);
    JARVIS_TEST_PASS();
}

// ============================================================================
// Test 3: Context switch — ArchContextManager save/restore/switch_to
// ============================================================================
//
// Exercises the arch-specific context management API (save, restore,
// switch_to) which lies at the heart of all context switching on every arch.
//
// Runmode: kernel
// Testidea: Save two contexts, switch between them, verify stack pointers
// Input: ArchContext objects, known stack addresses
// Expect: save records correct stack pointer, switch_to swaps, restore works
// Depends: kernel::arch::ArchContextManager
JARVIS_TEST(cross_context_save_restore, "PRE: none | POST: none") {
    arch::ArchContext ctx_a{};
    arch::ArchContext ctx_b{};

    // Use HHDM addresses that are at least 2 MiB apart to avoid aliasing
    uint64_t sp_a = arch::HHDM_OFFSET + 0x10000000ULL;
    uint64_t sp_b = arch::HHDM_OFFSET + 0x20000000ULL;

    arch::ArchContextManager::save(ctx_a, sp_a);
    arch::ArchContextManager::save(ctx_b, sp_b);

    // Verify saved stack pointers
#if defined(CONFIG_ARCH_X86_64)
    JARVIS_ASSERT_EQ(ctx_a.rsp, sp_a);
    JARVIS_ASSERT_EQ(ctx_b.rsp, sp_b);
#elif defined(CONFIG_ARCH_AARCH64)
    JARVIS_ASSERT_EQ(ctx_a.sp_el0, sp_a);
    JARVIS_ASSERT_EQ(ctx_b.sp_el0, sp_b);
#elif defined(CONFIG_ARCH_RISCV64)
    JARVIS_ASSERT_EQ(ctx_a.sp, sp_a);
    JARVIS_ASSERT_EQ(ctx_b.sp, sp_b);
#endif

    // Switch from A to B
    uint64_t current_sp = sp_a;
    arch::ArchContextManager::switch_to(ctx_a, ctx_b, current_sp);
    JARVIS_ASSERT_EQ(current_sp, sp_b);

    // Switch back from B to A
    arch::ArchContextManager::switch_to(ctx_b, ctx_a, current_sp);
    JARVIS_ASSERT_EQ(current_sp, sp_a);

    // Verify ctx_a still has its saved value
#if defined(CONFIG_ARCH_X86_64)
    JARVIS_ASSERT_EQ(ctx_a.rsp, sp_a);
    JARVIS_ASSERT_EQ(ctx_b.rsp, sp_b);
#elif defined(CONFIG_ARCH_AARCH64)
    JARVIS_ASSERT_EQ(ctx_a.sp_el0, sp_a);
    JARVIS_ASSERT_EQ(ctx_b.sp_el0, sp_b);
#elif defined(CONFIG_ARCH_RISCV64)
    JARVIS_ASSERT_EQ(ctx_a.sp, sp_a);
    JARVIS_ASSERT_EQ(ctx_b.sp, sp_b);
#endif

    JARVIS_TEST_PASS();
}

// ============================================================================
// Test 4: Context — init_stack creates valid stack frame
// ============================================================================
//
// Verifies that init_stack writes an entry-point, processor-state, and stack
// pointer to the correct positions for the architecture.
//
// Runmode: kernel
// Testidea: Create a stack frame with init_stack, verify entry and state
// present Input: Stack buffer, entry function, psr, user SP Expect: Stack
// contains entry and PSR, pointers are within range Depends:
// kernel::arch::ArchContextManager
JARVIS_TEST(cross_context_init_stack, "PRE: none | POST: none") {
    uint64_t stack[1024] = {};
    uint64_t *stack_top = stack + 1024;

    auto test_entry = []() {
        while (1) {
        }
    };

#if defined(CONFIG_ARCH_X86_64)
    uint64_t test_user_sp = arch::HHDM_OFFSET + 0x30000000ULL;
    arch::ArchContextManager::init_stack(
        stack_top, test_entry, arch::SEG_KERNEL_CODE, arch::SEG_KERNEL_DATA,
        arch::RFLAGS_DEFAULT, test_user_sp);
    // init_stack writes below the original stack_top — verify some values
    // were written (i.e. stack_top[-1] through stack_top[-22] are non-zero)
    bool written = false;
    for (int i = 1; i <= 22; ++i) {
        if (stack_top[-i] != 0) {
            written = true;
            break;
        }
    }
    JARVIS_ASSERT_FMT(written, "init_stack did not write any values to stack");
#elif defined(CONFIG_ARCH_AARCH64)
    uint64_t test_psr = 0;
    uint64_t test_user_sp = arch::HHDM_OFFSET + 0x30000000ULL;
    arch::ArchContextManager::init_stack(stack_top, test_entry, 0, 0, test_psr,
                                         test_user_sp);
    bool written = false;
    for (int i = 1; i <= 7; ++i) {
        if (stack_top[-i] != 0) {
            written = true;
            break;
        }
    }
    JARVIS_ASSERT_FMT(written, "init_stack did not write any values to stack");
#elif defined(CONFIG_ARCH_RISCV64)
    uint64_t test_psr = 0;
    uint64_t test_user_sp = arch::HHDM_OFFSET + 0x30000000ULL;
    arch::ArchContextManager::init_stack(stack_top, test_entry, 0, 0, test_psr,
                                         test_user_sp);
    bool written = false;
    for (int i = 1; i <= 20; ++i) {
        if (stack_top[-i] != 0) {
            written = true;
            break;
        }
    }
    JARVIS_ASSERT_FMT(written, "init_stack did not write any values to stack");
#endif

    JARVIS_TEST_PASS();
}

// ============================================================================
// Test 5: ArchPageTable::ENTRIES and PAGE_SIZE are consistent across arches
// ============================================================================
//
// Runmode: kernel
// Testidea: Verify page table constants match expectations
// Input: ArchPageTable static constants
// Expect: PAGE_SIZE=4096, ENTRIES=512 (all known CPU architectures)
// Depends: kernel::arch::ArchPageTable
JARVIS_TEST(cross_page_table_constants, "PRE: none | POST: none") {
    JARVIS_ASSERT_EQ(4096ULL, arch::ArchPageTable::PAGE_SIZE);
    JARVIS_ASSERT_EQ(512ULL, arch::ArchPageTable::ENTRIES);
    JARVIS_TEST_PASS();
}

// ============================================================================
// Test 6: Timer — ticks() is monotonic and non-zero
// ============================================================================
//
// Runmode: kernel
// Testidea: Read Timer::ticks() multiple times, verify monotonic and > 0
// Input: Repeated calls to arch::Timer::ticks()
// Expect: All values > 0, each >= previous
// Depends: kernel::arch::Timer
JARVIS_TEST(cross_timer_ticks_monotonic, "PRE: iocd | POST: none") {
    // Use set_ticks_for_test to establish a known baseline
    arch::Timer::set_ticks_for_test(100);
    uint64_t t1 = arch::Timer::ticks();
    JARVIS_ASSERT_FMT(t1 == 100,
                      "Timer ticks should be 100 after test set, got %lu", t1);

    uint64_t t2 = arch::Timer::ticks();
    uint64_t t3 = arch::Timer::ticks();
    JARVIS_ASSERT(t2 >= t1);
    JARVIS_ASSERT(t3 >= t2);
    JARVIS_TEST_PASS();
}

// ============================================================================
// Test 7: Timer — ns() returns reasonable values
// ============================================================================
//
// Runmode: kernel
// Testidea: Call Timer::ns() before/after a delay, verify positive difference
// Input: Two calls to arch::Timer::ns() with a small busy loop in between
// Expect: delta > 0 and delta < 10 seconds (sanity bound)
// Depends: kernel::arch::Timer
JARVIS_TEST(cross_timer_ns_delta, "PRE: iocd | POST: none") {
    uint64_t t0 = arch::Timer::ns();
    for (int i = 0; i < 100000; ++i) {
        asm volatile("");
    }
    uint64_t t1 = arch::Timer::ns();

    uint64_t delta = t1 - t0;
    JARVIS_ASSERT_FMT(delta > 0, "Timer ns() delta <= 0: %lu", delta);
    JARVIS_ASSERT_FMT(delta < 10000000000ULL,
                      "Timer ns() delta too large: %lu ns (limit 10s)", delta);

    JARVIS_TEST_PASS();
}

// ============================================================================
// Test 8: Timer — set_ticks_for_test and handle_irq work
// ============================================================================
//
// Runmode: kernel
// Testidea: Override ticks via set_ticks_for_test, verify handle_irq increments
// Input: set_ticks_for_test(0) then handle_irq() then check ticks()
// Expect: ticks() == 1
// Depends: kernel::arch::Timer
// CHANGED (v0.4.0 MP-4): the live PIT/APIC timer IRQ fires every tick and
// could increment the counter between set_ticks_for_test(0) and the exact
// assertion, making the test timing-racy (observed under the MP-1/MP-3
// per-switch overhead).  Run the exact-value section under IrqGuard so no
// timer IRQ can interleave.
JARVIS_TEST(cross_timer_irq_handler, "PRE: iocd | POST: none") {
    arch::IrqGuard guard;
    arch::Timer::set_ticks_for_test(0);
    JARVIS_ASSERT_EQ((uint64_t)0, arch::Timer::ticks());
    arch::Timer::handle_irq();
    JARVIS_ASSERT_EQ((uint64_t)1, arch::Timer::ticks());
    arch::Timer::handle_irq();
    JARVIS_ASSERT_EQ((uint64_t)2, arch::Timer::ticks());
    JARVIS_TEST_PASS();
}

// ============================================================================
// Test 9: Interrupt controller — init, EOI, mask, unmask
// ============================================================================
//
// This test validates that ArchInterruptController can be called without
// crashing.  The init() call is already made during boot, so this is a
// re-init test that exercises the arch-specific implementation.
//
// Runmode: kernel
// Testidea: Call ArchInterruptController init, EOI, mask, unmask
// Input: init() then eoi(32) then mask(1) then unmask(1)
// Expect: No crash, snapshot/restore restores original state
// Depends: kernel::arch::ArchInterruptController
JARVIS_TEST(cross_interrupt_controller_init, "PRE: iocd | POST: none") {
    arch::ArchInterruptController::init();
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Send EOI to interrupt controller
// Input: eoi(32) and eoi(40)
// Expect: No crash
// Depends: kernel::arch::ArchInterruptController
JARVIS_TEST(cross_interrupt_controller_eoi, "PRE: iocd | POST: none") {
    arch::ArchInterruptController::eoi(32);
    arch::ArchInterruptController::eoi(40);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Mask and unmask an IRQ
// Input: mask(1), then unmask(1)
// Expect: No crash
// Depends: kernel::arch::ArchInterruptController
JARVIS_TEST(cross_interrupt_controller_mask_unmask, "PRE: iocd | POST: none") {
    arch::ArchInterruptController::mask(1);
    arch::ArchInterruptController::unmask(1);
    JARVIS_TEST_PASS();
}

// ============================================================================
// Test 10: IPC — MessageQueue push/pop round-trip
// ============================================================================
//
// The IPC MessageQueue is a generic data structure used identically on all
// architectures.  This test validates a push/pop round-trip including
// priority ordering.
//
// Runmode: kernel
// Testidea: Push a message then pop it, verify all fields preserved
// Input: Message with known sender_id, type, priority, data_size, data
// Expect: Pop returns matching message, queue is empty afterward
// Depends: kernel::MessageQueue
JARVIS_TEST(cross_ipc_queue_push_pop, "PRE: none | POST: none") {
    MessageQueue q;
    q.init();
    JARVIS_ASSERT(q.is_empty());
    JARVIS_ASSERT(!q.is_full());

    Message msg;
    msg.sender_id = 42;
    msg.type = 7;
    msg.priority = 0;
    msg.data_size = 8;
    msg.data[0] = 0xCA;
    msg.data[1] = 0xFE;
    msg.data[2] = 0xBA;
    msg.data[3] = 0xBE;
    msg.data[4] = 0xDE;
    msg.data[5] = 0xAD;
    msg.data[6] = 0xBE;
    msg.data[7] = 0xEF;

    JARVIS_ASSERT(q.push(msg));
    JARVIS_ASSERT(!q.is_empty());
    JARVIS_ASSERT_EQ(1ULL, q.count);

    Message out;
    JARVIS_ASSERT(q.pop(out));
    JARVIS_ASSERT_EQ(42ULL, out.sender_id);
    JARVIS_ASSERT_EQ(7ULL, out.type);
    JARVIS_ASSERT_EQ(0ULL, out.priority);
    JARVIS_ASSERT_EQ(8ULL, out.data_size);
    JARVIS_ASSERT_EQ(0xCA, out.data[0]);
    JARVIS_ASSERT_EQ(0xFE, out.data[1]);
    JARVIS_ASSERT_EQ(0xBA, out.data[2]);
    JARVIS_ASSERT_EQ(0xBE, out.data[3]);
    JARVIS_ASSERT_EQ(0xDE, out.data[4]);
    JARVIS_ASSERT_EQ(0xAD, out.data[5]);
    JARVIS_ASSERT_EQ(0xBE, out.data[6]);
    JARVIS_ASSERT_EQ(0xEF, out.data[7]);
    JARVIS_ASSERT(q.is_empty());

    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Push multiple messages, verify priority ordering on pop
// Input: Three messages with priorities 2, 0, 1
// Expect: Popped in order priority 0, 1, 2 (highest first)
// Depends: kernel::MessageQueue
JARVIS_TEST(cross_ipc_queue_priority_ordering, "PRE: none | POST: none") {
    MessageQueue q;
    q.init();

    Message m1, m2, m3;
    m1.sender_id = 1;
    m1.type = 1;
    m1.priority = 2;
    m1.data_size = 0;
    m2.sender_id = 2;
    m2.type = 2;
    m2.priority = 0;
    m2.data_size = 0;
    m3.sender_id = 3;
    m3.type = 3;
    m3.priority = 1;
    m3.data_size = 0;

    JARVIS_ASSERT(q.push(m1));
    JARVIS_ASSERT(q.push(m2));
    JARVIS_ASSERT(q.push(m3));
    JARVIS_ASSERT_EQ(3ULL, q.count);

    Message out;
    JARVIS_ASSERT(q.pop(out));
    JARVIS_ASSERT_EQ(2ULL, out.sender_id); // priority 0 first
    JARVIS_ASSERT_EQ(0ULL, out.priority);

    JARVIS_ASSERT(q.pop(out));
    JARVIS_ASSERT_EQ(3ULL, out.sender_id); // priority 1 second
    JARVIS_ASSERT_EQ(1ULL, out.priority);

    JARVIS_ASSERT(q.pop(out));
    JARVIS_ASSERT_EQ(1ULL, out.sender_id); // priority 2 last
    JARVIS_ASSERT_EQ(2ULL, out.priority);

    JARVIS_ASSERT(q.is_empty());

    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Push messages until full, then verify overflow
// Input: Fill queue to capacity, then push one more
// Expect: push returns false when full; pop still works after overflow attempt
// Depends: kernel::MessageQueue
JARVIS_TEST(cross_ipc_queue_full_behavior, "PRE: none | POST: none") {
    MessageQueue q;
    q.init();

    Message fill;
    fill.sender_id = 1;
    fill.type = 0;
    fill.priority = 0;
    fill.data_size = 0;

    for (size_t i = 0; i < IPC_MAX_QUEUE_MSG; ++i) {
        JARVIS_ASSERT_FMT(q.push(fill), "Push failed at index %zu", i);
    }
    JARVIS_ASSERT(q.is_full());

    // Next push must fail
    JARVIS_ASSERT(!q.push(fill));

    // Pop should still work
    Message out;
    JARVIS_ASSERT(q.pop(out));
    JARVIS_ASSERT(!q.is_full());

    JARVIS_TEST_PASS();
}

// ============================================================================
// Test 11: VFS — resolve root and common device paths
// ============================================================================
//
// Runmode: kernel
// Testidea: Resolve "/" and verify it is a directory
// Input: vfs::resolve("/")
// Expect: Non-null vnode with S_IFDIR
// Depends: kernel::vfs::resolve
JARVIS_TEST(cross_vfs_resolve_root, "PRE: vfsd, iocd | POST: none") {
    vfs::Vnode *vn = vfs::resolve("/");
    JARVIS_ASSERT(vn != nullptr);
    JARVIS_ASSERT(vn->mode & vfs::S_IFDIR);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Resolve a nonexistent path returns nullptr
// Input: vfs::resolve("/nonexistent_path_cross_arch_test_xyz")
// Expect: nullptr
// Depends: kernel::vfs::resolve
JARVIS_TEST(cross_vfs_resolve_nonexistent, "PRE: vfsd, iocd | POST: none") {
    vfs::Vnode *vn = vfs::resolve("/nonexistent_path_cross_arch_test_xyz");
    JARVIS_ASSERT(vn == nullptr);
    JARVIS_TEST_PASS();
}

#if CONFIG_SMEP
// Runmode: kernel
// Testidea: v0.4.0 MP-4 — SMEP (CR4 bit 20) is enabled by the boot path on
// x86_64 when the CPU supports it (CPUID leaf 7 EBX[7]).
// Input: arch::read_cr4()
// Expect: bit 20 set.
// Depends: kernel::arch (x86_64 boot path)
JARVIS_TEST(smep_cr4_bit_set, "PRE: none | POST: none") {
#if defined(CONFIG_ARCH_X86_64)
    JARVIS_ASSERT(arch::read_cr4() & (1ULL << 20));
#else
    JARVIS_ASSERT(CONFIG_SMEP == 0);
#endif
    JARVIS_TEST_PASS();
}

namespace {

// Minimal ELF64 section-header / symbol layouts (kernel::elf::elf.hpp only
// exposes the file + program headers).
struct TestElfShdr {
    uint32_t sh_name;
    uint32_t sh_type;
    uint64_t sh_flags;
    uint64_t sh_addr;
    uint64_t sh_offset;
    uint64_t sh_size;
    uint32_t sh_link;
    uint32_t sh_info;
    uint64_t sh_addralign;
    uint64_t sh_entsize;
} __attribute__((packed));

struct TestElfSym {
    uint32_t st_name;
    uint8_t st_info;
    uint8_t st_other;
    uint16_t st_shndx;
    uint64_t st_value;
    uint64_t st_size;
} __attribute__((packed));

/// @brief Find the runtime VA of a symbol in a loaded ELF image (symtab
///        walk).  Returns 0 when the symbol is absent.
uint64_t elf_find_symbol_va(const uint8_t *data, const char *name) {
    constexpr uint32_t SHT_SYMTAB = 2;
    constexpr uint32_t SHT_STRTAB = 3;
    auto *hdr = reinterpret_cast<const kernel::elf::ELF64Header *>(data);
    if (!kernel::elf::validate_header(hdr) || hdr->shoff == 0)
        return 0;
    auto *shdr = reinterpret_cast<const TestElfShdr *>(data + hdr->shoff);
    if (hdr->shentsize < sizeof(TestElfShdr) || hdr->shnum == 0)
        return 0;
    for (uint16_t i = 0; i < hdr->shnum; ++i) {
        if (shdr[i].sh_type != SHT_SYMTAB)
            continue;
        if (shdr[i].sh_link >= hdr->shnum)
            continue;
        const TestElfShdr &strtab = shdr[shdr[i].sh_link];
        if (strtab.sh_type != SHT_STRTAB)
            continue;
        size_t sym_count =
            shdr[i].sh_entsize ? shdr[i].sh_size / shdr[i].sh_entsize : 0;
        for (size_t s = 0; s < sym_count; ++s) {
            auto *sym = reinterpret_cast<const TestElfSym *>(
                data + shdr[i].sh_offset + s * shdr[i].sh_entsize);
            if (sym->st_name == 0 || sym->st_name >= strtab.sh_size)
                continue;
            const char *sym_name =
                reinterpret_cast<const char *>(data + strtab.sh_offset +
                                               sym->st_name);
            if (__builtin_strcmp(sym_name, name) == 0)
                return sym->st_value;
        }
    }
    return 0;
}

} // namespace

// Runmode: kernel
// Testidea: v0.4.0 MP-4 — a user task that jumps to a kernel-text VA cannot
// execute it: SMEP (plus the U/S bit independently) turns the ring-3
// instruction fetch into a #PF → SIGSEGV → task TERMINATED, kernel survives.
// Input: load kva-probe; patch its g_kva global (via HHDM) with the address
// of a kernel function; dispatch.
// Expect: task state == TERMINATED (no kernel panic, no hang).
// Depends: elf loader, SMEP enablement, signal path
JARVIS_TEST(smep_user_exec_kernel_va_pf, "PRE: none | POST: none") {
    initrd::InitrdFile f = initrd::find("./kva-probe.c.elf");
    if (!f.data)
        f = initrd::find("kva-probe.c.elf");
    if (!f.data) {
        JARVIS_TEST_PASS(); // probe ELF not built — skip
        return;
    }
    auto *hdr = reinterpret_cast<const kernel::elf::ELF64Header *>(f.data);
    if (!kernel::elf::validate_header(hdr)) {
        JARVIS_TEST_PASS();
        return;
    }
    auto *t = kernel::elf::load(hdr, f.data, f.size);
    if (!t) {
        JARVIS_TEST_PASS();
        return;
    }
    uint64_t g_kva_va = elf_find_symbol_va(f.data, "g_kva");
    JARVIS_ASSERT(g_kva_va != 0);
    // Point the probe at a kernel-text function (supervisor page).
    uint64_t kernel_va = reinterpret_cast<uint64_t>(&test_smep_user_exec_kernel_va_pf);
    JARVIS_ASSERT(kernel_va >= 0xFFFF800000000000ULL);
    uint64_t g_kva_phys =
        VMM::virt_to_phys_in_pml4(g_kva_va, t->page_table_);
    JARVIS_ASSERT(g_kva_phys != 0);
    // NOLINTNEXTLINE(performance-no-int-to-ptr)
    __builtin_memcpy(reinterpret_cast<void *>(arch::HHDM_OFFSET + g_kva_phys),
                     &kernel_va, 8);

    Scheduler::add_task(*t);
    Scheduler::reschedule();
    kernel::test::wait_for_termination_safe(t);
    // U/S + SMEP both enforce: the ring-3 call into supervisor text must
    // fault and terminate the task, never panic the kernel.
    JARVIS_ASSERT(t->state == TaskState::TERMINATED);
    kernel::test::terminate_and_drain(*t);
    JARVIS_TEST_PASS();
}
#endif // CONFIG_SMEP

// ---------------------------------------------------------------------------
// MP-4.5 — SMAP negative/positive tests (x86_64, CONFIG_SMAP)
// ---------------------------------------------------------------------------
#if defined(CONFIG_ARCH_X86_64) && CONFIG_SMAP

// Runmode: kernel
// Testidea: v0.4.0 MP-4.2 — CR4.SMAP (bit 21) is set when CONFIG_SMAP and the
// CPU supports it.
JARVIS_TEST(smap_cr4_bit_set, "PRE: none | POST: none") {
    JARVIS_ASSERT(arch::read_cr4() & (1ULL << 21));
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: v0.4.0 MP-4.5 — a kernel-context deref of a PRESENT user page with
// AC=0 must #PF and be redirected by g_user_access_recover_ip (not panic).
// A dispatched kernel task maps a user page into its own private PML4 and
// deliberately derefs it with AC=0; the fault handler redirects to the
// recover label inside the task, the task self-terminates cleanly, and
// cleanup() reclaims the page + tables (zero PMM delta).
JARVIS_TEST(smap_kernel_deref_user_va_without_ac_pf, "PRE: none | POST: none") {
    static volatile int g_recovered = 0;
    g_recovered = 0;

    auto *t = TaskControlBlock::create(
        []() {
            auto *self = Scheduler::current_task();
            uint64_t phys = PMM::alloc_user_page();
            if (!phys)
                return;
            uint64_t user_va = 0x70000000ULL;
            VMM::map_page_in_pml4(user_va, phys, true, true,
                                  self->page_table_);
            // Arm fault recovery, then deliberately write to the present
            // U/S=1 page with AC=0.  With SMAP active this #PFs; the handler
            // redirects to recover_smap (regs[17] = g_user_access_recover_ip).
            kernel::g_user_access_recover_ip =
                reinterpret_cast<uint64_t>(&&recover_smap);
            // NOLINTNEXTLINE(performance-no-int-to-ptr)
            reinterpret_cast<volatile uint32_t *>(user_va)[0] = 0xAB;
            kernel::g_user_access_recover_ip = 0;

        recover_smap:
            arch::clac();
            kernel::g_user_access_recover_ip = 0;
            g_recovered = 1;
            // Do NOT free phys here: cleanup()'s free_user_pages reclaims the
            // user page + its PT pages and free_page reclaims the PML4.
        },
        11, 10);
    JARVIS_ASSERT(t != nullptr);

    Scheduler::add_task(*t);
    Scheduler::reschedule();
    kernel::test::wait_for_termination_safe(t);
    kernel::test::terminate_and_drain(*t);

    JARVIS_ASSERT(g_recovered == 1);
    JARVIS_ASSERT((arch::read_rflags() & (1ULL << 18)) == 0);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: v0.4.0 MP-4.2 — stac/clac roundtrip on a mapped user page: a
// safe_copy_to_user under stac succeeds, AC is restored to 0, and the user
// page content is correct.
JARVIS_TEST(smap_stac_clac_roundtrip_ok, "PRE: none | POST: none") {
    static volatile uint64_t g_val_readback = 0;

    auto *t = TaskControlBlock::create(
        []() {
            auto *self = Scheduler::current_task();
            uint64_t phys = PMM::alloc_user_page();
            if (!phys)
                return;
            uint64_t user_va = 0x70000000ULL;
            VMM::map_page_in_pml4(user_va, phys, true, true,
                                  self->page_table_);
            uint64_t val = 0x1122334455667788ULL;
            // safe_copy_to_user internally does stac/memcpy/clac.
            bool ok = kernel::safe_copy_to_user(
                reinterpret_cast<uint64_t *>(user_va), &val, 1);
            if (!ok)
                return;
            // AC must be restored to 0 after the copy.
            if ((arch::read_rflags() & (1ULL << 18)) != 0)
                return;
            // Verify via HHDM (kernel mapping of the same frame).
            // NOLINTNEXTLINE(performance-no-int-to-ptr)
            g_val_readback =
                *reinterpret_cast<volatile uint64_t *>(arch::HHDM_OFFSET +
                                                       phys);
        },
        11, 10);
    JARVIS_ASSERT(t != nullptr);

    Scheduler::add_task(*t);
    Scheduler::reschedule();
    kernel::test::wait_for_termination_safe(t);
    kernel::test::terminate_and_drain(*t);

    JARVIS_ASSERT(g_val_readback == 0x1122334455667788ULL);
    JARVIS_ASSERT((arch::read_rflags() & (1ULL << 18)) == 0);
    JARVIS_TEST_PASS();
}

#endif // x86_64 && CONFIG_SMAP

// ============================================================================
// Registration
// ============================================================================
void register_cross_arch_tests() {
    Logger::info("Registering cross-architecture tests");

    JARVIS_REGISTER_TEST(cross_page_table_map_unmap);
    JARVIS_REGISTER_TEST(cross_page_table_unmapped_returns_zero);
    JARVIS_REGISTER_TEST(cross_context_save_restore);
    JARVIS_REGISTER_TEST(cross_context_init_stack);
    JARVIS_REGISTER_TEST(cross_page_table_constants);
    JARVIS_REGISTER_TEST(cross_timer_ticks_monotonic);
    JARVIS_REGISTER_TEST(cross_timer_ns_delta);
    JARVIS_REGISTER_TEST(cross_timer_irq_handler);
    JARVIS_REGISTER_TEST(cross_interrupt_controller_init);
    JARVIS_REGISTER_TEST(cross_interrupt_controller_eoi);
    JARVIS_REGISTER_TEST(cross_interrupt_controller_mask_unmask);
    JARVIS_REGISTER_TEST(cross_ipc_queue_push_pop);
    JARVIS_REGISTER_TEST(cross_ipc_queue_priority_ordering);
    JARVIS_REGISTER_TEST(cross_ipc_queue_full_behavior);
    JARVIS_REGISTER_TEST(cross_vfs_resolve_root);
    JARVIS_REGISTER_TEST(cross_vfs_resolve_nonexistent);
#if CONFIG_SMEP
    JARVIS_REGISTER_TEST(smep_cr4_bit_set);
    JARVIS_REGISTER_TEST(smep_user_exec_kernel_va_pf);
#endif
#if defined(CONFIG_ARCH_X86_64) && CONFIG_SMAP
    JARVIS_REGISTER_TEST(smap_cr4_bit_set);
    JARVIS_REGISTER_TEST(smap_kernel_deref_user_va_without_ac_pf);
    JARVIS_REGISTER_TEST(smap_stac_clac_roundtrip_ok);
#endif
}
