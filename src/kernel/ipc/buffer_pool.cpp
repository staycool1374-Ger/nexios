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

/// @file buffer_pool.cpp
/// @brief Zero-copy buffer pool implementation — page allocation, PTE
/// manipulation, transfer.

#include <kernel/ipc/buffer_pool.hpp>
#include <kernel/task/scheduler.hpp>
#include <kernel/memory/pmm.hpp>
#include <kernel/memory/vmm.hpp>
#include <kernel/test/resource_tracker.hpp>
#include <assert.hpp>
#include <string.hpp>

namespace kernel {

BufferPool::Entry BufferPool::entries[MAX_BUFFERS];
constinit int32_t BufferPool::free_head_ = BufferPool::LIST_EMPTY;
constinit uint32_t BufferPool::next_cookie_ = 1;
uint64_t BufferPool::pool_pages_[POOL_PAGES];
constinit size_t BufferPool::pool_count_ = 0;

/// @brief Clear a single page-table entry for @p virt_addr in the given PML4.
/// Handles x86_64, AArch64, and RISC-V page-table hierarchies.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
static void clear_pte_in_pml4(uint64_t virt_addr, uint64_t pml4_phys) {
    constexpr uint64_t PAGE_PRESENT = 1ULL << 0;

#if defined(CONFIG_ARCH_RISCV64)
    constexpr uint64_t L0_SHIFT = 30;
    constexpr uint64_t L1_SHIFT = 21;
    constexpr uint64_t L2_SHIFT = 12;
    constexpr uint64_t PAGE_READ = 1ULL << 1;
    constexpr uint64_t PAGE_WRITE = 1ULL << 2;
    constexpr uint64_t PAGE_EXEC = 1ULL << 3;
    constexpr uint64_t RWX = PAGE_READ | PAGE_WRITE | PAGE_EXEC;

    auto *l0 = reinterpret_cast<uint64_t *>(arch::HHDM_OFFSET +
                                            (pml4_phys & ~0xFFFULL));
    size_t l0_idx = (virt_addr >> L0_SHIFT) & 0x1FF;
    if (!(l0[l0_idx] & PAGE_PRESENT))
        return;
    auto *l1 = reinterpret_cast<uint64_t *>(arch::HHDM_OFFSET +
                                            (l0[l0_idx] & ~0xFFFULL));
    size_t l1_idx = (virt_addr >> L1_SHIFT) & 0x1FF;
    if (!(l1[l1_idx] & PAGE_PRESENT))
        return;
    if (l1[l1_idx] & RWX) {
        l1[l1_idx] = 0;
        return;
    } // 2MB block
    auto *l2 = reinterpret_cast<uint64_t *>(arch::HHDM_OFFSET +
                                            (l1[l1_idx] & ~0xFFFULL));
    size_t l2_idx = (virt_addr >> L2_SHIFT) & 0x1FF;
    l2[l2_idx] = 0;
#else
    constexpr uint64_t PML4_SHIFT = 39;
    constexpr uint64_t PDPT_SHIFT = 30;
    constexpr uint64_t PD_SHIFT = 21;
    constexpr uint64_t PT_SHIFT = 12;

    // NOLINTNEXTLINE(performance-no-int-to-ptr)
    auto *pml4 = reinterpret_cast<uint64_t *>(arch::HHDM_OFFSET +
                                              (pml4_phys & ~0xFFFULL));
    size_t pml4_idx = (virt_addr >> PML4_SHIFT) & 0x1FF;
    size_t pdpt_idx = (virt_addr >> PDPT_SHIFT) & 0x1FF;
    size_t pd_idx = (virt_addr >> PD_SHIFT) & 0x1FF;
    size_t pt_idx = (virt_addr >> PT_SHIFT) & 0x1FF;

    if (!(pml4[pml4_idx] & PAGE_PRESENT))
        return;
    uint64_t pdpt_phys = pml4[pml4_idx] & ~0xFFFULL;
    if (!PMM::is_allocated(pdpt_phys))
        return;
    // NOLINTNEXTLINE(performance-no-int-to-ptr)
    auto *pdpt = reinterpret_cast<uint64_t *>(arch::HHDM_OFFSET + pdpt_phys);
    if (!(pdpt[pdpt_idx] & PAGE_PRESENT))
        return;
    uint64_t pd_phys = pdpt[pdpt_idx] & ~0xFFFULL;
    if (!PMM::is_allocated(pd_phys))
        return;
    // NOLINTNEXTLINE(performance-no-int-to-ptr)
    auto *pd = reinterpret_cast<uint64_t *>(arch::HHDM_OFFSET + pd_phys);
    if (pd[pd_idx] & PAGE_PRESENT) {
#if defined(CONFIG_ARCH_AARCH64)
        constexpr uint64_t PAGE_TABLE = 1ULL << 1;
        if ((pd[pd_idx] & (PAGE_PRESENT | PAGE_TABLE)) == PAGE_PRESENT)
#else
        if (pd[pd_idx] & (1ULL << 7))
#endif
        {
            pd[pd_idx] = 0;
            return;
        } // huge page
        // NOLINTNEXTLINE(performance-no-int-to-ptr)
        auto *pt = reinterpret_cast<uint64_t *>(arch::HHDM_OFFSET +
                                                (pd[pd_idx] & ~0xFFFULL));
        pt[pt_idx] = 0;
    }
#endif
}

/// @brief BUGS.md#020 red-zone guard (defined in full below validate()).
static inline void bp_check_guard();

/// @brief Insert a buffer entry at the head of a task's linked list.
static void list_insert(TaskControlBlock &task, int32_t idx) {
    bp_check_guard();
    ENSURE(idx >= 0 && static_cast<size_t>(idx) < BufferPool::MAX_BUFFERS);
    int32_t old_head = task.buf_list_head;
    if (old_head != BufferPool::LIST_EMPTY)
        ENSURE(static_cast<size_t>(old_head) < BufferPool::MAX_BUFFERS);
    BufferPool::entries[idx].list_prev = BufferPool::LIST_EMPTY;
    BufferPool::entries[idx].list_next = old_head;
    if (old_head != BufferPool::LIST_EMPTY)
        BufferPool::entries[old_head].list_prev = idx;
    task.buf_list_head = idx;
}

/// @brief Remove a buffer entry from a task's linked list.
static void list_remove(TaskControlBlock &task, int32_t idx) {
    bp_check_guard();
    ENSURE(idx >= 0 && static_cast<size_t>(idx) < BufferPool::MAX_BUFFERS);
    int32_t prev = BufferPool::entries[idx].list_prev;
    int32_t next = BufferPool::entries[idx].list_next;
    if (prev != -1) {
        ENSURE(static_cast<size_t>(prev) < BufferPool::MAX_BUFFERS);
        BufferPool::entries[prev].list_next = next;
    } else
        task.buf_list_head = next;
    if (next != -1) {
        ENSURE(static_cast<size_t>(next) < BufferPool::MAX_BUFFERS);
        BufferPool::entries[next].list_prev = prev;
    }
    BufferPool::entries[idx].list_prev = -1;
    BufferPool::entries[idx].list_next = -1;
}

/// @brief Initialise the pool — build the free list, pre-allocate pages.
void BufferPool::init() {
    free_head_ = -1;
    next_cookie_ = 1;
    for (size_t i = 0; i < MAX_BUFFERS; ++i) {
        entries[i].phys_addr = 0;
        entries[i].generation = 0;
        entries[i].refcount = 0;
        entries[i].owner_task = 0;
        entries[i].mapped_va = 0;
        entries[i].list_prev = BufferPool::LIST_EMPTY;
        entries[i].list_next = BufferPool::LIST_EMPTY;
        free_entry(static_cast<int32_t>(i));
    }
    // Pre-allocate physical pages for buffer data.
    // These are mapped as user-accessible in task page tables, so they
    // must be USER-owned pages for isolation correctness.
    for (size_t i = 0; i < POOL_PAGES; ++i) {
        uint64_t phys = PMM::alloc_user_page();
        ENSURE(phys != 0);
        pool_pages_[i] = phys;
    }
    pool_count_ = POOL_PAGES;
}

/// @brief Pop a physical page from the pre-allocated pool.
///        Falls back to PMM::alloc_user_page() when the pool is exhausted.
uint64_t BufferPool::alloc_page() {
    if (__atomic_load_n(&pool_count_, __ATOMIC_RELAXED) > 0) {
        uint64_t idx = __atomic_sub_fetch(&pool_count_, 1UL,
                                          __ATOMIC_RELAXED);
        uint64_t phys = pool_pages_[idx];
        pool_pages_[idx] = 0; // scrub stale slot (C4)
        // Pool pages may have been freed via PMM::free_page() in a prior
        // lifecycle (before the pool-only reclaim below), which resets the
        // owner bit to KERNEL.  Ensure USER ownership.
        if (!PMM::is_user_page(phys)) {
            PMM::free_page(phys);
            phys = PMM::alloc_user_page();
            if (!phys)
                return 0;
        }
        // v0.3.11: the pool is a tracked cache — a page pulled from the pool
        // is a tracked alloc (balances free_page()'s tracked push) so a
        // PMM-allocated buffer page absorbed into the pool does not read as a
        // leaked PMM allocation (buffer_pool_alloc_after_exhaustion_and_free).
        kernel::test::ResourceTracker::instance().track_pmm_alloc(1);
        return phys;
    }
    return PMM::alloc_user_page();
}

/// @brief Return a physical page to the pre-allocated pool.
///        Pages stay in the pool (USER-owned, mapped in task page tables) so
///        they never round-trip through PMM's free list — the pool is a
///        bounded cache sized CONFIG_BUFFER_POOL_PAGES.  If the pool is full
///        the page is returned to PMM instead of being dropped (the old
///        behaviour silently leaked the page AND left it allocated in PMM's
///        owner bitmap, which drifted the ResourceTracker baseline and caused
///        spurious +1 PMM "leaks" on every subsequent buffer_pool test).
void BufferPool::free_page(uint64_t phys) {
    if (__atomic_load_n(&pool_count_, __ATOMIC_RELAXED) < POOL_PAGES) {
        // v0.3.11 FIX: use __atomic_fetch_add (returns the OLD count) so the
        // page is stored at the slot being filled, then the count increments.
        // The previous __atomic_add_fetch (returns the NEW count) stored at
        // old+1 — off-by-one against alloc_page()'s __atomic_sub_fetch pop
        // (reads old-1).  A pushed page therefore landed at a slot the pop
        // never reads (pool grew, tracker drifted +1 in
        // buffer_pool_alloc_after_exhaustion_and_free; stale slot 0 was
        // popped by the entry-512 re-alloc).
        pool_pages_[__atomic_fetch_add(&pool_count_, 1UL,
                                       __ATOMIC_RELAXED)] = phys;
        // v0.3.11: tracked push — balances alloc_page()'s tracked pop so a
        // page cached in the pool does not read as a leaked PMM allocation.
        kernel::test::ResourceTracker::instance().track_pmm_free(1);
        return;
    }
    // Pool full — return the page to PMM (track_pmm_free fires inside
    // PMM::free_page).  No new owner-bit invariant is broken: the page is no
    // longer a live buffer, so KERNEL ownership is correct for reuse.
    PMM::free_page(phys);
}

/// @brief Pop an entry from the free list.
/// @return index, or BufferPool::BUF_INVALID_INDEX if the pool is exhausted.
int32_t BufferPool::alloc_entry() {
    bp_check_guard();
    if (free_head_ == BufferPool::LIST_EMPTY)
        return BufferPool::BUF_INVALID_INDEX;
    ENSURE(static_cast<size_t>(free_head_) < MAX_BUFFERS);
    int32_t idx = free_head_;
    ENSURE(static_cast<size_t>(idx) < MAX_BUFFERS);
    free_head_ = static_cast<int32_t>(entries[idx].list_next);
    if (free_head_ != BufferPool::LIST_EMPTY)
        ENSURE(static_cast<size_t>(free_head_) < MAX_BUFFERS);
    ENSURE(entries[idx].phys_addr == 0);
    entries[idx].list_next = BufferPool::LIST_EMPTY;
    kernel::test::ResourceTracker::instance().track_bufpool_alloc();
    return idx;
}

/// @brief Return an entry to the free list and zero its fields.
void BufferPool::free_entry(int32_t idx) {
    ENSURE(idx >= 0 && static_cast<size_t>(idx) < MAX_BUFFERS);
    entries[idx].phys_addr = 0;
    entries[idx].refcount = 0;
    entries[idx].owner_task = 0;
    entries[idx].mapped_va = 0;
    entries[idx].list_prev = BufferPool::LIST_EMPTY;
    entries[idx].list_next = free_head_;
    free_head_ = idx;
    kernel::test::ResourceTracker::instance().track_bufpool_free();
}

/// @brief Validate a handle (index + generation match).
/// @return index, BufferPool::BUF_INVALID_HANDLE, or BufferPool::BUF_INVALID_INDEX.
/// @brief BUGS.md#020: verify the `entries[]` red-zone guard word is intact.
/// An OOB `entries[idx]` write (idx >= MAX_BUFFERS) corrupts this sentinel,
/// which sits immediately after the array; panic with a precise message so the
/// overflow is caught at its source rather than later as a stray GPF.
static inline void bp_check_guard() {
#ifdef CONFIG_DEBUG
    if (BufferPool::entries_guard_ != BufferPool::ENTRIES_GUARD_MAGIC) {
        kernel::Logger::fatal("BUGS.md#020 BUFFERPOOL OOB: entries[] red-zone "
                              "guard clobbered (0x%llx) — out-of-bounds write "
                              "past entries[MAX_BUFFERS=%zu]",
                              (unsigned long long)BufferPool::entries_guard_,
                              BufferPool::MAX_BUFFERS);
        panic("BufferPool entries[] overflow detected");
    }
#endif
}

int32_t BufferPool::validate(uint64_t handle) {
    if (handle == 0)
        return BufferPool::BUF_INVALID_HANDLE;
    uint32_t idx = static_cast<uint32_t>(handle & 0xFFFFFFFFULL);
    uint32_t gen = static_cast<uint32_t>(handle >> 32);
    bp_check_guard();
    if (idx >= MAX_BUFFERS)
        return BufferPool::BUF_INVALID_INDEX;
    if (entries[idx].phys_addr == 0)
        return BufferPool::BUF_INVALID_HANDLE;
    if (entries[idx].generation != gen)
        return BufferPool::BUF_INVALID_HANDLE;
    return static_cast<int32_t>(idx);
}


/// @brief Allocate a buffer, map it at virtual address @p va in the given
/// task's space.
/// @return handle, or 0 on failure.
uint64_t BufferPool::alloc(TaskControlBlock &task, uint64_t va) {
    bp_check_guard();
    // v0.4.0 MP-1: every task owns a private PML4, so this address-space gate
    // is effectively always open — kernel driver tasks (is_user_ == false)
    // may still map a user buffer through their (otherwise empty) user half.
    if (!task.page_table_)
        return 0;

    if (va >= USER_SPACE_LIMIT)
        return 0;

    int32_t idx = alloc_entry();
    if (idx < 0)
        return 0;

    uint64_t phys = alloc_page();
    if (!phys) {
        free_entry(idx);
        return 0;
    }
    ENSURE(PMM::is_user_page(phys) &&
           "BufferPool::alloc: page not user-owned before map_page_in_pml4");

    VMM::map_page_in_pml4(va, phys, true, task.page_table_);

    entries[idx].phys_addr = phys;
    entries[idx].generation =
        __atomic_fetch_add(&next_cookie_, 1U, __ATOMIC_RELAXED);
    entries[idx].owner_task = static_cast<uint32_t>(task.id);
    entries[idx].mapped_va = va;

    list_insert(task, idx);

    return (static_cast<uint64_t>(entries[idx].generation) << 32) |
           static_cast<uint64_t>(idx);
}

/// @brief Free a buffer: unmap, free the physical page, return entry to pool.
/// @return true on success.
bool BufferPool::free(TaskControlBlock &task, uint64_t handle) {
    bp_check_guard();
    int32_t idx = validate(handle);
    if (idx < 0)
        return false;
    if (entries[idx].owner_task != static_cast<uint32_t>(task.id))
        return false;

    if (entries[idx].mapped_va) {
        clear_pte_in_pml4(entries[idx].mapped_va, task.page_table_);
        entries[idx].mapped_va = 0;
    }

    list_remove(task, idx);
    BufferPool::free_page(entries[idx].phys_addr);
    free_entry(idx);
    return true;
}

/// @brief Map an existing (transferred) buffer at virtual address @p va.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
bool BufferPool::map(TaskControlBlock &task, uint64_t handle, uint64_t va) {
    bp_check_guard();
    int32_t idx = validate(handle);
    if (idx < 0)
        return false;
    if (va >= USER_SPACE_LIMIT)
        return false;

    VMM::map_page_in_pml4(va, entries[idx].phys_addr, true, task.page_table_);

    entries[idx].owner_task = static_cast<uint32_t>(task.id);
    entries[idx].mapped_va = va;

    // NOTE: list is managed by transfer() / alloc(). map() only sets up the
    // page-table mapping.  The entry is already in the task's list from
    // transfer() or alloc().
    return true;
}

/// @brief Unmap a buffer (PTE clear, list remove) without freeing the page.
bool BufferPool::unmap(TaskControlBlock &task, uint64_t handle) {
    bp_check_guard();
    int32_t idx = validate(handle);
    if (idx < 0)
        return false;
    if (entries[idx].owner_task != static_cast<uint32_t>(task.id))
        return false;
    if (!entries[idx].mapped_va)
        return false;

    clear_pte_in_pml4(entries[idx].mapped_va, task.page_table_);
    entries[idx].mapped_va = 0;

    list_remove(task, idx);
    return true;
}

/// @brief Transfer buffer ownership from one task to another (IPC send path).
bool BufferPool::transfer(uint64_t handle, TaskControlBlock &from,
                          TaskControlBlock &to) {
    bp_check_guard();
    int32_t idx = validate(handle);
    if (idx < 0)
        return false;
    if (entries[idx].owner_task != static_cast<uint32_t>(from.id))
        return false;

    if (entries[idx].mapped_va) {
        clear_pte_in_pml4(entries[idx].mapped_va, from.page_table_);
        entries[idx].mapped_va = 0;
    }

    list_remove(from, idx);

    entries[idx].owner_task = static_cast<uint32_t>(to.id);

    list_insert(to, idx);

    return true;
}

/// @brief Snapshot pool state into a flat buffer (test isolation).
void BufferPool::capture_state(uint8_t *dst, size_t max_bytes) {
    (void)max_bytes;
    __builtin_memcpy(dst, entries, sizeof(entries));
    dst += sizeof(entries);
    __builtin_memcpy(dst, &free_head_, sizeof(free_head_));
    dst += sizeof(free_head_);
    __builtin_memcpy(dst, &next_cookie_, sizeof(next_cookie_));
    dst += sizeof(next_cookie_);
    __builtin_memcpy(dst, &pool_count_, sizeof(pool_count_));
    dst += sizeof(pool_count_);
    // pool_pages_ must be captured too: without it, snapshot_restore leaves the
    // pool holding whatever phys the LAST test cached, while the rewound PMM
    // bitmap frees those pages — the pool double-books free pages and the
    // ResourceTracker drifts (+1..+3 PMM pages per buffer_pool test, +128 when
    // the whole pool is replaced).  v0.3.11 root cause.
    __builtin_memcpy(dst, pool_pages_, sizeof(pool_pages_));
}

/// @brief Restore pool state from a flat buffer (test isolation).
void BufferPool::restore_state(const uint8_t *src, size_t max_bytes) {
    (void)max_bytes;
    __builtin_memcpy(entries, src, sizeof(entries));
    src += sizeof(entries);
    __builtin_memcpy(&free_head_, src, sizeof(free_head_));
    src += sizeof(free_head_);
    __builtin_memcpy(&next_cookie_, src, sizeof(next_cookie_));
    src += sizeof(next_cookie_);
    __builtin_memcpy(&pool_count_, src, sizeof(pool_count_));
    src += sizeof(pool_count_);
    __builtin_memcpy(pool_pages_, src, sizeof(pool_pages_));
}

/// @brief Unmap and free all buffers owned by a task (called from
/// cleanup/exec).
void BufferPool::unmap_all(TaskControlBlock &task) {
    bp_check_guard();
    int32_t idx = task.buf_list_head;
    uint32_t loop_guard = 0;
    while (idx != -1) {
        ENSURE(static_cast<size_t>(idx) < MAX_BUFFERS);
        int32_t next = entries[idx].list_next;
        if (next != -1)
            ENSURE(static_cast<size_t>(next) < MAX_BUFFERS);
        ENSURE(++loop_guard < MAX_BUFFERS); // cycle/runaway-list guard
        ENSURE(entries[idx].owner_task == static_cast<uint32_t>(task.id));

        if (entries[idx].mapped_va) {
            clear_pte_in_pml4(entries[idx].mapped_va, task.page_table_);
            entries[idx].mapped_va = 0;
        }

        BufferPool::free_page(entries[idx].phys_addr);
        free_entry(idx);
        idx = next;
    }
    task.buf_list_head = -1;
}

} // namespace kernel
