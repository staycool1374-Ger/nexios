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

#include <kernel/memory/vmm.hpp>
#include <kernel/memory/vmm_path.hpp> // issue #347 Slice D
#include <kernel/memory/vmm_internal.hpp> // issue #363 shared TU internals
#include <kernel/memory/pmm.hpp>
#include <kernel/cap/frame.hpp>
#include <kernel/cap/mmio.hpp>
#include <kernel/arch/pci.hpp>
#include <kernel/task/scheduler.hpp>
#include <kernel/arch/io.hpp>
#include <constants.hpp>
#include <kernel/arch/page_table.hpp>
#include <assert.hpp>
#include <kernel/memory/vmm_errors.hpp>
#include <kernel/arch/qemu_debugcon.hpp>

namespace kernel {

constinit uint64_t VMM::kernel_pml4_ = 0;
constinit uint64_t VMM::pml4_kernel_template_[512] = {};
bool VMM::hhdm_modified_ = false;
bool VMM::identity_modified_ = false;

#if defined(CONFIG_DEBUG)
// Fault-injection mask for vmm_step() (issue #347 Slice D, declared in
// vmm_path.hpp). Test-only; absent in release.
uint64_t g_test_skip_vmm_step = 0;
// Countdown state for the test-only table-alloc fault injector
// (issue #361; VMM:: API declared in vmm.hpp). Test-only; absent in
// release. fail_at == 0 means disarmed; firing is single-shot.
static size_t g_test_table_fail_at = 0;
static size_t g_test_table_fail_remaining = 0;
#endif

/// @brief Initialise the VMM: capture current PML4, zero residual bootloader
/// entries.
void VMM::init() {
    kernel_pml4_ = arch::read_cr3();

#if defined(CONFIG_ARCH_X86_64)
    // NOLINTNEXTLINE(performance-no-int-to-ptr)
    auto *pml4 = reinterpret_cast<uint64_t *>(arch::HHDM_OFFSET +
                                              (kernel_pml4_ & PAGE_TABLE_ADDR_MASK));

    // Zero unused entries in all boot-constructed page tables so the page
    // table walker never follows uninitialised garbage pointers.  The boot
    // code only sets entry 0 in the PDPTs and entries 0-63 in the PDs;
    // the rest may contain residual GRUB/BIOS data with PAGE_PRESENT set.

    // Zero PDPT_IDENTITY[1-511]
    {
        uint64_t pdpt_phys = pml4[0] & PAGE_TABLE_ADDR_MASK;
        // NOLINTNEXTLINE(performance-no-int-to-ptr)
        auto *pdpt_ident =
            reinterpret_cast<uint64_t *>(arch::HHDM_OFFSET + pdpt_phys);
        for (size_t i = 1; i < PAGE_TABLE_ENTRIES; ++i)
            pdpt_ident[i] = 0;
    }

    // Zero PDPT_HIGHER[1-511]
    {
        uint64_t pdpt_phys = pml4[256] & PAGE_TABLE_ADDR_MASK;
        // NOLINTNEXTLINE(performance-no-int-to-ptr)
        auto *pdpt_higher =
            reinterpret_cast<uint64_t *>(arch::HHDM_OFFSET + pdpt_phys);
        for (size_t i = 1; i < PAGE_TABLE_ENTRIES; ++i)
            pdpt_higher[i] = 0;
    }

    // Zero PD_IDENTITY[64-511] (entries 0-63 are valid huge pages)
    {
        // NOLINTNEXTLINE(performance-no-int-to-ptr)
        auto *pdpt_ident_p = reinterpret_cast<uint64_t *>(
            arch::HHDM_OFFSET + (pml4[0] & PAGE_TABLE_ADDR_MASK));
        // NOLINTNEXTLINE(performance-no-int-to-ptr)
        auto *pd_ident = reinterpret_cast<uint64_t *>(
            arch::HHDM_OFFSET + (pdpt_ident_p[0] & PAGE_TABLE_ADDR_MASK));
        for (size_t i = 64; i < PAGE_TABLE_ENTRIES; ++i)
            pd_ident[i] = 0;
    }

    // Zero PD_HIGHER[64-511] (entries 0-63 are valid huge pages)
    {
        // NOLINTNEXTLINE(performance-no-int-to-ptr)
        auto *pdpt_higher_p = reinterpret_cast<uint64_t *>(
            arch::HHDM_OFFSET + (pml4[256] & PAGE_TABLE_ADDR_MASK));
        // NOLINTNEXTLINE(performance-no-int-to-ptr)
        auto *pd_higher = reinterpret_cast<uint64_t *>(
            arch::HHDM_OFFSET + (pdpt_higher_p[0] & PAGE_TABLE_ADDR_MASK));
        for (size_t i = 64; i < PAGE_TABLE_ENTRIES; ++i)
            pd_higher[i] = 0;
    }

    // Zero unused PML4 entries [1-255, 257-511] so get_table never follows
    // residual UEFI/GRUB data left by the bootloader.  The boot code only
    // sets PML4[0] and PML4[256]; the rest may contain garbage.
    for (size_t i = 1; i < PAGE_TABLE_ENTRIES; ++i) {
        if (i == 256)
            continue;
        pml4[i] = 0;
    }
#elif defined(CONFIG_ARCH_AARCH64)
    // Boot.S already set up page tables.  Identity map uses 2048 L2 entries
    // across 4 tables (0-4GB).  Higher half has 2 L2 entries for kernel
    // (0xFFFF800040000000-0xFFFF800040400000).  VMM zeroing below is
    // x86_64-specific (PDPT/PD structure mismatch) and would corrupt the
    // identity L1[1] entry needed for kernel code at PA 0x40080000.
#elif defined(CONFIG_ARCH_RISCV64)
    // Boot.S already set up Sv39 page tables with HHDM mapping.
    // Kernel maps PA 0x80200000+ to VA 0xFFFFFFC080200000+ via 2MB pages.
    // No additional zeroing needed.
#endif
}

extern "C" {
// Linker section bounds (4K-aligned starts on all arches; see
// linker/linker_{x86_64,aarch64,riscv64}.ld). Mirrors the integrity.hpp
// _text_start/_text_end declaration idiom.
extern uint64_t _text_start[];
extern uint64_t _text_end[];
extern uint64_t _rodata_start[];
extern uint64_t _rodata_end[];
extern uint64_t _data_start[];
extern uint64_t _bss_end[];
}

void VMM::harden_kernel_half() {
    // Issue #376 (boot-text W^X epic remainder): convert the RWX
    // boot-established mappings over the image to text RX / rodata RO+NX /
    // data+bss RW+NX. Runs once at end of bring-up (single-threaded BSP,
    // IRQs off, PMM up) BEFORE snapshot_kernel_template() so the template
    // and every later clone inherit hardened flags by value. Identity map
    // and non-image HHDM (boot code/stack, AP trampoline, boot structs)
    // stay RWX by design. Any mapping failure panics: a half-hardened root
    // must never boot.
    const uint64_t pml4 = kernel_pml4_;
    if (pml4 == 0)
        panic("VMM: harden with null kernel PML4");
    // NOLINTNEXTLINE(performance-no-int-to-ptr)
    const uint64_t text_start = reinterpret_cast<uint64_t>(_text_start);
    // NOLINTNEXTLINE(performance-no-int-to-ptr)
    const uint64_t text_end = reinterpret_cast<uint64_t>(_text_end);
    // NOLINTNEXTLINE(performance-no-int-to-ptr)
    const uint64_t rodata_start = reinterpret_cast<uint64_t>(_rodata_start);
    // NOLINTNEXTLINE(performance-no-int-to-ptr)
    const uint64_t rodata_end = reinterpret_cast<uint64_t>(_rodata_end);
    // NOLINTNEXTLINE(performance-no-int-to-ptr)
    const uint64_t data_start = reinterpret_cast<uint64_t>(_data_start);
    // NOLINTNEXTLINE(performance-no-int-to-ptr)
    const uint64_t bss_end = reinterpret_cast<uint64_t>(_bss_end);
    if (!(text_start < text_end && text_end <= rodata_start &&
          rodata_start < rodata_end && rodata_end <= data_start &&
          data_start < bss_end))
        panic("VMM: harden section bounds disordered");

    // Per-page worker: resolve phys (unmapped holes panic — the image is
    // contiguous), install a 4K non-user leaf via try (splits boot huge
    // entries; split fills preserve flags), then encode the region bits.
    // text: RX (RO encode keeps X via the kernel_text_exec hook);
    // rodata: RO+NX; data+bss: RW+NX (no RO step).
    auto harden_page = [&](uint64_t va, bool exec, bool read_only) {
        const uint64_t phys = virt_to_phys_in_pml4(va, pml4);
        if (phys == 0)
            panic("VMM: harden unmapped image page");
        // Text installs executable (RWX window, never NX — an NX window
        // on the live text would triple-fault the next fetch after the
        // map's internal TLB flush); rodata/data install non-executable
        // (never executed, window safe). The RO encode then clears W.
        if (map_page_in_pml4_try(va, phys, false, exec, pml4) !=
            errors::VMM_ERR_OK)
            panic("VMM: harden map failed");
        uint64_t *leaf = pte_leaf_in_pml4(va, pml4);
        if (leaf == nullptr)
            panic("VMM: harden leaf missing after map");
        if (read_only)
            *leaf = vmm_arch_encode_ro(*leaf, exec);
        if (exec)
            *leaf = vmm_arch_kernel_text_exec(*leaf);
        // Issue #389: route through the shootdown choke (local-first,
        // then cross-CPU fan-out; PCID default-(b) — kernel-half va).
        tlb_shootdown(va, 0, false);
    };

    for (uint64_t va = text_start; va < text_end; va += arch::PAGE_SIZE)
        harden_page(va, true, true);
    for (uint64_t va = rodata_start; va < rodata_end; va += arch::PAGE_SIZE)
        harden_page(va, false, true);
    for (uint64_t va = data_start; va < bss_end; va += arch::PAGE_SIZE)
        harden_page(va, false, false);
    // Issue #389: post-harden full purge through the choke (remote CPUs
    // may hold entries for the remapped image half).
    tlb_shootdown(0, 0, true);
}

/// @brief Walk or create a page-table entry at the current level.
/// @param table     Pointer to the current-level page table.
/// @param index     Entry index within @p table.
/// @param create    If true, allocate a new table when missing.
/// @param user_alloc If true, allocate USER-owned pages for the new table.
/// @return Pointer to the next-level table, or nullptr if not present and
/// !create.
/// WCET: intentional flat walk/split (bounded depth, at most one table
/// alloc per call); a failed alloc returns nullptr and the caller drops
/// the mapping through note_silent_drop_once (never retries in a loop).
#if defined(CONFIG_DEBUG)
// Test-only table-alloc fault injector (issue #361).  Countdown covers
// allocation ATTEMPTS only: every gate sits after the presence/split-need
// checks, immediately before the PMM::alloc_* call, so present-table hits
// never consume the countdown and fail-at-n is deterministic.
void VMM::test_arm_table_alloc_fail_at(size_t alloc_index) noexcept {
    g_test_table_fail_at = alloc_index;
    g_test_table_fail_remaining = alloc_index;
}

void VMM::test_disarm_table_alloc_fail() noexcept {
    g_test_table_fail_at = 0;
    g_test_table_fail_remaining = 0;
}

void VMM::test_table_alloc_fail_begin_call() noexcept {
    g_test_table_fail_remaining = g_test_table_fail_at;
}

bool VMM::test_maybe_fail_table_alloc() noexcept {
    if (g_test_table_fail_at == 0 || g_test_table_fail_remaining == 0)
        return false;
    --g_test_table_fail_remaining;
    if (g_test_table_fail_remaining == 0) {
        g_test_table_fail_at = 0; // single-shot: firing disarms
        return true;
    }
    return false;
}
#endif

void VMM::test_table_alloc_fail_snapshot_reset() noexcept {
#if defined(CONFIG_DEBUG)
    g_test_table_fail_at = 0;
    g_test_table_fail_remaining = 0;
#endif
}

uint64_t *VMM::get_table(uint64_t *table, size_t index, bool create,
                         bool user_alloc) {

    if (table[index] & PAGE_PRESENT) {
        // Arch child-table decode (issue #363 P2: Sv39 PTE codec vs
        // frame mask).
        uint64_t target_phys = vmm_arch_table_child(table[index]);
        if (!PMM::is_allocated(target_phys)) {
            table[index] = 0;
        } else if (vmm_arch_split_needed(table[index])) {
            if (!create)
                return nullptr;
#if defined(CONFIG_DEBUG)
            if (test_maybe_fail_table_alloc()) // issue #361: injected fault
                return nullptr;
#endif
            uint64_t new_page = PMM::alloc_page_table();
            if (!new_page)
                return nullptr;
            // NOLINTNEXTLINE(performance-no-int-to-ptr)
            auto *new_table =
                reinterpret_cast<uint64_t *>(arch::HHDM_OFFSET + new_page);
            // Arch split fill + link (issue #363 P2): huge-base decode,
            // flag preservation, and the 512-entry fill live per-arch.
            vmm_arch_split_fill(new_table, table[index]);
            table[index] = vmm_arch_table_link(new_page);
            return new_table;
        } else {
            // NOLINTNEXTLINE(performance-no-int-to-ptr)
            // Arch child-table decode (issue #363 P2).
            return reinterpret_cast<uint64_t *>(arch::HHDM_OFFSET +
                                                vmm_arch_table_child(
                                                    table[index]));
        }
    }
    if (!create)
        return nullptr;

#if defined(CONFIG_DEBUG)
    if (test_maybe_fail_table_alloc()) // issue #361: injected fault
        return nullptr;
#endif
    uint64_t new_page =
        user_alloc ? PMM::alloc_user_page() : PMM::alloc_page_table();
    if (!new_page)
        return nullptr;

    // NOLINTNEXTLINE(performance-no-int-to-ptr)
    auto *new_table =
        reinterpret_cast<uint64_t *>(arch::HHDM_OFFSET + new_page);
    for (size_t i = 0; i < PAGE_TABLE_ENTRIES; ++i) {
        new_table[i] = 0;
    }

    // Arch table link (issue #363 P2).
    table[index] = vmm_arch_table_link(new_page);

    return new_table;
}

void warn_test_kernel_touch(bool gate, TestTouchWarn warn,
                                   uint64_t va, size_t idx) {
    if (!gate || !Scheduler::is_test_active()) {
        return;
    }
    switch (warn) {
        case TestTouchWarn::kMapL1:
            Logger::warn("map_page: test modifying kernel-space VA 0x%lx "
                         "(l0_idx=%zu) — L1 restore will clean up",
                         va, idx);
            break;
        case TestTouchWarn::kMapPd:
            Logger::warn("map_page: test modifying kernel-space VA 0x%lx "
                         "(pml4_idx=%zu) — PD restore will clean up",
                         va, idx);
            break;
        case TestTouchWarn::kUnmapL0:
            Logger::warn("unmap_page: test unmapping kernel-space VA 0x%lx "
                         "(l0_idx=%zu)",
                         va, idx);
            break;
        case TestTouchWarn::kUnmapPml4:
            Logger::warn("unmap_page: test unmapping kernel-space VA 0x%lx "
                         "(pml4_idx=%zu)",
                         va, idx);
            break;
        case TestTouchWarn::kAccessPml4:
            Logger::warn("virt_to_phys: test accessing kernel-space VA 0x%lx "
                         "(pml4_idx=%zu)",
                         va, idx);
            break;
    }
}

void arm_test_touch_flags(bool arm_hhdm, bool arm_identity) {
    if (!Scheduler::is_test_active()) {
        return;
    }
    // Issue #60/#152: release stores pair with the acquire take in
    // snapshot_restore (a concurrent take can never lose a set).
    if (arm_hhdm) {
        VMM::set_hhdm_modified();
    }
    if (arm_identity) {
        VMM::set_identity_modified();
    }
}

// Hex-value half of the throttled diagnostics below (issue #254 H8: shared
// by note_silent_drop_once and the free_user_pages skip log — the 0x-print
// loop was duplicated).  Debugcon port IO only (lock-free, ISR-safe;
// no-op off x86_64).
void write_hex_debugcon(uint64_t val) {
    char buf[24];
    int p = 0;
    buf[p++] = '0';
    buf[p++] = 'x';
    bool started = false;
    for (int sh = 60; sh >= 0; sh -= 4) {
        unsigned nib = static_cast<unsigned>((val >> sh) & 0xF);
        if (nib || started || sh == 0) {
            buf[p++] = "0123456789abcdef"[nib];
            started = true;
        }
    }
    buf[p++] = '\n';
    arch::QemuDebugcon::write(buf, static_cast<size_t>(p));
}

// Bounded one-shot diagnostic for silently-dropped mappings (issue #254 H7).
// WHY silent drops exist: a missing table or a failed split-alloc means "no
// such mapping" to the caller, so these paths return without an error code.
// The throttled note keeps them observable without flooding the log.
// Lock-free (atomics + debugcon port IO only): safe from any context,
// including tick/ISR paths.  Never touches Logger/serial/shell.
void note_silent_drop_once(const char *tag, uint64_t val) {
    static uint64_t count = 0;
    if (__atomic_load_n(&count, __ATOMIC_RELAXED) >= kFupSkipLogCap) {
        return;
    }
    __atomic_fetch_add(&count, 1UL, __ATOMIC_RELAXED);
    arch::QemuDebugcon::write(tag);
    write_hex_debugcon(val);
}

/// @brief Map a virtual page in the kernel table (legacy void wrapper:
///        behavior-identical to the pre-#325 revision — walk allocation
///        failures stay silent drops with a one-time log; fallible
///        callers use map_page_try / map_page_err).
void VMM::map_page(uint64_t virt_addr, uint64_t phys_addr, bool user) {
    (void)map_page_try(virt_addr, phys_addr, user);
}

/// @brief Unmap a virtual page from the kernel page table.
/// @param virt_addr Virtual address to unmap.
void VMM::unmap_page(uint64_t virt_addr) {
#if defined(CONFIG_ARCH_RISCV64)
    auto *l0 = reinterpret_cast<uint64_t *>(arch::HHDM_OFFSET +
                                            (kernel_pml4_ & PAGE_TABLE_ADDR_MASK));

    size_t l0_idx = (virt_addr & VMM::L0_MASK) >> VMM::L0_SHIFT;
    size_t l1_idx = (virt_addr & VMM::L1_MASK) >> VMM::L1_SHIFT;
    size_t l2_idx = (virt_addr & VMM::L2_MASK) >> VMM::L2_SHIFT;

    // Issue #152: mirror map_page (x86_64 unmap_page arms the identity
    // flag only, plus a warn for kernel-space VAs; see the notifiers above).
    warn_test_kernel_touch(l0_idx == kHhdmL0Idx, TestTouchWarn::kUnmapL0,
                           virt_addr, l0_idx);

    auto *l1 = get_table(l0, l0_idx, false);
    if (!l1) [[unlikely]] {
        note_silent_drop_once("[UNMAP-DROP] l1 ", virt_addr);
        return;
    }
    auto *l2 = get_table(l1, l1_idx, false);
    if (!l2) [[unlikely]] {
        note_silent_drop_once("[UNMAP-DROP] l2 ", virt_addr);
        return;
    }

    l2[l2_idx] = 0;
    // Issue #389: rv64 unmap leg through the choke (PCID default-(b)).
    tlb_shootdown(virt_addr, 0, false);
#else
    // NOLINTNEXTLINE(performance-no-int-to-ptr)
    auto *pml4 = reinterpret_cast<uint64_t *>(arch::HHDM_OFFSET +
                                              (kernel_pml4_ & PAGE_TABLE_ADDR_MASK));

    size_t pml4_idx = arch::ArchPageTable::pml4_index(virt_addr);
    warn_test_kernel_touch(pml4_idx >= arch::PML4_USER_COUNT,
                           TestTouchWarn::kUnmapPml4, virt_addr, pml4_idx);
    // Low identity-map VAs: flag for PD_IDENTITY restore (see map_page).
    arm_test_touch_flags(false, pml4_idx < arch::PML4_USER_COUNT);

    size_t pdpt_idx = arch::ArchPageTable::pdpt_index(virt_addr);
    size_t pd_idx = arch::ArchPageTable::pd_index(virt_addr);
    size_t pt_idx = arch::ArchPageTable::pt_index(virt_addr);

    auto *pdpt = get_table(pml4, pml4_idx, false);
    if (!pdpt) [[unlikely]] {
        note_silent_drop_once("[UNMAP-DROP] pdpt ", virt_addr);
        return;
    }
    auto *pd = get_table(pdpt, pdpt_idx, false);
    if (!pd) [[unlikely]] {
        note_silent_drop_once("[UNMAP-DROP] pd ", virt_addr);
        return;
    }
    auto *pt = get_table(pd, pd_idx, false);
    if (!pt) [[unlikely]] {
        note_silent_drop_once("[UNMAP-DROP] pt ", virt_addr);
        return;
    }

    pt[pt_idx] = 0;
    // Issue #389: x86 unmap leg through the choke (PCID default-(b)).
    tlb_shootdown(virt_addr, 0, false);
#endif
}

/// @brief Translate a virtual address to a physical address via the kernel
/// PML4.
/// @param virt_addr Virtual address.
/// @return Physical address, or 0 if not mapped.
uint64_t VMM::virt_to_phys(uint64_t virt_addr) {
#if defined(CONFIG_ARCH_RISCV64)
    auto *l0 = reinterpret_cast<uint64_t *>(arch::HHDM_OFFSET +
                                            (kernel_pml4_ & PAGE_TABLE_ADDR_MASK));

    size_t l0_idx = (virt_addr & VMM::L0_MASK) >> VMM::L0_SHIFT;
    size_t l1_idx = (virt_addr & VMM::L1_MASK) >> VMM::L1_SHIFT;
    size_t l2_idx = (virt_addr & VMM::L2_MASK) >> VMM::L2_SHIFT;

    auto *l1 = get_table(l0, l0_idx, false);
    if (!l1)
        return 0;

    // Check for 2MB block mapping (leaf at L1 level).  Arch block-base
    // decode (issue #363 P2: shift-based Sv39 codec vs frame masks).
    if ((l1[l1_idx] & PAGE_PRESENT) && vmm_arch_split_needed(l1[l1_idx])) {
        return vmm_arch_block_base(l1[l1_idx], true) +
               (virt_addr & kHuge2MOffsetMask);
    }

    auto *l2 = get_table(l1, l1_idx, false);
    if (!l2 || !(l2[l2_idx] & PAGE_PRESENT))
        return 0;

    return vmm_arch_block_base(l2[l2_idx], false) +
           (virt_addr & kPageOffsetMask);
#else
    // NOLINTNEXTLINE(performance-no-int-to-ptr)
    auto *pml4 = reinterpret_cast<uint64_t *>(arch::HHDM_OFFSET +
                                              (kernel_pml4_ & PAGE_TABLE_ADDR_MASK));

    size_t pml4_idx = arch::ArchPageTable::pml4_index(virt_addr);
    warn_test_kernel_touch(pml4_idx >= arch::PML4_USER_COUNT,
                           TestTouchWarn::kAccessPml4, virt_addr, pml4_idx);

    size_t pdpt_idx = arch::ArchPageTable::pdpt_index(virt_addr);
    size_t pd_idx = arch::ArchPageTable::pd_index(virt_addr);
    size_t pt_idx = arch::ArchPageTable::pt_index(virt_addr);

    auto *pdpt = get_table(pml4, pml4_idx, false);
    if (!pdpt)
        return 0;
    auto *pd = get_table(pdpt, pdpt_idx, false);
    if (!pd)
        return 0;

    // Arch block-leaf test + frame decode (issue #363 P2).
    if (vmm_arch_split_needed(pd[pd_idx])) {
        return vmm_arch_block_base(pd[pd_idx], true) +
               (virt_addr & kHuge2MOffsetMask);
    }

    auto *pt = get_table(pd, pd_idx, false);
    if (!pt || !(pt[pt_idx] & PAGE_PRESENT))
        return 0;

    return vmm_arch_block_base(pt[pt_idx], false) +
           (virt_addr & kPageOffsetMask);
#endif
}

/// @brief Read the current PML4 physical address from CR3 (or equivalent).
/// @return Physical address of the active PML4.
uint64_t VMM::current_pml4() {
    return arch::read_cr3();
}

// Weak default: single-CPU runs need no cross-CPU invalidation.  SMP
// ports override this symbol (tracked separately from #386).
__attribute__((weak)) void VMM::smp_tlb_shootdown_hook(uint64_t va,
                                                       uint16_t context_id,
                                                       bool all) {
    (void)va;
    (void)context_id;
    (void)all;
}

void VMM::tlb_shootdown(uint64_t va, uint16_t context_id, bool all) {
    if (all) {
        arch::ArchPageTable::tlb_flush_all();
    } else {
        arch::ArchPageTable::tlb_flush_va_asid(va, context_id);
    }
    smp_tlb_shootdown_hook(va, context_id, all);
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
/// @brief Map a page into a specific (non-kernel) PML4 page table.
/// @param virt_addr Page-aligned virtual address.
/// @param phys_addr Page-aligned physical address.
/// @brief Maps a page into a specific page table with explicit W^X
///        permission (legacy void wrapper: behavior-identical to the
///        pre-#325 revision — walk allocation failures stay silent
///        drops with a one-time log; fallible callers use
///        map_page_in_pml4_try / map_page_in_pml4_err).
void VMM::map_page_in_pml4(uint64_t virt_addr, uint64_t phys_addr, bool user,
                           bool executable, uint64_t pml4_phys) {
    (void)map_page_in_pml4_try(virt_addr, phys_addr, user, executable,
                               pml4_phys);
}


} // namespace kernel

// --- Error-returning overloads ---
namespace kernel {

using namespace errors;

/// @brief Initialise VMM with error-code return.
/// @return VMM_ERR_OK.
VmmError VMM::init_err() {
    init();
    return VMM_ERR_OK;
}

/// @brief Map a page in the kernel table with alignment validation and
/// error-code return.
/// @param virt_addr Page-aligned virtual address.
/// @param phys_addr Page-aligned physical address.
/// @param user      If true, mark as user-accessible.
/// @return VmmError code.
VmmError VMM::map_page_err(uint64_t virt_addr, uint64_t phys_addr, bool user) {
    if ((virt_addr & kPageOffsetMask) != 0 || (phys_addr & kPageOffsetMask) != 0) {
        return VMM_ERR_INVALID_ADDR;
    }
    return map_page_try(virt_addr, phys_addr, user);
}

/// @brief Unmap a page with alignment validation and error-code return.
/// @param virt_addr Virtual address to unmap.
/// @return VmmError code.
VmmError VMM::unmap_page_err(uint64_t virt_addr) {
    if ((virt_addr & kPageOffsetMask) != 0) {
        return VMM_ERR_INVALID_ADDR;
    }
    // Presence probe first (issue #366 Q2b): pte_leaf excludes missing
    // tables, clear leaves, and huge/block entries — all NOT_MAPPED and
    // never cleared.  The void unmap_page below owns the test-touch
    // warn/arm side effects, which only fire for actually-present leaves.
    if (pte_leaf_in_pml4(virt_addr, kernel_pml4_) == nullptr) {
        return VMM_ERR_NOT_MAPPED;
    }
    unmap_page(virt_addr);
    return VMM_ERR_OK;
}

/// @brief Translate virtual to physical via kernel PML4 with error-code return.
/// @param virt_addr Virtual address.
/// @param[out] out_phys_addr Physical address on success.
/// @return VmmError code.
VmmError VMM::virt_to_phys_err(uint64_t virt_addr, uint64_t &out_phys_addr) {
    if ((virt_addr & kPageOffsetMask) != 0) {
        return VMM_ERR_INVALID_ADDR;
    }
    out_phys_addr = virt_to_phys(virt_addr);
    return out_phys_addr != 0 ? VMM_ERR_OK : VMM_ERR_NOT_MAPPED;
}

/// @brief Clone kernel PML4 with error-code return.
/// @param[out] out_pml4_phys Physical address of new PML4 on success.
/// @return VmmError code.
VmmError VMM::clone_kernel_pml4_err(uint64_t &out_pml4_phys) {
    uint64_t phys = clone_kernel_pml4();
    if (!phys) {
        return VMM_ERR_PML4_ALLOC;
    }
    out_pml4_phys = phys;
    return VMM_ERR_OK;
}

/// @brief Map page into a specific PML4 with alignment validation and
/// error-code return.
/// @param virt_addr Page-aligned virtual address.
/// @param phys_addr Page-aligned physical address.
/// @param user      If true, mark as user-accessible.
/// @param pml4_phys Physical address of the target PML4.
/// @return VmmError code.
VmmError VMM::map_page_in_pml4_err(uint64_t virt_addr, uint64_t phys_addr,
                                    bool user, uint64_t pml4_phys,
                                    bool executable) {
    if ((virt_addr & kPageOffsetMask) != 0 || (phys_addr & kPageOffsetMask) != 0) {
        return VMM_ERR_INVALID_ADDR;
    }
    return map_page_in_pml4_try(virt_addr, phys_addr, user, executable,
                                pml4_phys);
}

/// @brief Free all user pages from a PML4 with error-code return.
/// @param pml4_phys Physical address of the user PML4.
/// @return VmmError code.
VmmError VMM::free_user_pages_err(uint64_t pml4_phys) {
    free_user_pages(pml4_phys);
    return VMM_ERR_OK;
}

/// @brief Translate virtual to physical in a specific PML4 with error-code
/// return.
/// @param virt_addr Virtual address.
/// @param pml4_phys Physical address of the target PML4.
/// @param[out] out_phys_addr Physical address on success.
/// @return VmmError code.
VmmError VMM::virt_to_phys_in_pml4_err(uint64_t virt_addr, uint64_t pml4_phys,
                                       uint64_t &out_phys_addr) {
    if ((virt_addr & kPageOffsetMask) != 0) {
        return VMM_ERR_INVALID_ADDR;
    }
    out_phys_addr = virt_to_phys_in_pml4(virt_addr, pml4_phys);
    return out_phys_addr != 0 ? VMM_ERR_OK : VMM_ERR_NOT_MAPPED;
}


} // namespace kernel
