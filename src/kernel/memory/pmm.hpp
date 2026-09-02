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

/// @file pmm.hpp
/// @brief Physical Memory Manager — bitmap-based page allocator with owner
/// tracking.

#pragma once

#include <types.hpp>
#include <constants.hpp>
#include <kernel/memory/pmm_errors.hpp>

namespace kernel::test {
struct PtPoolSnapshot;
}



namespace kernel {

/// @brief Physical memory manager using a bitmap to track free 4 KiB pages.
/// @note All physical page allocation and deallocation goes through PMM.
///       Tracks USER vs KERNEL page ownership for safety in free_user_pages.
class PMM {
  public:
    /// @brief Initialize the physical memory manager with a bitmap.
    /// @param mem_size Total physical memory size in bytes.
    /// @param kernel_start Start of kernel image in physical memory.
    /// @param kernel_end End of kernel image in physical memory.
    /// @param window_base Physical base of the allocatable RAM window
    /// (min usable-region base; 0 on x86_64).  Must be covered by the
    /// architecture's MMU higher-half mapping.
    static void init(uint64_t mem_size, uint64_t kernel_start,
                     uint64_t kernel_end, uint64_t window_base = 0);
    /// @brief Initialize with error code.
    /// @param mem_size Total physical memory size in bytes.
    /// @param kernel_start Start of kernel image in physical memory.
    /// @param kernel_end End of kernel image in physical memory.
    /// @param window_base Physical base of the allocatable RAM window.
    /// @return PmmError code.
    static errors::PmmError init_err(uint64_t mem_size, uint64_t kernel_start,
                                     uint64_t kernel_end,
                                     uint64_t window_base = 0);

    /// @brief Mark the PMM as fully initialized (post-boot).
    /// After this call, alloc_page() checks CONFIG_STATIC_POOLS_ONLY
    /// and panics/returns 0 if a dynamic allocation is attempted.
    static void mark_init_done();

    /// @brief Allocates a single 4 KiB page (KERNEL ownership).
    static uint64_t alloc_page();
    /// @brief Allocates a single 4 KiB page (KERNEL ownership) with error code.
    /// @param[out] out_phys_addr Physical address of allocated page.
    /// @return PmmError code.
    static errors::PmmError alloc_page_err(uint64_t &out_phys_addr);

    /// @brief Allocates a contiguous block of pages (KERNEL ownership).
    static uint64_t alloc_contiguous(size_t count);
    /// @brief Allocates a contiguous block of pages (KERNEL ownership) with
    /// error code.
    /// @param count Number of contiguous pages to allocate.
    /// @param[out] out_phys_addr Physical address of first page.
    /// @return PmmError code.
    static errors::PmmError alloc_contiguous_err(size_t count,
                                                 uint64_t &out_phys_addr);

    /// @brief Allocates a single 4 KiB page (USER ownership).
    static uint64_t alloc_user_page();
    /// @brief Allocates a single 4 KiB page (USER ownership) with error code.
    /// @param[out] out_phys_addr Physical address of allocated page.
    /// @return PmmError code.
    static errors::PmmError alloc_user_page_err(uint64_t &out_phys_addr);

    /// @brief Allocates contiguous pages (USER ownership).
    static uint64_t alloc_user_contiguous(size_t count);
    /// @brief Allocates contiguous pages (USER ownership) with error code.
    /// @param count Number of contiguous pages to allocate.
    /// @param[out] out_phys_addr Physical address of first page.
    /// @return PmmError code.
    static errors::PmmError alloc_user_contiguous_err(size_t count,
                                                      uint64_t &out_phys_addr);

    /// @brief Allocates a single 4 KiB page for page tables (KERNEL
    /// ownership, from reserved low-memory pool).
    static uint64_t alloc_page_table();
    /// @brief Allocates a single 4 KiB page for page tables with error code.
    /// @param[out] out_phys_addr Physical address of allocated page table page.
    /// @return PmmError code.
    static errors::PmmError alloc_page_table_err(uint64_t &out_phys_addr);

    /// @brief Frees a page regardless of ownership.
    static void free_page(uint64_t phys_addr);
    /// @brief Frees a page with error code.
    /// @param phys_addr Physical address to free.
    /// @return PmmError code.
    static errors::PmmError free_page_err(uint64_t phys_addr);

    /// @brief Marks a physical page range [start_phys, end_phys) as allocated
    ///        KERNEL-owned (boot-time data: GRUB's Multiboot2 info must never
    ///        be handed to an allocator).  Call right after init() before any
    ///        allocation.  No-op on pages outside the allocatable window or
    ///        already marked.
    /// @param start_phys Inclusive first physical address.
    /// @param end_phys   Exclusive end physical address.
    static void reserve_range(uint64_t start_phys, uint64_t end_phys);

    /// @brief Returns true if a physical page is currently allocated.
    /// @param phys_addr Physical address to check.
    /// @return true if the allocation bit is set.
    static bool is_allocated(uint64_t phys_addr);

    /// @brief Returns true if the page was allocated as USER ownership.
    static bool is_user_page(uint64_t phys_addr);
    /// @brief Returns true if the page was allocated as USER ownership with
    /// error code.
    /// @param phys_addr Physical address to check.
    /// @param[out] out_is_user Output: true if user page.
    /// @return PmmError code.
    static errors::PmmError is_user_page_err(uint64_t phys_addr,
                                             bool &out_is_user);

    /// @brief Return the amount of free physical memory in bytes.
    static uint64_t free_memory() noexcept {
        return free_pages_ * arch::PAGE_SIZE;
    }
    /// @brief Return the total physical memory size in bytes.
    static uint64_t total_memory() noexcept {
        return total_pages_ * arch::PAGE_SIZE;
    }

    /// @brief Allocatable-window geometry in absolute page indices.
    struct WindowPages {
        uint64_t base_page; ///< First allocatable page index.
        uint64_t end_page;  ///< One past the last allocatable page index.
    };

    /// @brief Compute the allocatable window for a memory layout (pure).
    ///
    /// The window is the first HHDM_WINDOW_SIZE bytes of RAM starting at
    /// window_base, clamped to the bitmap span (mem_size).  base_page ==
    /// end_page encodes an empty window (window_base beyond the span).
    /// @param mem_size    Total span covered by the bitmap in bytes.
    /// @param window_base Physical base of usable RAM.
    /// @return Window page range [base_page, end_page).
    static constexpr WindowPages
    compute_window_pages(uint64_t mem_size, uint64_t window_base) {
        uint64_t total = mem_size / PAGE_SIZE;
        uint64_t base = window_base / PAGE_SIZE;
        uint64_t end = base + arch::HHDM_WINDOW_SIZE / PAGE_SIZE;
        if (end > total) {
            end = total;
        }
        if (base > end) {
            base = end;
        }
        return WindowPages{base, end};
    }

    /// @brief First allocatable physical page index (diagnostic/test only).
    static uint64_t window_base_page() noexcept {
        return window_base_page_;
    }
    /// @brief One past the last allocatable physical page index
    /// (diagnostic/test only).
    static uint64_t window_end_page() noexcept {
        return window_end_page_;
    }

    /// @brief OOM handler type — called when allocation fails.
    /// Should try to free memory.
    /// @return true if memory may have been freed (caller should retry).
    using OOMHandler = bool (*)();
    /// @brief Install an OOM handler that the allocator calls when pages are
    /// exhausted.
    /// @param h Function pointer; return true if memory may have been freed.
    static void set_oom_handler(OOMHandler h) {
        oom_handler_ = h;
    }
    /// @brief Get the currently installed OOM handler.
    /// @return Function pointer, or nullptr if none.
    static OOMHandler get_oom_handler() {
        return oom_handler_;
    }

    /// @name Test-isolation helpers
    /// @brief Expose internal bitmaps for snapshot/restore.
    /// @return Pointer to the allocation bitmap.
    static uint8_t *bitmap_ptr() {
        // NOLINTNEXTLINE(performance-no-int-to-ptr)
        return reinterpret_cast<uint8_t *>(bitmap_);
    }
    /// @brief Return pointer to the owner-tracking bitmap.
    static uint8_t *owner_bitmap_ptr() {
        // NOLINTNEXTLINE(performance-no-int-to-ptr)
        return reinterpret_cast<uint8_t *>(owner_bitmap_);
    }
    /// @brief Return size of each bitmap in bytes.
    static uint64_t bitmap_bytes() {
        return bitmap_size_;
    }
    /// @brief Return mutable reference to the free-page counter (for
    /// snapshot/restore).
    static uint64_t &free_pages_ref() {
        return free_pages_;
    }
    /// @brief Capture page-table pool state into a snapshot struct.
    static void capture_pool_snapshot(
        kernel::test::PtPoolSnapshot &out);
    /// @brief Restore page-table pool state from a snapshot struct.
    static void restore_pool_snapshot(
        const kernel::test::PtPoolSnapshot &src);
    /// @brief Page-table pool base physical address (0 if pool not active).
    static uint64_t pool_start() noexcept {
        return page_table_pool_start_;
    }
    /// @brief Page-table pool end physical address.
    static uint64_t pool_end() noexcept {
        return page_table_pool_end_;
    }
    /// @brief Number of allocated pages in the page-table pool.
    /// @note Diagnostic/introspection only — O(pool_size_pages).  Must NOT
    ///       be called from any WCET-budgeted hot path.
    static uint64_t pool_used_pages() noexcept;
    /// @brief Total capacity of the page-table pool in pages.
    /// @note Diagnostic/introspection only — O(1).
    static uint64_t pool_total_pages() noexcept;
    /// @brief Page-table pool generation (incremented on each snapshot capture).
    static uint64_t pool_generation() noexcept { return pool_generation_; }
    /// @brief Bump the pool generation.
    static void pool_bump_generation() noexcept { ++pool_generation_; }
    /// @brief Pool reference count (tasks using pool pages).
    static uint64_t pool_refcount() noexcept { return pool_refcount_; }
    /// @brief Increment pool reference count.
    static void pool_ref_inc() noexcept { ++pool_refcount_; }
    /// @brief Decrement pool reference count.
    static void pool_ref_dec() noexcept { --pool_refcount_; }
    /// @brief True if the pool is mapped in the kernel PML4.
    static bool pool_is_mapped() noexcept { return pool_mapped_; }
    /// @brief Mark the page-table pool as tainted (HW error detected).
    static void pool_set_tainted() noexcept {
        pool_tainted_ = true;
    }
    /// @brief Mark the page-table pool as poisoned (buffer overflow / UAF).
    static void pool_set_poisoned() noexcept {
        pool_poisoned_ = true;
    }
    /// @brief True if the pool has been touched by a HW error.
    static bool pool_is_tainted() noexcept {
        return pool_tainted_;
    }
    /// @brief True if the pool has been corrupted by a buffer overflow / UAF.
    static bool pool_is_poisoned() noexcept {
        return pool_poisoned_;
    }

    /// @brief Rebuild the general + pool free lists from the current bitmap.
    ///        Called after PMM bitmap + pool snapshot restore to re-sync
    ///        freelist heads (free_head_, pool_free_head_) with the restored
    ///        bitmap state.  Without this, alloc_page()/alloc_page_table()
    ///        follow stale freelist entries and return already-allocated pages.
    static void rebuild_free_list() noexcept;

  private:
    static constexpr uint64_t PAGE_SIZE = CONFIG_PAGE_SIZE;

    static constinit uint64_t total_pages_;
    static constinit uint64_t free_pages_;
    static constinit uint64_t bitmap_;
    static constinit uint64_t bitmap_size_;
    static constinit uint64_t owner_bitmap_;
    static constinit uint64_t page_table_pool_start_;
    static constinit uint64_t page_table_pool_end_;
    static constinit bool pool_tainted_;
    static constinit bool pool_poisoned_;
    static constinit bool pool_mapped_;
    static constinit uint64_t pool_generation_;
    static constinit uint64_t pool_refcount_;
    static constinit OOMHandler oom_handler_;

    /// @brief Free-list array (one entry per page, next-index or sentinel).
    static constinit uint64_t free_list_;
    /// @brief Head of the general free list.
    static constinit uint64_t free_head_;
    /// @brief Head of the page-table-pool free list.
    static constinit uint64_t pool_free_head_;

    /// @brief First allocatable page index (absolute; 0 on x86_64).
    static constinit uint64_t window_base_page_;
    /// @brief One past the last allocatable page index (absolute).
    static constinit uint64_t window_end_page_;

    /// @brief Mark a page as allocated in the bitmap.
    /// @param index Page index.
    static void bitmap_set(size_t index);
    /// @brief Mark a page as free in the bitmap.
    /// @param index Page index.
    static void bitmap_clear(size_t index);
    /// @brief Test whether a page is allocated.
    /// @param index Page index.
    /// @return true if the page is allocated.
    static bool bitmap_test(size_t index);

    /// @brief Mark a page as USER-owned in the owner bitmap.
    /// @param index Page index.
    static void owner_set_user(size_t index);
    /// @brief Mark a page as KERNEL-owned in the owner bitmap.
    /// @param index Page index.
    static void owner_set_kernel(size_t index);
    /// @brief Test whether a page is USER-owned.
    /// @param index Page index.
    /// @return true if USER-owned (false = KERNEL-owned).
    static bool owner_test(size_t index);

    /// @brief Allocate contiguous USER pages from the bitmap (linear scan).
    /// @param count Number of consecutive pages.
    /// @return Physical address of first page, or 0 on failure.
    static uint64_t try_alloc_user(size_t count);
    /// @brief Allocate contiguous KERNEL pages from the bitmap (linear scan).
    /// @param count Number of consecutive pages.
    /// @return Physical address of first page, or 0 on failure.
    static uint64_t try_alloc_kernel(size_t count);
};

} // namespace kernel
