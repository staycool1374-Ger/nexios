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

/// @file mempool.hpp
/// @brief Fixed-size block memory pool allocator (k malloc).  O(1) alloc/free
///        via embedded free-list per pool class (9 classes, 16–4480 bytes).

#include <types.hpp>
#include <kernel/memory/mempool_errors.hpp>

namespace kernel {

/// @brief Fixed-size block memory allocator with 9 pool classes.
///
/// Each pool is a contiguous page-aligned region partitioned into fixed-size
/// blocks.  Free blocks are linked via an embedded singly-linked free-list
/// (the first 8 bytes of each free block store the next index).
///
/// @note O(1) allocation: find_pool (linear scan of 9 classes) + pop from
///       free-list head.  O(1) free: linear scan of 9 pools to find owner
///       + push to free-list head.  No bitmap scans or dynamic allocation.
class MemPool {
  public:
    static constexpr size_t POOL_COUNT = 9;

    /// @brief Describes a single pool of fixed-size blocks.
    /// @note Free-list is embedded in the data pages (each free
    /// block's first 8 bytes store the next index).
    struct Pool {
        size_t block_size;
        size_t block_count;
        size_t free_count;
        uint8_t *data;
        size_t first_free;
        bool initialized;

        /// @brief Default constructor — zero-initialises all fields.
        Pool()
            : block_size(0), block_count(0), free_count(0), data(nullptr),
              first_free(0), initialized(false) {
        }

        /// @brief Check if a block index is marked as freed in the bitmap.
        /// @param idx Block index.
        /// @return true if the block is free.
        bool is_block_freed(size_t idx) const {
            return freed_bitmap[idx / 64] & (1ULL << (idx % 64));
        }

        /// @brief Mark a block index as freed in the bitmap.
        /// @param idx Block index.
        void set_block_freed(size_t idx) {
            freed_bitmap[idx / 64] |= (1ULL << (idx % 64));
        }

        /// @brief Clear the freed flag for a block index (mark as allocated).
        /// @param idx Block index.
        void clear_block_freed(size_t idx) {
            freed_bitmap[idx / 64] &= ~(1ULL << (idx % 64));
        }

        /// @brief Copy the freed bitmap to an external buffer.
        /// @param[out] dst Destination array (5 x uint64_t).
        void copy_freed_bitmap(uint64_t *dst) const {
            for (int i = 0; i < 5; ++i)
                dst[i] = freed_bitmap[i];
        }

        /// @param src Source array (5 x uint64_t).
        void write_freed_bitmap(const uint64_t *src) {
            for (int i = 0; i < 5; ++i)
                freed_bitmap[i] = src[i];
        }
        /// @brief Copy the pinned bitmap to an external buffer.
        /// @param[out] dst Destination array (5 x uint64_t).
        void copy_pinned_bitmap(uint64_t *dst) const {
            for (int i = 0; i < 5; ++i)
                dst[i] = pinned_bitmap[i];
        }

        /// @param src Source array (5 x uint64_t).
        void write_pinned_bitmap(const uint64_t *src) {
            for (int i = 0; i < 5; ++i)
                pinned_bitmap[i] = src[i];
        }
        /// @brief Clear all pinned flags.
        void clear_pinned_bitmap() {
            for (int i = 0; i < 5; ++i)
                pinned_bitmap[i] = 0;
        }

        /// @brief Check if a block index is pinned (never freed / reused).
        /// @param idx Block index.
        /// @return true if the block is pinned.
        bool is_block_pinned(size_t idx) const {
            return pinned_bitmap[idx / 64] & (1ULL << (idx % 64));
        }

        /// @brief Mark a block index as pinned.
        /// @param idx Block index.
        void set_block_pinned(size_t idx) {
            pinned_bitmap[idx / 64] |= (1ULL << (idx % 64));
        }

        /// @brief Clear the pinned flag for a block index.
        /// @param idx Block index.
        void clear_block_pinned(size_t idx) {
            pinned_bitmap[idx / 64] &= ~(1ULL << (idx % 64));
        }

      private:
        uint64_t freed_bitmap[5]; // 320 bits — covers largest pool (320 blocks)
        uint64_t pinned_bitmap[5]; // 320 bits — covers largest pool (320 blocks)
    };

    /// @brief Initialises all pool classes from pre-allocated memory.
    static void init();
    /// @brief Initialises with error code.
    /// @return MemPoolError code.
    static errors::MemPoolError init_err();

    /// @brief Allocates a block of at least the requested size.
    /// @param size Minimum number of bytes.
    /// @return Pointer to the allocated block, or nullptr.
    static void *alloc(size_t size);
    /// @brief Allocates a block with error code.
    /// @param size Minimum number of bytes.
    /// @param[out] out_ptr Pointer to allocated block on success.
    /// @return MemPoolError code.
    static errors::MemPoolError alloc_err(size_t size, void *&out_ptr);

    /// @brief Frees a block previously returned by alloc().
    /// @param block Pointer to the block to free.
    static void free(void *block);
    /// @brief Frees a block with error code.
    /// @param block Pointer to the block to free.
    /// @return MemPoolError code.
    static errors::MemPoolError free_err(void *block);

    /// @brief Reserve (pin) `count` blocks in pool `pool_idx` at init time.
    static errors::MemPoolError reserve(size_t pool_idx, size_t count);

    /// @brief Check if a pointer falls within any MemPool pool range.
    /// @param ptr Pointer to check.
    /// @return true if ptr is owned by MemPool.
    static bool contains(void *ptr);

    /// @brief Check whether the MemPool subsystem has been initialised.
    /// @return true after init() completes successfully.
    static bool is_ready() {
        return ready_;
    }

    /// @name Test-isolation helpers (snapshot / restore)
    struct PoolMeta {
        size_t first_free;
        size_t free_count;
        size_t block_count;
        size_t block_size;
        uint8_t *data;
        uint64_t freed_bitmap[5]; // 320 bits — covers largest pool (320 blocks)
        uint64_t pinned_bitmap[5]; // 320 bits — pinned (never freed/reused)
    };
    /// @brief Return the number of pool classes.
    static size_t pool_count() {
        return POOL_COUNT;
    }
    /// @brief Return the free block count for a given pool.
    /// @param idx Pool index (0..POOL_COUNT-1).
    /// @return Number of free blocks in that pool.
    static size_t pool_free_count(size_t idx) {
        return pools_[idx].free_count;
    }
    /// @brief Snapshot the metadata of one pool (free-list head, free count,
    /// bitmap).
    /// @param idx  Pool index.
    /// @param[out] out  Filled with the pool's current metadata.
    static void capture_pool_meta(size_t idx, PoolMeta &out);
    /// @brief Restore a pool's metadata from a previous snapshot and rebuild
    /// its free list.
    /// @param idx  Pool index.
    /// @param meta Previously captured metadata.
    static void restore_pool_meta(size_t idx, const PoolMeta &meta);
    /// @brief Copy all pool block data into a contiguous external buffer.
    /// @param[out] dst  Destination buffer (size must be >= pool_data_bytes()).
    static void capture_pool_data(uint8_t *dst);
    /// @brief Overwrite all pool block data from a contiguous external buffer.
    /// @param src  Source buffer previously filled by capture_pool_data().
    static void restore_pool_data(const uint8_t *src);
    /// @brief Calculate total bytes needed to hold all pool block data.
    /// @return Sum of (block_size * block_count) over all initialised pools.
    static size_t pool_data_bytes();

    /// @brief Pin a block so it is never returned by alloc() and never
    ///        released by free().  Used by the test-isolation snapshot to
    ///        reserve the baseline task-control-block memory, which the
    ///        scheduler snapshot references by raw pointer — if those blocks
    ///        were recycled onto test tasks, the captured pointers would alias
    ///        foreign TCBs and corrupt the live task set.
    /// @param ptr Pointer inside a pool block (must be block-aligned).
    static void pin_block(void *ptr);

    /// @brief Clear the pin on a previously pinned block.
    /// @param ptr Pointer inside a pool block.
    static void unpin_block(void *ptr);

    /// @brief True if the pointer refers to a pinned block.
    /// @param ptr Pointer to query.
    static bool is_block_pinned(void *ptr);

  private:
    // NOLINTNEXTLINE(bugprone-dynamic-static-initializers)
    static Pool pools_[POOL_COUNT];
    static constinit bool ready_;

    /// @brief No-pool sentinel (issue #254 H5/H6: was a bare
    ///        `static_cast<size_t>(-1)` at the find_pool return site).
    ///        Value-identical to the old literal.
    static constexpr size_t kNoPool = static_cast<size_t>(-1);

    /// @brief Finds the smallest pool class that satisfies a given size.
    ///        Linear scan over POOL_COUNT (9) entries — O(1) with small
    ///        constant.
    /// @param size Requested allocation size.
    /// @return Pool index, or kNoPool if too large.
    static size_t find_pool(size_t size);

    /// @brief Pop the head block of pool @p idx (issue #254 H8: shared by
    ///        alloc/alloc_err — the pop + double-ENSURE sequence was
    ///        duplicated).  Caller holds mempool_lock_.  WCET: O(1) flat.
    /// @return Popped block index.
    static size_t pop_block_locked(size_t idx);
    /// @brief Push @p block_idx of pool @p idx back onto its free list
    ///        (issue #254 H8: shared by free/free_err — the double-free
    ///        ENSURE + poison + push + track sequence was duplicated).
    ///        Caller holds mempool_lock_.  WCET: O(1) flat.
    static void push_block_locked(size_t idx, uint8_t *block_ptr,
                                  size_t block_idx);
    /// @brief Locate the pool owning @p block (issue #254 H8: shared by
    ///        free/free_err/contains/pin/unpin/is_block_pinned — the
    ///        initialized-skip + range walk was duplicated six times).
    ///        Lock-agnostic: callers hold mempool_lock_ except the
    ///        lock-free contains() probe (unchanged from before).
    /// @return true on hit with pool/offset/block indices filled.
    static bool find_owner_pool(void *block, size_t &pool_idx_out,
                                size_t &offset_out, size_t &block_idx_out);
};

} // namespace kernel
