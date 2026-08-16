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
#include <kernel/task/task.hpp>
#include <kernel/task/scheduler.hpp>
#include <kernel/memory/pmm.hpp>
#include <kernel/memory/vmm.hpp>
#include <kernel/memory/mempool.hpp>
#include <kernel/vfs/vfs.hpp>
#include <kernel/log/dmesg.hpp>
#include <kernel/sync/spinlock_guard.hpp>
#include <kernel/arch/hal/irq_guard.hpp>
#include <kernel/syscall/syscall_helpers.hpp>
#include <kernel/ipc/buffer_pool.hpp>
#include <kernel/arch/timer.hpp>
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
char ElfLoader::msg_buf_[16][160] = {};
uint32_t ElfLoader::msg_idx_ = 0;

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
    // Resolve the file OUTSIDE the spinlock (vfs::resolve may block on I/O).
    vfs::Vnode *vn = vfs::resolve(path);
    if (!vn)
        return LoadResult::FILE_NOT_FOUND;
    uint64_t fsize = vn->size;

    SpinLockGuard<sync::SpinLock> guard(lock_);
    if (state_ != LoadState::IDLE)
        return LoadResult::ALREADY_LOADING;
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

TaskControlBlock *ElfLoader::take_completed() {
    SpinLockGuard<sync::SpinLock> guard(lock_);
    auto *t = completed_tcb_;
    completed_tcb_ = nullptr;
    return t;
}

void ElfLoader::release_completed() {
    SpinLockGuard<sync::SpinLock> guard(lock_);
    if (completed_tcb_) {
        destroy_completed_tcb(completed_tcb_);
        completed_tcb_ = nullptr;
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
    if (tcb->page_table_) {
        kernel::BufferPool::unmap_all(*tcb);
        // free_user_pages reclaims ALL user pages in the PML4 — including the
        // user stack and heap (mapped there by alloc_user_stack_and_heap).  Do
        // NOT free tcb->user_stack_ separately (double-free).
        VMM::free_user_pages(tcb->page_table_);
        PMM::free_page(tcb->page_table_);
        tcb->page_table_ = 0;
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
    SpinLockGuard<sync::SpinLock> guard(lock_);
    if (completed_tcb_) {
        completed_tcb_->cleanup();
        delete completed_tcb_;
        completed_tcb_ = nullptr;
    }
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

bool ElfLoader::cancel_pending(uint64_t generation) {
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
        // Wall-clock start time in seconds (boot epoch + ticks/1000).
        append_str(" at ");
        append_dec(kernel::gs::get_boot_epoch() + arch::Timer::ticks() / 1000);
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
    log::dmesg_push_base(code, slot, 0);
    kernel::Logger::info("%s", slot);
}

void ElfLoader::cleanup_and_idle() {
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

    pml4_ = VMM::clone_kernel_pml4();
    if (!pml4_) {
        post_event(0xDB05, " failed: not enough memory", 0, PostKind::OTHER);
        cleanup_and_idle();
        return;
    }
    state_ = LoadState::COPYING_SEGMENTS;

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
        uint64_t num_pages = (vaddr_end - vaddr_base) / arch::PAGE_SIZE;
        while (page_in_seg_ < num_pages) {
            uint64_t vaddr = vaddr_base + page_in_seg_ * arch::PAGE_SIZE;
            uint64_t file_off =
                phdr->offset + page_in_seg_ * arch::PAGE_SIZE;
            uint64_t in_file = 0;
            uint64_t seg_file_end = phdr->offset + phdr->filesz;
            if (file_off < seg_file_end) {
                in_file = seg_file_end - file_off;
                if (in_file > kChunkSize)
                    in_file = kChunkSize;
            }
            if (in_file > 0) {
                int64_t r = vn->ops->read(*vn, chunk_buf_, in_file, file_off);
                if (r != static_cast<int64_t>(in_file)) {
                    post_event(0xDB07, " failed: read error", 0, PostKind::OTHER);
                    cleanup_and_idle();
                    return;
                }
            } else {
                __builtin_memset(chunk_buf_, 0, kChunkSize);
            }
            uint64_t phys = PMM::alloc_user_page();
            if (!phys) {
                post_event(0xDB05, " failed: not enough memory", 0,
                           PostKind::OTHER);
                cleanup_and_idle();
                return;
            }
            VMM::map_page_in_pml4(vaddr, phys, true, pml4_);
            __builtin_memcpy(reinterpret_cast<void *>(arch::HHDM_OFFSET + phys),
                             chunk_buf_, in_file);
            if (in_file < kChunkSize)
                __builtin_memset(
                    reinterpret_cast<void *>(arch::HHDM_OFFSET + phys +
                                             in_file),
                    0, kChunkSize - in_file);

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

    completed_tcb_ = tcb;
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
