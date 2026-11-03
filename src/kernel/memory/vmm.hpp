#pragma once

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

/// @file vmm.hpp
/// @brief Virtual Memory Manager — cross-arch page table management with
///        bounded page-walk depth (4-level x86_64/aarch64, 3-level riscv64
///        Sv39).

#include <types.hpp>
#include <constants.hpp>
#include <kernel/memory/vmm_errors.hpp>

namespace kernel {

namespace cap {
class FrameCap;
class MmioCap;
}

/// @brief Virtual memory manager — maps virtual to physical pages.
/// @note Naming (issue #363): `pml4` names the page-table ROOT cross-arch
///       (x86_64 PML4, aarch64 L0, riscv64 Sv39 L0) — not the Intel format.
/// @note Page-walk depth is fixed per architecture (4 levels for
/// x86_64/aarch64,
///       3 levels for riscv64 Sv39).  Huge-page split (512 iterations) occurs
///       at most once per 2 MB region.  Both walks are bounded, meeting hard-RT
///       WCET requirements for map_page / map_page_in_pml4.
/// @note Supports user/kernel page flags, TLB flush after each mapping.
class VMM {
  public:
    /// @brief Initialises the VMM and sets up the kernel page tables.
    /// @note Concurrency/caller contract (issue #366): Lock: none.
    ///       Context: boot-only (kernel.cpp bring-up, pre-scheduler).
    ///       SMP: boot-single-CPU.
    static void init();
    /// @brief Initialises the VMM with error code.
    /// @return VmmError code (always VMM_ERR_OK: init only zeroes
    ///         boot-constructed tables and allocates nothing).
    /// @note Concurrency/caller contract (issue #366): Lock: none.
    ///       Context: boot-only (same call window as init).
    ///       SMP: boot-single-CPU.
    [[nodiscard]] static errors::VmmError init_err();

    /// @brief Maps a virtual page to a physical page in the kernel page table.
    /// @param virt_addr Virtual address (page-aligned).
    /// @param phys_addr Physical address (page-aligned).
    /// @param user      If true, sets the user-accessible flag.
    /// @note WCET is bounded: fixed-depth page walk + at most one huge-page
    ///       split (512 iterations) when the target PD entry is a 2 MB block.
    /// @note Concurrency/caller contract (issue #366): Lock: none
    ///       (VMM-internal; PMM allocs self-serialize via pmm_lock_).
    ///       Context: kernel-only. SMP: user address spaces are CPU0-pinned
    ///       (scheduler.md S3.3); callers needing IRQ-atomicity hold
    ///       IrqGuard themselves (the pager fault path does).
    static void map_page(uint64_t virt_addr, uint64_t phys_addr,
                         bool user = false);
    /// @brief Maps a virtual page to a physical page with error code.
    /// @param virt_addr Virtual address (page-aligned).
    /// @param phys_addr Physical address (page-aligned).
    /// @param user      If true, sets the user-accessible flag.
    /// @return VmmError code.
    /// @note Concurrency/caller contract (issue #366): same as map_page
    ///       (Lock: none; Context: kernel-only; SMP: CPU0-pinned user
    ///       spaces, caller-held IrqGuard for IRQ-atomicity).
    [[nodiscard]] static errors::VmmError map_page_err(uint64_t virt_addr, uint64_t phys_addr,
                                         bool user = false);
    /// @brief Fallible kernel-table map worker (issue #325-B): the
    ///        map_page body with walk allocation failures surfaced
    ///        instead of silently dropped (same contract shape as
    ///        map_page_in_pml4_try).
    /// @return VMM_ERR_OK, or VMM_ERR_PAGE_ALLOC when a table page or
    ///         huge-page split allocation fails mid-walk.
    /// @note Concurrency/caller contract (issue #366): same as map_page
    ///       (Lock: none; Context: kernel-only; SMP: CPU0-pinned, caller
    ///       IrqGuard for IRQ-atomicity).
    [[nodiscard]] static errors::VmmError map_page_try(uint64_t virt_addr, uint64_t phys_addr,
                                         bool user = false);

    /// @brief Unmaps a virtual page.
    /// @param virt_addr Virtual address to unmap.
    /// @note Concurrency/caller contract (issue #366): Lock: none
    ///       (VMM-internal; walk-only, allocates nothing). Context:
    ///       kernel-only. SMP: as map_page (CPU0-pinned, caller IrqGuard
    ///       for IRQ-atomicity).
    static void unmap_page(uint64_t virt_addr);
    /// @brief Unmaps a virtual page with error code.
    /// @param virt_addr Virtual address to unmap.
    /// @return VmmError code: VMM_ERR_INVALID_ADDR on misalignment,
    ///         VMM_ERR_NOT_MAPPED when no present leaf exists for the VA
    ///         (missing table, clear leaf, or huge/block entry — never
    ///         cleared), VMM_ERR_OK when a leaf was cleared.
    /// @note Concurrency/caller contract (issue #366): same as unmap_page
    ///       (Lock: none; Context: kernel-only; SMP: CPU0-pinned).
    [[nodiscard]] static errors::VmmError unmap_page_err(uint64_t virt_addr);

    /// @brief Translates a virtual address to its physical address.
    /// @param virt_addr Virtual address.
    /// @return Physical address, or 0 if not mapped.
    /// @note Concurrency/caller contract (issue #366): Lock: none.
    ///       Context: any-task (walk-only, no store). SMP: N/A
    ///       (read-only walk).
    static uint64_t virt_to_phys(uint64_t virt_addr);
    /// @brief Translates a virtual address to its physical address with error
    /// code.
    /// @param virt_addr Virtual address.
    /// @param[out] out_phys_addr Physical address on success.
    /// @return VmmError code.
    /// @note Concurrency/caller contract (issue #366): same as
    ///       virt_to_phys (Lock: none; Context: any-task; SMP: N/A).
    [[nodiscard]] static errors::VmmError virt_to_phys_err(uint64_t virt_addr,
                                              uint64_t &out_phys_addr);

    /// @brief Returns the physical address of the current PML4 table.
    /// @return Physical address of PML4.
    /// @note Concurrency/caller contract (issue #366): Lock: none.
    ///       Context: any-task (register read). SMP: N/A.
    static uint64_t current_pml4();

    /// @brief Cross-CPU shootdown hook, overridden by SMP ports (issue
    ///        #352).  Weak no-op default: single-CPU runs need only the
    ///        local invalidate in tlb_shootdown.  SMP enablement replaces
    ///        this (tracked separately — not #386, which owns globals).
    /// @note Concurrency/caller contract: Lock: none.  Context: any-task.
    ///       SMP: N/A (default body).
    static void smp_tlb_shootdown_hook(uint64_t va, uint16_t context_id,
                                       bool all);

    /// @brief Invalidate TLB state for an address space (issue #352):
    ///        local arch invalidate (full flush for all=true, else
    ///        VA+context-ID targeted) followed by the SMP hook above.
    ///        Test-facing entry point for shootdown-hook invocation
    ///        coverage.  No callers yet — unmap/teardown paths keep their
    ///        existing local invalidates until QE adopts this choke point.
    /// @note Concurrency/caller contract (issue #366): Lock: none.
    ///       Context: any-task. SMP: local-only + hook.
    static void tlb_shootdown(uint64_t va, uint16_t context_id, bool all);

    /// @brief Creates a fresh PML4: zeroes user entries (0-255),
    /// copies kernel entries (256-511). Used during exec/load —
    /// NOT for fork (which needs parent user entries).
    /// @return Physical address of the new PML4, or 0 on failure.
    /// @note Concurrency/caller contract (issue #366): Lock: none
    ///       (VMM-internal; TaskControlBlock::clone runs without IrqGuard
    ///       here). Context: kernel-only (exec/spawn/fork paths).
    ///       SMP: child tables are not live on any CPU (pre-CR3/publish
    ///       window) — safety rests on non-publication, not locking.
    static uint64_t clone_kernel_pml4();
    /// @brief Creates a fresh PML4 with error code.
    /// @param[out] out_pml4_phys Physical address of the new PML4.
    /// @return VmmError code.
    /// @note Concurrency/caller contract (issue #366): same as
    ///       clone_kernel_pml4 (Lock: none; Context: kernel-only;
    ///       SMP: pre-publish window).
    [[nodiscard]] static errors::VmmError clone_kernel_pml4_err(uint64_t &out_pml4_phys);

    /// @brief Converge a child PML4's kernel half (entries 256..511)
    ///        toward @p src_pml4 (issue #96, kernel-half merge).  Recursive
    ///        top-down walk (PML4→PDPT→PD→PT): wherever the child lacks an
    ///        entry present in the source, the entry is copied by value
    ///        (link semantics — lower tables stay shared, never
    ///        duplicated); entries already present are left untouched
    ///        (idempotent); huge/leaf entries are copied by value.
    ///        Performs ZERO allocations, so it cannot OOM by
    ///        construction; the bool preserves the failure contract for
    ///        future allocating revisions (callers must destroy the child
    ///        table on false).  No locking: single-threaded, runs before
    ///        the first CR3 switch to the child (same window MP-7 uses).
    /// @note Shared-table contract (issue #325-F): shared lower tables
    ///       are KERNEL-owned (allocated without user_alloc), so child
    ///       teardown (free_user_pages) always skips them — a child can
    ///       never free a table the live kernel half still references.
    ///       Late kernel mappings and huge-page splits rewrite shared
    ///       entries in place and are visible to all sharers; splits
    ///       allocate fresh tables, never mutate shared ones in a way
    ///       that orphans sharers.
    /// @note No copy-on-write exists in this kernel (answer to "when
    ///       does COW take place": never): fork deep-copies user pages
    ///       (deep_copy_user_pages) and shares only kernel-half tables,
    ///       which are never written through user paths.
    /// @note TLB locality contract (issue #325-F): every VMM map/unmap
    ///       site flushes the VA locally (issue #225 discipline).
    ///       Local-only sufficiency rests on: (a) user tasks execute on
    ///       CPU0 exclusively (scheduler CPU0 pin, #25 C1), so user TLB
    ///       entries live on one CPU; (b) CR3 switches flush untagged
    ///       entries (exec path) and pcid_free purges on PCID release;
    ///       (c) freed tables are never reactivated (teardown destroys
    ///       the PML4; exec switches before freeing).  Cross-CPU
    ///       invalidation beyond this is owned by the selective/lazy
    ///       shootdown mechanisms (#157/#158).
    /// @note Free-vs-use ordering (issue #325-F): page-table teardown
    ///       runs only for dead tasks (cleanup/reap paths) which cannot
    ///       run on another CPU (CPU0 pin + dead-state filter), after
    ///       the pager/cap/ELF drains (see TaskControlBlock::cleanup
    ///       ordering) — a live mapping is never freed under a user.
    /// @param src_pml4 Physical address of the source (canonical) PML4.
    /// @param dst_pml4 Physical address of the child PML4 to converge.
    /// @return true on success, false on invalid arguments (null or
    ///         identical roots — never on allocation failure).
    /// @note Concurrency/caller contract (issue #366): Lock: none.
    ///       Context: kernel-only. SMP: single-threaded pre-CR3-switch
    ///       window (same window MP-7 uses) — see the No-locking note
    ///       above; the canonical example for this triple.
    [[nodiscard]] static bool merge_kernel_half(uint64_t src_pml4, uint64_t dst_pml4);
    /// @brief Snapshot the live kernel PML4's kernel half (entries
    ///        256..511) into the bring-up template (issue #96).  Call
    ///        ONCE at end of bring-up, before the first task spawns.
    /// @note Concurrency/caller contract (issue #366): Lock: none.
    ///       Context: boot-only (single call). SMP: boot-single-CPU.
    static void snapshot_kernel_template();
    /// @brief Harden boot-established kernel-half permissions to W^X
    ///        (issue #376 epic remainder): text RX, rodata RO+NX, data/bss
    ///        RW+NX over the linker section ranges. Splits boot huge/block
    ///        entries to 4K as needed (split fills preserve flags, see the
    ///        arch notes), then applies per-page bits. TLB flushed per
    ///        page as each leaf reaches its final state, plus one full
    ///        flush at the end. Call ONCE at end of bring-up after PMM/VMM
    ///        init and
    ///        BEFORE snapshot_kernel_template() so the template and every
    ///        later clone_kernel_pml4 inherit hardened flags by value.
    ///        Panics on any mapping failure (a half-hardened root must
    ///        never boot). Single-threaded BSP, IRQs off; no locks taken.
    /// @note Concurrency/caller contract (issue #366): Lock: none.
    ///       Context: boot-only (single call). SMP: boot-single-CPU.
    static void harden_kernel_half();
    /// @brief True when @p a_pml4 and @p b_pml4 agree on every kernel-half
    ///        entry (256..511), compared recursively through shared
    ///        tables (present+equal u64, or both-absent, at each level).
    /// @note Concurrency/caller contract (issue #366): Lock: none.
    ///       Context: any-task (diagnostics + tests; pure compare).
    ///       SMP: N/A.
    [[nodiscard]] static bool kernel_half_equal(uint64_t a_pml4, uint64_t b_pml4);
    /// @brief True when @p pml4_phys matches the bring-up template on
    ///        every kernel-half entry (diagnostics + tests; drift from
    ///        legitimate late mappings is NOT a failure here).
    /// @note Concurrency/caller contract (issue #366): Lock: none.
    ///       Context: any-task (diagnostics + tests). SMP: N/A.
    [[nodiscard]] static bool kernel_half_matches_template(uint64_t pml4_phys);
    /// @brief Halt unless @p child_pml4 converged to the LIVE kernel
    ///        PML4's kernel half (issue #96, structural audit).  Panics
    ///        (debug AND release) on mismatch; template drift is
    ///        debug-logged only — legitimate late mappings must converge,
    ///        not panic (paper §4 row 2).
    /// @note Concurrency/caller contract (issue #366): Lock: none.
    ///       Context: kernel-only (exec/load paths + tests).
    ///       SMP: pre-CR3/publish window (panics both builds on mismatch).
    static void assert_kernel_half_converged(uint64_t child_pml4);

    /// @brief Maps a page into a specific page table with explicit W^X
    ///        permission (VULN-H1).  When @p executable is false the
    ///        architectural NX bit is set (x86_64 bit 63, aarch64 bit 54)
    ///        or the X bit is dropped (riscv64), so the page cannot execute.
    /// @param virt_addr Virtual address (page-aligned).
    /// @param phys_addr Physical address (page-aligned).
    /// @param user      If true, sets user-accessible flag.
    /// @param executable If true, page is executable (NX clear); if false, NX
    /// set.
    /// @param pml4_phys Physical address of the target PML4.
    /// @note Concurrency/caller contract (issue #366): Lock: none
    ///       (VMM-internal). Context: kernel-only. SMP: user PML4s are
    ///       CPU0-pinned; the pager fault path holds IrqGuard across its
    ///       map/unmap sequence (pager_registry.cpp) — other callers
    ///       (buffer/task/loader) map private or current-task tables.
    static void map_page_in_pml4(uint64_t virt_addr, uint64_t phys_addr,
                                 bool user, bool executable,
                                 uint64_t pml4_phys);
    /// @brief Maps a user page read-only with explicit executability
    ///        (issue #271 P0 — retained-golden primitive). The page is
    ///        first mapped through the standard path (identical table
    ///        allocation), then the write permission is cleared with
    ///        the arch RO encoding (x86_64 RW-clear; aarch64 AP_USER |
    ///        AP_RO = EL0 read-only; riscv64 W-clear); NX/UXN follows
    ///        @p executable like the 5-arg overload. First run and
    ///        reruns map through THIS function so perms are identical.
    /// @param virt_addr Virtual address (page-aligned).
    /// @param phys_addr Physical address (page-aligned).
    /// @param executable If true, page is executable (NX clear).
    /// @param pml4_phys Physical address of the target PML4.
    /// @note Concurrency/caller contract (issue #366): Lock: none.
    ///       Context: kernel-only (loader/golden paths). SMP: pre-CR3
    ///       window / CPU0-pinned (same as map_page_in_pml4).
    static void map_page_ro_in_pml4(uint64_t virt_addr, uint64_t phys_addr,
                                    bool executable, uint64_t pml4_phys);
    /// @brief Test inspector: reports whether the mapped leaf is
    ///        read-only per the RO encoding (walk-only, never allocates).
    ///        @p executable_out is written ONLY when the leaf is RO
    ///        (callers must not read it on a false return).
    /// @return false when unmapped (or a huge/block entry, not ours).
    /// @note Concurrency/caller contract (issue #366): Lock: none.
    ///       Context: any-task (test inspector, walk-only, never
    ///       allocates). SMP: N/A.
    [[nodiscard]] static bool pte_is_ro_in_pml4(uint64_t virt_addr, uint64_t pml4_phys,
                                   bool *executable_out);
    /// @brief Test inspector: reports whether the mapped leaf is
    ///        executable per the arch NX encoding (walk-only, never
    ///        allocates; issue #325-D NX pinning).  x86_64: NX bit 63
    ///        clear; aarch64: UXN (bit 54) clear; riscv64: X bit set.
    /// @return false when unmapped (or a huge/block entry, not ours).
    /// @note Concurrency/caller contract (issue #366): Lock: none.
    ///       Context: any-task (test inspector, walk-only). SMP: N/A.
    [[nodiscard]] static bool pte_is_executable_in_pml4(uint64_t virt_addr,
                                          uint64_t pml4_phys);
#if defined(CONFIG_ARCH_RISCV64)
    /// @brief Sv39 PTE address codecs (issue #206: PTEs carry PPN<<10, so
    ///        raw masking is x86-domain-wrong).  Public for the
    ///        compile-time roundtrip pin below; all production users go
    ///        through the walk helpers.
    static constexpr uint64_t sv39_pte_phys(uint64_t pte) {
        return ((pte >> 10) << 12) & 0xFFFFFFFFFFF000ULL;
    }
    static constexpr uint64_t sv39_phys_pte(uint64_t phys) {
        return (phys >> 12) << 10;
    }
    static constexpr bool sv39_is_leaf(uint64_t pte) {
        return (pte & 0xEU) != 0;  // R|W|X set = leaf (table: V only)
    }
#endif
    /// @note Concurrency/caller contract (issue #366) for the Sv39
    ///       codecs above: Lock: none. Context: any (pure functions).
    ///       SMP: N/A.
    /// @brief Remap every mapped page in [vaddr, vaddr+size) read-only
    ///        through the RO primitive (issue #271 P0 — load paths and
    ///        the P2 rerun builder share this). Unmapped holes are
    ///        skipped; bounds are page-aligned outward with overflow
    ///        guard (empty range → no-op).
    /// @note Concurrency/caller contract (issue #366): Lock: none.
    ///       Context: kernel-only (loader/P2 rerun paths). SMP: pre-CR3
    ///       window / CPU0-pinned.
    static void remap_range_ro_in_pml4(uint64_t pml4_phys, uint64_t vaddr,
                                       uint64_t size, bool executable);
    /// @brief Maps a page into a specific page table with error code.
    /// @param virt_addr Virtual address (page-aligned).
    /// @param phys_addr Physical address (page-aligned).
    /// @param user      If true, sets user-accessible flag.
    /// @param pml4_phys Physical address of the target PML4.
    /// @param executable If true, page is executable (default preserves the
    ///        legacy 4-arg mapping behaviour; pass false for W^X data).
    /// @return VmmError code (walk allocation failures propagate as
    ///         VMM_ERR_PAGE_ALLOC).
    /// @note Concurrency/caller contract (issue #366): same as
    ///       map_page_in_pml4 (Lock: none; Context: kernel-only;
    ///       SMP: caller-held IrqGuard on the pager fault path).
    [[nodiscard]] static errors::VmmError map_page_in_pml4_err(uint64_t virt_addr,
                                                 uint64_t phys_addr, bool user,
                                                 uint64_t pml4_phys,
                                                 bool executable = true);

    /// @brief Return the physical address of the kernel PML4 table.
    /// @return Physical address of the kernel page-table root.
    /// @note Concurrency/caller contract (issue #366): Lock: none.
    ///       Context: any-task (plain load). SMP: N/A.
    static uint64_t get_kernel_pml4() {
        return kernel_pml4_;
    }

    /// @brief Frees all user-space pages and page tables from a
    /// user PML4. Used during exec() to clean up the old address
    /// space. Skips kernel-owned pages (PMM::is_user_page owner-bit
    /// check) — v0.4.0 MP-7: no task shares another's user entries, so
    /// this is always the full teardown of a private address space.
    /// Defensive block sites (1 GiB / 2 MiB) validate level alignment
    /// via decode_block_base and skip misaligned entries, never freeing
    /// them: no in-tree producer creates user huge/block leaves (#365).
    /// @param pml4_phys Physical address of the user PML4.
    /// @brief Deep-copy user-space page tables from src_pml4 to dst_pml4.
    ///        Allocates new PDPT/PD/PT pages and copies data page content.
    /// @param src_pml4 Physical address of source PML4.
    /// @param dst_pml4 Physical address of destination PML4 (must already
    ///        have kernel entries populated).
    /// @return true on success, false on OOM or on a user-OWNED huge/block
    ///         entry (issue #325-E: by-value copy would alias one writable
    ///         page parent/child — fail closed instead; issue #387:
    ///         kernel-owned blocks reachable in the user index range —
    ///         riscv64 L0[0] low-half MMIO/identity from clone_kernel_pml4 —
    ///         are inherited BY VALUE, never a fork failure, U=0 kept).
    ///         Partial state may remain on false, but every installed TABLE
    ///         page is USER-owned and every inherited BLOCK is kernel-owned
    ///         U=0 (skipped by every free_user_pages block site on all
    ///         arches), so the caller MUST destroy the child
    ///         (TaskControlBlock::destroy → cleanup → free_user_pages
    ///         reclaims all partials — task.cpp documents this invariant).
    ///         Leaf flags (USER/write/dirty/accessed/NX) copy verbatim;
    ///         non-RAM frames are skipped (child leaf stays unmapped).
    /// @note Concurrency/caller contract (issue #366): Lock: none
    ///       (VMM-internal; TaskControlBlock::clone runs without IrqGuard
    ///       here). Context: kernel-only (fork path). SMP: source and
    ///       destination are not live on any CPU (pre-publish window);
    ///       the caller MUST destroy the child on false (see above).
    static bool deep_copy_user_pages(uint64_t src_pml4, uint64_t dst_pml4);
    /// @note Concurrency/caller contract (issue #366): Lock: none
    ///       (VMM-internal). Context: kernel-only, dead-task teardown
    ///       (cleanup/reap/ELF error paths). SMP: CPU0-pinned + dead-state
    ///       filter — freed tables are never reactivated (see the
    ///       free-vs-use note on merge_kernel_half).
    static void free_user_pages(uint64_t pml4_phys);
    /// @brief Frees all user-space pages and page tables with error code.
    /// @param pml4_phys Physical address of the user PML4.
    /// @return VmmError code (always VMM_ERR_OK: teardown walks with
    ///         create=false and frees; it cannot fail).
    /// @note Concurrency/caller contract (issue #366): same as
    ///       free_user_pages (Lock: none; Context: kernel-only dead-task
    ///       teardown; SMP: CPU0-pinned + dead-state filter).
    [[nodiscard]] static errors::VmmError free_user_pages_err(uint64_t pml4_phys);

    /// @brief Maps the frame(s) owned by a FrameCap into @p pml4_phys.
    ///        Refuses a revoked FrameCap (ROADMAP 0.4.1 CSpace) — mapping
    ///        capability-gated memory requires a live, non-revoked cap.
    /// @param fc        The FrameCap (must be pinned by the caller).
    /// @param virt_addr Base virtual address (page-aligned).
    /// @param user      If true, sets user-accessible flag.
    /// @param pml4_phys Physical address of the target PML4.
    /// @return true when every frame was mapped, false on a revoked cap or
    ///         a zero frame.
    /// @note Mapped non-executable (issue #325-D NX policy).
    /// @note Concurrency/caller contract (issue #366): Lock: none
    ///       (VMM-internal; the cap pin is the caller's). Context:
    ///       kernel-only. SMP: CPU0-pinned user targets; the _err twin
    ///       rolls back on mid-range failure (target must be unmapped).
    [[nodiscard]] static bool map_frame_from_cap(class cap::FrameCap *fc, uint64_t virt_addr,
                                   bool user, uint64_t pml4_phys);
    /// @brief Maps the frame(s) owned by a FrameCap with error code and
    ///        transactional rollback (issue #325-A): when a page fails
    ///        mid-range, the already-mapped prefix is unmapped again, so
    ///        the caller observes all-or-nothing.
    /// @warning Requires an unmapped target range: rollback clears every
    ///        leaf it installed and cannot distinguish a pre-existing
    ///        live mapping (overlapping ranges are a caller bug).
    /// @return VMM_ERR_OK when every frame was mapped; VMM_ERR_INVALID_ADDR
    ///         on a null cap, misaligned base, wrap-around range, or IO-bar
    ///         misuse; VMM_ERR_REVOKED on a revoked cap;
    ///         VMM_ERR_EMPTY_RANGE on a zero frame; VMM_ERR_PAGE_ALLOC when
    ///         a table page allocation fails mid-walk (prefix rolled back).
    /// @note NX policy (issue #325-D): cap-mapped memory defaults
    ///       non-executable (no production caller executes from it);
    ///       pass executable=true only for code.
    /// @note Concurrency/caller contract (issue #366): same as
    ///       map_frame_from_cap (Lock: none; Context: kernel-only;
    ///       SMP: CPU0-pinned, unmapped-target precondition).
    [[nodiscard]] static errors::VmmError map_frame_from_cap_err(class cap::FrameCap *fc,
                                                   uint64_t virt_addr,
                                                   bool user,
                                                   uint64_t pml4_phys,
                                                   bool executable = false);

    /// @brief Unmaps one page previously mapped via map_frame_from_cap().
    /// @param virt_addr Virtual address to unmap.
    /// @param pml4_phys Physical address of the target PML4.
    /// @note Naming convention (issue #325-C): every `unmap_*` clears the
    ///       leaf (not-present) and flushes the VA.  Mapping with phys 0
    ///       is NEVER an unmap — it installs a PRESENT supervisor leaf to
    ///       physical page zero.
    /// @note Concurrency/caller contract (issue #366): Lock: none.
    ///       Context: kernel-only. SMP: CPU0-pinned (same as the other
    ///       unmaps).
    static void unmap_frame_from_cap(uint64_t virt_addr, uint64_t pml4_phys);
    /// @brief Unmaps one frame-mapped page with error code (issue #366 Q2b).
    ///        Revocation is NOT checked: unmap-after-revoke must work (the
    ///        revoke path itself unmaps revoked caps).
    /// @param fc        The FrameCap (must be non-null; pin not required
    ///        for the single-leaf walk, matching the void twin).
    /// @param virt_addr Page-aligned virtual address to unmap.
    /// @param pml4_phys Physical address of the target PML4.
    /// @return VMM_ERR_INVALID_ADDR on a null cap or misaligned base,
    ///         VMM_ERR_NOT_MAPPED when no present leaf exists,
    ///         VMM_ERR_OK when a leaf was cleared and flushed.
    /// @note Concurrency/caller contract (issue #366): same as
    ///       unmap_frame_from_cap (Lock: none; Context: kernel-only;
    ///       SMP: CPU0-pinned).
    [[nodiscard]] static errors::VmmError unmap_frame_from_cap_err(
        class cap::FrameCap *fc, uint64_t virt_addr, uint64_t pml4_phys);

    /// @brief Maps an MmioCap's memory BAR range into @p pml4_phys
    ///        (v0.4.2, issue #3).  Refuses a revoked cap and IO-type ranges.
    ///        Page-granular: the BAR tail is rounded up to a full page.
    /// @param mmio      The MmioCap (must be pinned by the caller).
    /// @param virt_addr Base virtual address (page-aligned).
    /// @param user      If true, sets user-accessible flag.
    /// @param pml4_phys Physical address of the target PML4.
    /// @return true when every page was mapped, false on a revoked/IO-type
    ///         cap or a zero range.
    /// @note Mapped non-executable (issue #325-D NX policy).
    /// @note Concurrency/caller contract (issue #366): Lock: none
    ///       (VMM-internal; the cap pin is the caller's). Context:
    ///       kernel-only. SMP: as map_frame_from_cap (CPU0-pinned,
    ///       unmapped-target precondition on the _err twin).
    [[nodiscard]] static bool map_mmio_from_cap(class cap::MmioCap *mmio, uint64_t virt_addr,
                                   bool user, uint64_t pml4_phys);
    /// @brief Maps an MmioCap's BAR range with error code and transactional
    ///        rollback (issue #325-A): same all-or-nothing contract as
    ///        map_frame_from_cap_err.
    /// @warning Requires an unmapped target range (same rollback caveat
    ///        as map_frame_from_cap_err).
    /// @return VMM_ERR_OK when every page was mapped; error codes as in
    ///         map_frame_from_cap_err (IO-type BAR → VMM_ERR_INVALID_ADDR).
    /// @note NX policy (issue #325-D): device memory is never executable.
    /// @note Concurrency/caller contract (issue #366): same as
    ///       map_frame_from_cap_err (Lock: none; Context: kernel-only;
    ///       SMP: CPU0-pinned, unmapped-target precondition).
    [[nodiscard]] static errors::VmmError map_mmio_from_cap_err(class cap::MmioCap *mmio,
                                                  uint64_t virt_addr,
                                                  bool user,
                                                  uint64_t pml4_phys,
                                                  bool executable = false);

    /// @brief Unmaps an MmioCap's memory BAR range previously mapped via
    ///        map_mmio_from_cap().
    /// @param mmio      The MmioCap (must be pinned by the caller).
    /// @param virt_addr Base virtual address the range was mapped at.
    /// @param pml4_phys Physical address of the target PML4.
    /// @note Same clear-leaf convention as unmap_frame_from_cap (never
    ///       map-with-zero).
    /// @note Concurrency/caller contract (issue #366): Lock: none.
    ///       Context: kernel-only. SMP: CPU0-pinned (same as
    ///       unmap_frame_from_cap).
    static void unmap_mmio_from_cap(class cap::MmioCap *mmio,
                                    uint64_t virt_addr, uint64_t pml4_phys);
    /// @brief Unmaps an MmioCap BAR range with error code (issue #366 Q2b).
    ///        Two-pass (verify every leaf present, then clear all) so a
    ///        hole reports NOT_MAPPED with nothing cleared — no rollback
    ///        needed and no partial unmap.  Revocation/IO-type are NOT
    ///        checked (see unmap_frame_from_cap_err).
    /// @param mmio      The MmioCap (must be non-null, non-zero size).
    /// @param virt_addr Base virtual address the range was mapped at.
    /// @param pml4_phys Physical address of the target PML4.
    /// @return VMM_ERR_INVALID_ADDR on a null cap or misaligned base,
    ///         VMM_ERR_EMPTY_RANGE on a zero-size cap,
    ///         VMM_ERR_NOT_MAPPED when any page in the range has no
    ///         present leaf, VMM_ERR_OK when every leaf was cleared.
    /// @note Concurrency/caller contract (issue #366): same as
    ///       unmap_mmio_from_cap (Lock: none; Context: kernel-only;
    ///       SMP: CPU0-pinned).
    [[nodiscard]] static errors::VmmError unmap_mmio_from_cap_err(
        class cap::MmioCap *mmio, uint64_t virt_addr, uint64_t pml4_phys);

    /// @brief Unmaps one user page previously mapped into @p pml4_phys
    ///        (issue #95: shared-library RO pages must be unmapped —
    ///        never freed — by each task teardown; the cache owns shared
    ///        phys lifetime by refcount).  Clears the PT leaf and flushes
    ///        the VA; huge entries and absent tables are left alone.
    /// @param virt_addr Page-aligned virtual address to unmap.
    /// @param pml4_phys Physical address of the target PML4.
    /// @note Concurrency/caller contract (issue #366): Lock: none.
    ///       Context: kernel-only (shared-library RO unmap + teardown).
    ///       SMP: CPU0-pinned (same as the other unmaps).
    static void unmap_page_in_pml4(uint64_t virt_addr, uint64_t pml4_phys);
    /// @brief Unmaps one user page with error code (issue #366 Q2b).
    /// @param virt_addr Page-aligned virtual address to unmap.
    /// @param pml4_phys Physical address of the target PML4.
    /// @return VMM_ERR_INVALID_ADDR on misalignment, VMM_ERR_NOT_MAPPED
    ///         when no present leaf exists (missing table, clear leaf,
    ///         or huge/block entry — never cleared), VMM_ERR_OK when a
    ///         leaf was cleared and flushed.
    /// @note Concurrency/caller contract (issue #366): same as
    ///       unmap_page_in_pml4 (Lock: none; Context: kernel-only;
    ///       SMP: CPU0-pinned).
    [[nodiscard]] static errors::VmmError unmap_page_in_pml4_err(uint64_t virt_addr,
                                                   uint64_t pml4_phys);

    /// @brief Translates a virtual address to a physical address
    /// using a specific PML4.
    /// @param virt_addr Virtual address to translate.
    /// @param pml4_phys Physical address of the target PML4.
    /// @return Physical address, or 0 if not mapped.
    /// @note Concurrency/caller contract (issue #366): Lock: none.
    ///       Context: any-task (walk-only, no store). SMP: N/A.
    static uint64_t virt_to_phys_in_pml4(uint64_t virt_addr,
                                         uint64_t pml4_phys);
    /// @brief Translates a virtual address to a physical address using a
    /// specific PML4 with error code.
    /// @param virt_addr Virtual address to translate.
    /// @param pml4_phys Physical address of the target PML4.
    /// @param[out] out_phys_addr Physical address on success.
    /// @return VmmError code.
    /// @note Concurrency/caller contract (issue #366): same as
    ///       virt_to_phys_in_pml4 (Lock: none; Context: any-task;
    ///       SMP: N/A).
    [[nodiscard]] static errors::VmmError virt_to_phys_in_pml4_err(uint64_t virt_addr,
                                                      uint64_t pml4_phys,
                                                      uint64_t &out_phys_addr);
    /// @brief Strip arch upper bits from a LIVE root register value to a
    ///        table phys (issue #171 P6 aa64 fault): x86 CR3 carries PCID
    ///        in the low 12 (already stripped by the walk mask); aa64
    ///        TTBR0/1 carry ASID in bits [63:48] (NOT stripped by the
    ///        walk mask — passing it raw wraps the L0 base non-canonical
    ///        and aborts in the walk); riscv read_cr3() pre-strips SATP
    ///        to phys. Idempotent on clean phys roots (TCB fields).
    ///        One definition per arch TU (public: pure codec, no state).
    /// @param live_root Live arch root register value (CR3/TTBR/SATP-phys).
    /// @return Table physical address suitable as a walk root.
    /// @note Concurrency/caller contract: Lock: none; Context: any-task;
    ///       SMP: N/A (pure function of its argument).
    static uint64_t vmm_arch_root_phys(uint64_t live_root);

  private:
    static constexpr uint64_t PAGE_SIZE = CONFIG_PAGE_SIZE;
    static constexpr uint64_t PAGE_TABLE_ENTRIES = 512;
    /// @brief Clears the low 12 flag/offset bits of a page-TABLE address
    ///        (issue #348-C).  Table addresses only — never PTEs (riscv64
    ///        Sv39 PTEs carry PPN<<10 and need the shift codec; x86_64
    ///        PTEs may carry NX above the frame).
    static constexpr uint64_t PAGE_TABLE_ADDR_MASK = ~0xFFFULL;
    static_assert((PAGE_TABLE_ADDR_MASK | 0xFFFULL) == ~0ULL,
                  "addr mask must preserve all high bits");
    static_assert((0xFFFULL & PAGE_TABLE_ADDR_MASK) == 0,
                  "addr mask must clear the low 12 bits");

#if defined(CONFIG_ARCH_AARCH64)
    // aarch64 stage-1 page-table descriptor bit layout.
    // Table descriptors (L0-L2): bits[1:0]=11 (V=1, bit1=1 → next-level
    // table).  Block descriptors (L1-L2): bits[1:0]=01 (bit1=0).
    // Page descriptors (L3): bits[1:0]=11 — bit1=0 at L3 is a
    // block-at-invalid-level encoding and faults as a translation fault.
    // AP[1:0] in bits[7:6];  00=EL1-RW/EL0-*, 01=EL1-RW/EL0-RW,
    //   10=EL1-RO/EL0-*, 11=EL1-RO/EL0-RO.
    // AF (Access Flag) = bit 10 (stage-1); an AF=0 leaf faults on first
    // access.  Bits 9:8 = SH (shareability).
    static constexpr uint64_t PAGE_PRESENT = 1ULL << 0;
    static constexpr uint64_t PAGE_TABLE = 1ULL << 1;
    static constexpr uint64_t PAGE_AF = 1ULL << 10; // Access Flag (stage-1)
    static constexpr uint64_t PAGE_AP_USER = 1ULL
                                             << 6;    // AP[0] - EL0 accessible
    static constexpr uint64_t PAGE_AP_RO = 1ULL << 7; // AP[1] - read-only (EL1)
    // MP-4.4: PXN (bit 53) forbids EL1 execution; UXN (bit 54) forbids EL0
    // execution.  Kernel data pages carry both; user pages carry PXN
    // (SMEP parity) unless NX adds UXN.
    static constexpr uint64_t PAGE_PXN = 1ULL << 53;
    static constexpr uint64_t PAGE_UXN = 1ULL << 54;
    /// @brief MAIR attribute index (bits[4:2]) for Normal cacheable memory.
    /// boot.S programs MAIR: attr0=Device, attr1=Normal WB — leaves must
    /// select index 1 (Device memory is never executable).
    static constexpr uint64_t PAGE_ATTR_NORMAL = 1ULL << 2;

    // Compatibility aliases (used in flag-building expressions)
    static constexpr uint64_t PAGE_WRITE = 0; // no-op - writable by default
    static constexpr uint64_t PAGE_USER = PAGE_AP_USER;
    static constexpr uint64_t PAGE_HUGE = 0; // not a flag; detected via level
#elif defined(CONFIG_ARCH_RISCV64)
    // RISC-V Sv39 PTE format (from privilege spec)
    // bit 0: V (Valid)
    // bit 1: R (Readable)
    // bit 2: W (Writable)
    // bit 3: X (Executable)
    // bit 4: U (User)
    // bit 5: G (Global)
    // bit 6: A (Accessed)
    // bit 7: D (Dirty)
    // bit 8: RSW (reserved for software)
    // bits 9-53: PPN[0..44] (physical page number)
    // bits 54-63: reserved for future use
    static constexpr uint64_t PAGE_PRESENT = 1ULL << 0;  // V
    static constexpr uint64_t PAGE_READ = 1ULL << 1;     // R
    static constexpr uint64_t PAGE_WRITE = 1ULL << 2;    // W
    static constexpr uint64_t PAGE_EXEC = 1ULL << 3;     // X
    static constexpr uint64_t PAGE_USER = 1ULL << 4;     // U
    static constexpr uint64_t PAGE_GLOBAL = 1ULL << 5;   // G
    static constexpr uint64_t PAGE_ACCESSED = 1ULL << 6; // A
    static constexpr uint64_t PAGE_DIRTY = 1ULL << 7;    // D
    // Table entry: V=1, R=W=X=0 (points to next level)
    static constexpr uint64_t PAGE_TABLE =
        PAGE_PRESENT; // V=1, no RWX = table pointer
    // Block/leaf detection: if V=1 and (R|W|X)=1 then leaf page
    static constexpr uint64_t PAGE_HUGE = 0; // detected via R|W|X bits
#else
    static constexpr uint64_t PAGE_PRESENT = 1ULL << 0;
    static constexpr uint64_t PAGE_WRITE = 1ULL << 1;
    static constexpr uint64_t PAGE_USER = 1ULL << 2;
    static constexpr uint64_t PAGE_HUGE = 1ULL << 7;
    // NX lives in the x86_64 HAL (arch::ArchPageTable::NX, issue #363 P3):
    // it is an x86-PTE encoding bit, not VMM vocabulary.
#endif

#if defined(CONFIG_ARCH_AARCH64)
    // aarch64: physical frame bits [47:12] (48-bit PA).  Masks off flags,
    // UXN (bit 54) and PXN (bit 53).
    static constexpr uint64_t PAGE_FRAME_MASK = 0x0000FFFFFFFFF000ULL;
    static constexpr uint64_t PAGE_HUGE_FRAME_MASK = 0x0000FFFFFFE00000ULL;
#elif defined(CONFIG_ARCH_RISCV64)
    // Sv39: PPN bits [43:12] (34-bit PA).  Masks off flags + reserved bits.
    static constexpr uint64_t PAGE_FRAME_MASK = 0x00000003FFFFFFF000ULL;
    static constexpr uint64_t PAGE_HUGE_FRAME_MASK = 0x00000003FFFFE00000ULL;
    // Issue #206: the masks above are x86-domain (phys & mask) and WRONG for
    // Sv39 PTEs, which carry (PPN << 10).  Use the shift-based helpers in
    // the public section below for every Sv39 PTE address codec; the masks
    // stay for x86/aarch64 and for documentation of the PA width (1TB cap).
#else
    // x86_64: physical frame bits [51:12] (52-bit PA).  Masks off all flags
    // INCLUDING NX (bit 63) — without this, a leaf PTE with NX set yields a
    // "physical address" whose bit 63 wraps HHDM_OFFSET + phys to a
    // non-canonical address (VULN-H1 GPF).
    static constexpr uint64_t PAGE_FRAME_MASK = 0x000FFFFFFFFFF000ULL;
    static constexpr uint64_t PAGE_HUGE_FRAME_MASK = 0x000FFFFFFFE00000ULL;
#endif

  public:
    // Sv39 index geometry is public (issue #363 P3): shared TU code
    // (vmm_internal.hpp static_asserts, riscv walks) addresses tables
    // through these; the HAL keeps its own private copies.
#if defined(CONFIG_ARCH_RISCV64)
    // Sv39: 3-level page table
    // L0 (1GB): bits 38:30
    // L1 (2MB): bits 29:21
    // L2 (4KB): bits 20:12
    static constexpr uint64_t L0_SHIFT = 30;
    static constexpr uint64_t L1_SHIFT = 21;
    static constexpr uint64_t L2_SHIFT = 12;
    static constexpr uint64_t TABLE_MASK = 0x1FFULL;
    static constexpr uint64_t L0_MASK = TABLE_MASK << L0_SHIFT;
    static constexpr uint64_t L1_MASK = TABLE_MASK << L1_SHIFT;
    static constexpr uint64_t L2_MASK = TABLE_MASK << L2_SHIFT;
#endif

  private:
    static constinit uint64_t kernel_pml4_;

    /// @brief Bring-up snapshot of the kernel PML4's kernel half
    ///        (issue #96).  Written once by snapshot_kernel_template(),
    ///        never afterwards; read by kernel_half_matches_template().
    static constinit uint64_t pml4_kernel_template_[512];

  public:
    /// @brief Set to true when a test modifies kernel-space page tables
    ///        (HHDM range).  Reset after PD restore in snapshot_restore.
    ///        Plain bool with __atomic_* access: set-true is idempotent
    ///        (no lost update), and restore takes it with an exchange
    ///        (issue #60 VAR-17 SMP re-audit).
    static bool hhdm_modified_;

    /// @brief Set to true when a test modifies the LOW identity map
    ///        (PD_IDENTITY, phys 0x3000 — VAs in PML4[0] below
    ///        PML4_USER_COUNT).  Reset after PD restore in snapshot_restore.
    static bool identity_modified_;

    /// @brief Set the HHDM modification flag (issue #366: the single SET
    ///        path — release store, pairs with the acquire
    ///        `hhdm_was_modified` / acq_rel `take_hhdm_modified` reads;
    ///        callable only from the test-active arm path and the
    ///        flag-semantics test; set-true is idempotent, no gate inside).
    /// @note Concurrency/caller contract (issue #366): Lock: none
    ///       (lock-free atomics). Context: test-only. SMP:
    ///       test-serialized; release/acquire pairing is the contract.
    static void set_hhdm_modified() {
        __atomic_store_n(&hhdm_modified_, true, __ATOMIC_RELEASE);
    }
    /// @brief Returns true if any test has modified the HHDM page tables
    ///        since the last clear/take call (acquire: pairs with the
    ///        release store in map_page).
    /// @note Concurrency/caller contract (issue #366): Lock: none
    ///       (lock-free atomics). Context: test-only. SMP:
    ///       test-serialized.
    static bool hhdm_was_modified() {
        return __atomic_load_n(&hhdm_modified_, __ATOMIC_ACQUIRE);
    }
    /// @brief Reset the HHDM modification flag (called after PD restore).
    /// @note Concurrency/caller contract (issue #366): Lock: none
    ///       (lock-free atomics). Context: test-only (snapshot restore).
    ///       SMP: test-serialized.
    static void clear_hhdm_modified() {
        __atomic_store_n(&hhdm_modified_, false, __ATOMIC_RELEASE);
    }
    /// @brief Atomically take the HHDM flag: returns the previous value
    ///        and clears it in one step, so a set landing after the take
    ///        is preserved for the next restore (fail-safe over-restore
    ///        instead of fail-dangerous skip).  Used by snapshot_restore.
    /// @note Concurrency/caller contract (issue #366): Lock: none
    ///       (lock-free atomics). Context: test-only (snapshot restore).
    ///       SMP: test-serialized; acq_rel take is the restore gate.
    static bool take_hhdm_modified() {
        return __atomic_exchange_n(&hhdm_modified_, false,
                                   __ATOMIC_ACQ_REL);
    }

    /// @brief Set the identity modification flag (issue #366: same
    ///        single-SET-path contract as the HHDM setter above).
    /// @note Concurrency/caller contract (issue #366): Lock: none
    ///       (lock-free atomics). Context: test-only. SMP:
    ///       test-serialized; release/acquire pairing is the contract.
    static void set_identity_modified() {
        __atomic_store_n(&identity_modified_, true, __ATOMIC_RELEASE);
    }
    /// @brief Returns true if any test has modified the low identity page
    ///        tables since the last clear/take call.
    /// @note Concurrency/caller contract (issue #366): Lock: none
    ///       (lock-free atomics). Context: test-only. SMP:
    ///       test-serialized.
    static bool identity_was_modified() {
        return __atomic_load_n(&identity_modified_, __ATOMIC_ACQUIRE);
    }
    /// @brief Reset the identity modification flag (called after PD restore).
    /// @note Concurrency/caller contract (issue #366): Lock: none
    ///       (lock-free atomics). Context: test-only (snapshot restore).
    ///       SMP: test-serialized.
    static void clear_identity_modified() {
        __atomic_store_n(&identity_modified_, false, __ATOMIC_RELEASE);
    }
    /// @brief Atomically take the identity flag (same contract as the
    ///        HHDM take above).  Used by snapshot_restore.
    /// @note Concurrency/caller contract (issue #366): Lock: none
    ///       (lock-free atomics). Context: test-only (snapshot restore).
    ///       SMP: test-serialized; acq_rel take is the restore gate.
    static bool take_identity_modified() {
        return __atomic_exchange_n(&identity_modified_, false,
                                   __ATOMIC_ACQ_REL);
    }

  private:
    /// @brief Fallible page-map worker (issue #325-A): the 5-arg
    ///        map_page_in_pml4 body with walk allocation failures surfaced
    ///        instead of silently dropped.  Callers needing atomicity
    ///        (cap-map loops) roll back on non-OK.
    /// @return VMM_ERR_OK, or VMM_ERR_PAGE_ALLOC when a table page or
    ///         huge-page split allocation fails mid-walk (tables
    ///         allocated before the failure stay installed; the caller
    ///         owns rollback).
    [[nodiscard]] static errors::VmmError map_page_in_pml4_try(uint64_t virt_addr,
                                                 uint64_t phys_addr, bool user,
                                                 bool executable,
                                                 uint64_t pml4_phys);
  public:
    /// @brief Fallible contiguous-range mapper shared by the cap-map
    ///        transactions (issue #325-A): maps @p page_count pages from
    ///        @p phys_addr at @p virt_addr, rolling back the mapped
    ///        prefix when a page fails mid-range (all-or-nothing).
    /// @return VMM_ERR_OK on full success; VMM_ERR_INVALID_ADDR on a
    ///         misaligned base or wrap-around range (including
    ///         page_count == 0 — the public cap wrappers report a zero
    ///         range as VMM_ERR_EMPTY_RANGE instead); VMM_ERR_PAGE_ALLOC
    ///         on a mid-walk table allocation failure (prefix rolled
    ///         back before returning).
    /// @param executable If true, pages are executable (issue #325-D;
    ///        cap wrappers default false).
    /// @note  Public since issue #347 (Slice D): the XOR path-tracer
    ///        drives this primitive directly; it is a general fallible
    ///        range mapper with no hidden state (kernel pml4 or an
    ///        explicit @p pml4_phys).
    /// @note Concurrency/caller contract (issue #366): Lock: none
    ///       (VMM-internal). Context: kernel-only (cap-map transactions
    ///       + path tracer). SMP: inherits the caller's window
    ///       (CPU0-pinned user targets; caller-held IrqGuard on the
    ///       pager path).
    [[nodiscard]] static errors::VmmError map_phys_range_try(uint64_t virt_addr,
                                               uint64_t phys_addr,
                                               size_t page_count, bool user,
                                               uint64_t pml4_phys,
                                               bool executable);
#if defined(CONFIG_DEBUG)
    /// @brief Arms the test-only table-alloc fault injector (issue #361).
    ///        The n-th table allocation attempt of the next
    ///        map_phys_range_try() call fails instead of allocating.
    ///        1-based; 0 disarms.  Single-shot: firing disarms.
    ///        Test-only; absent in release.  Caller must hold IrqGuard.
    static void test_arm_table_alloc_fail_at(size_t alloc_index) noexcept;
    /// @brief Disarms the injector unconditionally (issue #361).  Test-only.
    static void test_disarm_table_alloc_fail() noexcept;
    /// @brief Clears one 4K leaf WITHOUT any TLB invalidation (issue #390).
    ///        Test-only single-source walk for the shootdown negative
    ///        control: walk-only via pte_leaf_in_pml4, verify the leaf
    ///        frame against @p expect_phys (fail-loud on walk drift),
    ///        then clear. Zero flush, zero hook, zero alloc, zero logging.
    /// @return False on any missing table, huge/block entry, or frame
    ///         mismatch. CONFIG_DEBUG only (release has no def).
    [[nodiscard]] static bool test_unmap_no_shootdown(
        uint64_t virt_addr, uint64_t pml4_phys, uint64_t expect_phys) noexcept;
#endif
    /// @brief Clears injector state for snapshot restore (issue #361).
    ///        Unconditional decl so test_isolate.cpp needs no #if;
    ///        release def is a no-op.
    static void test_table_alloc_fail_snapshot_reset() noexcept;
  private:
    /// @brief Walks or creates a page-table entry at the given level.
    /// @param table Pointer to the current-level page table.
    /// @param index Index into the table.
    /// @param create If true, allocates a new table if missing.
    /// @param user_alloc If true, allocate page table page as USER-owned.
    /// @return Pointer to the next-level table, or nullptr.
    static uint64_t *get_table(uint64_t *table, size_t index, bool create,
                               bool user_alloc = false);
#if defined(CONFIG_DEBUG)
    /// @brief Resets the per-call countdown at range-map entry (issue #361).
    ///        Called by map_phys_range_try(); not for direct test use.
    static void test_table_alloc_fail_begin_call() noexcept;
    /// @brief Returns true when the imminent alloc must fail (issue #361).
    ///        Call only where an alloc is about to happen.  Test-only.
    [[nodiscard]] static bool test_maybe_fail_table_alloc() noexcept;
#endif
    /// @brief Walk to the leaf PTE for a VA without allocating
    ///        (issue #271 P0 — shared by the RO primitive + inspector).
    /// @return Pointer to the leaf entry, or nullptr on any missing
    ///         table or huge/block entry (not ours).
    static uint64_t *pte_leaf_in_pml4(uint64_t virt_addr,
                                      uint64_t pml4_phys);
    /// @brief True when @p entry points at a next-level table (issue
    ///        #96).  Arch-aware leaf detection (x86_64 PS bit, RISC-V
    ///        R|W|X, AArch64 descriptor type); level 3 is always a leaf.
    ///        Delegates the arch arm to vmm_arch_is_table (per-arch TU).
    static bool is_table_entry(uint64_t entry, unsigned level);
    /// @brief Arch leaf-codec helpers (issue #363 P2/P4): one definition
    ///        per arch TU (vmm_x86_64/aarch64/riscv64.cpp, guards-in-file),
    ///        bit-identical to the pre-split inline encodings. Shared walk
    ///        skeletons (policy: snapshot pairing, ENSURE, drop notes) call
    ///        these; walk SHAPES stay shared (documented VMM-private residual).
    ///        NOT HAL attr_from_flags: HAL adds SH/NS + AP policy the VMM map
    ///        path deliberately omits, so rewiring would change PTE bits.
    static bool vmm_arch_is_table(uint64_t entry);
    static uint64_t vmm_arch_table_child(uint64_t entry);
    static bool vmm_arch_split_needed(uint64_t entry);
    static void vmm_arch_split_fill(uint64_t *new_table, uint64_t src_entry);
    static uint64_t vmm_arch_table_link(uint64_t new_phys);
    static uint64_t vmm_arch_encode_kernel_leaf(uint64_t phys, bool user);
    static uint64_t vmm_arch_encode_map_leaf(uint64_t phys, bool user,
                                             bool executable);
    static uint64_t vmm_arch_encode_ro(uint64_t leaf, bool executable);
    static bool vmm_arch_leaf_is_ro(uint64_t leaf, bool *executable_out);
    static bool vmm_arch_leaf_is_executable(uint64_t leaf);
    /// @brief Ensure EL1/kernel-mode executability on a kernel-half leaf
    ///        (issue #376 boot-text W^X). The 5-arg map path installs
    ///        non-executable kernel leaves by policy (slice-1 data default),
    ///        so the harden pass restores X on text leaves through this
    ///        hook after the RO encode. Kernel-half only — never applied
    ///        to user pages (aarch64 keeps PXN there as ret2usr backstop).
    ///        x86_64 clears NX; aarch64 clears UXN+PXN; riscv64 sets X.
    static uint64_t vmm_arch_kernel_text_exec(uint64_t leaf);
    static uint64_t vmm_arch_block_base(uint64_t entry, bool huge);
    /// @brief Ownership test for a huge/block entry hit during fork
    ///        deep-copy (issue #387): true iff the block is user-owned —
    ///        the entry's USER bit is set OR the frame came from a user
    ///        page allocation (PMM::is_user_page).  Kernel-owned blocks
    ///        reachable in the user index range (riscv64 L0[0] low-half
    ///        MMIO/identity inherited from clone_kernel_pml4) are NOT
    ///        user-owned: the copy inherits them by value instead of
    ///        failing the fork.  No new privilege (U=0 preserved).
    /// @note Lock: none. Context: any. SMP: N/A (pre-publish window).
    [[nodiscard]] static bool vmm_leaf_is_user_owned(uint64_t entry,
                                                     uint64_t frame) noexcept;
    /// @brief Extract the flag bits of a 4 KiB leaf entry (issue #352):
    ///        everything except the frame address (x86/aarch64: complement
    ///        of the frame mask; riscv: low-10 flag bits plus high
    ///        reserved bits [63:54], never the PPN).  One definition per
    ///        arch TU; used by deep-copy so flag handling stays in the
    ///        codec home instead of open-coded per call site.
    static uint64_t vmm_arch_leaf_flags(uint64_t entry);
    /// @brief Walk geometry shims (issue #378): table levels from the root
    ///        (4 on x86_64/aarch64, 3 on Sv39 riscv64) + the per-level VA
    ///        index vector (idx[levels] valid, idx[3] zeroed on 3-level).
    ///        One definition per arch TU; lets the five VMM walk sites
    ///        share index computation instead of per-arch #if branches.
    ///        Walk BODIES stay VMM-private by design (#363 P4: snapshot
    ///        pairing, ENSUREs, drop notes are policy, not geometry).
    static unsigned vmm_arch_walk_levels();
    static void vmm_arch_walk_indices(uint64_t virt_addr, size_t idx[4]);
    /// @brief Recursive merge worker (issue #96): converge @p dst_table
    ///        toward @p src_table over [@p start, 512).  Zero allocations.
    static void merge_table_level(uint64_t *src_table, uint64_t *dst_table,
                                  unsigned level, size_t start);
    /// @brief Recursive equality worker (issue #96).
    static bool tables_equal_level(const uint64_t *a_table,
                                   const uint64_t *b_table, unsigned level,
                                   size_t start);
};

#if defined(CONFIG_ARCH_RISCV64)
// Compile-time codec roundtrip (issue #348-C): PPN shift must be lossless
// for page-aligned addresses.  Outside the class (in-class asserts cannot
// evaluate members of their own incomplete class on all toolchains); the
// helpers above are public for exactly this pin.
static_assert(VMM::sv39_pte_phys(VMM::sv39_phys_pte(0x80200000ULL) |
                                 (1ULL << 0) | (1ULL << 1)) == 0x80200000ULL,
              "Sv39 PTE codec must roundtrip");
#endif

} // namespace kernel
