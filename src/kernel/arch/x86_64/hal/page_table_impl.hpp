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

/// @file page_table_impl.hpp
/// @brief x86_64 page-table management — CR3 activation and TLB invalidation
/// wrappers.

#pragma once

#include <types.hpp>
#include <kernel/arch/hal/io.hpp>

namespace arch {

/// @brief Return the physical address of the currently active PML4 page table.
/// @return CR3 value (physical address of the top-level page table).
inline uint64_t arch_page_table_current() {
    return read_cr3();
}
/// @brief Activate a page table by writing its physical address into CR3.
/// @param pml4_phys Physical address of the PML4 table to activate.
inline void arch_page_table_activate(uint64_t pml4_phys) {
    write_cr3(pml4_phys);
}
/// @brief Flush the TLB entry for a single virtual address (INVLPG
/// instruction).
/// @param virt_addr Virtual address whose TLB entry should be invalidated.
inline void arch_page_table_tlb_flush(uint64_t virt_addr) {
    asm volatile("invlpg (%0)" : : "r"(virt_addr) : "memory");
}
/// @brief Flush the entire TLB by reloading CR3 (issue #157: the low
///        12 bits must clear — a tagged reload is NOT a full flush
///        under PCIDE, it retains the loaded PCID's entries).
inline void arch_page_table_tlb_flush_all() {
    uint64_t cr3 = read_cr3();
    write_cr3(cr3 & ~0xFFFULL);
}

/// @brief Per-kind TLB-invalidation event counters (issue #157).
///        Observability for tests (routing proofs) and the #160
///        latency work.  Monotonic; tests assert deltas, never absolutes.
struct TlbFlushStats {
    uint64_t single_va;    ///< Single-address flushes (INVLPG, any CPU).
    uint64_t ctx_invpcid;  ///< Single-context INVPCID purges issued.
    uint64_t ctx_fallback; ///< Single-context CR3-reload fallbacks.
    uint64_t all_invpcid;  ///< All-context INVPCID purges issued.
    uint64_t all_fallback; ///< All-context CR3-reload fallbacks.
};

/// @brief Process-wide flush counters (one instance, atomic updates).
inline TlbFlushStats &tlb_flush_stats_store() {
    static TlbFlushStats stats{};
    return stats;
}

/// @brief Snapshot current flush counters (test/diagnostic read).
inline TlbFlushStats tlb_flush_stats() {
    TlbFlushStats out{};
    TlbFlushStats &store = tlb_flush_stats_store();
    out.single_va = __atomic_load_n(&store.single_va, __ATOMIC_RELAXED);
    out.ctx_invpcid = __atomic_load_n(&store.ctx_invpcid, __ATOMIC_RELAXED);
    out.all_invpcid = __atomic_load_n(&store.all_invpcid, __ATOMIC_RELAXED);
    out.ctx_fallback =
        __atomic_load_n(&store.ctx_fallback, __ATOMIC_RELAXED);
    out.all_fallback =
        __atomic_load_n(&store.all_fallback, __ATOMIC_RELAXED);
    return out;
}

/// @brief Purge one PCID's non-global entries (issue #157): INVPCID
///        single-context when available, else same-root CR3 reload.
///        Local CPU only; cross-CPU invalidation is #158's scope.
/// @param pcid PCID to purge (0 purges the untagged/kernel context).
inline void tlb_purge_context(uint16_t pcid) {
    TlbFlushStats &store = tlb_flush_stats_store();
    if (has_invpcid()) {
        __atomic_fetch_add(&store.ctx_invpcid, 1ULL, __ATOMIC_RELAXED);
        InvpcidDesc desc = invpcid_build_desc(InvpcidType::SINGLE_CONTEXT,
                                              pcid, 0);
        invpcid_emit(InvpcidType::SINGLE_CONTEXT, desc);
    } else {
        __atomic_fetch_add(&store.ctx_fallback, 1ULL, __ATOMIC_RELAXED);
        uint64_t cr3 = read_cr3();
        write_cr3(cr3 & ~0xFFFULL);
    }
}

/// @brief Purge everything incl. globals (issue #157): INVPCID
///        all-context when available, else untagged CR3 reload (a
///        tagged reload is NOT a full flush — low bits must clear).
///        Local CPU only; cross-CPU invalidation is #158's scope.
inline void tlb_purge_all() {
    TlbFlushStats &store = tlb_flush_stats_store();
    if (has_invpcid()) {
        __atomic_fetch_add(&store.all_invpcid, 1ULL, __ATOMIC_RELAXED);
        InvpcidDesc desc =
            invpcid_build_desc(InvpcidType::ALL_INCL_GLOBAL, 0, 0);
        invpcid_emit(InvpcidType::ALL_INCL_GLOBAL, desc);
    } else {
        __atomic_fetch_add(&store.all_fallback, 1ULL, __ATOMIC_RELAXED);
        uint64_t cr3 = read_cr3();
        write_cr3(cr3 & ~0xFFFULL);
    }
}

/// @brief Static wrapper class for page-table operations.
/// Provides a uniform interface for use in generic VMM code.
class ArchPageTable {
  public:
    static inline uint64_t current() {
        return arch_page_table_current();
    }
    static inline void activate(uint64_t pml4_phys) {
        arch_page_table_activate(pml4_phys);
    }
    static inline void tlb_flush(uint64_t virt_addr) {
        TlbFlushStats &store = tlb_flush_stats_store();
        __atomic_fetch_add(&store.single_va, 1ULL, __ATOMIC_RELAXED);
        arch_page_table_tlb_flush(virt_addr);
    }
    static inline void tlb_flush_all() {
        tlb_purge_all();
    }

    /// @brief Size of a single page (from kernel config).
    static constexpr uint64_t PAGE_SIZE = CONFIG_PAGE_SIZE;
    /// @brief Number of entries per page-table level.
    static constexpr uint64_t ENTRIES = 512;

    // ─── Page-table index constants (x86_64 4-level paging) ───────────────
    static constexpr uint64_t PML4_SHIFT = 39;
    static constexpr uint64_t PDPT_SHIFT = 30;
    static constexpr uint64_t PD_SHIFT   = 21;
    static constexpr uint64_t PT_SHIFT   = 12;
    static constexpr uint64_t PML4_MASK  = 0x1FFULL << PML4_SHIFT;
    static constexpr uint64_t PDPT_MASK  = 0x1FFULL << PDPT_SHIFT;
    static constexpr uint64_t PD_MASK    = 0x1FFULL << PD_SHIFT;
    static constexpr uint64_t PT_MASK    = 0x1FFULL << PT_SHIFT;

    /// @brief Extract the PML4 index from a virtual address.
    static inline size_t pml4_index(uint64_t vaddr) {
        return (vaddr & PML4_MASK) >> PML4_SHIFT;
    }
    static inline size_t pdpt_index(uint64_t vaddr) {
        return (vaddr & PDPT_MASK) >> PDPT_SHIFT;
    }
    static inline size_t pd_index(uint64_t vaddr) {
        return (vaddr & PD_MASK) >> PD_SHIFT;
    }
    static inline size_t pt_index(uint64_t vaddr) {
        return (vaddr & PT_MASK) >> PT_SHIFT;
    }
};

} // namespace arch
