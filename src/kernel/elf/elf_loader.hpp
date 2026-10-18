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
#include <kernel/elf/elf_shared.hpp>

namespace kernel {
namespace elf {

/// @brief Loader state (public for tests / shell).
enum class LoadState : uint8_t {
    IDLE = 0,             ///< No load in flight; loader blocked on wake sem.
    VALIDATING,           ///< Open fd, read+validate header + phdrs.
    COPYING_SEGMENTS,     ///< Per-chunk read→alloc→map→copy.
    LOADING_DEPS,         ///< Issue #95: DT_NEEDED recursive load.
    LOADING_RELOC,        ///< Issue #95: post-order relocate pass.
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

/// @brief How a load event message is decorated with timing.
enum class PostKind : uint8_t {
    STARTED,   ///< "loading <path> started at <sec>"
    COMPLETED, ///< "loading <path> completed successfully in <sec.ms>s"
    OTHER,     ///< "loading <path> canceled" / "loading <path> failed: ..."
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
    static constexpr uint64_t kLoaderPriority = 15;  // above harness (10),
                                                       // below daemons (20);
                                                       // yields per chunk so
                                                       // daemons stay on-time
    static constexpr size_t kMaxPath = 128;
    /// @brief Writable-byte pin bound for rerun retention (issue #271):
    ///        images with more RW file bytes are loadable but never
    ///        retained (explicit refusal at rerun).
    static constexpr uint64_t kMaxRerunWritableBytes = 1 * 1024 * 1024;
    /// @brief Golden-page bound: RO bytes beyond the #46 scan cap are
    ///        never retained (same cap, same fail-closed).
    static constexpr size_t kMaxGoldenPages =
        static_cast<size_t>(1024 * 1024 * 4) / 4096;
    /// @brief One retained set max (R6): take overwrites, never appends.
    static constexpr size_t kMaxGoldenRanges = 8;
    static constexpr size_t kMaxPinRanges = 8;

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
    ///        tests).  Ownership transfers to the caller, which sets the
    ///        settled priority/base_priority/period/deadline BEFORE add_task
    ///        (never add_task then mutate) and admits via add_task_err.
    static TaskControlBlock *take_completed();

    /// @brief Entry VA latched with the retained completion (issue #231):
    ///        read hdr_.entry at publish time so a later load cannot move
    ///        it under the retained TCB. 0 when no completion is retained.
    ///        Cleared wherever completed_tcb_ is cleared.
    static uint64_t completed_entry() noexcept;

    /// @brief Release (cleanup + delete) the completed TCB.  Used by tests /
    ///        future runelf failure paths.
    static void release_completed();

    /// @brief Whether a rerunnable retention exists (issue #271 P1):
    ///        a valid set adopted at take, not yet superseded/reset/
    ///        released. Rerun refuses unless this is true (P2 checks the
    ///        outcome gate on top).
    [[nodiscard]] static bool has_retained() noexcept;

    /// @brief Outcome of the retained set's last launch (issue #271 P1).
    enum class RunOutcome : uint8_t {
        UNKNOWN = 0, ///< Never launched, or ended by an untracked path.
        CLEAN = 1,   ///< Exited via sys_exit (code recorded).
        FAULTED = 2, ///< Killed by fault/auth-kill/death path.
    };

    /// @brief Record a launch of the retained set's task (issue #271 P1).
    /// Call after successful add_task of a taken/retained TCB; arms
    /// outcome tracking for this task id. Unknown ids in note_task_end
    /// stay UNKNOWN (never launched).
    static void note_launched(uint64_t task_id);

    /// @brief Record the end of a tracked launch (issue #271 P1).
    /// sys_exit passes clean=true + the exit code; fault/auth-kill/
    /// death paths pass clean=false. Calls for untracked ids are
    /// ignored. Rerun requires CLEAN + code 0 (checked at build).
    static void note_task_end(uint64_t task_id, bool clean, uint64_t code);

    /// @brief Current outcome of the retained set (tests + P2 gate).
    static RunOutcome retained_outcome();

    /// @brief Drop the never-added taker's task ref after an admission
    ///        failure (shell runelf denials; future pcall sites). Public
    ///        so shell.cpp can call it outside the elf namespace;
    ///        IRQ-masked; bounded; never reschedule inside.
    static void abandon_retention();

    /// @brief Try to build a rerun TCB from retention (issue #271 P2).
    /// Checks IDLE + valid + static + CLEAN/0; on success returns a
    /// fresh TCB (caller owns it, must set settled params before
    /// add_task like take_completed). On failure returns nullptr and
    /// sets *reason to a static message (no elf loaded / dynamic /
    /// not clean / busy / rebuild failed). IRQ-masked for the gate;
    /// allocations/mappings run unlocked (may reschedule only via
    /// explicit reschedule points outside the lock).
    static TaskControlBlock *try_rerun(const char **reason);

    /// @brief Unmap every retained-golden page from @p tcb's table and
    ///        drop that task's set ref (issue #271 P1). Never frees here:
    ///        pages are freed only when retention refs == 0 AND task
    ///        refs == 0 (freed inline at that transition). Called from
    ///        task teardown paths BEFORE blind free_user_pages (which
    ///        then naturally skips the unmapped pages). Lock-atomic;
    ///        IRQ-masked; never held across reschedule.
    static void unmap_golden(TaskControlBlock *tcb);

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

    /// @brief Tear down a completed-but-never-scheduled TCB (built by
    ///        finalize_loaded_task).  Skips all scheduler interactions.
    static void destroy_completed_tcb(TaskControlBlock *tcb);

    /// @brief Per-validation-step cancel check (issue #95: shared by the
    ///        dep-resolve backend in elf_shared.cpp).
    [[nodiscard]] static bool cancel_pending(uint64_t generation) noexcept;

    /// @brief Current load generation for DepResolveContext setup.
    static uint64_t generation() { return load_generation_; }

  private:
    // One full load cycle; ends IDLE (or DONE-with-completed_tcb_).
    static void run_load();
    // Single-owner cleanup; idempotent guards; ends IDLE.
    static void cleanup_and_idle();
    // Open the load file in the loader task's fd table.
    static int open_owned_file(const char *path);
    // Post a "loading <path> <verb>[ at/in <time>]" event to dmesg + log.
    // @param kind  PostKind::STARTED appends " at <sec>", PostKind::COMPLETED
    //              appends " in <sec.ms>s", PostKind::OTHER appends nothing.
    static void post_event(uint64_t code, const char *verb, uint64_t ticks,
                           PostKind kind);
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
    static uint64_t completed_entry_;
    // Issue #271 P1: retained golden set (at most one). Adopted at
    // take_completed from the taken TCB's live mappings (never during
    // COPYING); abandoned on take-overwrite/reset/release. Golden phys
    // are owned by mapping tasks, never by retention: the list + refs
    // decide frees (zero/zero frees inline in the ref-drop). The pin
    // (bounded RW file-byte copy) is retention-owned static storage.
    struct GoldenPage {
        uint64_t phys = 0;
        bool executable = false;
    };
    struct GoldenRange {
        uint64_t vaddr = 0;
        uint64_t page_count = 0;
    };
    struct PinRange {
        uint64_t vaddr = 0;    // destination VA in the fresh build
        uint64_t pin_off = 0;  // offset into pin_buf_
        uint64_t len = 0;      // file bytes (never BSS tail)
        uint64_t memsz = 0;    // full RW span incl. BSS tail (fresh-mapped + zeroed)
    };
    struct Retention {
        bool valid = false;
        char path[kMaxPath] = {};
        uint64_t file_size = 0;
        uint64_t entry = 0;
        // Packed header image for rerun finalization: real header with
        // phoff rewritten to sizeof(ELF64Header) + packed table, so
        // install_segment_canaries (hdr->phoff based) reads correctly.
        ELF64Header hdr = {};
        uint8_t phdr_image[sizeof(ELF64Header) +
                           64 * sizeof(ELF64ProgramHeader)] = {};
        bool has_dynamic = false;
        GoldenPage golden[kMaxGoldenPages] = {};
        size_t golden_pages = 0;
        GoldenRange ranges[kMaxGoldenRanges] = {};
        size_t range_count = 0;
        uint8_t pin_buf[kMaxRerunWritableBytes] = {};
        size_t pin_len = 0;
        PinRange pin_ranges[kMaxPinRanges] = {};
        size_t pin_count = 0;
        uint64_t task_refs = 0; // live tasks mapping the set
        uint64_t epoch = 0;     // bumped on every adopt/abandon (builder race detector)
        bool retained = false;  // retention holds a ref
        RunOutcome outcome = RunOutcome::UNKNOWN;
        uint64_t outcome_code = 0;
        uint64_t outcome_task = 0;
        uint32_t ro_crc32 = 0; // retained #46 original (compare, never adopt)
        TaskControlBlock::RoRange ro_ranges[TaskControlBlock::kMaxRoRanges] = {};
        size_t ro_count = 0;
        uint64_t canary_slot = 0; // TEXT before-canary VA (P2 re-scan carve)
    };
    static Retention retention_;
    static Retention staging_;
    // Issue #271 P1: retention internals. All assume lock_ held +
    // IRQ-masked by the caller; never reschedule inside (bounded walks
    // and struct copies only; PMM frees mirror the destroy-under-lock
    // precedent in reset()).
    static void abandon_retention_locked();
    static void fill_staging_locked(TaskControlBlock *tcb);
    static void adopt_staging_locked();
    static void drop_task_ref_locked();
    // Drop the try_rerun builder ref ONLY if retention still refers to
    // the epoch the builder snapshotted: a concurrent adopt swaps the
    // whole Retention (new epoch, new owner), and an unconditional drop
    // would corrupt the new set's count. Assumes lock_ held + IRQ-masked.
    static void drop_builder_ref_locked(uint64_t epoch);
    // Unwind a half-built rerun table: unmap retained-golden VAs (no ref
    // change — caller drops separately), free the rest, free the PML4.
    // Assumes lock_ held + IRQ-masked by the caller.
    static void fail_fresh_build_locked(uint64_t pml4,
                                          const GoldenRange *ranges,
                                          size_t range_count);
    // Unmap-only half of the above (no frees): for post-TCB failure
    // paths where destroy_completed_tcb owns the frees. Assumes lock_.
    static void unmap_fresh_golden_locked(uint64_t pml4,
                                          const GoldenRange *ranges,
                                          size_t range_count);
    static char msg_buf_[16][160];
    static uint32_t msg_idx_;
    // Issue #95: per-request dep-resolve context (loader single-owner;
    // lives across run_load phases so cleanup_and_idle can release it).
    static DepResolveContext dep_ctx_;
    // Exec mapped range from Stage A (for lib overlap checks).
    static uint64_t exec_base_;
    static uint64_t exec_size_;
    // Issue #46: incremental CRC32 over the read-only (non-PF_W) file
    // bytes as they land per chunk; the post-relocation mapped scan in
    // MAPPING cross-checks (static) or adopts (dynamic) it.
    static uint32_t load_crc_;
};

/// @brief The loader task's entry (extern for scheduler).
void elf_loader_task_main();

} // namespace elf
} // namespace kernel
