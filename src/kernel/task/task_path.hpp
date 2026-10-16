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

/// @file task_path.hpp
/// @brief Task create/cleanup step registries (issue #347 Slice C).
///
/// Two traced segments over TaskControlBlock::create()/cleanup(), one
/// step per code block. Steps mirror the issue #324 12-step list; the
/// code carries two extra blocks (exit-stats snapshot, cap/IRQ release),
/// so the cleanup registry has 14 steps — the mapping is documented per
/// enumerator. Conditional blocks (user stack, kslot, page tables) fold
/// into their region's step: the step marks the region's completion,
/// whatever sub-path it took.
///
/// Terminal paths (record a prefix, validate against nothing full):
/// poisoned-TCB skip (empty chain), pinned early-return (Validate +
/// WatchdogDisarm only), create()-OOM legs (partial chain — destroy()
/// on the failure path runs cleanup(), which steps into the same slot).
///
/// Slot discipline (shared with the dispatch registry): tests own reset;
/// production never resets or validates. Tests must hold IF=0 across
/// reset → drive → capture: the timer ISR runs next_task(), whose
/// dispatch steps would otherwise pollute the shared slot.

#pragma once

#include <types.hpp>
#include <kernel/core/path_tracer.hpp>

namespace kernel {

/// @brief TaskControlBlock::create() steps (issue #347 Slice C).
enum class TaskCreateSteps : uint64_t {
    Alloc     = 1ULL << 0, ///< MemPool block allocated
    ZeroInit  = 1ULL << 1, ///< memset + init_task_common done
    StackAlloc = 1ULL << 2, ///< kernel-stack phys allocated
    Pml4Clone = 1ULL << 3, ///< private kernel-half PML4 cloned
    FrameBuild = 1ULL << 4, ///< initial register frame built
};

/// @brief TaskControlBlock::cleanup() steps (issue #347 Slice C).
/// @note  Numbering follows the code blocks; the #324 12-step list maps:
///        1 validate, 2 watchdog, 3 snapshot-pin (terminal), 4 scheduler
///        structs, 5 parent/child, 6 timers, 7 sync, 8 notifications,
///        9 VFS, 10 memory, 11 page tables/cap/mmio, 12 IPC. ExitStats
///        and CapRelease are code blocks the issue list predates.
enum class TaskCleanupSteps : uint64_t {
    Validate       = 1ULL << 0,  ///< is_valid + REAPED mark (#324.1)
    WatchdogDisarm = 1ULL << 1,  ///< wdog disarmed, gen bumped (#324.2)
    ExitStats      = 1ULL << 2,  ///< exit-stats snapshot (post-#324 block)
    Unregister     = 1ULL << 3,  ///< scheduler unregister (#324.4)
    ParentUnbind   = 1ULL << 4,  ///< parent/child unlink (#324.5)
    TimerDrain     = 1ULL << 5,  ///< wheel + POSIX drains (#324.6)
    SyncDetach     = 1ULL << 6,  ///< queue/sync waiter detach (#324.7)
    Notify         = 1ULL << 7,  ///< death/notify/debug publish (#324.8)
    VfsDrain       = 1ULL << 8,  ///< fd table + cwd refs (#324.9)
    MemFree        = 1ULL << 9,  ///< user stack + kstack freed (#324.10)
    PageTeardown   = 1ULL << 10, ///< buffers/cap/pager/libs/PTs (#324.11)
    IpcDestroy     = 1ULL << 11, ///< msgq/notify/eventgroup (#324.12)
    CapRelease     = 1ULL << 12, ///< iopb/IRQ/cap ledger (post-#324 block)
    Finalize       = 1ULL << 13, ///< cwd drop + task-remove accounting
};

/// @brief Full create() segment: Alloc through FrameBuild.
inline constexpr uint64_t k_task_create_expected =
    path_expected(TaskCreateSteps::Alloc, TaskCreateSteps::ZeroInit,
                  TaskCreateSteps::StackAlloc, TaskCreateSteps::Pml4Clone,
                  TaskCreateSteps::FrameBuild);

/// @brief Full cleanup() segment: all 14 blocks.
inline constexpr uint64_t k_task_cleanup_expected =
    path_expected(TaskCleanupSteps::Validate, TaskCleanupSteps::WatchdogDisarm,
                  TaskCleanupSteps::ExitStats, TaskCleanupSteps::Unregister,
                  TaskCleanupSteps::ParentUnbind, TaskCleanupSteps::TimerDrain,
                  TaskCleanupSteps::SyncDetach, TaskCleanupSteps::Notify,
                  TaskCleanupSteps::VfsDrain, TaskCleanupSteps::MemFree,
                  TaskCleanupSteps::PageTeardown,
                  TaskCleanupSteps::IpcDestroy, TaskCleanupSteps::CapRelease,
                  TaskCleanupSteps::Finalize);

/// @brief Pinned early-return terminal value (teardown skipped).
inline constexpr uint64_t k_task_cleanup_pinned_expected =
    path_expected(TaskCleanupSteps::Validate,
                  TaskCleanupSteps::WatchdogDisarm);

// Non-zero and pairwise-distinct from the empty chain — a fold-value typo
// or a broken mix would collapse these.
static_assert(k_task_create_expected != 0);
static_assert(k_task_cleanup_expected != k_task_create_expected);
static_assert(k_task_cleanup_pinned_expected != k_task_create_expected);
static_assert(k_task_cleanup_pinned_expected != k_task_cleanup_expected);

// Bit-distinctness: create uses bits 0..4, cleanup bits 0..13 — a
// duplicated bit (e.g. after a renumbering) would hide a skipped step.
static_assert((static_cast<uint64_t>(to_underlying(TaskCreateSteps::Alloc)) |
               static_cast<uint64_t>(to_underlying(TaskCreateSteps::ZeroInit)) |
               static_cast<uint64_t>(to_underlying(TaskCreateSteps::StackAlloc)) |
               static_cast<uint64_t>(to_underlying(TaskCreateSteps::Pml4Clone)) |
               static_cast<uint64_t>(to_underlying(TaskCreateSteps::FrameBuild))) ==
              0x1FULL);
static_assert((static_cast<uint64_t>(to_underlying(TaskCleanupSteps::Validate)) |
               static_cast<uint64_t>(to_underlying(TaskCleanupSteps::WatchdogDisarm)) |
               static_cast<uint64_t>(to_underlying(TaskCleanupSteps::ExitStats)) |
               static_cast<uint64_t>(to_underlying(TaskCleanupSteps::Unregister)) |
               static_cast<uint64_t>(to_underlying(TaskCleanupSteps::ParentUnbind)) |
               static_cast<uint64_t>(to_underlying(TaskCleanupSteps::TimerDrain)) |
               static_cast<uint64_t>(to_underlying(TaskCleanupSteps::SyncDetach)) |
               static_cast<uint64_t>(to_underlying(TaskCleanupSteps::Notify)) |
               static_cast<uint64_t>(to_underlying(TaskCleanupSteps::VfsDrain)) |
               static_cast<uint64_t>(to_underlying(TaskCleanupSteps::MemFree)) |
               static_cast<uint64_t>(to_underlying(TaskCleanupSteps::PageTeardown)) |
               static_cast<uint64_t>(to_underlying(TaskCleanupSteps::IpcDestroy)) |
               static_cast<uint64_t>(to_underlying(TaskCleanupSteps::CapRelease)) |
               static_cast<uint64_t>(to_underlying(TaskCleanupSteps::Finalize))) ==
              0x3FFFULL);

#if defined(CONFIG_DEBUG)
/// @brief Fault-injection mask for task_step() (test-only, Slice C).
///        Compiled out in release — production can never observe it.
extern uint64_t g_test_skip_task_step;
#endif

/// @brief Record one task-path step (honors the test skip mask in debug).
inline void task_step(uint64_t step_bit) noexcept {
#if defined(CONFIG_DEBUG)
    if ((g_test_skip_task_step & step_bit) != 0) {
        return;
    }
#endif
    path_step(step_bit);
}

/// @brief Record one task-path step from a registry.
template <typename E>
inline void task_step(E step_value) noexcept {
    task_step(static_cast<uint64_t>(to_underlying(step_value)));
}

} // namespace kernel
