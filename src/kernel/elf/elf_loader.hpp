#pragma once

/*
 * NexIOS RTOS — Background chunked ELF loader
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

/// @file elf_loader.hpp
/// @brief Background, preemptible, cancellable ELF loader (spec:
///        docs/specs/elf-loader.md).
///
/// A low-priority kernel task (`elf_loader_task_main`) loads an ELF file from
/// the filesystem in 4 KiB chunks, yielding between chunks so the daemons and
/// the deadline monitor keep their deadlines.  The shell issues `load` /
/// `cancel-load`; both return immediately; the loader task is the single
/// owner of all resource cleanup.

#include <types.hpp>
#include <kernel/sync/spinlock.hpp>
#include <kernel/elf/elf.hpp>

namespace kernel {
namespace elf {

/// @brief Loader state (public for tests / shell).
enum class LoadState : uint8_t {
    IDLE = 0,             ///< No load in flight; loader blocked on wake sem.
    VALIDATING,           ///< Open fd, read+validate header + phdrs.
    COPYING_SEGMENTS,     ///< Per-chunk read→alloc→map→copy.
    MAPPING,              ///< Stack+heap allocation + TCB finalize (bounded).
    DONE,                 ///< Completed; completed_tcb_ retained.
    FAILED,               ///< Error; cleanup already ran, about to be IDLE.
    CANCELED,             ///< Cancel requested (shell) or observed (loader).
};

/// @brief Result of a shell-initiated request.
enum class LoadResult : uint8_t {
    OK = 0,               ///< Accepted (load started / cancel queued).
    ALREADY_LOADING,      ///< request_load while a load is in flight.
    NOT_LOADING,          ///< request_cancel while idle.
    FILE_NOT_FOUND,       ///< Path did not resolve to a file.
};

/// @brief Background ELF loader singleton.
///
/// Threading model: the shell task only sets request parameters / flags under
/// the spinlock and posts the wake semaphore; the loader task performs all
/// chunk work and is the single owner of every resource it allocates.  No
/// lock is ever held across `Scheduler::reschedule()`.
class ElfLoader {
  public:
    static constexpr uint64_t kChunkSize = 4096;
    static constexpr uint64_t kLoaderPriority = 1;   // below shell (2)
    static constexpr size_t kMaxPath = 128;

    /// @brief Create the background loader task (idempotent).  Called from
    ///        kernel.cpp before the test runner so the TCB is in the snapshot
    ///        baseline.
    static void ensure_task();

    /// @brief Shell-side: start a background load.  Copies the path, sets
    ///        state=VALIDATING, posts the wake semaphore.  Returns
    ///        immediately.
    static LoadResult request_load(const char *path);

    /// @brief Shell-side: request cancellation of the in-flight load.  The
    ///        loader observes the flag at the next chunk boundary and runs
    ///        cleanup.
    static LoadResult request_cancel();

    /// @brief Current loader state (tests / shell status).
    static LoadState state();

    /// @brief Path of the current (or last) load.
    static const char *current_path();

    /// @brief Completed TCB from the last successful load (future runelf /
    ///        tests).  Ownership transfers to the caller.
    static TaskControlBlock *take_completed();

    /// @brief Release (cleanup + delete) the completed TCB.  Used by tests /
    ///        future runelf failure paths.
    static void release_completed();

    /// @brief Test/API contract: if a load is in flight, request cancel; the
    ///        caller MUST then wait_loader_idle().  Releases any completed
    ///        TCB and re-inits the wake semaphore (drops stale counts).
    static void reset();

    /// @brief Spin until the loader returns to IDLE (safe on UP: the loader
    ///        is more urgent than the harness, so it runs on the next tick
    ///        after each reschedule).
    static void wait_loader_idle();

    /// @brief The loader task's main loop (scheduler entry via the free
    ///        elf_loader_task_main bridge).  Blocks on the wake semaphore,
    ///        runs one load per accepted request.
    static void task_main();

  private:
    // One full load cycle; ends IDLE (or DONE-with-completed_tcb_).
    static void run_load();
    // Per-chunk / per-validation-step cancel check.
    static bool cancel_pending(uint64_t generation);
    // Single-owner cleanup; idempotent guards; ends IDLE.
    static void cleanup_and_idle();
    // Open the load file in the loader task's fd table.
    static int open_owned_file(const char *path);
    // Post a "loading <path> <size> <verb>[ in <ticks>]" event to dmesg + log.
    static void post_event(uint64_t code, const char *verb, uint64_t size,
                           uint64_t ticks, bool include_ticks);
    // Stable message buffer ring (dmesg stores const char*).
    static char *next_msg_slot();

    static sync::SpinLock lock_;
    static volatile LoadState state_;
    static bool cancel_requested_;
    static char path_[kMaxPath];
    static uint64_t file_size_;
    static uint64_t start_ticks_;
    static uint64_t load_generation_;
    static int fd_;
    static uint64_t pml4_;
    static uint16_t seg_idx_;
    static uint64_t page_in_seg_;
    static ELF64Header hdr_;
    static uint8_t phdr_image_[sizeof(ELF64Header) + 64 * sizeof(ELF64ProgramHeader)];
    static uint8_t chunk_buf_[kChunkSize];
    static TaskControlBlock *loader_tcb_;
    static TaskControlBlock *completed_tcb_;
    static char msg_buf_[4][160];
    static uint32_t msg_idx_;
};

/// @brief The loader task's entry (extern for scheduler).
void elf_loader_task_main();

} // namespace elf
} // namespace kernel
