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

/// @file elf_loader.cpp
/// @brief Background, preemptible, cancellable ELF loader implementation.

#include <kernel/elf/elf_loader.hpp>
#include <kernel/elf/elf_shared.hpp>
#include <crc32.hpp>
#include <kernel/task/task.hpp>
#include <kernel/task/scheduler.hpp>
#include <kernel/memory/pmm.hpp>
#include <kernel/memory/vmm.hpp>
#include <kernel/memory/mempool.hpp>
#include <kernel/vfs/vfs.hpp>
#include <kernel/log/dmesg.hpp>
#include <kernel/console/urc.hpp>
#include <kernel/sync/spinlock_guard.hpp>
#include <kernel/arch/hal/irq_guard.hpp>
#include <kernel/syscall/syscall_helpers.hpp>
#include <kernel/ipc/buffer_pool.hpp>
#if defined(CONFIG_ARCH_X86_64) && CONFIG_PCID
#include <kernel/arch/x86_64/hal/pcid.hpp>
#endif
#include <kernel/arch/timer.hpp>
#include <services/terminal/terminal.hpp>
#include <kernel/core/global_state.hpp>
#include <kernel/test/resource_tracker.hpp>
#include <logger.hpp>
#include <string.hpp>
#include <assert.hpp>

namespace kernel {
namespace elf {

/// @brief Copy a bounded string (release-safe: no libc strncpy dependency).
static inline void copy_bounded(char *dst, const char *src, size_t max) {
    size_t i = 0;
    while (src[i] && i < max - 1) {
        dst[i] = src[i];
        ++i;
    }
    dst[i] = '\0';
}

/// @brief Page-align a virtual address up (round toward +inf).
static inline uint64_t page_align_up(uint64_t addr) {
    return (addr + arch::PAGE_SIZE - 1) & ~(arch::PAGE_SIZE - 1);
}
/// @brief Page-align a virtual address down.
static inline uint64_t page_align_down(uint64_t addr) {
    return addr & ~(arch::PAGE_SIZE - 1);
}

// ---------------------------------------------------------------------------
// Singleton state
// ---------------------------------------------------------------------------
sync::SpinLock ElfLoader::lock_;
volatile LoadState ElfLoader::state_{LoadState::IDLE};
bool ElfLoader::cancel_requested_ = false;
char ElfLoader::path_[ElfLoader::kMaxPath] = {};
uint64_t ElfLoader::file_size_ = 0;
uint64_t ElfLoader::start_ticks_ = 0;
uint64_t ElfLoader::load_generation_ = 0;
int ElfLoader::fd_ = -1;
uint64_t ElfLoader::pml4_ = 0;
uint16_t ElfLoader::seg_idx_ = 0;
uint64_t ElfLoader::page_in_seg_ = 0;
ELF64Header ElfLoader::hdr_ = {};
uint8_t ElfLoader::phdr_image_[sizeof(ELF64Header) +
                               64 * sizeof(ELF64ProgramHeader)] = {};
uint8_t ElfLoader::chunk_buf_[ElfLoader::kChunkSize] = {};
TaskControlBlock *ElfLoader::loader_tcb_ = nullptr;
TaskControlBlock *ElfLoader::completed_tcb_ = nullptr;
uint64_t ElfLoader::completed_entry_ = 0;
ElfLoader::Retention ElfLoader::retention_ = {};
ElfLoader::Retention ElfLoader::staging_ = {};
char ElfLoader::msg_buf_[16][160] = {};
uint32_t ElfLoader::msg_idx_ = 0;
DepResolveContext ElfLoader::dep_ctx_ = {};
uint64_t ElfLoader::exec_base_ = 0;
uint64_t ElfLoader::exec_size_ = 0;
uint32_t ElfLoader::load_crc_ = CRC32::INITIAL;

/// @brief The loader task's entry: block on the wake semaphore, run one load
///        per accepted request, loop.  Idle = blocked (zero CPU).
void elf_loader_task_main() { ElfLoader::task_main(); }

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void ElfLoader::ensure_task() {
    if (loader_tcb_ && loader_tcb_->magic == TaskControlBlock::TCB_MAGIC)
        return;
    auto *t = TaskControlBlock::create(elf_loader_task_main, kLoaderPriority,
                                       0);
    if (!t)
        return;
    copy_bounded(t->name, "elf-load", CONFIG_TASK_NAME_LEN);
    t->name[CONFIG_TASK_NAME_LEN - 1] = '\0';
    Scheduler::add_task(*t);
    loader_tcb_ = t;
}

LoadResult ElfLoader::request_load(const char *path) {
    if (!path)
        return LoadResult::FILE_NOT_FOUND;
    // Validate existence + size OUTSIDE the spinlock (fs I/O may block)
    // with an open/stat/close probe: the fd lifecycle owns every vnode
    // reference symmetrically on every filesystem.  A bare vfs::resolve
    // cannot be released safely here — filesystems disagree on whether
    // lookup results are owned (initrd: fresh refcount-1 nodes) or
    // borrowed (tmpfs canonical nodes, mount roots), so neither a bare
    // dec (corrupts borrowed counts) nor no dec (leaks fresh nodes) is
    // correct for all paths (audit S3 on the first fix attempt).
    int probe = syscall_path_open(path, vfs::O_RDONLY);
    if (probe < 0)
        return LoadResult::FILE_NOT_FOUND;
    uint64_t fsize = 0;
    {
        auto *cur = Scheduler::current_task();
        auto *fd =
            (cur != nullptr) ? cur->fd_table.get(probe) : nullptr;
        if (fd == nullptr || fd->vnode == nullptr) {
            if (cur != nullptr)
                cur->fd_table.free(probe);
            return LoadResult::FILE_NOT_FOUND;
        }
        fsize = fd->vnode->size;
        cur->fd_table.free(probe);
    }

    arch::IrqGuard irq_guard{};
    SpinLockGuard<sync::SpinLock> guard(lock_);
    if (state_ != LoadState::IDLE)
        return LoadResult::ALREADY_LOADING;
    // Issue #77 step 3 (RETAIN-FOR-RETRY): a previous load may have
    // completed without ever being taken. The new path already validated
    // above (resolve succeeded), so free the retained image now — never
    // free-before-validate (a bad new path must not destroy a good
    // retained image), and never overwrite (that would leak it when
    // run_load publishes the new completion). At most one retained image
    // exists by construction.
    if (completed_tcb_) {
        destroy_completed_tcb(completed_tcb_);
        completed_tcb_ = nullptr;
        completed_entry_ = 0;
    }
    // Untaken completion gone: its staging goes with it (post-take
    // retention is untouched — completed_tcb_ is null after take).
    staging_ = Retention{};
    file_size_ = fsize;

    copy_bounded(path_, path, kMaxPath);
    cancel_requested_ = false;
    start_ticks_ = arch::Timer::ticks();
    ++load_generation_;
    state_ = LoadState::VALIDATING;
    // Wake the idle loader.  The wake is keyed on READY-QUEUE membership, not
    // `state == BLOCKED`: after a test-boundary snapshot_restore the loader
    // TCB's restored state field can be a snapshot-time non-BLOCKED value while
    // it is NOT in the ready queue (H2-family stranding — `[RS] cur=1 next=0
    // hi=0`; test-2 `wait_loader_idle` ENSURE panic at elf_loader.cpp:245).  A
    // state-only check skips the wake and leaves the loader not-queued forever.
    // `set_task_ready` refuses double-enqueues, so re-waking an already-queued
    // loader is safe.  A RUNNING loader is excluded: it is mid-dispatch
    // (context.rsp is live, no valid iret frame) and must not be re-enqueued.
    if (loader_tcb_ && !loader_tcb_->in_ready_queue_ &&
        loader_tcb_->state != TaskState::RUNNING) {
        Scheduler::set_task_ready(*loader_tcb_);
    }
    return LoadResult::OK;
}

LoadResult ElfLoader::request_cancel() {
    SpinLockGuard<sync::SpinLock> guard(lock_);
    if (state_ == LoadState::IDLE)
        return LoadResult::NOT_LOADING;
    cancel_requested_ = true;
    state_ = LoadState::CANCELED;
    return LoadResult::OK;
}

LoadState ElfLoader::state() {
    return static_cast<LoadState>(
        __atomic_load_n(reinterpret_cast<volatile uint8_t *>(&state_),
                        __ATOMIC_RELAXED));
}

const char *ElfLoader::current_path() { return path_; }

namespace {

// PF_X covering a VA in packed phdrs (loader layout: table at
// sizeof(ELF64Header) regardless of file phoff). False when no PT_LOAD
// covers the page (then the page is not a golden candidate anyway).
bool exec_for_va(const uint8_t *phdr_image, uint16_t phnum, uint64_t phentsize,
                 uint64_t va) {
    for (uint16_t i = 0; i < phnum; ++i) {
        auto *phdr = reinterpret_cast<const ELF64ProgramHeader *>(
            phdr_image + sizeof(ELF64Header) + (uint64_t)i * phentsize);
        if (!phdr || phdr->type != PT_LOAD)
            continue;
        uint64_t base = phdr->vaddr & ~(arch::PAGE_SIZE - 1);
        uint64_t top = phdr->vaddr + phdr->memsz;
        if (va >= base && va < top)
            return (phdr->flags & PF_X) != 0;
    }
    return false;
}

// Copy a VA range out of a user table via HHDM. False on any hole
// (fail closed — the range is only complete when fully mapped).
bool copy_user_range(uint64_t pml4, uint64_t vaddr, uint64_t len,
                     uint8_t *out) {
    uint64_t off = 0;
    while (off < len) {
        uint64_t va = vaddr + off;
        uint64_t page_off = va & (arch::PAGE_SIZE - 1);
        uint64_t chunk = arch::PAGE_SIZE - page_off;
        if (chunk > len - off)
            chunk = len - off;
        uint64_t phys =
            VMM::virt_to_phys_in_pml4(va & ~(arch::PAGE_SIZE - 1), pml4);
        if (!phys)
            return false;
        __builtin_memcpy(
            out + off,
            reinterpret_cast<const void *>(arch::HHDM_OFFSET + phys +
                                           page_off),
            chunk);
        off += chunk;
    }
    return true;
}

} // namespace

// Abandon retention: clear refs + pin. Golden phys are freed ONLY when
// no live mapper holds them (task_refs == 0); a live mapper's teardown
// frees them through its normal path later. Never negative.
void ElfLoader::abandon_retention_locked() {
    if (!retention_.valid)
        return;
    if (retention_.task_refs == 0) {
        for (size_t i = 0; i < retention_.golden_pages; ++i) {
            if (retention_.golden[i].phys)
                PMM::free_page(retention_.golden[i].phys);
        }
    }
    retention_ = Retention{};
}

// Fill staging from a just-completed load (all loader state is this
// load's here). Ineligible (dynamic / over caps / walk hole) leaves
// staging invalid — silent by design (has_retained() == false tells;
// the rerun path reports). Bounded: no reschedule inside.
void ElfLoader::fill_staging_locked(TaskControlBlock *tcb) {
    staging_ = Retention{};
    if (!tcb || !tcb->get_page_table())
        return;
    bool has_dynamic = false;
    for (uint16_t i = 0; i < hdr_.phnum; ++i) {
        auto *phdr = reinterpret_cast<const ELF64ProgramHeader *>(
            phdr_image_ + sizeof(ELF64Header) +
            (uint64_t)i * hdr_.phentsize);
        if (phdr && phdr->type == PT_DYNAMIC) {
            has_dynamic = true;
            break;
        }
    }
    if (has_dynamic)
        return; // excluded entirely: no pin, no golden, no refs.
    // R5 note: dynamic images are refused explicitly at LOAD time by
    // the dep backend ("failed: ..." with reason verbs — verified live:
    // a synthetic ET_EXEC+PT_DYNAMIC fails closed there), so nothing is
    // ever retained and try_rerun's "no elf loaded" is accurate, not
    // misleading. The kDynamic gate below stays as defense-in-depth.
    TaskControlBlock::RoRange ros[TaskControlBlock::kMaxRoRanges] = {};
    uint64_t n = collect_ro_ranges(
        kernel::Span<const uint8_t>(phdr_image_, sizeof(phdr_image_)),
        sizeof(ELF64Header), hdr_.phnum, hdr_.phentsize, file_size_, ros);
    if (n == (uint64_t)-1 || n > TaskControlBlock::kMaxRoRanges)
        return;
    // Golden walk: every mapped page covering the RO ranges.
    size_t gp = 0;
    size_t rp = 0;
    for (uint64_t r = 0; r < n; ++r) {
        uint64_t base = ros[r].vaddr & ~(arch::PAGE_SIZE - 1);
        uint64_t top = ros[r].vaddr + ros[r].len;
        uint64_t end = (top + arch::PAGE_SIZE - 1) & ~(arch::PAGE_SIZE - 1);
        if (rp >= kMaxGoldenRanges)
            return;
        staging_.ranges[rp].vaddr = base;
        staging_.ranges[rp].page_count = 0;
        for (uint64_t va = base; va < end; va += arch::PAGE_SIZE) {
            if (gp >= kMaxGoldenPages)
                return;
            uint64_t phys =
                VMM::virt_to_phys_in_pml4(va, tcb->get_page_table());
            if (!phys)
                return; // hole: fail closed (cannot happen post-load)
            staging_.golden[gp].phys = phys;
            staging_.golden[gp].executable =
                exec_for_va(phdr_image_, hdr_.phnum, hdr_.phentsize, va);
            ++gp;
            ++staging_.ranges[rp].page_count;
        }
        ++rp;
    }
    staging_.golden_pages = gp;
    staging_.range_count = rp;
    // Writable pin: RW file bytes only (never BSS tails), bounded.
    size_t pin_off = 0;
    size_t pr = 0;
    for (uint16_t i = 0; i < hdr_.phnum; ++i) {
        auto *phdr = reinterpret_cast<const ELF64ProgramHeader *>(
            phdr_image_ + sizeof(ELF64Header) +
            (uint64_t)i * hdr_.phentsize);
        if (!phdr || phdr->type != PT_LOAD || !(phdr->flags & PF_W))
            continue;
        // NOTE: no filesz==0 skip — a BSS-only RW segment still needs
        // its memsz span mapped+zeroed in the rerun build (canary slots
        // and program break live there); len 0 simply copies nothing.
        if (pin_off + phdr->filesz > kMaxRerunWritableBytes)
            return;
        // S3 hardening: cap the BSS span too — an enormous RW memsz
        // would otherwise drive allocate-until-OOM in the builder.
        // Bound total mapped span the same as file bytes.
        if (phdr->memsz > kMaxRerunWritableBytes)
            return;
        if (pr >= kMaxPinRanges)
            return;
        if (!copy_user_range(tcb->get_page_table(), phdr->vaddr, phdr->filesz,
                             staging_.pin_buf + pin_off))
            return;
        staging_.pin_ranges[pr].vaddr = phdr->vaddr;
        staging_.pin_ranges[pr].pin_off = pin_off;
        staging_.pin_ranges[pr].len = phdr->filesz;
        staging_.pin_ranges[pr].memsz = phdr->memsz;
        ++pr;
        pin_off += phdr->filesz;
    }
    staging_.pin_len = pin_off;
    staging_.pin_count = pr;
    // Retained #46 original (compare, never re-adopt) + canary slot.
    staging_.ro_crc32 = tcb->ro_crc32_;
    size_t rc = tcb->ro_seg_count_ < TaskControlBlock::kMaxRoRanges
                    ? (size_t)tcb->ro_seg_count_
                    : TaskControlBlock::kMaxRoRanges;
    for (size_t i = 0; i < rc; ++i)
        staging_.ro_ranges[i] = tcb->ro_ranges_[i];
    staging_.ro_count = rc;
    uint64_t slot = 0;
    if (text_canary_slot(phdr_image_, sizeof(ELF64Header), hdr_.phnum,
                         hdr_.phentsize, file_size_, &slot))
        staging_.canary_slot = slot;
    copy_bounded(staging_.path, path_, kMaxPath);
    staging_.file_size = file_size_;
    staging_.entry = hdr_.entry;
    staging_.hdr = hdr_;
    staging_.hdr.phoff = sizeof(ELF64Header);
    __builtin_memcpy(staging_.phdr_image, phdr_image_,
                     sizeof(staging_.phdr_image));
    staging_.has_dynamic = false;
    staging_.valid = true;
}

// Move staging into retention (newest take wins). Abandon-first covers
// ineligible staging too (a new take supersedes unconditionally).
void ElfLoader::adopt_staging_locked() {
    abandon_retention_locked();
    if (!staging_.valid) {
        staging_ = Retention{};
        return;
    }
    retention_ = staging_;
    retention_.task_refs = 1; // taken TCB maps the set
    retention_.retained = true;
    retention_.outcome = RunOutcome::UNKNOWN;
    retention_.outcome_code = 0;
    retention_.outcome_task = 0;
    staging_ = Retention{};
}

// Drop one task ref; free golden phys at zero/zero (nobody owns them:
// retention dropped while task refs remain means a live mapper owns
// them through its own teardown later).
void ElfLoader::drop_task_ref_locked() {
    if (!retention_.valid || retention_.task_refs == 0)
        return;
    --retention_.task_refs;
    if (retention_.task_refs == 0 && !retention_.retained) {
        for (size_t i = 0; i < retention_.golden_pages; ++i) {
            if (retention_.golden[i].phys)
                PMM::free_page(retention_.golden[i].phys);
        }
        retention_ = Retention{};
    }
}

// try_rerun bumped task_refs at the gate; a concurrent adopt swaps the
// whole Retention (new epoch). Dropping against the new set corrupts its
// count (1->0 while the new mapper is live -> a later abandon frees live
// golden). Drop only when our epoch is still current; an abandoned
// (invalid) retention needs no drop, a replaced one owns no ref of ours.
void ElfLoader::drop_builder_ref_locked(uint64_t epoch) {
    if (retention_.valid && retention_.epoch == epoch)
        drop_task_ref_locked();
}

void ElfLoader::unmap_golden(TaskControlBlock *tcb) {
    if (!tcb || !tcb->get_page_table())
        return;
    arch::IrqGuard irq_guard{};
    SpinLockGuard<sync::SpinLock> guard(lock_);
    if (!retention_.valid || retention_.task_refs == 0)
        return;
    // Walk retained ranges over this table; unmap pages whose phys
    // matches the retained slot (a non-mapper's table never matches).
    // Fork children deep-copy (private phys), so they never match and
    // never disturb the count — no clone hook needed.
    size_t idx = 0;
    bool mapped = false;
    for (size_t r = 0; r < retention_.range_count; ++r) {
        for (size_t p = 0; p < retention_.ranges[r].page_count; ++p) {
            uint64_t va =
                retention_.ranges[r].vaddr + p * arch::PAGE_SIZE;
            uint64_t phys =
                VMM::virt_to_phys_in_pml4(va, tcb->get_page_table());
            if (phys != 0 && idx < retention_.golden_pages &&
                phys == retention_.golden[idx].phys) {
                VMM::unmap_page_in_pml4(va, tcb->get_page_table());
                mapped = true;
            }
            ++idx;
        }
    }
    if (mapped)
        drop_task_ref_locked();
}

// Unwind helper for try_rerun failure paths: unmap any retained-golden
// pages from the fresh table (they stay owned by retention/live mapper),
// then free the fresh-only pages + PML4. Caller drops its builder ref
// separately via drop_task_ref_locked. Assumes lock_ held + IRQ-masked.
void ElfLoader::unmap_fresh_golden_locked(uint64_t pml4,
                                             const GoldenRange *ranges,
                                             size_t range_count) {
    if (!pml4 || !ranges)
        return;
    // Unmap every mapped page in OUR ranges unconditionally: the fresh
    // table maps golden phys exactly here by construction (stack/heap/
    // pin went elsewhere), so no phys matching against possibly-
    // superseded retention state is needed.
    for (size_t r = 0; r < range_count && r < kMaxGoldenRanges; ++r) {
        for (size_t p = 0; p < ranges[r].page_count; ++p) {
            uint64_t va = ranges[r].vaddr + p * arch::PAGE_SIZE;
            if (VMM::virt_to_phys_in_pml4(va, pml4))
                VMM::unmap_page_in_pml4(va, pml4);
        }
    }
}

void ElfLoader::fail_fresh_build_locked(uint64_t pml4,
                                          const GoldenRange *ranges,
                                          size_t range_count) {
    if (!pml4)
        return;
    unmap_fresh_golden_locked(pml4, ranges, range_count);
    VMM::free_user_pages(pml4);
    PMM::free_page(pml4);
}

TaskControlBlock *ElfLoader::try_rerun(const char **reason) {
    static const char kNoElf[] = "no elf loaded";
    static const char kDynamic[] = "dynamic image needs reload";
    static const char kNotClean[] = "last run not clean, reload required";
    static const char kBusy[] = "loader busy";
    static const char kRebuildPml4[] = "rebuild failed: pml4";
    static const char kRebuildKstack[] = "rebuild failed: kstack";
    static const char kRebuildGolden[] = "rebuild failed: golden-map";
    static const char kRebuildStack[] = "rebuild failed: stack";
    static const char kRebuildHeap[] = "rebuild failed: heap";
    static const char kRebuildPin[] = "rebuild failed: pin";
    static const char kRebuildTcb[] = "rebuild failed: tcb-alloc";
    static const char kRebuildFinal[] = "rebuild failed: finalize";
    static const char kRebuildScan[] = "rebuild failed: rescan";
    static const char kRebuildSup[] = "rebuild failed: superseded";
    // Small stack view: big arrays (golden phys, phdr_image, pin_buf)
    // are streamed from retention_ directly. A concurrent adopt/abandon
    // (only via take/reset/release, same-thread callers in practice) is
    // caught by the epoch re-check at publish; torn mid-build reads can
    // only cause a fail-closed refuse, never a fault (retention_ is
    // always mapped kernel memory).
    struct View {
        GoldenRange ranges[kMaxGoldenRanges] = {};
        size_t range_count = 0;
        size_t golden_pages = 0;
        PinRange pin_ranges[kMaxPinRanges] = {};
        size_t pin_count = 0;
        ELF64Header hdr = {};
        char path[kMaxPath] = {};
        uint64_t file_size = 0;
        TaskControlBlock::RoRange ro_ranges[TaskControlBlock::kMaxRoRanges] = {};
        size_t ro_count = 0;
        uint32_t ro_crc32 = 0;
        uint64_t canary_slot = 0;
        uint64_t epoch = 0;
    };
    View view{};
    {
        arch::IrqGuard irq_guard{};
        SpinLockGuard<sync::SpinLock> guard(lock_);
        if (state_ != LoadState::IDLE) {
            if (reason) *reason = kBusy;
            return nullptr;
        }
        if (!retention_.valid) {
            if (reason) *reason = kNoElf;
            return nullptr;
        }
        if (retention_.has_dynamic) {
            if (reason) *reason = kDynamic;
            return nullptr;
        }
        if (retention_.outcome != RunOutcome::CLEAN ||
            retention_.outcome_code != 0) {
            if (reason) *reason = kNotClean;
            return nullptr;
        }
        // Bump first: abandon cannot free golden phys while we build.
        retention_.task_refs++;
        view.epoch = retention_.epoch;
        view.range_count = retention_.range_count;
        for (size_t i = 0; i < view.range_count && i < kMaxGoldenRanges; ++i)
            view.ranges[i] = retention_.ranges[i];
        view.golden_pages = retention_.golden_pages;
        view.pin_count = retention_.pin_count;
        for (size_t i = 0; i < view.pin_count && i < kMaxPinRanges; ++i)
            view.pin_ranges[i] = retention_.pin_ranges[i];
        view.hdr = retention_.hdr;
        for (size_t i = 0; i < kMaxPath; ++i) {
            view.path[i] = retention_.path[i];
            if (!retention_.path[i]) break;
        }
        view.file_size = retention_.file_size;
        view.ro_count = retention_.ro_count;
        for (size_t i = 0; i < view.ro_count &&
                            i < TaskControlBlock::kMaxRoRanges; ++i)
            view.ro_ranges[i] = retention_.ro_ranges[i];
        view.ro_crc32 = retention_.ro_crc32;
        view.canary_slot = retention_.canary_slot;
    }
    // Fresh address space.
    uint64_t pml4 = VMM::clone_kernel_pml4();
    if (!pml4) {
        arch::IrqGuard irq_guard{};
        SpinLockGuard<sync::SpinLock> guard(lock_);
        drop_builder_ref_locked(view.epoch); // release our builder ref
        if (reason) *reason = kRebuildPml4;
        return nullptr;
    }
    VMM::assert_kernel_half_converged(pml4);
    // Map golden RO pages. Golden phys stream from retention_ (8-byte
    // aligned reads); a torn mix from a concurrent supersede is caught
    // by the epoch re-check at publish -> fail-closed refuse.
    size_t gidx = 0;
    for (size_t r = 0; r < view.range_count && r < kMaxGoldenRanges; ++r) {
        for (size_t p = 0; p < view.ranges[r].page_count; ++p) {
            uint64_t va = view.ranges[r].vaddr + p * arch::PAGE_SIZE;
            bool map_ok = true;
            if (gidx < view.golden_pages) {
                uint64_t gphys = retention_.golden[gidx].phys;
                bool gexec = retention_.golden[gidx].executable;
                if (gphys) {
                    VMM::map_page_ro_in_pml4(va, gphys, gexec, pml4);
                    map_ok = (VMM::virt_to_phys_in_pml4(va, pml4) != 0);
                } else {
                    map_ok = false;
                }
            } else {
                map_ok = false;
            }
            if (!map_ok) {
                arch::IrqGuard irq_guard{};
                SpinLockGuard<sync::SpinLock> guard(lock_);
                fail_fresh_build_locked(pml4, view.ranges, view.range_count);
                drop_builder_ref_locked(view.epoch);
                if (reason) *reason = kRebuildGolden;
                return nullptr;
            }
            ++gidx;
        }
    }
    // Allocate user stack + heap (fresh).
    uint64_t ustack_phys = 0;
    // Reuse loader helper: maps stack/heap into pml4.
    // alloc_user_stack_and_heap is static in elf.cpp, not visible here,
    // so duplicate the mapping via VMM directly: allocate and map.
    // Minimal: allocate stack pages and heap pages via PMM and map.
    // Use PMM::alloc_user_contiguous for stack, heap.
    // Stack
    {
        size_t stack_pages = (TaskControlBlock::STACK_SIZE + 4095) / arch::PAGE_SIZE;
        uint64_t sphys = PMM::alloc_user_contiguous(stack_pages);
        if (!sphys) {
            arch::IrqGuard irq_guard{};
            SpinLockGuard<sync::SpinLock> guard(lock_);
            fail_fresh_build_locked(pml4, view.ranges, view.range_count);
            drop_builder_ref_locked(view.epoch);
            if (reason) *reason = kRebuildStack;
            return nullptr;
        }
        for (size_t i = 0; i < stack_pages; ++i) {
            // Issue #325-D: rebuild stack is data (never executable).
            if (VMM::map_page_in_pml4_err(
                    mem::STACK_VADDR + arch::PAGE_SIZE + i * arch::PAGE_SIZE,
                    sphys + i * arch::PAGE_SIZE, true, pml4,
                    false) != errors::VMM_ERR_OK) {
                for (size_t rest = i; rest < stack_pages; ++rest) {
                    PMM::free_page(sphys + rest * arch::PAGE_SIZE);
                }
                arch::IrqGuard irq_guard{};
                SpinLockGuard<sync::SpinLock> guard(lock_);
                fail_fresh_build_locked(pml4, view.ranges, view.range_count);
                drop_builder_ref_locked(view.epoch);
                if (reason) *reason = kRebuildStack;
                return nullptr;
            }
        }
        ustack_phys = sphys;
        // Zero stack via HHDM
        for (size_t i = 0; i < stack_pages; ++i) {
            __builtin_memset(reinterpret_cast<void*>(arch::HHDM_OFFSET + sphys + i * arch::PAGE_SIZE), 0, arch::PAGE_SIZE);
        }
    }
    // Heap (initial)
    {
        size_t heap_pages = 4; // 16 KiB initial heap like load_segments_and_stack
        uint64_t hphys = PMM::alloc_user_contiguous(heap_pages);
        if (!hphys) {
            arch::IrqGuard irq_guard{};
            SpinLockGuard<sync::SpinLock> guard(lock_);
            fail_fresh_build_locked(pml4, view.ranges, view.range_count);
            drop_builder_ref_locked(view.epoch);
            if (reason) *reason = kRebuildHeap;
            return nullptr;
        }
        for (size_t i = 0; i < heap_pages; ++i) {
            // Issue #325-D: rebuild heap is data (never executable).
            if (VMM::map_page_in_pml4_err(mem::HEAP_VADDR + i * arch::PAGE_SIZE,
                                          hphys + i * arch::PAGE_SIZE, true,
                                          pml4,
                                          false) != errors::VMM_ERR_OK) {
                for (size_t rest = i; rest < heap_pages; ++rest) {
                    PMM::free_page(hphys + rest * arch::PAGE_SIZE);
                }
                arch::IrqGuard irq_guard{};
                SpinLockGuard<sync::SpinLock> guard(lock_);
                fail_fresh_build_locked(pml4, view.ranges, view.range_count);
                drop_builder_ref_locked(view.epoch);
                if (reason) *reason = kRebuildHeap;
                return nullptr;
            }
        }
        __builtin_memset(reinterpret_cast<void*>(arch::HHDM_OFFSET + hphys), 0, heap_pages * arch::PAGE_SIZE);
    }
    // Allocate fresh writable pages for RW segments (full memsz span,
    // zeroed incl. BSS tails), then copy pin file bytes into them.
    for (size_t i = 0; i < view.pin_count && i < kMaxPinRanges; ++i) {
        uint64_t base = view.pin_ranges[i].vaddr & ~(arch::PAGE_SIZE - 1);
        uint64_t top = view.pin_ranges[i].vaddr + view.pin_ranges[i].memsz;
        uint64_t end = (top + arch::PAGE_SIZE - 1) & ~(arch::PAGE_SIZE - 1);
        for (uint64_t va = base; va < end; va += arch::PAGE_SIZE) {
            uint64_t wp = PMM::alloc_user_page();
            if (!wp) {
                arch::IrqGuard irq_guard{};
                SpinLockGuard<sync::SpinLock> guard(lock_);
                fail_fresh_build_locked(pml4, view.ranges, view.range_count);
                drop_builder_ref_locked(view.epoch);
                if (reason) *reason = kRebuildPin;
                return nullptr;
            }
            if (VMM::map_page_in_pml4_err(va, wp, true, pml4) !=
                errors::VMM_ERR_OK) {
                // Silent map failure: free the orphan page, then fail.
                PMM::free_page(wp);
                arch::IrqGuard irq_guard{};
                SpinLockGuard<sync::SpinLock> guard(lock_);
                fail_fresh_build_locked(pml4, view.ranges, view.range_count);
                drop_builder_ref_locked(view.epoch);
                if (reason) *reason = kRebuildPin;
                return nullptr;
            }
            __builtin_memset(reinterpret_cast<void*>(arch::HHDM_OFFSET + wp),
                             0, arch::PAGE_SIZE);
        }
    }
    // Copy writable pin bytes. Pin bytes stream from retention_
    // (torn mix on concurrent supersede -> epoch refuse at publish).
    for (size_t i = 0; i < view.pin_count && i < kMaxPinRanges; ++i) {
        uint64_t va = view.pin_ranges[i].vaddr;
        uint64_t off = view.pin_ranges[i].pin_off;
        uint64_t len = view.pin_ranges[i].len;
        uint64_t phys = VMM::virt_to_phys_in_pml4(va & ~(arch::PAGE_SIZE-1), pml4);
        if (!phys) {
            arch::IrqGuard irq_guard{};
            SpinLockGuard<sync::SpinLock> guard(lock_);
            fail_fresh_build_locked(pml4, view.ranges, view.range_count);
            drop_builder_ref_locked(view.epoch);
            if (reason) *reason = kRebuildPin;
            return nullptr;
        }
        // May span pages; copy via HHDM per page chunk
        uint64_t remaining = len;
        uint64_t cur_va = va;
        uint64_t cur_off = off;
        while (remaining > 0) {
            uint64_t page_va = cur_va & ~(arch::PAGE_SIZE-1);
            uint64_t page_off = cur_va & (arch::PAGE_SIZE-1);
            uint64_t chunk = arch::PAGE_SIZE - page_off;
            if (chunk > remaining) chunk = remaining;
            uint64_t p = VMM::virt_to_phys_in_pml4(page_va, pml4);
            if (!p) {
                arch::IrqGuard irq_guard{};
                SpinLockGuard<sync::SpinLock> guard(lock_);
                fail_fresh_build_locked(pml4, view.ranges, view.range_count);
                drop_builder_ref_locked(view.epoch);
                if (reason) *reason = kRebuildPin;
                return nullptr;
            }
            __builtin_memcpy(reinterpret_cast<void*>(arch::HHDM_OFFSET + p + page_off),
                             retention_.pin_buf + cur_off, chunk);
            cur_va += chunk;
            cur_off += chunk;
            remaining -= chunk;
        }
    }
    // Build TCB via finalize path: reuse elf::load finalizer helpers
    // For now, allocate TCB directly and minimally initialize.
    auto *tcb = static_cast<TaskControlBlock*>(kernel::MemPool::alloc(sizeof(TaskControlBlock)));
    if (!tcb) {
        arch::IrqGuard irq_guard{};
        SpinLockGuard<sync::SpinLock> guard(lock_);
        fail_fresh_build_locked(pml4, view.ranges, view.range_count);
        drop_builder_ref_locked(view.epoch);
        if (reason) *reason = kRebuildTcb;
        return nullptr;
    }
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wclass-memaccess"
    __builtin_memset(tcb, 0, sizeof(TaskControlBlock));
#pragma GCC diagnostic pop
    tcb->iopb_slot_ = TaskControlBlock::IOPB_SLOT_NONE;
    tcb->tls_base_ = 0; // issue #74: fresh image starts with no TLS
    tcb->wdog_armed = false;
    tcb->wdog_period_ticks = 0;
    tcb->wdog_last_kick_tick = 0;
    tcb->wdog_expiry_tick = 0;
    tcb->wdog_gen = 0;
    tcb->magic = TaskControlBlock::TCB_MAGIC;
    tcb->id = Scheduler::alloc_id();
    tcb->state = TaskState::READY;
    tcb->priority = 2;
    tcb->period_ticks = 0;
    tcb->base_priority = 2;
    tcb->executed_ticks = 0;
    tcb->exec_ns_total = 0;
    tcb->exec_period_ns = 0;
    tcb->exec_window_start_tick = arch::Timer::ticks(); // issue #331
    tcb->exec_stamp_ns = 0;
    tcb->wcet_observed_ns = 0;
    tcb->mem_alloc_ops_ = 0;
    tcb->mem_free_ops_ = 0;
    tcb->mem_prev_outstanding_ = 0;
    tcb->mem_prev_used_ = 0;
    tcb->mem_leak_streak_ = 0;
    tcb->sched_policy = SchedPolicy::AUTO;
    tcb->edf_exempt = false;
    tcb->edf_next_ = nullptr;
    tcb->edf_prev_ = nullptr;
    tcb->in_edf_queue_ = false;
    tcb->ready_deferred_ = false;
    tcb->remaining_ticks = 0;
    tcb->deadline_ticks = arch::Timer::ticks();
    tcb->clear_page_table();
    tcb->stack_phys_ = 0;
    tcb->kernel_stack = nullptr;
    tcb->kernel_stack_top = 0;
    tcb->user_stack_ = 0;
    tcb->user_stack_size_ = 0;
    tcb->program_break_start = 0;
    tcb->program_break = 0;
    tcb->buf_list_head = -1;
    tcb->fpu_used = false;
    tcb->sporadic_server = nullptr;

    init_task_common(*tcb);

    size_t kstack_pages =
        (TaskControlBlock::STACK_SIZE + 4095) / arch::PAGE_SIZE;
    uint64_t kstack_phys = PMM::alloc_contiguous(kstack_pages);
    if (!kstack_phys) {
        // No page_table_ yet: destroy handles IPC/fds/cwd teardown.
        destroy_completed_tcb(tcb);
        arch::IrqGuard irq_guard{};
        SpinLockGuard<sync::SpinLock> guard(lock_);
        fail_fresh_build_locked(pml4, view.ranges, view.range_count);
        drop_builder_ref_locked(view.epoch);
        if (reason) *reason = kRebuildKstack;
        return nullptr;
    }
    tcb->stack_phys_ = kstack_phys;
    // #434 P2: route through the kslot window (guard page below the
    // stack) via the shared funnel — HHDM fallback only on exhaustion.
    TaskControlBlock::provision_kstack(tcb, kstack_phys,
                                       TaskControlBlock::STACK_SIZE);

    tcb->install_page_table(pml4);
    tcb->is_user_ = true;
#if defined(CONFIG_ARCH_X86_64) && CONFIG_PCID
    tcb->pcid_ = arch::pcid_alloc();
#endif
    // Full init via shared finalizer (identical to first-run TCBs:
    // std fds, user stack frame, canaries, sizes, TLS guard).
    // hdr/phdr_image stream from retention_ (epoch-checked at publish).
    // The tail returns false without teardown on failure, so unmap OUR
    // golden (else blind free below takes retained phys), then destroy
    // owns kstack/IPC/fds/fresh-pages/MemPool teardown.
    // destroy does not touch refs; drop our builder ref after.
    if (!finalize_user_tcb_tail(tcb, &view.hdr, ustack_phys,
                                retention_.phdr_image, view.file_size)) {
        // finalize failed partway (kstack/frame/canaries): unmap OUR
        // golden (else blind free below takes retained phys), then
        // destroy owns kstack/IPC/fds/fresh-pages/MemPool teardown.
        // destroy does not touch refs; drop our builder ref after.
        {
            arch::IrqGuard irq_guard{};
            SpinLockGuard<sync::SpinLock> guard(lock_);
            unmap_fresh_golden_locked(tcb->get_page_table(), view.ranges,
                                      view.range_count);
            drop_builder_ref_locked(view.epoch);
        }
        destroy_completed_tcb(tcb);
        if (reason) *reason = kRebuildFinal;
        return nullptr;
    }
    // Rescan-compare: re-walk RO ranges (canary slot carved) against
    // the retained baseline. Catches decay since retention; never
    // re-adopts (compare-only per spec).
    {
        uint32_t acc = CRC32::INITIAL;
        bool scan_ok = true;
        for (size_t i = 0; i < view.ro_count && i < TaskControlBlock::kMaxRoRanges && scan_ok; ++i) {
            uint64_t vs = view.ro_ranges[i].vaddr;
            uint64_t ve = vs + view.ro_ranges[i].len;
            uint64_t cs = view.canary_slot;
            uint64_t ce = cs + 8;
            // Head before slot.
            if (cs != 0 && vs < cs && cs < ve) {
                if (!crc_user_range(pml4, vs, cs - vs, acc))
                    scan_ok = false;
                vs = ce;
            } else if (cs != 0 && cs <= vs && vs < ce) {
                vs = ce; // range starts inside slot: skip head
            }
            if (scan_ok && vs < ve) {
                if (!crc_user_range(pml4, vs, ve - vs, acc))
                    scan_ok = false;
            }
        }
        if (!scan_ok || CRC32::finalize(acc) != view.ro_crc32) {
            // Decay (or torn-pin mix): unmap OUR golden first so the
            // blind free in destroy cannot take retained phys, then
            // destroy + drop our builder ref.
            {
                arch::IrqGuard irq_guard{};
                SpinLockGuard<sync::SpinLock> guard(lock_);
                unmap_fresh_golden_locked(tcb->get_page_table(), view.ranges,
                                          view.range_count);
                drop_builder_ref_locked(view.epoch);
            }
            destroy_completed_tcb(tcb);
            if (reason) *reason = kRebuildScan;
            return nullptr;
        }
    }
    // Carry retained RO baseline (compare, never re-adopt) + UNVERIFIED.
    for (size_t i = 0; i < TaskControlBlock::kMaxRoRanges; ++i) {
        if (i < view.ro_count) {
            tcb->ro_ranges_[i] = view.ro_ranges[i];
        }
    }
    tcb->ro_seg_count_ = view.ro_count;
    tcb->ro_crc32_ = view.ro_crc32;
    tcb->ro_verify_state_ = TaskControlBlock::RoVerifyState::UNVERIFIED;
    // Copy name from retention path leaf
    {
        const char *slash = view.path;
        const char *p = view.path;
        while (*p) { if (*p=='/') slash=p+1; ++p; }
        size_t n=0; while (slash[n] && n+1 < CONFIG_TASK_NAME_LEN) { tcb->name[n]=slash[n]; ++n; }
        tcb->name[n]='\0';
    }
    // Publish: re-validate epoch (no supersede/abandon mid-build).
    // Our builder ref was bumped at the gate and is already counted.
    bool still_current = false;
    {
        arch::IrqGuard irq_guard{};
        SpinLockGuard<sync::SpinLock> guard(lock_);
        still_current =
            retention_.valid && retention_.epoch == view.epoch;
    }
    if (!still_current) {
        // Superseded while we built: unmap OUR golden (view ranges are
        // what WE mapped, regardless of current retention), then destroy
        // owns the rest; drop our builder ref explicitly since destroy
        // never touches refs.
        {
            arch::IrqGuard irq_guard{};
            SpinLockGuard<sync::SpinLock> guard(lock_);
            unmap_fresh_golden_locked(tcb->get_page_table(), view.ranges,
                                      view.range_count);
            drop_builder_ref_locked(view.epoch);
        }
        destroy_completed_tcb(tcb);
        if (reason) *reason = kRebuildSup;
        return nullptr;
    }
    return tcb;
}

TaskControlBlock *ElfLoader::take_completed() {
    arch::IrqGuard irq_guard{};
    SpinLockGuard<sync::SpinLock> guard(lock_);
    auto *t = completed_tcb_;
    completed_tcb_ = nullptr;
    // completed_entry_ intentionally NOT cleared: it stays latched to
    // the taken TCB until the next load/release/reset (the shell reads
    // it after take_completed for the entry breakpoint).
    // Issue #271 P1: move staging into retention (newest take wins,
    // even when the new load is ineligible — abandon covers that).
    if (t != nullptr)
        adopt_staging_locked();
    return t;
}

uint64_t ElfLoader::completed_entry() noexcept {
    SpinLockGuard<sync::SpinLock> guard(lock_);
    return completed_entry_;
}

void ElfLoader::release_completed() {
    arch::IrqGuard irq_guard{};
    SpinLockGuard<sync::SpinLock> guard(lock_);
    if (completed_tcb_) {
        destroy_completed_tcb(completed_tcb_);
        completed_tcb_ = nullptr;
        completed_entry_ = 0;
    }
    // Untaken completion gone: its staging goes with it. Post-take
    // retention is untouched (completed_tcb_ is null after take).
    staging_ = Retention{};
}

void ElfLoader::abandon_retention() {
    arch::IrqGuard irq_guard{};
    SpinLockGuard<sync::SpinLock> guard(lock_);
    abandon_retention_locked();
    staging_ = Retention{};
}

bool ElfLoader::has_retained() noexcept {
    arch::IrqGuard irq_guard{};
    SpinLockGuard<sync::SpinLock> guard(lock_);
    return retention_.valid;
}

ElfLoader::RunOutcome ElfLoader::retained_outcome() {
    arch::IrqGuard irq_guard{};
    SpinLockGuard<sync::SpinLock> guard(lock_);
    if (!retention_.valid)
        return RunOutcome::UNKNOWN;
    return retention_.outcome;
}

void ElfLoader::note_launched(uint64_t task_id) {
    arch::IrqGuard irq_guard{};
    SpinLockGuard<sync::SpinLock> guard(lock_);
    if (!retention_.valid)
        return;
    retention_.outcome = RunOutcome::UNKNOWN;
    retention_.outcome_code = 0;
    retention_.outcome_task = task_id;
}

void ElfLoader::note_task_end(uint64_t task_id, bool clean, uint64_t code) {
    arch::IrqGuard irq_guard{};
    SpinLockGuard<sync::SpinLock> guard(lock_);
    if (!retention_.valid)
        return;
    if (retention_.outcome_task != task_id)
        return; // never launched (or a different task): stay UNKNOWN
    if (clean) {
        retention_.outcome = RunOutcome::CLEAN;
        retention_.outcome_code = code;
    } else {
        retention_.outcome = RunOutcome::FAULTED;
        retention_.outcome_code = code;
    }
}

/// @brief Tear down a completed-but-never-scheduled TCB.  It was built by
///        finalize_loaded_task and never add_task'd, so cleanup()'s
///        Scheduler::unregister_task / parent / daemon logic must NOT run.
///        Frees the same resources: PML4 user pages + page, user stack,
///        kernel stack, IPC objects, fd table, then the MemPool block.
void ElfLoader::destroy_completed_tcb(TaskControlBlock *tcb) {
    if (!tcb || tcb->magic != TaskControlBlock::TCB_MAGIC)
        return;
    if (loader_tcb_ && tcb == loader_tcb_)
        return;
    // Teardown of a completed-but-NEVER-scheduled TCB (built by
    // finalize_loaded_task, never add_task'd).  cleanup() is unsafe here
    // (Scheduler::unregister_task removes an absent task; parent/daemon logic
    // and track_task_remove assume add_task ran).  Free the resources directly,
    // mirroring cleanup()'s frees but skipping all scheduler + task-accounting
    // interactions.
    if (tcb->get_page_table()) {
        kernel::BufferPool::unmap_all(*tcb);
        // Issue #95: release shared libs (unmap shared RO + drop
        // refcounts) BEFORE the blind free_user_pages below.
        kernel::elf::release_task_libs(tcb);
        // Issue #271 P1: the never-scheduled path has no task teardown
        // that could have freed golden pages (cleanup() owns that for
        // scheduled paths). The untaken completion's golden refs are
        // request_load's destroy (above); this path never touches
        // retention (vanilla two-step destroy).
        VMM::free_user_pages(tcb->get_page_table());
        PMM::free_page(tcb->get_page_table());
        tcb->clear_page_table();
    }
    if (tcb->stack_phys_) {
        size_t pages = (TaskControlBlock::STACK_SIZE + 4095) / arch::PAGE_SIZE;
        for (size_t i = 0; i < pages; ++i)
            PMM::free_page(tcb->stack_phys_ + i * arch::PAGE_SIZE);
        tcb->stack_phys_ = 0;
    }
    // IPC objects (track_*_add'd by init_task_common).
    tcb->msg_queue.~MessageQueue();
    kernel::test::ResourceTracker::instance().track_msg_queue_remove();
    tcb->notify.~Notify();
    kernel::test::ResourceTracker::instance().track_notify_remove();
    tcb->event_group.~EventGroup();
    kernel::test::ResourceTracker::instance().track_event_group_remove();
    // std fds (open_std_fds: alloc'd via fd_table.alloc => track_fd_add) share
    // /dev/tty WITHOUT vnode_ref_inc.  Close them manually: track_fd_remove +
    // ops->close, but NOT vnode_ref_dec (would over-decrement /dev/tty).
    for (size_t i = 0; i < vfs::MAX_FDS; ++i) {
        if (tcb->fd_table.fds[i].used) {
            auto *vn = tcb->fd_table.fds[i].vnode;
            if (vn && vn->ops && vn->ops->close)
                vn->ops->close(*vn);
            kernel::test::ResourceTracker::instance().track_fd_remove();
            tcb->fd_table.fds[i].used = false;
            tcb->fd_table.fds[i].vnode = nullptr;
        }
    }
    if (tcb->cwd_vnode)
        vfs::vnode_ref_dec(tcb->cwd_vnode);
    tcb->cwd_vnode = nullptr;
    tcb->magic = 0;
    kernel::MemPool::free(tcb);
}

void ElfLoader::reset() {
    if (state() != LoadState::IDLE) {
        request_cancel();
        wait_loader_idle();
    }
    arch::IrqGuard irq_guard{};
    SpinLockGuard<sync::SpinLock> guard(lock_);
    if (completed_tcb_) {
        // Issue #77 step 3: a completed TCB was built by
        // finalize_loaded_task and never add_task'd — teardown MUST be
        // destroy_completed_tcb (direct frees, no scheduler/task-accounting
        // interaction). cleanup()+delete here would unregister an absent
        // task and run parent/daemon logic on a never-scheduled TCB.
        destroy_completed_tcb(completed_tcb_);
        completed_tcb_ = nullptr;
        completed_entry_ = 0;
    }
    // Issue #271 P1: reset abandons retention (live-map guard inside:
    // pages mapped by a live run are NOT freed) and drops staging.
    abandon_retention_locked();
    staging_ = Retention{};
    cancel_requested_ = false;
}

void ElfLoader::wait_loader_idle() {
    // request_load woke the loader (set_task_ready), so it is READY + queued
    // and the harness's reschedule() dispatches it on the next tick.  Spin
    // until the loader returns to IDLE.
    for (uint64_t spins = 0; spins < 1000000 && state() != LoadState::IDLE;
         ++spins) {
        Scheduler::reschedule();
        arch::pause();
    }
    ENSURE(state() == LoadState::IDLE);
}

// ---------------------------------------------------------------------------
// Loader task internals
// ---------------------------------------------------------------------------

void ElfLoader::task_main() {
    for (;;) {
        // Idle: block the loader (BLOCKED + dequeue_ready) so it leaves the
        // ready queue and is selectable again when request_load wakes it.
        // The state check + block is ATOMIC under lock_: request_load sets
        // state=VALIDATING under the SAME lock, so a request arriving between
        // our IDLE check and our BLOCKED store is seen here — we skip blocking
        // and run it.  Without this, a request firing in the IDLE->BLOCKED
        // window would see the loader RUNNING (not BLOCKED), skip set_task_ready,
        // and the loader would block forever with state=VALIDATING (lost wakeup).
        if (state_ == LoadState::IDLE) {
            bool should_block = false;
            // IrqGuard: the critical section below (lock_ acquire -> set self
            // BLOCKED -> dequeue_ready -> release) MUST be atomic w.r.t. the
            // timer ISR.  Without it the loader can be PREEMPTED while holding
            // lock_ (dequeue_ready takes no IrqGuard), leave itself BLOCKED and
            // lock_ held, and the harness's reset()/request_load then spins on
            // lock_ forever (deadlock: loader BLOCKED-holding-lock, harness
            // spinning, timer never re-dispatches the BLOCKED loader).
            {
                arch::IrqGuard irq_guard{};
                SpinLockGuard<sync::SpinLock> guard(lock_);
                if (state_ == LoadState::IDLE) {
                    auto *self = Scheduler::current_task();
                    if (self) {
                        self->state = TaskState::BLOCKED;
                        Scheduler::dequeue_ready(*self);
                        should_block = true;
                    }
                }
            }
            if (should_block)
                Scheduler::reschedule();
            continue;
        }
        run_load();
    }
}

bool ElfLoader::cancel_pending(uint64_t generation) noexcept {
    return state() == LoadState::CANCELED || cancel_requested_ ||
           load_generation_ != generation;
}

int ElfLoader::open_owned_file(const char *path) {
    if (!loader_tcb_)
        return -1;
    return syscall_path_open(path, vfs::O_RDONLY);
}

char *ElfLoader::next_msg_slot() {
    uint32_t i = __atomic_fetch_add(&msg_idx_, 1U, __ATOMIC_RELAXED);
    return msg_buf_[i % 16];
}

/// @brief Build a load event message into the stable msg ring, then push to
///        dmesg and the log.  Message pattern:
///          loading <path> started at <sec>            (0xDB01)
///          loading <path> completed successfully in <sec.ms>  (0xDB02)
///          loading <path> canceled                    (0xDB03)
///          loading <path> failed: <reason>            (0xDB04-0xDB07)
/// @param code   Event code (0xDB01-0xDB07).
/// @param verb   Tail after "loading <path>": " started", " completed",
///               " canceled", " failed: ...".
/// @param ticks  Elapsed ticks for the completion event (1 tick = 1 ms).
/// @param kind   Event kind (started / completed / other).
void ElfLoader::post_event(uint64_t code, const char *verb, uint64_t ticks,
                           PostKind kind) {
    char *slot = next_msg_slot();
    int n = 0;
    auto append_str = [&slot, &n](const char *s) {
        while (*s && n < 159)
            slot[n++] = *s++;
    };
    auto append_dec = [&slot, &n](uint64_t v) {
        char tmp[24];
        int ti = 0;
        if (v == 0)
            tmp[ti++] = '0';
        while (v > 0 && ti < 23) {
            tmp[ti++] = static_cast<char>('0' + (v % 10));
            v /= 10;
        }
        while (ti > 0 && n < 159)
            slot[n++] = tmp[--ti];
    };

    append_str("loading ");
    append_str(path_);
    append_str(verb);
    if (kind == PostKind::STARTED) {
        // Wall-clock start time as HH:MM:SS (boot epoch + elapsed seconds).
        uint64_t total_sec =
            kernel::gs::get_boot_epoch() + arch::Timer::ticks() / 1000;
        uint64_t rem = total_sec % 86400ULL;
        uint64_t hh = rem / 3600ULL;
        uint64_t mm = (rem % 3600ULL) / 60ULL;
        uint64_t ss = rem % 60ULL;
        auto append_two = [&slot, &n, &append_dec](uint64_t v) {
            if (v < 10)
                slot[n++] = '0';
            append_dec(v);
        };
        append_str(" at ");
        append_two(hh);
        append_str(":");
        append_two(mm);
        append_str(":");
        append_two(ss);
    } else if (kind == PostKind::COMPLETED) {
        // Elapsed wall time as seconds.milliseconds (1 tick = 1 ms).
        append_str(" in ");
        append_dec(ticks / 1000);
        append_str(".");
        uint64_t ms = ticks % 1000;
        if (ms < 100)
            slot[n++] = '0';
        if (ms < 10)
            slot[n++] = '0';
        append_dec(ms);
        append_str("s");
    }
    slot[n] = '\0';
    // Issue #234: translate the legacy BASE 0xDBxx event code to the
    // canonical ELF record (base + offset) with catalog severity, then
    // push under the ELF subsystem. Unknown codes fail closed to ERROR.
    uint64_t canon = log::kDmesgBase_ELF + 4;
    log::LogSeverity sev = log::LogSeverity::ERROR;
    switch (code) {
    case 0xDB01:
        canon = log::kDmesgBase_ELF + 1;
        sev = log::LogSeverity::INFO;
        break;
    case 0xDB02:
        canon = log::kDmesgBase_ELF + 2;
        sev = log::LogSeverity::INFO;
        break;
    case 0xDB03:
        canon = log::kDmesgBase_ELF + 3;
        sev = log::LogSeverity::INFO;
        break;
    case 0xDB04:
        canon = log::kDmesgBase_ELF + 4;
        break;
    case 0xDB05:
        canon = log::kDmesgBase_ELF + 5;
        break;
    case 0xDB06:
        canon = log::kDmesgBase_ELF + 6;
        break;
    case 0xDB07:
        canon = log::kDmesgBase_ELF + 7;
        break;
    case 0xDB08:
        // 0xDB08 is produced only by the missing-shared-lib site: the
        // canonical record is ELF+10 (ERROR), not ELF+8 (rejected/WARN).
        canon = log::kDmesgBase_ELF + 10;
        break;
    case 0xDB09:
        // 0xDB09 is produced only by the layout-conflict site: the
        // canonical record is ELF+11 (ERROR), not ELF+9 (rejected/WARN).
        canon = log::kDmesgBase_ELF + 11;
        break;
    case 0xDB0A:
        canon = log::kDmesgBase_ELF + 12;
        break;
    case 0xDB0B:
        canon = log::kDmesgBase_ELF + 13;
        break;
    case 0xDB0C:
        canon = log::kDmesgBase_ELF + 14;
        sev = log::LogSeverity::FATAL;
        break;
    case 0xDB0D:
        canon = log::kDmesgBase_ELF + 15;
        break;
    default:
        break;
    }
    log::dmesg_push_sev(log::ErrorSubsystem::ELF, canon, sev, slot, 0);
    // Issue #407 item 2: the console line is a deferred URC, drained at
    // the next prompt redraw — no raw interleave with typed input.  The
    // queue is bounded: on overflow the legacy immediate pair keeps the
    // line visible, so no notice disappears.
    if (!kernel::console::urc::emit(kernel::console::urc::Tag::Elf, slot)) {
        kernel::Logger::info("%s", slot);
        // Live-session finding (#77 report): loader progress must ALSO reach
        // the framebuffer console — Logger serves serial/dmesg only, so
        // framebuffer users never saw completions.  write_fb renders pixels
        // without touching serial (no duplicate lines) or the shell capture
        // buffer (async loader text must not pollute redirections).
        service::Terminal::write_fb(slot);
        service::Terminal::write_fb("\n");
    }
}

void ElfLoader::cleanup_and_idle() {
    // Issue #95: dep-request teardown (no-op for static loads: the
    // context is zeroed).  Unmap shared RO from the dying pml4 FIRST
    // (cache owns shared phys by refcount; the blind free below must
    // not see it), then drop refcounts, then free retained buffers.
    // Skipped when pml4_ is already gone (handoff-fail path destroys
    // the TCB, which drains via release_task_libs instead).
    if (pml4_ != 0) {
        for (uint64_t i = 0; i < dep_ctx_.acquired_count; ++i) {
            unmap_acquired_ro(pml4_, &dep_ctx_, i);
        }
    }
    for (uint64_t i = 0; i < dep_ctx_.acquired_count; ++i) {
        SharedLibCache::instance().release(dep_ctx_.acquired[i]);
        dep_ctx_.acquired[i] = nullptr;
    }
    dep_ctx_.acquired_count = 0;
    free_resolve_images(&dep_ctx_);
    if (pml4_) {
        VMM::free_user_pages(pml4_);
        PMM::free_page(pml4_);
        pml4_ = 0;
    }
    if (fd_ >= 0 && loader_tcb_) {
        loader_tcb_->fd_table.free(fd_);
        fd_ = -1;
    }
    cancel_requested_ = false;
    state_ = LoadState::IDLE;
}

// ---------------------------------------------------------------------------
// run_load: the full chunked load cycle
// ---------------------------------------------------------------------------

void ElfLoader::run_load() {
    uint64_t gen = load_generation_;
    fd_ = -1;
    pml4_ = 0;
    seg_idx_ = 0;
    page_in_seg_ = 0;
    // Issue #46: the CRC table must be live before the first update — the
    // idle task (the other init caller) may never have run (notably in
    // tests), and an uninitialized table makes update() ignore its input,
    // which would reduce the whole scheme to theater.  Idempotent.
    CRC32::init();
    load_crc_ = CRC32::INITIAL; // issue #46: incremental RO baseline reset

    // Note the load start in dmesg (0xDB01 "ELF load started").  Posted from
    // the loader task so the entry is attributed to it.
    post_event(0xDB01, " started", 0, PostKind::STARTED);

    // ---- VALIDATING ----
    fd_ = open_owned_file(path_);
    if (fd_ < 0) {
        post_event(0xDB06, " failed: file not found", 0, PostKind::OTHER);
        cleanup_and_idle();
        return;
    }
    auto *fd_entry = loader_tcb_->fd_table.get(fd_);
    if (!fd_entry || !fd_entry->vnode || !fd_entry->vnode->ops->read) {
        post_event(0xDB07, " failed: read error", 0, PostKind::OTHER);
        cleanup_and_idle();
        return;
    }
    auto *vn = fd_entry->vnode;

    int64_t n = vn->ops->read(*vn, reinterpret_cast<uint8_t *>(&hdr_),
                              sizeof(ELF64Header), 0);
    if (n != static_cast<int64_t>(sizeof(ELF64Header))) {
        post_event(0xDB07, " failed: read error", 0, PostKind::OTHER);
        cleanup_and_idle();
        return;
    }
    if (!validate_header(&hdr_)) {
        post_event(0xDB04, " failed: invalid elf-file", 0, PostKind::OTHER);
        cleanup_and_idle();
        return;
    }
    // Issue #77: the background loader serves runelf activation, which
    // dispatches the image as a plain ET_EXEC user task — dynamic/interp
    // images are rejected here with a clear error, never loaded to die at
    // rip 0.  Shared validate_header stays ET_EXEC+ET_DYN-capable: the #95
    // shared-lib loader resolves dependencies through its own backend,
    // never through this validation.
    if (hdr_.type != ET_EXEC) {
        post_event(0xDB04, " failed: only ET_EXEC supported", 0,
                   PostKind::OTHER);
        cleanup_and_idle();
        return;
    }
    uint64_t phnum_sz = static_cast<uint64_t>(hdr_.phnum) * hdr_.phentsize;
    if (phnum_sz > sizeof(phdr_image_) - sizeof(ELF64Header)) {
        post_event(0xDB04, " failed: invalid elf-file", 0, PostKind::OTHER);
        cleanup_and_idle();
        return;
    }
    __builtin_memcpy(phdr_image_, &hdr_, sizeof(ELF64Header));
    n = vn->ops->read(*vn, phdr_image_ + sizeof(ELF64Header), phnum_sz,
                      hdr_.phoff);
    if (n != static_cast<int64_t>(phnum_sz)) {
        post_event(0xDB07, " failed: read error", 0, PostKind::OTHER);
        cleanup_and_idle();
        return;
    }

    for (uint16_t i = 0; i < hdr_.phnum; ++i) {
        auto *phdr = reinterpret_cast<const ELF64ProgramHeader *>(
            phdr_image_ + sizeof(ELF64Header) +
            static_cast<uint64_t>(i) * hdr_.phentsize);
        if (!validate_segment(phdr, file_size_)) {
            post_event(0xDB04, " failed: invalid elf-file", 0, PostKind::OTHER);
            cleanup_and_idle();
            return;
        }
    }
    if (cancel_pending(gen)) {
        post_event(0xDB03, " canceled", 0, PostKind::OTHER);
        cleanup_and_idle();
        return;
    }

    // Issue #46: locate the TEXT before-canary slot (0 = none) so both
    // the incremental CRC and the baseline scan exclude the 8 bytes the
    // canary subsystem owns and checks itself.
    uint64_t canary_slot = 0;
    // NOTE: phdr_image_ is a PACKED buffer ([header@0][phdrs@64]) — pass the
    // buffer offset, not the file offset hdr_.phoff (they differ whenever an
    // image places its phdrs elsewhere; validate_header permits that).
    (void)text_canary_slot(phdr_image_, sizeof(ELF64Header), hdr_.phnum,
                           hdr_.phentsize, file_size_, &canary_slot);

    pml4_ = VMM::clone_kernel_pml4();
    if (!pml4_) {
        post_event(0xDB05, " failed: not enough memory", 0, PostKind::OTHER);
        cleanup_and_idle();
        return;
    }
    // Issue #96: structural audit on the background-loaded image.
    VMM::assert_kernel_half_converged(pml4_);
    state_ = LoadState::COPYING_SEGMENTS;
    exec_base_ = 0;
    exec_size_ = 0;

    // ---- COPYING_SEGMENTS ----
    while (seg_idx_ < hdr_.phnum) {
        auto *phdr = reinterpret_cast<const ELF64ProgramHeader *>(
            phdr_image_ + sizeof(ELF64Header) +
            static_cast<uint64_t>(seg_idx_) * hdr_.phentsize);
        if (phdr->type != PT_LOAD) {
            ++seg_idx_;
            page_in_seg_ = 0;
            continue;
        }
        uint64_t vaddr_base = page_align_down(phdr->vaddr);
        uint64_t vaddr_end = page_align_up(phdr->vaddr + phdr->memsz);
        // Issue #95: track the exec range for lib overlap checks
        // (segments validated overflow-free in VALIDATING).
        if (exec_size_ == 0) {
            exec_base_ = vaddr_base;
            exec_size_ = vaddr_end - vaddr_base;
        } else {
            if (vaddr_base < exec_base_) {
                exec_size_ += exec_base_ - vaddr_base;
                exec_base_ = vaddr_base;
            }
            if (vaddr_end > exec_base_ + exec_size_) {
                exec_size_ = vaddr_end - exec_base_;
            }
        }
        uint64_t num_pages = (vaddr_end - vaddr_base) / arch::PAGE_SIZE;
        while (page_in_seg_ < num_pages) {
            uint64_t vaddr = vaddr_base + page_in_seg_ * arch::PAGE_SIZE;
            // File offset of this page's first byte. Negative for the
            // first page of a skewed segment (vaddr not page-aligned):
            // ELF congruence guarantees offset%4K == vaddr%4K, but
            // neither is 0 in general — copying file[offset..] to the
            // page start (as before) misplaces every page by the skew
            // (issue #75: .data/.fini_array landed 0xbd8 low, stdin FILE
            // zeroed, exit() called address 0). Mirror the direct
            // loader's offset_in_region (elf.cpp): file bytes go to
            // page_start + (vaddr - vaddr_base).
            int64_t rel = static_cast<int64_t>(vaddr) -
                          static_cast<int64_t>(phdr->vaddr);
            uint64_t dst_off = 0;
            uint64_t src_off = 0;
            if (rel < 0) {
                dst_off = static_cast<uint64_t>(-rel);
                src_off = 0;
            } else {
                src_off = static_cast<uint64_t>(rel);
            }
            uint64_t copy_len = 0;
            if (src_off < phdr->filesz) {
                copy_len = phdr->filesz - src_off;
                uint64_t room = kChunkSize - dst_off;
                if (copy_len > room)
                    copy_len = room;
            }
            if (copy_len > 0) {
                int64_t r = vn->ops->read(*vn, chunk_buf_, copy_len,
                                          phdr->offset + src_off);
                if (r != static_cast<int64_t>(copy_len)) {
                    post_event(0xDB07, " failed: read error", 0, PostKind::OTHER);
                    cleanup_and_idle();
                    return;
                }
                // Issue #46: fold read-only file bytes into the incremental
                // baseline as they land (chunk_buf_ holds exactly the
                // copy_len bytes just read; skew/padding excluded).  Chunk
                // byte i maps to VA (phdr->vaddr + src_off + i) in every
                // skew case; bytes inside the TEXT before-canary slot are
                // skipped (canary-owned, covered by the canary check).
                if (!(phdr->flags & PF_W)) {
                    uint64_t va0 = phdr->vaddr + src_off;
                    if (canary_slot != 0 && va0 < canary_slot + 8 &&
                        va0 + copy_len > canary_slot) {
                        if (va0 < canary_slot) {
                            load_crc_ = CRC32::update(
                                load_crc_, chunk_buf_, canary_slot - va0);
                        }
                        uint64_t after = canary_slot + 8;
                        if (va0 + copy_len > after) {
                            uint64_t skip = after - va0;
                            load_crc_ = CRC32::update(
                                load_crc_, chunk_buf_ + skip, copy_len - skip);
                        }
                    } else {
                        load_crc_ =
                            CRC32::update(load_crc_, chunk_buf_, copy_len);
                    }
                }
            }
            uint64_t phys = PMM::alloc_user_page();
            if (!phys) {
                post_event(0xDB05, " failed: not enough memory", 0,
                           PostKind::OTHER);
                cleanup_and_idle();
                return;
            }
            if (VMM::map_page_in_pml4_err(vaddr, phys, true, pml4_) !=
                errors::VMM_ERR_OK) {
                post_event(0xDB05, " failed: page-table OOM", 0,
                           PostKind::OTHER);
                PMM::free_page(phys);
                cleanup_and_idle();
                return;
            }
            uint8_t *page = reinterpret_cast<uint8_t *>(arch::HHDM_OFFSET +
                                                        phys);
            __builtin_memset(page, 0, kChunkSize);
            if (copy_len > 0)
                __builtin_memcpy(page + dst_off, chunk_buf_, copy_len);
            // Issue #271 P0: read-only segment pages map PTE-read-only
            // through the shared primitive (content flowed via HHDM,
            // perm-independent; canary install later uses HHDM phys).
            if (!(phdr->flags & PF_W)) {
                VMM::map_page_ro_in_pml4(vaddr, phys,
                                         (phdr->flags & PF_X) != 0, pml4_);
            }

            ++page_in_seg_;
            if (cancel_pending(gen)) {
                post_event(0xDB03, " canceled", 0, PostKind::OTHER);
                cleanup_and_idle();
                return;
            }
            // Yield: let higher-priority tasks run between chunks.  No locks
            // held here.
            Scheduler::reschedule();
        }
        ++seg_idx_;
        page_in_seg_ = 0;
    }

    // ---- LOADING_DEPS / LOADING_RELOC (issue #95) ----
    // Static execs (no PT_DYNAMIC) skip both stages trivially.
    {
        bool has_dynamic = false;
        for (uint16_t i = 0; i < hdr_.phnum; ++i) {
            auto *phdr = reinterpret_cast<const ELF64ProgramHeader *>(
                phdr_image_ + sizeof(ELF64Header) +
                static_cast<uint64_t>(i) * hdr_.phentsize);
            if (phdr->type == PT_DYNAMIC) {
                has_dynamic = true;
                break;
            }
        }
        if (has_dynamic) {
            state_ = LoadState::LOADING_DEPS;
            if (file_size_ == 0 || file_size_ > MAX_SO_FILE_SIZE) {
                post_event(0xDB05, " failed: not enough memory", 0,
                           PostKind::OTHER);
                cleanup_and_idle();
                return;
            }
            uint64_t npages =
                (file_size_ + arch::PAGE_SIZE - 1) / arch::PAGE_SIZE;
            uint64_t exec_buf = PMM::alloc_contiguous(npages);
            if (!exec_buf) {
                post_event(0xDB05, " failed: not enough memory", 0,
                           PostKind::OTHER);
                cleanup_and_idle();
                return;
            }
            bool read_ok = true;
            for (uint64_t i = 0; i < npages; ++i) {
                uint64_t want = arch::PAGE_SIZE;
                if (i + 1 == npages) {
                    want = file_size_ - i * arch::PAGE_SIZE;
                }
                int64_t r = vn->ops->read(
                    *vn,
                    reinterpret_cast<uint8_t *>(arch::HHDM_OFFSET + exec_buf +
                                                i * arch::PAGE_SIZE),
                    want, i * arch::PAGE_SIZE);
                if (r != static_cast<int64_t>(want)) {
                    read_ok = false;
                    break;
                }
                if (cancel_pending(gen)) {
                    for (uint64_t j = 0; j < npages; ++j) {
                        PMM::free_page(exec_buf + j * arch::PAGE_SIZE);
                    }
                    post_event(0xDB03, " canceled", 0, PostKind::OTHER);
                    cleanup_and_idle();
                    return;
                }
                Scheduler::reschedule();
            }
            if (!read_ok) {
                for (uint64_t j = 0; j < npages; ++j) {
                    PMM::free_page(exec_buf + j * arch::PAGE_SIZE);
                }
                post_event(0xDB07, " failed: read error", 0, PostKind::OTHER);
                cleanup_and_idle();
                return;
            }
            init_resolve_context(&dep_ctx_, pml4_, gen);
            dep_ctx_.exec_base = exec_base_;
            dep_ctx_.exec_size = exec_size_;
            dep_ctx_.image_phys[0] = exec_buf; // Uniform teardown:
            dep_ctx_.image_npages[0] = npages; // free_resolve_images
                                               // frees the exec buffer
                                               // on every path.
            // NOLINTNEXTLINE(performance-no-int-to-ptr)
            const auto *exec_img = reinterpret_cast<const ELF64Header *>(
                arch::HHDM_OFFSET + exec_buf);
            // NOLINTNEXTLINE(performance-no-int-to-ptr)
            const uint8_t *exec_bytes = reinterpret_cast<const uint8_t *>(
                arch::HHDM_OFFSET + exec_buf);
            bool deps_ok = resolve_dependencies(exec_img, exec_bytes,
                                                file_size_, &dep_ctx_, 0,
                                                exec_base_);
            state_ = LoadState::LOADING_RELOC;
            bool reloc_ok = deps_ok && relocate_closure(&dep_ctx_);
            // Error verbs carry the soname chain (0xDB08-0xDB0B).
            if (!deps_ok || !reloc_ok) {
                char verb[160] = {};
                const char *prefix = " failed: ";
                uint64_t code = 0xDB04;
                switch (dep_ctx_.last_error) {
                    case ElfError::NOT_FOUND:
                        prefix = " failed: missing shared lib ";
                        code = 0xDB08;
                        break;
                    case ElfError::LAYOUT:
                        prefix = " failed: layout conflict ";
                        code = 0xDB09;
                        break;
                    case ElfError::CYCLE:
                    case ElfError::DEPTH:
                        prefix = " failed: dep cycle/depth ";
                        code = 0xDB0A;
                        break;
                    case ElfError::NOMEM:
                        prefix = " failed: not enough memory";
                        code = 0xDB05;
                        break;
                    case ElfError::CANCELED:
                        prefix = " canceled";
                        code = 0xDB03;
                        break;
                    default:
                        prefix = " failed: invalid shared lib ";
                        code = 0xDB0B;
                        break;
                }
                uint64_t n = 0;
                while (prefix[n] != '\0' && n < sizeof(verb) - 1) {
                    verb[n] = prefix[n];
                    ++n;
                }
                for (uint64_t i = 0; dep_ctx_.fail_chain[i] != '\0' &&
                                    n < sizeof(verb) - 1;
                     ++i) {
                    verb[n++] = dep_ctx_.fail_chain[i];
                }
                verb[n] = '\0';
                // Teardown (unmap shared RO, releases, buffers, pml4)
                // lives in cleanup_and_idle; just report + run it.
                post_event(code, verb, 0, PostKind::OTHER);
                cleanup_and_idle();
                return;
            }
    // Success path continues below; the retained buffers (exec +
    // libs) are freed after the TCB handoff via free_resolve_images.
        } // end if (has_dynamic): static execs skip deps/reloc entirely.
    } // end Stage B/C block.
    state_ = LoadState::MAPPING;

    // ---- MAPPING (bounded: stack + heap + TCB) ----
    if (cancel_pending(gen)) {
        post_event(0xDB03, " canceled", 0, PostKind::OTHER);
        cleanup_and_idle();
        return;
    }
    uint64_t ustack_phys = 0;
    if (!alloc_user_stack_and_heap(pml4_, &ustack_phys)) {
        post_event(0xDB05, " failed: not enough memory", 0, PostKind::OTHER);
        cleanup_and_idle();
        return;
    }
    auto *tcb = finalize_loaded_task(&hdr_, pml4_, ustack_phys, phdr_image_,
                                     file_size_);
    if (!tcb) {
        post_event(0xDB05, " failed: not enough memory", 0, PostKind::OTHER);
        cleanup_and_idle();
        return;
    }
    // Issue #46: capture the post-relocation RO baseline into the TCB
    // before publication.  Static images cross-check the incremental
    // chunk CRC (copy-path corruption fails the load); dynamic images
    // adopt the scan (relocations legitimately rewrote bytes).
    {
        bool has_dynamic = false;
        for (uint16_t i = 0; i < hdr_.phnum; ++i) {
            auto *phdr = reinterpret_cast<const ELF64ProgramHeader *>(
                phdr_image_ + sizeof(ELF64Header) +
                static_cast<uint64_t>(i) * hdr_.phentsize);
            if (phdr->type == PT_DYNAMIC) {
                has_dynamic = true;
                break;
            }
        }
        // NOTE: packed buffer (see above) — sizeof(ELF64Header), not phoff.
        if (!snapshot_ro_baseline(pml4_, phdr_image_, sizeof(ELF64Header),
                                  hdr_.phnum,
                                  hdr_.phentsize, file_size_, has_dynamic,
                                  load_crc_, canary_slot, tcb)) {
            destroy_completed_tcb(tcb);            pml4_ = 0; // Owned + freed by destroy; cleanup must not re-free.
            post_event(0xDB0D, " failed: authenticity baseline", 0,
                       PostKind::OTHER);
            cleanup_and_idle();
            return;
        }
    }
    tcb->priority = 2;
    tcb->base_priority = 2;
    {
        const char *slash = path_;
        const char *p = path_;
        while (*p) {
            if (*p == '/')
                slash = p + 1;
            ++p;
        }
        copy_bounded(tcb->name, slash, CONFIG_TASK_NAME_LEN);
        tcb->name[CONFIG_TASK_NAME_LEN - 1] = '\0';
    }
    // Issue #95: hand request-local acquired sonames to the completed
    // TCB (owns the refcounts from here; released in TCB::cleanup or
    // destroy_completed_tcb).  Fail closed when a name cannot be
    // recorded exactly (a truncated copy could never match its cache
    // key, leaking the refcount).
    tcb->needed_lib_count = 0;
    bool handoff_ok = true;
    for (uint64_t i = 0;
         i < dep_ctx_.acquired_count && i < TaskControlBlock::kMaxTaskLibs;
         ++i) {
        const char *s = dep_ctx_.acquired[i];
        uint64_t n = 0;
        while (s[n] != '\0' && n < TaskControlBlock::kMaxLibSoname) {
            ++n;
        }
        if (n >= TaskControlBlock::kMaxLibSoname || s[n] != '\0') {
            handoff_ok = false;
            break;
        }
        for (uint64_t j = 0; j <= n; ++j) {
            tcb->needed_libs[tcb->needed_lib_count][j] = s[j];
        }
        ++tcb->needed_lib_count;
    }
    if (!handoff_ok ||
        dep_ctx_.acquired_count > TaskControlBlock::kMaxTaskLibs) {
        // Reset the partial TCB list so destroy releases nothing;
        // cleanup below drops every acquired refcount exactly once
        // (unmap skipped: pml4_ is gone — destroy drained via the TCB).
        tcb->needed_lib_count = 0;
        destroy_completed_tcb(tcb);
        pml4_ = 0; // Owned + freed by destroy; cleanup must not re-free.
        post_event(0xDB05, " failed: not enough memory", 0, PostKind::OTHER);
        cleanup_and_idle();
        return;
    }

    completed_tcb_ = tcb;
    // Issue #231: latch the entry VA with the retained completion so a
    // later load cannot move it under the TCB the shell is about to take.
    completed_entry_ = hdr_.entry;
    // Issue #271 P1: stage rerun retention from this load (loader state
    // is this load's here; take moves it). Scoped lock: bounded walks
    // only, never reschedule (run_load is otherwise lock-free).
    {
        arch::IrqGuard irq_guard{};
        SpinLockGuard<sync::SpinLock> stage_guard(lock_);
        fill_staging_locked(tcb);
    }
    // Ownership transfer complete: the TCB holds refcounts (released
    // at task death); drop the request-local records + retained buffers
    // WITHOUT releasing (that would steal the task's references).
    free_resolve_images(&dep_ctx_);
    dep_ctx_.acquired_count = 0;
    state_ = LoadState::DONE;
    uint64_t elapsed = arch::Timer::ticks() - start_ticks_;
    post_event(0xDB02, " completed successfully", elapsed, PostKind::COMPLETED);

    if (fd_ >= 0 && loader_tcb_) {
        loader_tcb_->fd_table.free(fd_);
        fd_ = -1;
    }
    state_ = LoadState::IDLE;
}

} // namespace elf
} // namespace kernel
