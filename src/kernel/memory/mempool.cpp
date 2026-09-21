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

#include <kernel/memory/mempool.hpp>
#include <kernel/memory/pmm.hpp>
#include <kernel/test/resource_tracker.hpp>
#include <kernel/sync/irq_spinlock_guard.hpp>
#include <logger.hpp>
#include <assert.hpp>
#include <constants.hpp>

namespace kernel {

static sync::SpinLock mempool_lock_{};

MemPool::Pool MemPool::pools_[POOL_COUNT] = {};
constinit bool MemPool::ready_ = false;

/// @brief Initialise all 9 pool classes from page-aligned PMM allocations.
///        Each pool carves its region into fixed-size blocks and builds an
///        embedded free list.
void MemPool::init() {
    sync::IrqSpinLockGuard lock(mempool_lock_);

    static const size_t sizes[POOL_COUNT] = {16,  32,   64,   128, 256,
                                              512, 1024, 2048,
#if defined(CONFIG_ARCH_AARCH64)
                                              // Issue #104: aarch64
                                              // TaskControlBlock is 8256 B
                                              // (larger ArchContext + debug
                                              // ring) — pool8 must clear it.
                                              16384};
#else
                                              8192};
#endif
    static const size_t counts[POOL_COUNT] = {256, 128, 320, 32, 16,
                                              8,   16,  64, 64};

    for (size_t i = 0; i < POOL_COUNT; ++i) {
        auto &pool = pools_[i];
        pool.block_size = sizes[i];
        pool.block_count = counts[i];
        pool.free_count = counts[i];
        pool.first_free = 0;

        size_t pool_bytes = pool.block_count * pool.block_size;
        size_t pages = (pool_bytes + arch::PAGE_SIZE - 1) / arch::PAGE_SIZE;
        uint64_t phys = PMM::alloc_contiguous(pages);
        ENSURE(phys != 0);

        // NOLINTNEXTLINE(performance-no-int-to-ptr)
        pool.data = reinterpret_cast<uint8_t *>(phys + arch::HHDM_OFFSET);

        pool.first_free = 0;
        for (size_t j = 0; j < pool.block_count; ++j) {
            // NOLINTNEXTLINE(performance-no-int-to-ptr)
            size_t *next =
                reinterpret_cast<size_t *>(pool.data + j * pool.block_size);
            *next = j + 1;
            pool.set_block_freed(j);
        }
        {
            size_t *next = reinterpret_cast<size_t *>(
                pool.data + (pool.block_count - 1) * pool.block_size);
            *next = static_cast<size_t>(-1);
        }

        pool.initialized = true;
        pool.clear_pinned_bitmap();
    }
    ready_ = true;
}

/// @brief Allocate a block from the smallest pool class that fits @p size.
/// @param size Minimum number of bytes required.
/// @return Pointer to the block, or nullptr on failure.
void *MemPool::alloc(size_t size) {
    sync::IrqSpinLockGuard lock(mempool_lock_);
    size_t idx = find_pool(size);
    if (idx >= POOL_COUNT)
        return nullptr;

    auto &pool = pools_[idx];
    if (pool.free_count == 0)
        return nullptr;

    size_t block = pool.first_free;
    ENSURE(block < pool.block_count);
    ENSURE(pool.is_block_freed(block) &&
           "free-list corruption: alloc of already-allocated block");
    size_t *next =
        reinterpret_cast<size_t *>(pool.data + block * pool.block_size);
    pool.first_free = *next;
    --pool.free_count;
    pool.clear_block_freed(block);

    kernel::test::ResourceTracker::instance().track_mempool_alloc(idx);
    return pool.data + block * pool.block_size;
}

/// @brief Return a previously allocated block to its pool.
/// @param block Pointer returned by a previous alloc() call (null is a no-op).
void MemPool::free(void *block) {
    if (!block)
        return;

    sync::IrqSpinLockGuard lock(mempool_lock_);

    for (size_t i = 0; i < POOL_COUNT; ++i) {
        auto &pool = pools_[i];
        if (!pool.initialized)
            continue;

        uint8_t *start = pool.data;
        uint8_t *end = pool.data + pool.block_size * pool.block_count;
        uint8_t *p = static_cast<uint8_t *>(block);

        if (p >= start && p < end) {
            size_t offset = static_cast<size_t>(p - start);
            size_t block_idx = offset / pool.block_size;
            ENSURE(offset % pool.block_size == 0);
            ENSURE(block_idx < pool.block_count);
            // Pinned blocks are reserved (e.g. baseline TCBs referenced by the
            // test-isolation snapshot).  Never return them to the free list;
            // keep them allocated so they cannot be recycled onto test tasks.
            if (pool.is_block_pinned(block_idx)) {
                Logger::warn("MemPool::free: pinned block %zu in pool %zu",
                             block_idx, i);
                return;
            }
            ENSURE(!pool.is_block_freed(block_idx) && "double-free detected");
#ifdef CONFIG_DEBUG
            __builtin_memset(p, 0xDD, pool.block_size);
#endif
            pool.set_block_freed(block_idx);
            size_t *next = reinterpret_cast<size_t *>(p);
            *next = pool.first_free;
            pool.first_free = block_idx;
            ++pool.free_count;
            kernel::test::ResourceTracker::instance().track_mempool_free(i);
            return;
        }
    }
}
/// @param ptr Pointer to test.
/// @return true if the pointer is owned by any initialised pool.
bool MemPool::contains(void *ptr) {
    if (!ptr)
        return false;
    uint8_t *p = static_cast<uint8_t *>(ptr);
    for (size_t i = 0; i < POOL_COUNT; ++i) {
        auto &pool = pools_[i];
        if (!pool.initialized)
            continue;
        uint8_t *start = pool.data;
        uint8_t *end = pool.data + pool.block_size * pool.block_count;
        if (p >= start && p < end)
            return true;
    }
    return false;
}

/// @brief Find the smallest initialised pool class whose block_size >= size.
/// @param size Requested allocation size.
/// @return Pool index, or (size_t)-1 if no pool fits.
size_t MemPool::find_pool(size_t size) {
    for (size_t i = 0; i < POOL_COUNT; ++i) {
        if (pools_[i].block_size >= size && pools_[i].initialized) {
            return i;
        }
    }
    return static_cast<size_t>(-1);
}

// ---------------------------------------------------------------------------
// Test-isolation helpers
// ---------------------------------------------------------------------------

/// @brief Snapshot the free-list head, free count, and freed bitmap for a pool.
/// @param idx Pool index.
/// @param[out] out Destination to fill with metadata.
void MemPool::capture_pool_meta(size_t idx, PoolMeta &out) {
    sync::IrqSpinLockGuard lock(mempool_lock_);
    auto &p = pools_[idx];
    out.first_free = p.first_free;
    out.free_count = p.free_count;
    out.block_count = p.block_count;
    out.block_size = p.block_size;
    out.data = p.data;
    p.copy_freed_bitmap(out.freed_bitmap);
    p.copy_pinned_bitmap(out.pinned_bitmap);
}

/// @brief Restore a pool's metadata from a snapshot and rebuild the free list.
///        Any blocks added since the snapshot are marked free.
/// @param idx  Pool index.
/// @param meta Previously captured metadata.
void MemPool::restore_pool_meta(size_t idx, const PoolMeta &meta) {
    sync::IrqSpinLockGuard lock(mempool_lock_);
    auto &p = pools_[idx];

    // Restore block count FIRST so all subsequent operations use the
    // correct (saved) count.  If a test corrupted p.block_count, the
    // freed bitmap and free-list rebuild would be based on garbage.
    p.block_count = meta.block_count;
    p.block_size = meta.block_size;
    p.data = meta.data;

    p.write_freed_bitmap(meta.freed_bitmap);
    // Restore pinned state too: baseline TCB pins (captured at snapshot time,
    // AFTER snapshot_create's pin_block loop) are preserved, while pins added
    // by a test (e.g. reserve-all in static_pools) are rolled back so the pool
    // is not permanently starved across test cycles.
    p.write_pinned_bitmap(meta.pinned_bitmap);

    // If the pool was resized after the snapshot, mark new blocks free.
    if (p.block_count > meta.block_count) {
        for (size_t j = meta.block_count; j < p.block_count; ++j) {
            p.set_block_freed(j);
        }
    }

    // Rebuild the free-list next pointers from the bitmap so the pool is
    // internally consistent even if test code modified freed blocks.
    p.first_free = static_cast<size_t>(-1);
    p.free_count = 0;
    for (size_t j = 0; j < p.block_count; ++j) {
        if (p.is_block_pinned(j))
            continue;
        if (!p.is_block_freed(j))
            continue;
        auto *next = reinterpret_cast<uint64_t *>(p.data + j * p.block_size);
        *next = p.first_free;
        p.first_free = j;
        ++p.free_count;
    }
    // If the pool was completely full, first_free stays -1 — correct.
}

/// @brief Copy all pool block data into a contiguous buffer.
/// @param[out] dst Destination buffer (must be >= pool_data_bytes()).
void MemPool::capture_pool_data(uint8_t *dst) {
    sync::IrqSpinLockGuard lock(mempool_lock_);
    for (size_t i = 0; i < POOL_COUNT; ++i) {
        auto &p = pools_[i];
        if (!p.initialized || !p.data)
            continue;
        size_t bytes = p.block_count * p.block_size;
        __builtin_memcpy(dst, p.data, bytes);
        dst += bytes;
    }
}

/// @brief Restore all pool block data from a contiguous buffer.
/// @param src Source buffer previously filled by capture_pool_data().
void MemPool::restore_pool_data(const uint8_t *src) {
    sync::IrqSpinLockGuard lock(mempool_lock_);
    for (size_t i = 0; i < POOL_COUNT; ++i) {
        auto &p = pools_[i];
        if (!p.initialized || !p.data)
            continue;
        size_t bytes = p.block_count * p.block_size;
        __builtin_memcpy(p.data, src, bytes);
        src += bytes;
    }
}

/// @brief Compute total bytes across all initialised pools.
/// @return Sum of (block_count * block_size) for each pool.
size_t MemPool::pool_data_bytes() {
    size_t total = 0;
    for (size_t i = 0; i < POOL_COUNT; ++i) {
        if (!pools_[i].initialized)
            continue;
        total += pools_[i].block_count * pools_[i].block_size;
    }
    return total;
}

// ---------------------------------------------------------------------------
// Block pinning (test-isolation)
// ---------------------------------------------------------------------------

void MemPool::pin_block(void *block) {
    sync::IrqSpinLockGuard lock(mempool_lock_);
    uint8_t *p = static_cast<uint8_t *>(block);
    for (size_t i = 0; i < POOL_COUNT; ++i) {
        auto &pool = pools_[i];
        if (!pool.initialized)
            continue;
        uint8_t *start = pool.data;
        uint8_t *end = pool.data + pool.block_size * pool.block_count;
        if (p >= start && p < end) {
            size_t offset = static_cast<size_t>(p - start);
            size_t block_idx = offset / pool.block_size;
            if (offset % pool.block_size == 0 && block_idx < pool.block_count) {
                ENSURE(!pool.is_block_freed(block_idx) &&
                       "pin_block: cannot pin a free-list block");
                pool.set_block_pinned(block_idx);
            }
            return;
        }
    }
}

void MemPool::unpin_block(void *block) {
    sync::IrqSpinLockGuard lock(mempool_lock_);
    uint8_t *p = static_cast<uint8_t *>(block);
    for (size_t i = 0; i < POOL_COUNT; ++i) {
        auto &pool = pools_[i];
        if (!pool.initialized)
            continue;
        uint8_t *start = pool.data;
        uint8_t *end = pool.data + pool.block_size * pool.block_count;
        if (p >= start && p < end) {
            size_t offset = static_cast<size_t>(p - start);
            size_t block_idx = offset / pool.block_size;
            if (offset % pool.block_size == 0 && block_idx < pool.block_count)
                pool.clear_block_pinned(block_idx);
            return;
        }
    }
}

bool MemPool::is_block_pinned(void *block) {
    uint8_t *p = static_cast<uint8_t *>(block);
    for (size_t i = 0; i < POOL_COUNT; ++i) {
        auto &pool = pools_[i];
        if (!pool.initialized)
            continue;
        uint8_t *start = pool.data;
        uint8_t *end = pool.data + pool.block_size * pool.block_count;
        if (p >= start && p < end) {
            size_t offset = static_cast<size_t>(p - start);
            size_t block_idx = offset / pool.block_size;
            if (offset % pool.block_size == 0 && block_idx < pool.block_count)
                return pool.is_block_pinned(block_idx);
            return false;
        }
    }
    return false;
}

} // namespace kernel

// --- Error-returning overloads ---
namespace kernel {

using namespace errors;

/// @brief Initialise all pools and return OK.
/// @return MEMPOOL_ERR_OK after successful init.
MemPoolError MemPool::init_err() {
    init();
    return MEMPOOL_ERR_OK;
}

/// @brief Allocate a block with error-code return instead of nullptr.
/// @param size Minimum bytes required.
/// @param[out] out_ptr Set to the allocated block on success.
/// @return MemPoolError code.
MemPoolError MemPool::alloc_err(size_t size, void *&out_ptr) {
    sync::IrqSpinLockGuard lock(mempool_lock_);

    size_t idx = find_pool(size);
    if (idx >= POOL_COUNT) {
        return MEMPOOL_ERR_TOO_LARGE;
    }

    auto &pool = pools_[idx];
    if (pool.free_count == 0) {
        return MEMPOOL_ERR_OOM;
    }

    size_t block = pool.first_free;
    ENSURE(block < pool.block_count);
    ENSURE(pool.is_block_freed(block) &&
           "free-list corruption: alloc of already-allocated block");
    size_t *next =
        reinterpret_cast<size_t *>(pool.data + block * pool.block_size);
    pool.first_free = *next;
    --pool.free_count;
    pool.clear_block_freed(block);

    kernel::test::ResourceTracker::instance().track_mempool_alloc(idx);
    out_ptr = pool.data + block * pool.block_size;
    return MEMPOOL_ERR_OK;
}

/// @brief Free a block with error-code return.
/// @param block Pointer to free (null returns MEMPOOL_ERR_INVALID_PTR).
/// @return MemPoolError code.
MemPoolError MemPool::free_err(void *block) {
    if (!block) {
        return MEMPOOL_ERR_INVALID_PTR;
    }

    sync::IrqSpinLockGuard lock(mempool_lock_);

    for (size_t i = 0; i < POOL_COUNT; ++i) {
        auto &pool = pools_[i];
        if (!pool.initialized)
            continue;

        uint8_t *start = pool.data;
        uint8_t *end = pool.data + pool.block_size * pool.block_count;
        uint8_t *p = static_cast<uint8_t *>(block);

        if (p >= start && p < end) {
            size_t offset = static_cast<size_t>(p - start);
            size_t block_idx = offset / pool.block_size;
            ENSURE(offset % pool.block_size == 0);
            ENSURE(block_idx < pool.block_count);
            // Pinned blocks stay allocated (see MemPool::free).
            if (pool.is_block_pinned(block_idx))
                return MEMPOOL_ERR_OK;
            ENSURE(!pool.is_block_freed(block_idx) && "double-free detected");
#ifdef CONFIG_DEBUG
            __builtin_memset(p, 0xDD, pool.block_size);
#endif
            pool.set_block_freed(block_idx);
            size_t *next = reinterpret_cast<size_t *>(p);
            *next = pool.first_free;
            pool.first_free = block_idx;
            ++pool.free_count;
            kernel::test::ResourceTracker::instance().track_mempool_free(i);
            return MEMPOOL_ERR_OK;
        }
    }
    return MEMPOOL_ERR_INVALID_PTR;
}

MemPoolError MemPool::reserve(size_t pool_idx, size_t count) {
    sync::IrqSpinLockGuard lock(mempool_lock_);
    if (pool_idx >= POOL_COUNT)
        return MEMPOOL_ERR_OOM;
    auto &pool = pools_[pool_idx];
    if (count > pool.free_count)
        return MEMPOOL_ERR_OOM;
    size_t reserved = 0;
    for (size_t i = 0; i < pool.block_count && reserved < count; ++i) {
        if (pool.is_block_freed(i)) {
            pool.set_block_pinned(i);
            --pool.free_count;
            pool.clear_block_freed(i);
            ++reserved;
        }
    }
    return reserved == count ? MEMPOOL_ERR_OK : MEMPOOL_ERR_OOM;
}

} // namespace kernel
