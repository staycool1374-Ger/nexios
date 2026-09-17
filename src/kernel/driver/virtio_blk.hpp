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

/// @file virtio_blk.hpp
/// @brief Virtio block driver — modern (Virtio 1.0) PCI transport.

#pragma once

#include <types.hpp>
#include <kernel/arch/virtio.hpp>
#include <kernel/driver/block_device.hpp>
#include <kernel/sync/mutex.hpp>
#include <kernel/sync/spinlock.hpp>

namespace kernel {
struct TaskControlBlock;
} // namespace kernel

namespace kernel::block {

/// Virtio block I/O request types
enum VirtioBlkReqType : uint8_t {
    VIRTIO_BLK_T_IN = 0,
    VIRTIO_BLK_T_OUT = 1,
    VIRTIO_BLK_T_FLUSH = 4,
};

/// Virtio block request header (must be 16-byte aligned)
struct VirtioBlkReqHdr {
    uint32_t type;
    uint32_t reserved;
    uint64_t sector;
} __attribute__((packed));

/// Virtio block response status
enum VirtioBlkStatus : uint8_t {
    VIRTIO_BLK_S_OK = 0,
    VIRTIO_BLK_S_IOERR = 1,
    VIRTIO_BLK_S_UNSUP = 2,
};

/// Blocked-wait bound for one request (#65).  Generous vs the retired 1M
/// spin (~single-digit ms): the submitter/pump handoff needs tick
/// dispatch, whose EDF jitter under the ad-hoc test-task overload can
/// exceed 10ms.  Still fail-fast next to AHCI's 5s bound.
constexpr uint64_t VIRTIO_BLK_WAIT_TIMEOUT_US = 100000;

/// Queue-vector "no vector" sentinel (VIRTIO 1.0 §4.1.4.8).
constexpr uint16_t VIRTIO_NO_VECTOR = 0xFFFF;

/// Per-request completion record (#65).  Single instance — submit_lock_
/// allows at most one in-flight request.  Statically embedded: the IRQ
/// path never allocates (drivers.md §7.1 binding invariant).
struct VirtioBlkComplRecord {
    bool done = false;                  ///< ISR observed the completion
    uint8_t status = 0xFF;              ///< status-byte snapshot (0xFF = none)
    TaskControlBlock *waiter = nullptr; ///< blocked wait_request task
    uint32_t waiter_gen = 0;            ///< TCB generation at arm time
    uint16_t arm_used = 0;              ///< used-ring index at arm time
    uint16_t head_idx = 0;              ///< expected head descriptor id
};

/// Virtio block device driver (modern interface)
class VirtioBlkDriver final : public BlockDevice {
  public:
    VirtioBlkDriver(arch::VirtioTransport &transport);
    ~VirtioBlkDriver();

    bool init();
    bool read_sector(uint64_t lba, uint8_t *buffer) override;
    bool write_sector(uint64_t lba, const uint8_t *buffer) override;
    uint64_t sector_count() const override {
        return sector_count_;
    }
    uint64_t sector_size() const override {
        return BLOCK_SIZE;
    }
    bool is_read_only() const override {
        return false;
    }

    static VirtioBlkDriver *probe();

    /// ISR entry — drains the used ring, records the completion and wakes
    /// the armed waiter.  Interrupt context: no blocking, no allocation,
    /// completion lock only, wake applied outside the lock.
    /// @return true when a completion was consumed.
    bool handle_irq();

    /// Whether an IRQ vector is armed (test introspection).
    bool msi_armed() const {
        return irq_vector_ != 0;
    }

    /// Number of ISR-recorded completions (test introspection).
    uint64_t isr_completions() const {
        return isr_completions_;
    }

    /// Whether a waiter is currently armed (test introspection).
    bool has_armed_waiter() const {
        return compl_.waiter != nullptr;
    }

    /// Test-only: simulate an armed IRQ vector without PCI MSI/MSI-X (the
    /// mock transport has no MSI capability, so enable_irq() fail-closes).
    /// Delivery is simulated by calling handle_irq() directly.  Never
    /// touches PCI/IDT state (allocated_ stays false).  Not for production.
    void test_simulate_irq_armed() {
        irq_vector_ = 0xFE;
    }

    /// Pure completion matcher (#65, unit-testable without hardware): the
    /// single flight completes when the used index advanced past the
    /// pre-notify snapshot AND the echoed head id matches.
    static bool match_completion(uint16_t used_before, uint16_t used_now,
                                 uint32_t used_id, uint16_t head_idx,
                                 bool *out_done);

    // MSI-X/MSI wiring (#65, x86_64).  Returns false fail-closed (polling
    // preserved) when no vector is available.  Never arms queue-vector /
    // ISR state without a registered handler.  Public for teardown-drain
    // test coverage (disable path must be invocable independently).
    bool enable_irq();
    void disable_irq();

  private:
    bool submit_request(uint32_t type, uint64_t sector, uint8_t *data,
                        bool is_read);
    // Bounded polling completion wait (fallback when no IRQ is armed, no
    // task context exists, or the wheel arm fails).
    bool wait_request_poll(uint16_t used_snapshot);
    // Scheduler-blocked bounded completion wait (#65).
    bool wait_request(uint16_t used_snapshot, uint16_t head_idx);
    // Shared response tail: validates the status byte, copies data back
    // for reads.  Same error semantics on both wait paths.
    bool finish_request(bool is_read, uint8_t *data);

    static void isr_entry(uint64_t vector, uint64_t error_code, uint64_t rip);
    // ISR status byte read-and-ack (null-guarded for unmapped windows).
    uint8_t read_isr_status() const;

    arch::VirtioTransport transport_;
    uint64_t sector_count_ = 0;

    // Queue memory (physically contiguous)
    uint64_t desc_phys_ = 0;
    uint64_t avail_phys_ = 0;
    uint64_t used_phys_ = 0;

    // Virtual addresses for queue memory
    arch::VirtqDesc *desc_ = nullptr;
    arch::VirtqAvail *avail_ = nullptr;
    arch::VirtqUsed *used_ = nullptr;
    uint16_t queue_size_ = 0;
    uint16_t avail_idx_ = 0;

    // DMA buffer physical address
    uint64_t dma_buf_phys_ = 0;
    uint8_t *dma_buf_ = nullptr;

    // H-3 (audit-drivers-vfs-net-v0.4.2): serializes the whole
    // descriptor-fill → avail-push → notify → poll critical section.  The
    // driver shares one DMA buffer and the avail/used rings; concurrent
    // submit_request calls would corrupt the chain or lose completions.
    sync::Mutex submit_lock_{};

    // Completion state (#65): single embedded record (at most one waiter
    // — submit_lock_ serializes flights).  compl_lock_ is a leaf (ISR
    // takes only this lock; task paths take submit_lock_ → compl_lock_
    // if both are needed, never the reverse, never held across BLOCKED).
    VirtioBlkComplRecord compl_{};
    sync::SpinLock compl_lock_{};
    // Last consumed used-ring index (monotonic compare, wrap-safe via !=).
    uint16_t last_seen_used_ = 0;
    // Armed IRQ vector, 0 = none (polling).  irq_allocated_ tracks real
    // PCI/IDT ownership (test-simulated arming sets the vector only).
    uint8_t irq_vector_ = 0;
    bool irq_allocated_ = false;
    uint64_t isr_completions_ = 0;
};

} // namespace kernel::block
