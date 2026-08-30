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

#include <kernel/memory/pmm.hpp>
#include <kernel/test/test_isolate.hpp>
#include <kernel/task/scheduler.hpp>
#include <kernel/task/task.hpp>
#include <kernel/test/resource_tracker.hpp>
#include <kernel/sync/irq_spinlock_guard.hpp>
#include <constants.hpp>
#include <utils.hpp>
#include <assert.hpp>
#include <logger.hpp>
#include <kernel/memory/pmm_errors.hpp>

namespace kernel {

static sync::SpinLock pmm_lock_{};

constinit uint64_t PMM::total_pages_ = 0;
constinit uint64_t PMM::free_pages_ = 0;
constinit uint64_t PMM::bitmap_ = 0;
constinit uint64_t PMM::bitmap_size_ = 0;
constinit uint64_t PMM::owner_bitmap_ = 0;
constinit uint64_t PMM::page_table_pool_start_ = 0;
constinit uint64_t PMM::page_table_pool_end_ = 0;
constinit bool PMM::pool_tainted_ = false;
constinit bool PMM::pool_poisoned_ = false;
constinit bool PMM::pool_mapped_ = true;
constinit uint64_t PMM::pool_generation_ = 0;
constinit uint64_t PMM::pool_refcount_ = 0;
constinit PMM::OOMHandler PMM::oom_handler_ = nullptr;
constinit uint64_t PMM::free_list_ = 0;
constinit uint64_t PMM::free_head_ = UINT64_MAX;
constinit uint64_t PMM::pool_free_head_ = UINT64_MAX;
constinit uint64_t PMM::window_base_page_ = 0;
constinit uint64_t PMM::window_end_page_ = 0;

#if CONFIG_STATIC_POOLS_ONLY
static bool g_pmm_init_done = false;
#endif

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
/// @brief Initialise the PMM bitmap and mark kernel/reserved pages as
/// allocated.
/// @param mem_size    Total physical memory in bytes.
/// @param kernel_start Physical address of kernel image start.
/// @param kernel_end   Physical address of kernel image end.
/// @param window_base  Physical base of the allocatable RAM window (min
///                     usable-region base; 0 on x86_64).
void PMM::init(uint64_t mem_size, uint64_t kernel_start, uint64_t kernel_end,
               uint64_t window_base) {
    sync::IrqSpinLockGuard lock(pmm_lock_);
    total_pages_ = mem_size / PAGE_SIZE;
    free_pages_ = total_pages_;

    WindowPages win = compute_window_pages(mem_size, window_base);
    window_base_page_ = win.base_page;
    window_end_page_ = win.end_page;

    bitmap_size_ = align_up<uint64_t>(total_pages_ / 8, 8_KiB);
    uint64_t free_list_bytes = align_up<uint64_t>(total_pages_ * sizeof(uint64_t), 8_KiB);
    uint64_t bitmap_phys = align_up<uint64_t>(kernel_end, 8_KiB);
    uint64_t owner_bitmap_phys = bitmap_phys + bitmap_size_;
    uint64_t free_list_phys = owner_bitmap_phys + bitmap_size_;
    bitmap_ = arch::HHDM_OFFSET + bitmap_phys;
    owner_bitmap_ = arch::HHDM_OFFSET + owner_bitmap_phys;
    free_list_ = arch::HHDM_OFFSET + free_list_phys;

    // NOLINTNEXTLINE(performance-no-int-to-ptr)
    auto *bitmap = reinterpret_cast<uint8_t *>(bitmap_);
    // NOLINTNEXTLINE(performance-no-int-to-ptr)
    auto *owner = reinterpret_cast<uint8_t *>(owner_bitmap_);
    for (uint64_t i = 0; i < bitmap_size_; ++i) {
        bitmap[i] = 0;
        owner[i] = 0;
    }

    uint64_t kernel_start_page = kernel_start / PAGE_SIZE;
    uint64_t reserved_end = free_list_phys + free_list_bytes;
    uint64_t reserved_end_page = (reserved_end + PAGE_SIZE - 1) / PAGE_SIZE;

    for (uint64_t i = 0; i < kernel_start_page; ++i) {
        bitmap_set(i);
        owner_set_kernel(i);
        --free_pages_;
    }

    for (uint64_t i = kernel_start_page; i < reserved_end_page; ++i) {
        bitmap_set(i);
        owner_set_kernel(i);
        --free_pages_;
    }

    // Place the page-table pool at the end of the allocatable window
    // (128 MiB of linear-mapped RAM) so general allocations (try_alloc_kernel
    // scanning from the window base) naturally find free pages before the
    // pool.  Only alloc_page_table uses the pool range.  PtPoolSnapshot
    // protects the pool bitmap from PMM restore.
    uint64_t window_pages = window_end_page_ - window_base_page_;
    uint64_t pool_size_pages = CONFIG_PAGE_TABLE_POOL_SIZE;
    if (window_pages > pool_size_pages + 16) {
        uint64_t pool_start_page = window_base_page_ + window_pages -
                                   pool_size_pages - 16;
        // pool_start_page + pool_size_pages <= window_end_page_ holds by
        // construction (16-page guard above).
        if (pool_start_page > reserved_end_page) {
            page_table_pool_start_ = pool_start_page * PAGE_SIZE;
            page_table_pool_end_ =
                (pool_start_page + pool_size_pages) * PAGE_SIZE;
        }
    }

    // Reserve the last 16 pages.
    if (total_pages_ > 16) {
        for (uint64_t i = total_pages_ - 1; i >= total_pages_ - 16; --i) {
            if (!bitmap_test(i)) {
                bitmap_set(i);
                owner_set_kernel(i);
                --free_pages_;
            }
        }
    }

    // Build the O(1) free list (pages within the allocatable window only).
    rebuild_free_list();

    if (free_head_ == UINT64_MAX) {
        kernel::Logger::error(
            "[PMM] allocatable window [%lu, %lu) has no free pages",
            window_base_page_, window_end_page_);
    }

    // Register the default OOM handler based on CONFIG_OOM_POLICY.
#if CONFIG_OOM_POLICY == 1 && !CONFIG_OOM_HOOK
    if (!oom_handler_) {
        oom_handler_ = []() -> bool {
            kernel::Logger::fatal("OOM: no free pages (%lu remaining)",
                                  free_pages_);
            return false;
        };
    }
#endif
}

void PMM::rebuild_free_list() noexcept {
    free_head_ = UINT64_MAX;
    auto *fl = reinterpret_cast<uint64_t *>(free_list_);
    uint64_t pool_start = page_table_pool_start_ / PAGE_SIZE;
    uint64_t pool_end = page_table_pool_end_ / PAGE_SIZE;
    for (uint64_t idx = window_base_page_; idx < window_end_page_; ++idx) {
        if (bitmap_test(idx))
            continue;
        if (idx >= pool_start && idx < pool_end) {
            fl[idx] = pool_free_head_;
            pool_free_head_ = idx;
        } else {
            fl[idx] = free_head_;
            free_head_ = idx;
        }
    }
}

/// @brief O(1) free-list KERNEL alloc for single pages, bitmap scan for
///        multi-page contiguous requests (list rebuilt afterward).
/// @param count Number of contiguous pages.
/// @return Physical address or 0.
uint64_t PMM::try_alloc_kernel(size_t count) {
    if (count == 0) {
        return 0;
    }
    if (count == 1) {
        // O(1) fast path: pop from free list (pages within the allocatable
        // window).
        if (free_head_ >= window_base_page_ &&
            free_head_ < window_end_page_) {
            uint64_t idx = free_head_;
            free_head_ = reinterpret_cast<uint64_t *>(free_list_)[idx];
            bitmap_set(idx);
            owner_set_kernel(idx);
            --free_pages_;
            return idx * PAGE_SIZE;
        }
        // Fallback: bitmap scan within the allocatable window for pages not
        // on the free list (e.g., freed via paths that didn't update it).
        for (uint64_t idx = window_base_page_; idx < window_end_page_;
             ++idx) {
            if (!bitmap_test(idx)) {
                bitmap_set(idx);
                owner_set_kernel(idx);
                --free_pages_;
                return idx * PAGE_SIZE;
            }
        }
        return 0;
    }
    for (uint64_t idx = window_base_page_;
         idx + count <= window_end_page_; ++idx) {
        bool ok = true;
        for (size_t j = 0; j < count; ++j) {
            if (bitmap_test(idx + j)) {
                ok = false;
                break;
            }
        }
        if (ok) {
            for (size_t j = 0; j < count; ++j) {
                bitmap_set(idx + j);
                owner_set_kernel(idx + j);
                --free_pages_;
            }
            rebuild_free_list();
            return idx * PAGE_SIZE;
        }
    }
    return 0;
}

/// @brief O(1) free-list USER alloc for single pages, bitmap scan for
///        multi-page contiguous requests (list rebuilt afterward).
/// @param count Number of contiguous pages.
/// @return Physical address or 0.
uint64_t PMM::try_alloc_user(size_t count) {
    if (count == 0) {
        return 0;
    }
    if (count == 1) {
        if (free_head_ >= window_base_page_ &&
            free_head_ < window_end_page_) {
            uint64_t idx = free_head_;
            free_head_ = reinterpret_cast<uint64_t *>(free_list_)[idx];
            bitmap_set(idx);
            owner_set_user(idx);
            --free_pages_;
            return idx * PAGE_SIZE;
        }
        for (uint64_t idx = window_base_page_; idx < window_end_page_;
             ++idx) {
            if (!bitmap_test(idx)) {
                bitmap_set(idx);
                owner_set_user(idx);
                --free_pages_;
                return idx * PAGE_SIZE;
            }
        }
        return 0;
    }
    for (uint64_t idx = window_base_page_;
         idx + count <= window_end_page_; ++idx) {
        bool ok = true;
        for (size_t j = 0; j < count; ++j) {
            if (bitmap_test(idx + j)) {
                ok = false;
                break;
            }
        }
        if (ok) {
            for (size_t j = 0; j < count; ++j) {
                bitmap_set(idx + j);
                owner_set_user(idx + j);
                --free_pages_;
            }
            rebuild_free_list();
            return idx * PAGE_SIZE;
        }
    }
    return 0;
}

/// @brief Allocate a single KERNEL page.  Invokes OOM handler on failure.
/// @return Physical address, or 0 (asserts on persistent OOM).
uint64_t PMM::alloc_page() {
#if CONFIG_STATIC_POOLS_ONLY
    if (g_pmm_init_done) {
        ASSERT(false && "alloc_page after init with CONFIG_STATIC_POOLS_ONLY");
        return 0;
    }
#endif
    sync::IrqSpinLockGuard lock(pmm_lock_);
#if CONFIG_MEMORY_BUDGET
    auto *cur = Scheduler::current_task();
    if (cur && cur->magic == TaskControlBlock::TCB_MAGIC &&
        cur->memory_used_pages_ >= cur->memory_budget_pages_) {
        return 0;
    }
#endif
    uint64_t result = try_alloc_kernel(1);
    if (result) {
#if CONFIG_MEMORY_BUDGET
        if (cur && cur->magic == TaskControlBlock::TCB_MAGIC)
            cur->memory_used_pages_ += 1;
#endif
        kernel::test::ResourceTracker::instance().track_pmm_alloc(1);
        return result;
    }
    // OOM handler: release lock, call handler, re-acquire for retry.
    bool oom_retry = false;
    if (oom_handler_) {
        lock.unlock();
        oom_retry = oom_handler_();
        lock.lock();
#if CONFIG_MEMORY_BUDGET
        cur = Scheduler::current_task();
#endif
    }
    if (oom_retry) {
        result = try_alloc_kernel(1);
    }
    if (!result) {
        ASSERT(errors::PmmError::PMM_ERR_OOM);
    }
    if (result) {
#if CONFIG_MEMORY_BUDGET
        if (cur && cur->magic == TaskControlBlock::TCB_MAGIC)
            cur->memory_used_pages_ += 1;
#endif
        kernel::test::ResourceTracker::instance().track_pmm_alloc(1);
    }
    return result;
}

void PMM::mark_init_done() {
#if CONFIG_STATIC_POOLS_ONLY
    g_pmm_init_done = true;
#endif
}

/// @brief Allocate contiguous KERNEL pages.  Invokes OOM handler on failure.
/// @param count Number of pages.
/// @return Physical address, or 0 (asserts on persistent OOM).
uint64_t PMM::alloc_contiguous(size_t count) {
#if CONFIG_STATIC_POOLS_ONLY
    if (g_pmm_init_done) {
        ASSERT(false && "alloc_contiguous after init with CONFIG_STATIC_POOLS_ONLY");
        return 0;
    }
#endif
    if (count == 0 || count > total_pages_)
        return 0;
    sync::IrqSpinLockGuard lock(pmm_lock_);
#if CONFIG_MEMORY_BUDGET
    auto *cur = Scheduler::current_task();
    if (cur && cur->magic == TaskControlBlock::TCB_MAGIC &&
        cur->memory_used_pages_ + count > cur->memory_budget_pages_) {
        return 0;
    }
#endif
    uint64_t result = try_alloc_kernel(count);
    if (result) {
#if CONFIG_MEMORY_BUDGET
        if (cur && cur->magic == TaskControlBlock::TCB_MAGIC)
            cur->memory_used_pages_ += count;
#endif
        kernel::test::ResourceTracker::instance().track_pmm_alloc(count);
        return result;
    }
    // OOM handler: release lock, call handler, re-acquire for retry.
    bool oom_retry = false;
    if (oom_handler_) {
        lock.unlock();
        oom_retry = oom_handler_();
        lock.lock();
#if CONFIG_MEMORY_BUDGET
        cur = Scheduler::current_task();
#endif
    }
    if (oom_retry) {
        result = try_alloc_kernel(count);
    }
    if (!result) {
        ASSERT(errors::PmmError::PMM_ERR_OOM);
    }
    if (result) {
#if CONFIG_MEMORY_BUDGET
        if (cur && cur->magic == TaskControlBlock::TCB_MAGIC)
            cur->memory_used_pages_ += count;
#endif
        kernel::test::ResourceTracker::instance().track_pmm_alloc(count);
    }
    return result;
}

/// @brief Allocate a single USER page.  Invokes OOM handler on failure.
/// @return Physical address, or 0 (asserts on persistent OOM).
uint64_t PMM::alloc_user_page() {
    sync::IrqSpinLockGuard lock(pmm_lock_);
    uint64_t result = try_alloc_user(1);
    if (result) {
        kernel::test::ResourceTracker::instance().track_pmm_alloc(1);
        return result;
    }
    bool oom_retry = false;
    if (oom_handler_) {
        lock.unlock();
        oom_retry = oom_handler_();
        lock.lock();
    }
    if (oom_retry) {
        result = try_alloc_user(1);
    }
    if (!result) {
        ASSERT(errors::PmmError::PMM_ERR_USER_OOM);
    }
    if (result)
        kernel::test::ResourceTracker::instance().track_pmm_alloc(1);
    return result;
}

/// @brief Allocate contiguous USER pages.  Invokes OOM handler on failure.
/// @param count Number of pages.
/// @return Physical address, or 0 (asserts on persistent OOM).
uint64_t PMM::alloc_user_contiguous(size_t count) {
    if (count == 0 || count > total_pages_)
        return 0;
    sync::IrqSpinLockGuard lock(pmm_lock_);
    uint64_t result = try_alloc_user(count);
    if (result) {
        kernel::test::ResourceTracker::instance().track_pmm_alloc(count);
        return result;
    }
    bool oom_retry = false;
    if (oom_handler_) {
        lock.unlock();
        oom_retry = oom_handler_();
        lock.lock();
    }
    if (oom_retry) {
        result = try_alloc_user(count);
    }
    if (!result) {
        ASSERT(errors::PmmError::PMM_ERR_USER_OOM);
    }
    if (result)
        kernel::test::ResourceTracker::instance().track_pmm_alloc(count);
    return result;
}

/// @brief Allocate a page from the reserved page-table pool (O(1) free list).
///        Falls back to alloc_page() when the pool is exhausted.
/// @return Physical address, or 0 (asserts on OOM).
uint64_t PMM::alloc_page_table() {
    if (page_table_pool_start_ == 0 || page_table_pool_end_ == 0) {
        return alloc_page();
    }
    {
        sync::IrqSpinLockGuard lock(pmm_lock_);
        if (pool_free_head_ >= window_base_page_ &&
            pool_free_head_ < window_end_page_) {
            uint64_t idx = pool_free_head_;
            pool_free_head_ = reinterpret_cast<uint64_t *>(free_list_)[idx];
            bitmap_set(idx);
            owner_set_kernel(idx);
            --free_pages_;
            kernel::test::ResourceTracker::instance().track_pmm_alloc(1);
            return idx * PAGE_SIZE;
        }
    }
    uint64_t result = alloc_page();
    if (!result) {
        ASSERT(errors::PmmError::PMM_ERR_TABLE_OOM);
    }
    return result;
}

/// @brief Free a physical page regardless of ownership.
/// Ownership is managed at allocation time.
/// Free pushes the page onto the O(1) free list.
/// @param phys_addr Physical address to free.
void PMM::free_page(uint64_t phys_addr) {
    sync::IrqSpinLockGuard lock(pmm_lock_);
    uint64_t index = phys_addr / PAGE_SIZE;
    if (index >= total_pages_ || index < window_base_page_)
        return;
    if (bitmap_test(index)) {
        bitmap_clear(index);
        ++free_pages_;
        kernel::test::ResourceTracker::instance().track_pmm_free(1);
        auto *fl = reinterpret_cast<uint64_t *>(free_list_);
        // Pool pages must go back to the pool freelist, not the general
        // list.  But guard against uninitialized pool range (boot-time
        // free_page calls before pool is set up).
        uint64_t pool_start = page_table_pool_start_;
        uint64_t pool_end = page_table_pool_end_;
        if (pool_start != 0 && pool_end != 0) {
            uint64_t ps = pool_start / PAGE_SIZE;
            uint64_t pe = pool_end / PAGE_SIZE;
            if (index >= ps && index < pe) {
                fl[index] = pool_free_head_;
                pool_free_head_ = index;
                return;
            }
        }
        fl[index] = free_head_;
        free_head_ = index;
    }
}

/// @brief Check if a physical page is currently allocated.
/// @param phys_addr Physical address.
/// @return true if allocated.
bool PMM::is_allocated(uint64_t phys_addr) {
    uint64_t index = phys_addr / arch::PAGE_SIZE;
    if (index >= total_pages_)
        return false;
    return bitmap_test(index);
}

/// @brief Check if a physical page was allocated as USER-owned.
/// @param phys_addr Physical address.
/// @return true if USER-owned.
bool PMM::is_user_page(uint64_t phys_addr) {
    uint64_t index = phys_addr / arch::PAGE_SIZE;
    if (index >= total_pages_)
        return false;
    return owner_test(index);
}

/// @brief Set the allocation bit for a page index.
/// @param index Page index.
void PMM::bitmap_set(size_t index) {
    ENSURE(index < total_pages_);
    // NOLINTNEXTLINE(performance-no-int-to-ptr)
    auto *bitmap = reinterpret_cast<uint8_t *>(bitmap_);
    bitmap[index / 8] |= static_cast<uint8_t>(1 << (index % 8));
}

/// @brief Clear the allocation bit for a page index (mark free).
/// @param index Page index.
void PMM::bitmap_clear(size_t index) {
    ENSURE(index < total_pages_);
    // NOLINTNEXTLINE(performance-no-int-to-ptr)
    auto *bitmap = reinterpret_cast<uint8_t *>(bitmap_);
    bitmap[index / 8] &= static_cast<uint8_t>(~(1 << (index % 8)));
}

/// @brief Test the allocation bit for a page index.
/// @param index Page index.
/// @return true if allocated.
bool PMM::bitmap_test(size_t index) {
    ENSURE(index < total_pages_);
    // NOLINTNEXTLINE(performance-no-int-to-ptr)
    auto *bitmap = reinterpret_cast<uint8_t *>(bitmap_);
    return (bitmap[index / 8] >> (index % 8)) & 1;
}

/// @brief Mark a page as USER-owned in the owner bitmap.
/// @param index Page index.
void PMM::owner_set_user(size_t index) {
    ENSURE(index < total_pages_);
    // NOLINTNEXTLINE(performance-no-int-to-ptr)
    auto *owner = reinterpret_cast<uint8_t *>(owner_bitmap_);
    owner[index / 8] |= static_cast<uint8_t>(1 << (index % 8));
}

/// @brief Mark a page as KERNEL-owned in the owner bitmap.
/// @param index Page index.
void PMM::owner_set_kernel(size_t index) {
    ENSURE(index < total_pages_);
    // NOLINTNEXTLINE(performance-no-int-to-ptr)
    auto *owner = reinterpret_cast<uint8_t *>(owner_bitmap_);
    owner[index / 8] &= static_cast<uint8_t>(~(1 << (index % 8)));
}

/// @brief Test whether a page is USER-owned.
/// @param index Page index.
/// @return true if USER-owned, false if KERNEL-owned.
bool PMM::owner_test(size_t index) {
    ENSURE(index < total_pages_);
    // NOLINTNEXTLINE(performance-no-int-to-ptr)
    auto *owner = reinterpret_cast<uint8_t *>(owner_bitmap_);
    return (owner[index / 8] >> (index % 8)) & 1;
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
/// @brief Initialise PMM with error-code return.
/// @param mem_size    Total physical memory in bytes.
/// @param kernel_start Physical address of kernel image start.
/// @param kernel_end   Physical address of kernel image end.
/// @param window_base  Physical base of the allocatable RAM window.
/// @return PmmError code.
/// @note Unlike init(), this variant does not build the O(1) free list
///       (pre-existing behavior; no current callers).
errors::PmmError PMM::init_err(uint64_t mem_size, uint64_t kernel_start,
                               uint64_t kernel_end, uint64_t window_base) {
    sync::IrqSpinLockGuard lock(pmm_lock_);
    total_pages_ = mem_size / PAGE_SIZE;
    free_pages_ = total_pages_;

    WindowPages win = compute_window_pages(mem_size, window_base);
    window_base_page_ = win.base_page;
    window_end_page_ = win.end_page;

    bitmap_size_ = align_up<uint64_t>(total_pages_ / 8, 8_KiB);
    uint64_t bitmap_phys = align_up<uint64_t>(kernel_end, 8_KiB);
    uint64_t owner_bitmap_phys = bitmap_phys + bitmap_size_;
    bitmap_ = arch::HHDM_OFFSET + bitmap_phys;
    owner_bitmap_ = arch::HHDM_OFFSET + owner_bitmap_phys;

    // NOLINTNEXTLINE(performance-no-int-to-ptr)
    auto *bitmap = reinterpret_cast<uint8_t *>(bitmap_);
    // NOLINTNEXTLINE(performance-no-int-to-ptr)
    auto *owner = reinterpret_cast<uint8_t *>(owner_bitmap_);
    for (uint64_t i = 0; i < bitmap_size_; ++i) {
        bitmap[i] = 0;
        owner[i] = 0;
    }

    uint64_t kernel_start_page = kernel_start / PAGE_SIZE;
    uint64_t reserved_end = bitmap_phys + bitmap_size_ + bitmap_size_;
    uint64_t reserved_end_page = (reserved_end + PAGE_SIZE - 1) / PAGE_SIZE;

    for (uint64_t i = 0; i < kernel_start_page; ++i) {
        bitmap_set(i);
        owner_set_kernel(i);
        --free_pages_;
    }

    for (uint64_t i = kernel_start_page; i < reserved_end_page; ++i) {
        bitmap_set(i);
        owner_set_kernel(i);
        --free_pages_;
    }

    uint64_t window_pages = window_end_page_ - window_base_page_;
    uint64_t pool_size_pages = CONFIG_PAGE_TABLE_POOL_SIZE;
    if (window_pages > pool_size_pages + 16) {
        uint64_t pool_start_page = window_base_page_ + window_pages -
                                   pool_size_pages - 16;
        if (pool_start_page > reserved_end_page) {
            page_table_pool_start_ = pool_start_page * PAGE_SIZE;
            page_table_pool_end_ =
                (pool_start_page + pool_size_pages) * PAGE_SIZE;
        }
    }

    if (total_pages_ > 16) {
        for (uint64_t i = total_pages_ - 1; i >= total_pages_ - 16; --i) {
            if (!bitmap_test(i)) {
                bitmap_set(i);
                owner_set_kernel(i);
                --free_pages_;
            }
        }
    }

    return errors::PMM_ERR_OK;
}

/// @brief Allocate a single KERNEL page with error-code return.
/// @param[out] out_phys_addr Physical address on success.
/// @return PmmError code.
errors::PmmError PMM::alloc_page_err(uint64_t &out_phys_addr) {
    sync::IrqSpinLockGuard lock(pmm_lock_);
    uint64_t result = try_alloc_kernel(1);
    if (result) {
        kernel::test::ResourceTracker::instance().track_pmm_alloc(1);
        out_phys_addr = result;
        return errors::PMM_ERR_OK;
    }
    bool oom_retry = false;
    if (oom_handler_) {
        lock.unlock();
        oom_retry = oom_handler_();
        lock.lock();
    }
    if (oom_retry) {
        result = try_alloc_kernel(1);
    }
    if (!result) {
        return errors::PMM_ERR_OOM;
    }
    kernel::test::ResourceTracker::instance().track_pmm_alloc(1);
    out_phys_addr = result;
    return errors::PMM_ERR_OK;
}

/// @brief Allocate contiguous KERNEL pages with error-code return.
/// @param count Number of pages.
/// @param[out] out_phys_addr Physical address on success.
/// @return PmmError code.
errors::PmmError PMM::alloc_contiguous_err(size_t count,
                                           uint64_t &out_phys_addr) {
    if (count == 0 || count > total_pages_) {
        return errors::PMM_ERR_INVALID;
    }
    sync::IrqSpinLockGuard lock(pmm_lock_);
    uint64_t result = try_alloc_kernel(count);
    if (result) {
        kernel::test::ResourceTracker::instance().track_pmm_alloc(count);
        out_phys_addr = result;
        return errors::PMM_ERR_OK;
    }
    bool oom_retry = false;
    if (oom_handler_) {
        lock.unlock();
        oom_retry = oom_handler_();
        lock.lock();
    }
    if (oom_retry) {
        result = try_alloc_kernel(count);
    }
    if (!result) {
        return errors::PMM_ERR_OOM;
    }
    kernel::test::ResourceTracker::instance().track_pmm_alloc(count);
    out_phys_addr = result;
    return errors::PMM_ERR_OK;
}

/// @brief Allocate a single USER page with error-code return.
/// @param[out] out_phys_addr Physical address on success.
/// @return PmmError code.
errors::PmmError PMM::alloc_user_page_err(uint64_t &out_phys_addr) {
    sync::IrqSpinLockGuard lock(pmm_lock_);
    uint64_t result = try_alloc_user(1);
    if (result) {
        kernel::test::ResourceTracker::instance().track_pmm_alloc(1);
        out_phys_addr = result;
        return errors::PMM_ERR_OK;
    }
    bool oom_retry = false;
    if (oom_handler_) {
        lock.unlock();
        oom_retry = oom_handler_();
        lock.lock();
    }
    if (oom_retry) {
        result = try_alloc_user(1);
    }
    if (!result) {
        return errors::PMM_ERR_USER_OOM;
    }
    kernel::test::ResourceTracker::instance().track_pmm_alloc(1);
    out_phys_addr = result;
    return errors::PMM_ERR_OK;
}

/// @brief Allocate contiguous USER pages with error-code return.
/// @param count Number of pages.
/// @param[out] out_phys_addr Physical address on success.
/// @return PmmError code.
errors::PmmError PMM::alloc_user_contiguous_err(size_t count,
                                                uint64_t &out_phys_addr) {
    if (count == 0 || count > total_pages_) {
        return errors::PMM_ERR_INVALID;
    }
    sync::IrqSpinLockGuard lock(pmm_lock_);
    uint64_t result = try_alloc_user(count);
    if (result) {
        kernel::test::ResourceTracker::instance().track_pmm_alloc(count);
        out_phys_addr = result;
        return errors::PMM_ERR_OK;
    }
    bool oom_retry = false;
    if (oom_handler_) {
        lock.unlock();
        oom_retry = oom_handler_();
        lock.lock();
    }
    if (oom_retry) {
        result = try_alloc_user(count);
    }
    if (!result) {
        return errors::PMM_ERR_USER_OOM;
    }
    kernel::test::ResourceTracker::instance().track_pmm_alloc(count);
    out_phys_addr = result;
    return errors::PMM_ERR_OK;
}

/// @brief Allocate a page-table page (reserved pool, then fallback) with
/// error-code return.
/// @param[out] out_phys_addr Physical address on success.
/// @return PmmError code.
errors::PmmError PMM::alloc_page_table_err(uint64_t &out_phys_addr) {
    if (page_table_pool_start_ == 0 || page_table_pool_end_ == 0) {
        return alloc_page_err(out_phys_addr);
    }
    uint64_t pool_start_page = page_table_pool_start_ / PAGE_SIZE;
    uint64_t pool_end_page = page_table_pool_end_ / PAGE_SIZE;
    {
        sync::IrqSpinLockGuard lock(pmm_lock_);
        for (uint64_t i = pool_start_page; i < pool_end_page; ++i) {
            if (!bitmap_test(i)) {
                bitmap_set(i);
                owner_set_kernel(i);
                --free_pages_;
                kernel::test::ResourceTracker::instance().track_pmm_alloc(1);
                out_phys_addr = i * PAGE_SIZE;
                return errors::PMM_ERR_OK;
            }
        }
    }
    auto err = alloc_page_err(out_phys_addr);
    if (err != errors::PMM_ERR_OK) {
        return errors::PMM_ERR_TABLE_OOM;
    }
    return errors::PMM_ERR_OK;
}

/// @brief Free a physical page with error-code return.
/// @param phys_addr Physical address to free.
/// @return PmmError code.
errors::PmmError PMM::free_page_err(uint64_t phys_addr) {
    sync::IrqSpinLockGuard lock(pmm_lock_);
    uint64_t index = phys_addr / PAGE_SIZE;
    if (index >= total_pages_ || index < window_base_page_) {
        return errors::PMM_ERR_INVALID;
    }
    if (bitmap_test(index)) {
        bitmap_clear(index);
        ++free_pages_;
        kernel::test::ResourceTracker::instance().track_pmm_free(1);
    }
    return errors::PMM_ERR_OK;
}

/// @brief Check USER ownership with error-code return.
/// @param phys_addr Physical address.
/// @param[out] out_is_user Set to true if USER-owned.
/// @return PmmError code.
errors::PmmError PMM::is_user_page_err(uint64_t phys_addr, bool &out_is_user) {
    uint64_t index = phys_addr / arch::PAGE_SIZE;
    if (index >= total_pages_) {
        return errors::PMM_ERR_INVALID;
    }
    out_is_user = owner_test(index);
    return errors::PMM_ERR_OK;
}

// ---- Page-table pool snapshot (test isolation) ----

void PMM::capture_pool_snapshot(
    kernel::test::PtPoolSnapshot &out) {
    sync::IrqSpinLockGuard lock(pmm_lock_);
    out.base       = page_table_pool_start_;
    out.size_pages = (page_table_pool_end_ - page_table_pool_start_) / PAGE_SIZE;
    out.clean      = true;
    out.mapped     = true;
    out.tainted    = pool_tainted_;
    out.poisoned   = pool_poisoned_;
    pool_bump_generation();
    out.generation = pool_generation_;
    out.refcount   = pool_refcount_;
    out.crc32      = 0;

    if (out.size_pages == 0 ||
        out.size_pages > sizeof(out.bitmap) * 8) {
        out.size_pages = 0;
        return;
    }
    uint64_t start_bit = page_table_pool_start_ / PAGE_SIZE;
    uint64_t bytes = (out.size_pages + 7) / 8;
    __builtin_memset(out.bitmap, 0, sizeof(out.bitmap));
    __builtin_memset(out.owner, 0, sizeof(out.owner));
    __builtin_memcpy(out.bitmap,
                     reinterpret_cast<uint8_t *>(bitmap_) + start_bit / 8,
                     bytes);
    __builtin_memcpy(out.owner,
                     reinterpret_cast<uint8_t *>(owner_bitmap_) + start_bit / 8,
                     bytes);
}

void PMM::restore_pool_snapshot(
    const kernel::test::PtPoolSnapshot &src) {
    if (src.size_pages == 0 || src.poisoned) {
        return; // refuse to restore a corrupted pool
    }
    sync::IrqSpinLockGuard lock(pmm_lock_);
    uint64_t start_bit = page_table_pool_start_ / PAGE_SIZE;
    size_t max_bytes = sizeof(src.bitmap);
    size_t bytes = (src.size_pages + 7) / 8;
    if (bytes > max_bytes)
        bytes = max_bytes;
    __builtin_memcpy(reinterpret_cast<uint8_t *>(bitmap_) + start_bit / 8,
                     src.bitmap, bytes);
    __builtin_memcpy(reinterpret_cast<uint8_t *>(owner_bitmap_) + start_bit / 8,
                     src.owner, bytes);
}

uint64_t PMM::pool_used_pages() noexcept {
    if (page_table_pool_start_ == 0 || page_table_pool_end_ == 0)
        return 0;
    uint64_t start = page_table_pool_start_ / PAGE_SIZE;
    uint64_t end   = page_table_pool_end_   / PAGE_SIZE;
    uint64_t count = 0;
    for (uint64_t i = start; i < end; ++i) {
        if (bitmap_test(i))
            ++count;
    }
    return count;
}

uint64_t PMM::pool_total_pages() noexcept {
    if (page_table_pool_start_ == 0 || page_table_pool_end_ == 0)
        return 0;
    return (page_table_pool_end_ - page_table_pool_start_) / PAGE_SIZE;
}

} // namespace kernel
