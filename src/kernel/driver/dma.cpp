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

/// @file dma.cpp
/// @brief DMA driver implementation.

#include <kernel/driver/dma.hpp>
#include <kernel/memory/pmm.hpp>
#include <kernel/memory/vmm.hpp>
#include <kernel/arch/io.hpp>
#include <string.hpp>
#include <logger.hpp>

using namespace kernel;
using namespace arch;

namespace kernel::dma {

// --- DMA Buffer ---

DmaBuffer alloc_buffer(size_t size) {
    DmaBuffer buf = {};
    size_t page_count = (size + PAGE_SIZE - 1) / PAGE_SIZE;
    uint64_t phys = PMM::alloc_contiguous(page_count);
    if (!phys) {
        Logger::error("dma: alloc_buffer(%zu) failed", size);
        return buf;
    }
    buf.phys_addr = phys;
    buf.virt_addr = HHDM_OFFSET + phys;
    buf.size = page_count * PAGE_SIZE;
    buf.owned = true;

    for (size_t i = 0; i < page_count; ++i) {
        VMM::map_page(phys + i * PAGE_SIZE, phys + i * PAGE_SIZE, false);
    }

    // NOLINTNEXTLINE(performance-no-int-to-ptr)
    memset(reinterpret_cast<void *>(buf.virt_addr), 0, buf.size);
    return buf;
}

void free_buffer(DmaBuffer &buf) {
    if (!buf.owned || !buf.phys_addr)
        return;
    size_t page_count = buf.size / PAGE_SIZE;
    for (size_t i = 0; i < page_count; ++i) {
        VMM::unmap_page(buf.phys_addr + i * PAGE_SIZE);
        PMM::free_page(buf.phys_addr + i * PAGE_SIZE);
    }
    buf.phys_addr = 0;
    buf.virt_addr = 0;
    buf.size = 0;
    buf.owned = false;
}

// --- Scatter-Gather ---

bool sg_from_buffer(SgList &sg, const DmaBuffer &buf, size_t offset,
                    size_t length) {
    if (offset >= buf.size)
        return false;
    if (length == 0 || offset + length > buf.size) {
        length = buf.size - offset;
    }
    sg.count = 1;
    sg.total_length = length;
    sg.entries[0].phys_addr = buf.phys_addr + offset;
    sg.entries[0].virt_addr = buf.virt_addr + offset;
    sg.entries[0].length = length;
    return true;
}

bool sg_from_virt(SgList &sg, uint64_t virt_addr, size_t length) {
    sg.count = 0;
    sg.total_length = 0;
    uint64_t current = virt_addr;
    uint64_t end = virt_addr + length;
    while (current < end && sg.count < DMA_MAX_SG_ENTRIES) {
        uint64_t page_start = current & ~0xFFFULL;
        uint64_t page_end = page_start + PAGE_SIZE;
        uint64_t chunk_end = end < page_end ? end : page_end;

        uint64_t phys = VMM::virt_to_phys(page_start);
        if (!phys) {
            Logger::error("dma: sg_from_virt: unmapped virt %x", page_start);
            return false;
        }
        size_t offset_in_page = static_cast<size_t>(current - page_start);
        auto &e = sg.entries[sg.count];
        e.phys_addr = phys + offset_in_page;
        e.virt_addr = current;
        e.length = chunk_end - current;
        sg.total_length += e.length;
        ++sg.count;
        current = chunk_end;
    }
    return sg.count > 0;
}

void sg_reset(SgList &sg) {
    sg.count = 0;
    sg.total_length = 0;
}

// --- PRD Table ---

size_t prd_from_sg(PrdTable &prd, const SgList &sg, bool eot) {
    prd.count = 0;
    for (size_t i = 0; i < sg.count && prd.count < DMA_MAX_PRD_ENTRIES; ++i) {
        auto &entry = prd.entries[prd.count];
        entry.phys_addr =
            static_cast<uint32_t>(sg.entries[i].phys_addr & 0xFFFFFFFF);
        entry.byte_count =
            static_cast<uint16_t>((sg.entries[i].length - 1) & 0xFFFF);
        entry.flags = 0;
        ++prd.count;
    }
    if (prd.count > 0 && eot) {
        prd.entries[prd.count - 1].flags |= 0x8000; // EOT bit
    }
    return prd.count;
}

void prd_reset(PrdTable &prd) {
    prd.count = 0;
}

// --- PCI Bus Master ---

void pci_set_bus_master(arch::PciBdf bdf, bool enable) {
    uint32_t addr = arch::pci_make_addr(bdf, arch::PCI_COMMAND);
    uint16_t cmd = arch::pci_config_readw(addr);
    if (enable) {
        cmd |= arch::PCI_CMD_BUS_MASTER;
    } else {
        cmd &= static_cast<uint16_t>(~arch::PCI_CMD_BUS_MASTER);
    }
    arch::pci_config_writel(addr, cmd);
    Logger::info("dma: bus master %s on %d:%d.%d",
                 enable ? "enabled" : "disabled", bdf.bus, bdf.device,
                 bdf.function);
}

// --- BMDMA Channel ---

BmDmaChannel::BmDmaChannel(uint16_t bm_io_base) : bm_io_base_(bm_io_base) {
}

bool BmDmaChannel::init() {
    if (!bm_io_base_)
        return false;
    arch::outb(static_cast<uint16_t>(bm_io_base_ + 0), BMCMD_STOP);
    Logger::info("dma: BmDmaChannel init at I/O base 0x%x", bm_io_base_);
    return true;
}

bool BmDmaChannel::start(PrdTable &prd, Direction dir) {
    if (!bm_io_base_ || prd.count == 0)
        return false;

    uint64_t prd_phys = VMM::virt_to_phys(reinterpret_cast<uint64_t>(&prd));
    if (!prd_phys)
        return false;

    arch::outl(static_cast<uint16_t>(bm_io_base_ + 4),
               static_cast<uint32_t>(prd_phys & 0xFFFFFFFF));

    uint8_t cmd = BMCMD_START;
    if (dir == Direction::READ) {
        cmd |= BMCMD_READ;
    }
    arch::outb(static_cast<uint16_t>(bm_io_base_ + 0), cmd);

    Logger::info("dma: BmDmaChannel start prd_phys=0x%x dir=%s",
                 static_cast<unsigned>(prd_phys),
                 dir == Direction::READ ? "READ" : "WRITE");
    return true;
}

bool BmDmaChannel::is_busy() {
    if (!bm_io_base_)
        return false;
    uint8_t status = arch::inb(static_cast<uint16_t>(bm_io_base_ + 2));
    return (status & BMSTAT_ACTIVE) != 0;
}

bool BmDmaChannel::handle_irq() {
    if (!bm_io_base_)
        return false;
    uint8_t status = arch::inb(static_cast<uint16_t>(bm_io_base_ + 2));
    if (!(status & BMSTAT_INTR))
        return false;
    arch::outb(static_cast<uint16_t>(bm_io_base_ + 2),
               static_cast<uint8_t>(status & ~BMSTAT_INTR));
    arch::outb(static_cast<uint16_t>(bm_io_base_ + 2), status);
    return (status & BMSTAT_ERROR) == 0;
}

void BmDmaChannel::abort() {
    if (bm_io_base_) {
        arch::outb(static_cast<uint16_t>(bm_io_base_ + 0), BMCMD_STOP);
    }
}

// --- DMA Engine ---

DmaEngine::DmaEngine(DmaChannel &channel)
    : channel_(channel), active_(false), callback_(nullptr), callback_ctx_(0) {
}

bool DmaEngine::start_transfer(PrdTable &prd, Direction dir, DmaCallback cb,
                               uint64_t ctx) {
    sync::IrqSpinLockGuard guard(lock_);
    if (active_)
        return false;
    if (!channel_.start(prd, dir))
        return false;
    // cli (held by the guard) guarantees the completion IRQ cannot fire
    // between channel_.start() and active_ = true.
    active_ = true;
    callback_ = cb;
    callback_ctx_ = ctx;
    return true;
}

bool DmaEngine::is_busy() {
    sync::IrqSpinLockGuard guard(lock_);
    if (!active_)
        return false;
    if (!channel_.is_busy()) {
        active_ = false;
        return false;
    }
    return true;
}

bool DmaEngine::handle_irq() {
    DmaCallback cb;
    uint64_t ctx;
    bool success;
    {
        sync::IrqSpinLockGuard guard(lock_);
        if (!active_)
            return false;
        success = channel_.handle_irq();
        // Capture the callback into stack locals, clear the engine state,
        // then release the lock before invoking the callback (the callback
        // runs in IRQ context and must never fire under a spinlock).
        cb = callback_;
        ctx = callback_ctx_;
        active_ = false;
        callback_ = nullptr;
        callback_ctx_ = 0;
    }
    if (cb) {
        cb(ctx, success);
    }
    return true;
}

void DmaEngine::abort() {
    sync::IrqSpinLockGuard guard(lock_);
    channel_.abort(); // stop HW first -> no completion IRQ can fire
    active_ = false;
    callback_ = nullptr;
    callback_ctx_ = 0;
}

// --- Ping-Pong Double-Buffer ---

PingPongDma::PingPongDma(DmaEngine &engine, size_t buf_size)
    : engine_(engine), buf_size_(buf_size), prepare_idx_(0), xfer_idx_(1),
      active_(false), completed_(0), chain_cb_(nullptr), chain_ctx_(0) {
    bufs_[0] = {};
    bufs_[1] = {};
}

bool PingPongDma::init() {
    bufs_[0] = alloc_buffer(buf_size_);
    bufs_[1] = alloc_buffer(buf_size_);
    if (!bufs_[0].phys_addr || !bufs_[1].phys_addr) {
        shutdown();
        return false;
    }
    {
        sync::IrqSpinLockGuard guard(lock_);
        prepare_idx_ = 0;
        xfer_idx_ = 1;
        active_ = false;
        completed_ = 0;
        chain_cb_ = nullptr;
        chain_ctx_ = 0;
    }
    Logger::info("dma: PingPongDma init bufs at 0x%lx/0x%lx",
                 bufs_[0].phys_addr, bufs_[1].phys_addr);
    return true;
}

DmaBuffer *PingPongDma::prepare_buf() {
    sync::IrqSpinLockGuard guard(lock_);
    return active_ ? &bufs_[prepare_idx_] : &bufs_[0];
}

DmaBuffer *PingPongDma::xfer_buf() {
    sync::IrqSpinLockGuard guard(lock_);
    return active_ ? &bufs_[xfer_idx_] : &bufs_[1];
}

static void pingpong_completion_cb(uint64_t ctx, bool success) {
    // NOLINTNEXTLINE(performance-no-int-to-ptr)
    auto *pp = reinterpret_cast<PingPongDma *>(ctx);
    pp->on_completion(0, success);
}

bool PingPongDma::start_next(Direction dir, ChainCallback cb, uint64_t ctx) {
    sync::IrqSpinLockGuard guard(lock_);
    if (active_)
        return false;

    // Resolve the transfer buffer directly (do NOT call xfer_buf(): the
    // SpinLock is non-recursive and xfer_buf() would self-deadlock).
    DmaBuffer *xfer = active_ ? &bufs_[xfer_idx_] : &bufs_[1];

    // Build SG/PRD for the transfer buffer
    SgList sg{};
    sg_reset(sg);
    if (!sg_from_buffer(sg, *xfer))
        return false;

    PrdTable prd{};
    prd_from_sg(prd, sg, true);

    // Swap indices: what was being prepared becomes the transfer buffer
    uint8_t tmp = prepare_idx_;
    prepare_idx_ = xfer_idx_;
    xfer_idx_ = tmp;

    chain_cb_ = cb;
    chain_ctx_ = ctx;

    // Nested engine lock (fixed order: PingPongDma::lock_ -> DmaEngine::
    // lock_).  The engine's handle_irq releases the engine lock before
    // invoking on_completion, so the reverse order never occurs.
    if (!engine_.start_transfer(prd, dir, pingpong_completion_cb,
                                reinterpret_cast<uint64_t>(this))) {
        // Swap back on failure
        tmp = prepare_idx_;
        prepare_idx_ = xfer_idx_;
        xfer_idx_ = tmp;
        return false;
    }

    active_ = true;
    Logger::info("dma: PingPongDma start_next prepare=%u xfer=%u", prepare_idx_,
                 xfer_idx_);
    return true;
}

void PingPongDma::on_completion(uint64_t, bool success) {
    ChainCallback cb;
    uint64_t ctx;
    DmaBuffer *buf;
    {
        sync::IrqSpinLockGuard guard(lock_);
        active_ = false;
        ++completed_;
        cb = chain_cb_;
        ctx = chain_ctx_;
        buf = &bufs_[xfer_idx_];
    }
    // Invoke the chain callback outside the lock (it may re-enter the
    // driver / touch driver state).
    if (cb) {
        cb(ctx, buf, success);
    }
    Logger::info("dma: PingPongDma complete #%lu success=%d", completed_,
                 success);
}

void PingPongDma::shutdown() {
    {
        sync::IrqSpinLockGuard guard(lock_);
        engine_.abort(); // stop HW -> no late on_completion
        active_ = false;
        completed_ = 0;
        chain_cb_ = nullptr; // close the late-completion race
        chain_ctx_ = 0;
    }
    free_buffer(bufs_[0]);
    free_buffer(bufs_[1]);
    bufs_[0] = {};
    bufs_[1] = {};
}

} // namespace kernel::dma