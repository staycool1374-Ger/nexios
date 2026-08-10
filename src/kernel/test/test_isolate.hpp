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

/// @file test_isolate.hpp
/// @brief Test isolation snapshot/restore helpers.

#pragma once

#include <types.hpp>
#include <kernel/test/resource_tracker.hpp>

namespace kernel::test {

bool snapshot_create();
void snapshot_restore(const char *test_name = nullptr);
void snapshot_destroy();

/// @brief DEBUG watchdog: true when the snapshot buffer's canary region has
///        been overwritten by a stray write since the last snapshot_create.
///        Polled by on_tick() / context-switch epilogue (CONFIG_DEBUG) so a
///        corrupting write is detected within one tick instead of at the next
///        snapshot_restore boundary.  Read-only; never mutates snapshot state.
bool snapshot_canary_corrupted();

/// @brief True while a test-isolation snapshot exists (snapshot_create()
///        succeeded and snapshot_destroy() has not yet run).  Lets a test
///        distinguish "running under the isolation harness" from a bare
///        invocation where snapshot/restore is disabled.
bool snapshot_is_active();

/// @brief Terminate old daemon tasks and reload them from initrd.
///        Call AFTER snapshot_restore + snapshot_destroy to replace
///        corrupted page tables with fresh ones from initrd.
void reload_daemon_tasks();

/// @brief Run all tests from the generated registry, calling
/// setup/test/teardown
///        in sequence with snapshot isolation between each test.
void run_all_isolated_tests();

/// @brief Snapshot of the page-table pool state — survives PMM bitmap restore
///        so that kernel page-table pages (kstack window, VMM map_page)
///        remain valid across snapshot_restore cycles.
/// Max pool pages we can snapshot — must match CONFIG_PAGE_TABLE_POOL_SIZE
/// (default 4096).  bitmap/owner each need CONFIG_PAGE_TABLE_POOL_SIZE/8 bytes.
static constexpr size_t PT_POOL_SNAP_MAX_PAGES = 4096;

struct PtPoolSnapshot {
    uint64_t base;         ///< physical base (page_table_pool_start_)
    uint64_t size_pages;   ///< pool extent in pages

    bool     clean;         ///< known-good state after init
    bool     mapped;        ///< pages are live in kernel PML4/PD/PT
    bool     tainted;       ///< HW error (ECC, PCIe parity, etc.)
    bool     poisoned;      ///< buffer overflow / UAF detected in pool

    uint8_t  bitmap[PT_POOL_SNAP_MAX_PAGES / 8];  ///< covers pool bitmap
    uint8_t  owner[PT_POOL_SNAP_MAX_PAGES / 8];   ///< owner bitmap

    uint32_t generation;    ///< incremented per snapshot capture
    uint32_t refcount;      ///< tasks referencing this pool
    uint32_t crc32;         ///< integrity check over bitmap
    uint32_t _reserved[4];  ///< padding for forward compat
};

/// @brief Tracks whether any VFS syscall was invoked during the current test.
/// snapshot_restore() checks this flag to decide whether daemon tasks need
/// a full reload (VFS touched) or can be left running (VFS not touched).
extern bool g_vfs_touched; // NOLINT(bugprone-dynamic-static-initializers)

/// @brief Called by VFS syscall handlers to mark that VFS state was touched.
void mark_vfs_touched();

} // namespace kernel::test
