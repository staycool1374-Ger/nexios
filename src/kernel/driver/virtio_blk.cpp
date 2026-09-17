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

/// @file virtio_blk.cpp
/// @brief Virtio block driver implementation.

#include <kernel/driver/virtio_blk.hpp>
#include <kernel/memory/mempool.hpp>
#include <kernel/memory/pmm.hpp>
#include <kernel/arch/pci.hpp>
#include <kernel/arch/hal/idt.hpp>
#include <kernel/task/scheduler.hpp>
#include <kernel/task/task.hpp>
#include <kernel/ipc/ipc.hpp>
#include <kernel/sync/spinlock_guard.hpp>
#include <kernel/sync/irq_spinlock_guard.hpp>
#include <logger.hpp>
#include <string.hpp>
#include <lib/atomic.hpp>

using namespace kernel;
using namespace arch;

namespace kernel::block {

// TU-local ISR target (#65): set when the completion handler is
// registered, cleared on disable/teardown.  Never crosses TUs.
namespace {
VirtioBlkDriver *g_virtio_blk_isr_target = nullptr;
} // namespace

VirtioBlkDriver::VirtioBlkDriver(arch::VirtioTransport &transport)
    : transport_(transport) {
}

VirtioBlkDriver::~VirtioBlkDriver() {
    // #65: disarm the completion ISR first (masks the queue vector, acks
    // ISR status, drains an armed waiter with an error wake) so no
    // in-flight completion path can touch the freed queue pages.
    disable_irq();

    if (desc_phys_)
        PMM::free_page(desc_phys_);
    if (avail_phys_)
        PMM::free_page(avail_phys_);
    if (used_phys_)
        PMM::free_page(used_phys_);
    if (dma_buf_phys_)
        PMM::free_page(dma_buf_phys_);
}

bool VirtioBlkDriver::init() {
    if (!arch::virtio_init_transport(transport_)) {
        Logger::error("virtio-blk: transport init failed");
        return false;
    }

    // Negotiate features: require VERSION_1, offer nothing extra
    uint64_t features = VIRTIO_F_VERSION_1;
    if (!arch::virtio_negotiate_features(transport_, features)) {
        return false;
    }

    // Determine queue size
    queue_size_ = 16; // minimal for block I/O

    // Allocate physically contiguous pages for queue memory
    // Descriptor ring: queue_size * 16 bytes (16 per desc)
    // Available ring: 6 + queue_size * 2 bytes
    // Used ring: 8 + queue_size * 8 bytes
    // Total: roughly < 4KB for small queues, use one page each
    desc_phys_ = PMM::alloc_page();
    avail_phys_ = PMM::alloc_page();
    used_phys_ = PMM::alloc_page();
    dma_buf_phys_ = PMM::alloc_page();

    if (!desc_phys_ || !avail_phys_ || !used_phys_ || !dma_buf_phys_) {
        Logger::error("virtio-blk: OOM for queue memory");
        return false;
    }

    // NOLINTNEXTLINE(performance-no-int-to-ptr)
    desc_ = reinterpret_cast<arch::VirtqDesc *>(arch::HHDM_OFFSET + desc_phys_);
    // NOLINTNEXTLINE(performance-no-int-to-ptr)
    avail_ =
        reinterpret_cast<arch::VirtqAvail *>(arch::HHDM_OFFSET + avail_phys_);
    // NOLINTNEXTLINE(performance-no-int-to-ptr)
    used_ = reinterpret_cast<arch::VirtqUsed *>(arch::HHDM_OFFSET + used_phys_);
    // NOLINTNEXTLINE(performance-no-int-to-ptr)
    dma_buf_ = reinterpret_cast<uint8_t *>(arch::HHDM_OFFSET + dma_buf_phys_);

    memset(desc_, 0, PAGE_SIZE);
    memset(avail_, 0, PAGE_SIZE);
    memset(used_, 0, PAGE_SIZE);
    memset(dma_buf_, 0, PAGE_SIZE);

    if (!arch::virtio_setup_queue(transport_, 0, queue_size_, desc_phys_,
                                  avail_phys_, used_phys_)) {
        Logger::error("virtio-blk: queue setup failed");
        return false;
    }

    // Set DRIVER_OK
    uint8_t status = arch::virtio_read_status(transport_);
    arch::virtio_write_status(transport_, status | VIRTIO_STATUS_DRIVER_OK);

    // Read sector count from device config (at offset 0 for virtio-blk)
    // NOLINTNEXTLINE(performance-no-int-to-ptr)
    auto *cfg =
        reinterpret_cast<volatile uint64_t *>(transport_.device_cfg.virt_addr);
    sector_count_ = cfg[0];
    Logger::info("virtio-blk: %d sectors (%d MB)", sector_count_,
                 static_cast<uint64_t>(sector_count_) * BLOCK_SIZE /
                     (static_cast<uint64_t>(1024) * 1024));

    // #65: arm the completion ISR after the queue is live.  Fail-closed:
    // without MSI-X/MSI the bounded polling wait is preserved and no
    // queue-vector/ISR state is armed without a handler.
    last_seen_used_ = used_->idx;
    if (!enable_irq()) {
        Logger::info("virtio-blk: completion ISR unavailable — polling wait");
    }

    return true;
}

// ──────────────────────────────────────────────
//  Completion wait (#65)
// ──────────────────────────────────────────────

bool VirtioBlkDriver::match_completion(uint16_t used_before,
                                       uint16_t used_now, uint32_t used_id,
                                       uint16_t head_idx, bool *out_done) {
    if (!out_done)
        return false;
    // Single flight: an advance past the pre-notify snapshot is ours iff
    // the newest entry echoes our head descriptor.  An id mismatch means
    // a stale entry (late completion of a timed-out request reusing this
    // head) or a faulty device — never report success on it.
    *out_done = (used_now != used_before) && (used_id == head_idx);
    return true;
}

bool VirtioBlkDriver::wait_request_poll(uint16_t used_snapshot) {
    // M-2 interim bound: the former unbounded poll is capped at 1M
    // iterations with pause() between checks.  Preserved verbatim as the
    // fail-closed fallback (no ISR, no task context, wheel-arm failure).
    int timeout = 1000000;
    while (used_->idx == used_snapshot && --timeout > 0) {
        kernel::atomic_fence();
        arch::pause();
    }
    if (timeout <= 0) {
        Logger::error("virtio-blk: request timeout");
        return false;
    }
    return true;
}

bool VirtioBlkDriver::finish_request(bool is_read, uint8_t *data) {
    // Check status
    auto *status_ptr = reinterpret_cast<volatile uint8_t *>(
        dma_buf_ + sizeof(VirtioBlkReqHdr) + BLOCK_SIZE);
    if (*status_ptr != VIRTIO_BLK_S_OK) {
        Logger::error("virtio-blk: request failed (status=%d)", *status_ptr);
        return false;
    }

    // Copy data back for reads
    if (is_read) {
        memcpy(data, dma_buf_ + sizeof(VirtioBlkReqHdr), BLOCK_SIZE);
    }
    return true;
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
bool VirtioBlkDriver::wait_request(uint16_t used_snapshot,
                                   uint16_t head_idx) {
    // #65: scheduler-blocked bounded wait (closes FLAW-06).  Falls back
    // to the bounded poll when no IRQ is armed, no task context exists,
    // or the wheel arm fails (a BLOCKED task with no waker would park).
    if (irq_vector_ == 0)
        return wait_request_poll(used_snapshot);
    TaskControlBlock *cur = Scheduler::current_task();
    if (!cur)
        return wait_request_poll(used_snapshot);
    if (CONFIG_TICK_HZ == 0)
        return wait_request_poll(used_snapshot);

    // Same order as the retired 1M spin (~single-digit ms): fail fast.
    const uint64_t timeout_ticks = VIRTIO_BLK_WAIT_TIMEOUT_US / 1000 + 1;
    bool armed = IPC::recv_wait_arm(*cur, timeout_ticks);
    if (!armed)
        return wait_request_poll(used_snapshot);

    // Single registration (sys_irq_wait pattern): fast-path completion
    // check + waiter arm + BLOCKED under one lock scope, then dequeue +
    // reschedule outside the lock.  Never re-register in a loop.
    {
        sync::IrqSpinLockGuard guard(compl_lock_);
        uint16_t used_now = used_->idx;
        bool done = false;
        if (used_now != used_snapshot) {
            uint16_t newest =
                used_->ring[(used_now - 1) % queue_size_].id;
            match_completion(used_snapshot, used_now, newest, head_idx,
                             &done);
        }
        if (done) {
            IPC::recv_wait_cancel(*cur);
            return true;
        }
        compl_.done = false;
        compl_.status = 0xFF;
        compl_.waiter = cur;
        compl_.waiter_gen = cur->generation;
        compl_.arm_used = used_snapshot;
        compl_.head_idx = head_idx;
        cur->state = TaskState::BLOCKED;
    }

    Scheduler::dequeue_ready(*cur);
    Scheduler::reschedule();

    // reschedule() is deferred: spin on BLOCKED until the ISR completion
    // wake or the scheduler timeout-apply walk restores READY.  With
    // interrupts off no wake can arrive: roll back and poll.
    if (arch::interrupts_enabled()) {
        while (cur->state == TaskState::BLOCKED) {
            arch::pause();
        }
    } else {
        sync::IrqSpinLockGuard guard(compl_lock_);
        if (compl_.waiter == cur) {
            compl_.waiter = nullptr;
            compl_.waiter_gen = 0;
        }
        cur->state = TaskState::RUNNING;
        Scheduler::enqueue_ready(*cur);
        IPC::recv_wait_cancel(*cur);
        return wait_request_poll(used_snapshot);
    }

    // Woken: consume the completion record (delivery beats timeout).
    // The status snapshot is consumed here too: the teardown drain wakes
    // with done=true but status=0xFF (error sentinel, §12.3), which must
    // retire as failure — the live status byte may still hold a stale OK.
    bool done = false;
    uint8_t status = 0xFF;
    {
        sync::IrqSpinLockGuard guard(compl_lock_);
        done = compl_.done;
        status = compl_.status;
        if (compl_.waiter == cur) {
            compl_.waiter = nullptr;
            compl_.waiter_gen = 0;
        }
    }
    IPC::recv_wait_cancel(*cur);
    if (!done) {
        // Timeout: retire exactly like the poll path (log + false).
        Logger::error("virtio-blk: request timeout");
        return false;
    }
    if (status != VIRTIO_BLK_S_OK) {
        // Teardown error wake (or a device-reported failure snapshotted
        // by the ISR): same failure semantics as finish_request.
        Logger::error("virtio-blk: request failed (status=%d)", status);
        return false;
    }
    return true;
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
bool VirtioBlkDriver::submit_request(uint32_t type, uint64_t sector,
                                     uint8_t *data, bool is_read) {
    if (!desc_ || !avail_ || !used_)
        return false;

    // H-3 (audit-drivers-vfs-net-v0.4.2): hold the device mutex across the
    // whole submit chain — the driver shares a single DMA buffer and the
    // avail/used rings; concurrent submissions would corrupt the descriptor
    // chain or lose completion checks.
    submit_lock_.lock();

    // Build the request header in the DMA buffer
    auto *hdr = reinterpret_cast<VirtioBlkReqHdr *>(dma_buf_);
    hdr->type = type;
    hdr->reserved = 0;
    hdr->sector = sector;

    // Data goes after header (16 bytes) in the DMA buffer
    uint8_t *data_area = dma_buf_ + sizeof(VirtioBlkReqHdr);
    if (!is_read) {
        memcpy(data_area, data, BLOCK_SIZE);
    }

    // Build descriptor chain:
    // desc[0]: request header (driver write → device read)
    // desc[1]: data (driver read/write → device write/read)
    // desc[2]: status byte (device write → driver read)

    uint16_t idx = avail_idx_ % queue_size_;
    desc_[idx].addr = dma_buf_phys_;
    desc_[idx].len = sizeof(VirtioBlkReqHdr);
    desc_[idx].flags = VIRTIO_DESC_F_NEXT;
    desc_[idx].next = static_cast<uint16_t>((idx + 1) % queue_size_);

    uint16_t data_idx = static_cast<uint16_t>((idx + 1) % queue_size_);
    desc_[data_idx].addr = dma_buf_phys_ + sizeof(VirtioBlkReqHdr);
    desc_[data_idx].len = BLOCK_SIZE;
    desc_[data_idx].flags =
        VIRTIO_DESC_F_NEXT | (is_read ? VIRTIO_DESC_F_WRITE : 0);
    desc_[data_idx].next = static_cast<uint16_t>((idx + 2) % queue_size_);

    uint16_t status_idx = static_cast<uint16_t>((idx + 2) % queue_size_);
    desc_[status_idx].addr =
        dma_buf_phys_ + sizeof(VirtioBlkReqHdr) + BLOCK_SIZE;
    desc_[status_idx].len = 1;
    desc_[status_idx].flags = VIRTIO_DESC_F_WRITE;
    desc_[status_idx].next = 0;

    // H-3 (FLAW-03 pattern): snapshot the used index BEFORE the avail push and
    // virtio_notify — reading it after notify races fast completions (the
    // device may have already advanced used_->idx by the time we sample it).
    uint16_t used_idx = used_->idx;

    // Place in available ring
    avail_->ring[avail_->idx % queue_size_] = idx;
    kernel::atomic_fence();
    avail_->idx = static_cast<uint16_t>(avail_->idx + 1);
    kernel::atomic_fence();

    // Kick the device
    arch::virtio_notify(transport_, 0);

    // #65: scheduler-blocked bounded wait (poll fallback preserved).
    // The head descriptor id identifies our completion in the used ring.
    if (!wait_request(used_idx, idx)) {
        submit_lock_.unlock();
        return false;
    }

    if (!finish_request(is_read, data)) {
        submit_lock_.unlock();
        return false;
    }

    avail_idx_ = static_cast<uint16_t>(avail_idx_ + 1);
    submit_lock_.unlock();
    return true;
}

// ──────────────────────────────────────────────
//  Completion ISR (#65)
// ──────────────────────────────────────────────

/// @brief Read-and-ack the device ISR status byte (virtio 1.0: a read
/// returns queue/config interrupt bits and clears them).  Null-guarded:
/// an unmapped ISR window means no interrupt.
uint8_t VirtioBlkDriver::read_isr_status() const {
    if (transport_.isr_cfg.virt_addr == 0)
        return 0;
    // NOLINTNEXTLINE(performance-no-int-to-ptr)
    auto *reg =
        reinterpret_cast<volatile uint8_t *>(transport_.isr_cfg.virt_addr);
    return *reg;
}

void VirtioBlkDriver::isr_entry(uint64_t vector, uint64_t, uint64_t) {
    VirtioBlkDriver *target = g_virtio_blk_isr_target;
    if (!target || vector != target->irq_vector_)
        return;
    target->handle_irq();
}

bool VirtioBlkDriver::handle_irq() {
    // Collect-under-lock: drain new used-ring entries, match the armed
    // head id, capture the waiter into stack locals.
    TaskControlBlock *wake_task = nullptr;
    uint32_t wake_gen = 0;

    uint8_t isr = read_isr_status();
    if (isr == 0)
        return false;

    {
        sync::IrqSpinLockGuard guard(compl_lock_);
        kernel::atomic_fence();
        uint16_t used_now = used_->idx;
        // Bounded drain (≤ queue_size_): a coalesced/stray entry can never
        // stick the ring, and last_seen_used_ always advances.
        for (uint16_t pos = last_seen_used_;
             pos != used_now &&
             static_cast<uint16_t>(pos - last_seen_used_) < queue_size_;
             pos = static_cast<uint16_t>(pos + 1)) {
            uint16_t slot = static_cast<uint16_t>(pos % queue_size_);
            uint32_t entry_id = used_->ring[slot].id;
            last_seen_used_ = static_cast<uint16_t>(pos + 1);
            // Every newly-drained entry is an ISR-observed completion,
            // waiter or not (introspection counts what the ISR consumed).
            ++isr_completions_;
            if (!compl_.waiter || compl_.done)
                continue;
            if (pos < compl_.arm_used)
                continue;
            bool done = false;
            match_completion(pos, static_cast<uint16_t>(pos + 1), entry_id,
                             compl_.head_idx, &done);
            if (!done)
                continue;
            compl_.done = true;
            auto *status_ptr = reinterpret_cast<volatile uint8_t *>(
                dma_buf_ + sizeof(VirtioBlkReqHdr) + BLOCK_SIZE);
            compl_.status = *status_ptr;
            wake_task = compl_.waiter;
            wake_gen = compl_.waiter_gen;
            compl_.waiter = nullptr;
            compl_.waiter_gen = 0;
        }
    }

    // Wake-outside-lock (Notify discipline): liveness + BLOCKED +
    // generation, reject TERMINATED/REAPED.  No reschedule() from ISR.
    if (wake_task) {
        if (TaskControlBlock::is_valid(wake_task) &&
            wake_task->generation == wake_gen &&
            wake_task->state == TaskState::BLOCKED) {
            Scheduler::set_task_ready(*wake_task);
        }
    }
    return true;
}

bool VirtioBlkDriver::enable_irq() {
#if defined(CONFIG_ARCH_X86_64)
    if (irq_vector_ != 0)
        return true;
    // MSI-X first (per-queue entry), MSI fallback, else fail-closed poll.
    // The #64 writew fix covers both enable paths — no pci.cpp change.
    uint8_t vec = arch::pci_enable_msix(transport_.bdf, 0, 0);
    if (vec == 0)
        vec = arch::pci_enable_msi(transport_.bdf, 0);
    if (vec == 0) {
        Logger::info("virtio-blk: no MSI-X/MSI — staying on polling wait");
        return false;
    }
    // Registration order: IDT handler first, then the queue vector, so a
    // queue interrupt is never vectored nowhere (the #64 root-cause
    // class).  QUEUE_MSIX_VECTOR takes the MSI-X table ENTRY (0), not the
    // CPU vector; the MSI fallback needs no queue programming (single
    // message).  Re-select queue 0 first: QUEUE_SEL is shared state.
    g_virtio_blk_isr_target = this;
    arch::IDT::register_handler_raw(vec, &VirtioBlkDriver::isr_entry);
    arch::virtio_write_common16(transport_, VIRTIO_COMMON_QUEUE_SEL, 0);
    arch::virtio_write_common16(transport_, VIRTIO_COMMON_QUEUE_MSIX_VECTOR,
                                0);
    (void)read_isr_status(); // ack stale status
    irq_vector_ = vec;
    irq_allocated_ = true;
    Logger::info("virtio-blk: completion ISR armed on vector %u", vec);
    return true;
#else
    return false;
#endif
}

void VirtioBlkDriver::disable_irq() {
#if defined(CONFIG_ARCH_X86_64)
    if (irq_vector_ == 0)
        return;
    // Teardown order: mask the queue vector first (no new ISR entries),
    // then drain an armed waiter with an error wake under the lock, then
    // release PCI/IDT ownership (real allocations only — a
    // test-simulated arming owns nothing).
    arch::virtio_write_common16(transport_, VIRTIO_COMMON_QUEUE_SEL, 0);
    arch::virtio_write_common16(transport_, VIRTIO_COMMON_QUEUE_MSIX_VECTOR,
                                VIRTIO_NO_VECTOR);
    (void)read_isr_status();
    TaskControlBlock *drained = nullptr;
    {
        sync::IrqSpinLockGuard guard(compl_lock_);
        if (compl_.waiter) {
            drained = compl_.waiter;
            compl_.waiter = nullptr;
            compl_.waiter_gen = 0;
            compl_.done = true;
            compl_.status = 0xFF; // sentinel → error return, never stranding
        }
    }
    // Wakers own the wakeup contract (§12.3).
    if (drained && TaskControlBlock::is_valid(drained) &&
        drained->state == TaskState::BLOCKED) {
        Scheduler::set_task_ready(*drained);
    }
    if (irq_allocated_) {
        g_virtio_blk_isr_target = nullptr;
        arch::IDT::register_handler_raw(irq_vector_, nullptr);
        arch::pci_free_vector(irq_vector_);
    }
    irq_vector_ = 0;
    irq_allocated_ = false;
#endif
}

bool VirtioBlkDriver::read_sector(uint64_t lba, uint8_t *buffer) {
    return submit_request(VIRTIO_BLK_T_IN, lba, buffer, true);
}

bool VirtioBlkDriver::write_sector(uint64_t lba, const uint8_t *buffer) {
    // submit_request reads from the buffer for write requests; stage it into a
    // non-const local so we keep write_sector's const-correct interface.
    uint8_t staging[BLOCK_SIZE];
    __builtin_memcpy(staging, buffer, BLOCK_SIZE);
    return submit_request(VIRTIO_BLK_T_OUT, lba, staging, false);
}

VirtioBlkDriver *VirtioBlkDriver::probe() {
    arch::VirtioTransport transport{};
    if (!arch::virtio_find_device(VIRTIO_DEVICE_BLOCK, transport)) {
        Logger::info("virtio-blk: no device found");
        return nullptr;
    }

#ifndef __clang__
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wanalyzer-possible-null-dereference"
#endif
    auto *drv_mem = kernel::MemPool::alloc(sizeof(VirtioBlkDriver));
    if (!drv_mem) return nullptr;
    auto *drv = new (drv_mem) VirtioBlkDriver(transport);
#ifndef __clang__
#pragma GCC diagnostic pop
#endif
    if (!drv->init()) {
        drv->~VirtioBlkDriver(); kernel::MemPool::free(drv);
        return nullptr;
    }
    return drv;
}

} // namespace kernel::block
