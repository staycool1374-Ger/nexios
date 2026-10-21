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

/// @file virtio.hpp
/// @brief Virtio PCI transport — modern (1.0) interface via PCI capabilities.

#pragma once

#include <types.hpp>
#include <kernel/arch/pci.hpp>
#include <kernel/memory/vmm.hpp>

// NOLINTBEGIN(performance-no-int-to-ptr,bugprone-easily-swappable-parameters)
namespace arch {

/// Virtio PCI vendor ID
constexpr uint16_t VIRTIO_PCI_VENDOR = 0x1AF4;

/// Virtio PCI device IDs (modern = 0x1040 + type)
constexpr uint16_t VIRTIO_DEVICE_BLOCK = 0x1042; // modern virtio-blk
constexpr uint16_t VIRTIO_DEVICE_NET = 0x1041;   // modern virtio-net
constexpr uint16_t VIRTIO_DEVICE_GPU = 0x1050;   // modern virtio-gpu (issue #394)

/// Legacy device IDs
constexpr uint16_t VIRTIO_DEVICE_BLOCK_LEGACY = 0x1001;
constexpr uint16_t VIRTIO_DEVICE_NET_LEGACY = 0x1000;
constexpr uint16_t VIRTIO_DEVICE_GPU_LEGACY = 0x1010;

/// Virtio PCI capability types (cfg_type in virtio_pci_cap)
enum VirtioCfgType : uint8_t {
    VIRTIO_CFG_COMMON = 1,
    VIRTIO_CFG_NOTIFY = 2,
    VIRTIO_CFG_ISR = 3,
    VIRTIO_CFG_DEVICE = 4,
    VIRTIO_CFG_PCI_ACCESS = 5,
};

/// Device status register bits
enum VirtioDeviceStatus : uint8_t {
    VIRTIO_STATUS_RESET = 0x00,
    VIRTIO_STATUS_ACK = 1,
    VIRTIO_STATUS_DRIVER = 2,
    VIRTIO_STATUS_DRIVER_OK = 4,
    VIRTIO_STATUS_FEATURES_OK = 8,
    VIRTIO_STATUS_NEEDS_RESET = 64,
    VIRTIO_STATUS_FAILED = 128,
};

/// Virtio feature bits (common)
constexpr uint64_t VIRTIO_F_VERSION_1 = 1ULL << 32; // modern interface
constexpr uint64_t VIRTIO_F_RING_INDIRECT_DESC = 1ULL << 28;
constexpr uint64_t VIRTIO_F_RING_EVENT_IDX = 1ULL << 29;

/// Notify flags for kick
constexpr uint16_t VIRTIO_NOTIFY_NEXT = 0; // kick next descriptor

/// Max descriptor ring size
constexpr uint16_t VIRTIO_QUEUE_SIZE_MAX = 1024;

/// Virtio descriptor flags
// NOLINTNEXTLINE(performance-enum-size)
enum VirtioDescFlags : uint16_t {
    VIRTIO_DESC_F_NEXT = 1,
    VIRTIO_DESC_F_WRITE = 2,
    VIRTIO_DESC_F_INDIRECT = 4,
};

/// A single virtqueue descriptor (16 bytes)
struct VirtqDesc {
    uint64_t addr;
    uint32_t len;
    uint16_t flags;
    uint16_t next;
} __attribute__((packed));

/// Available ring header
struct VirtqAvail {
    uint16_t flags;
    uint16_t idx;
    uint16_t ring[];
} __attribute__((packed));

/// Used ring entry
struct VirtqUsedElem {
    uint32_t id;
    uint32_t len;
} __attribute__((packed));

/// Used ring header
struct VirtqUsed {
    uint16_t flags;
    uint16_t idx;
    VirtqUsedElem ring[];
} __attribute__((packed));

/// Mapped MMIO region for a Virtio capability
struct VirtioMmio {
    uint64_t virt_addr; // virtual address (HHDM-mapped)
    uint64_t phys_addr; // physical address (from BAR + offset)
    uint32_t length;
};

/// Common configuration register offsets (via VIRTIO_CFG_COMMON)
// NOLINTNEXTLINE(performance-enum-size)
enum VirtioCommonReg : uint16_t {
    VIRTIO_COMMON_DEVICE_FEATURE_SEL = 0x00,
    VIRTIO_COMMON_DEVICE_FEATURE = 0x04,
    VIRTIO_COMMON_DRIVER_FEATURE_SEL = 0x08,
    VIRTIO_COMMON_DRIVER_FEATURE = 0x0C,
    VIRTIO_COMMON_MSIX_CONFIG = 0x10,
    VIRTIO_COMMON_NUM_QUEUES = 0x12,
    VIRTIO_COMMON_DEVICE_STATUS = 0x14,
    VIRTIO_COMMON_CONFIG_GENERATION = 0x15,
    VIRTIO_COMMON_QUEUE_SEL = 0x16,
    VIRTIO_COMMON_QUEUE_SIZE = 0x18,
    VIRTIO_COMMON_QUEUE_MSIX_VECTOR = 0x1A,
    VIRTIO_COMMON_QUEUE_ENABLE = 0x1C,
    VIRTIO_COMMON_QUEUE_NOTIFY_OFF = 0x1E,
    VIRTIO_COMMON_QUEUE_DESC_LO = 0x20,
    VIRTIO_COMMON_QUEUE_DESC_HI = 0x24,
    VIRTIO_COMMON_QUEUE_DRIVER_LO = 0x28,
    VIRTIO_COMMON_QUEUE_DRIVER_HI = 0x2C,
    VIRTIO_COMMON_QUEUE_DEVICE_LO = 0x30,
    VIRTIO_COMMON_QUEUE_DEVICE_HI = 0x34,
};

/// Notify register offsets (via VIRTIO_CFG_NOTIFY)
constexpr uint32_t VIRTIO_NOTIFY_OFF_MUL = 0; // notify_off_multiplier

/// Virtio PCI capability structure (in PCI config space)
struct VirtioPciCap {
    uint8_t cap_vndr;
    uint8_t cap_next;
    uint8_t cap_len;
    uint8_t cfg_type;
    uint8_t bar;
    uint8_t padding[3];
    uint32_t offset;
    uint32_t length;
} __attribute__((packed));

/// Parsed Virtio device transport information
struct VirtioTransport {
    PciBdf bdf = {};
    uint16_t device_id = 0;
    bool modern = false;

    VirtioMmio common_cfg = {};
    VirtioMmio notify_cfg = {};
    VirtioMmio isr_cfg = {};
    VirtioMmio device_cfg = {};

    uint32_t notify_off_multiplier = 0;

    // Issue #350 (MMIO): appended — existing field offsets unchanged.
    bool mmio = false;            ///< true = MMIO v2 transport, not PCI
    uint64_t mmio_base_phys = 0;  ///< MMIO register base (phys)
    uint16_t mmio_irq = 0;        ///< GIC INTID / PLIC IRQ number
    uint16_t mmio_slot = 0;       ///< DTB discovery slot index
    uint16_t mmio_version = 0;    ///< MMIO interface version (1/2, 0 = unset)
};

/// Virtio MMIO (v2) register offsets — QEMU `virt` transport region.
enum VirtioMmioReg : uint16_t {
    VIRTIO_MMIO_MAGIC = 0x000,
    VIRTIO_MMIO_VERSION = 0x004,
    VIRTIO_MMIO_DEVICE_ID = 0x008,
    VIRTIO_MMIO_VENDOR_ID = 0x00C,
    VIRTIO_MMIO_DEVICE_FEATURES = 0x010,
    VIRTIO_MMIO_DEVICE_FEATURES_SEL = 0x014,
    VIRTIO_MMIO_DRIVER_FEATURES = 0x020,
    VIRTIO_MMIO_DRIVER_FEATURES_SEL = 0x024,
    VIRTIO_MMIO_GUEST_PAGE_SIZE = 0x028,
    VIRTIO_MMIO_QUEUE_SEL = 0x030,
    VIRTIO_MMIO_QUEUE_NUM_MAX = 0x034,
    VIRTIO_MMIO_QUEUE_NUM = 0x038,
    VIRTIO_MMIO_QUEUE_ALIGN = 0x03C,
    VIRTIO_MMIO_QUEUE_PFN = 0x040,
    VIRTIO_MMIO_QUEUE_READY = 0x044,
    VIRTIO_MMIO_QUEUE_NOTIFY = 0x050,
    VIRTIO_MMIO_INTERRUPT_STATUS = 0x060,
    VIRTIO_MMIO_INTERRUPT_ACK = 0x064,
    VIRTIO_MMIO_STATUS = 0x070,
    VIRTIO_MMIO_QUEUE_DESC_LOW = 0x080,
    VIRTIO_MMIO_QUEUE_DESC_HIGH = 0x084,
    VIRTIO_MMIO_QUEUE_DRIVER_LOW = 0x090,
    VIRTIO_MMIO_QUEUE_DRIVER_HIGH = 0x094,
    VIRTIO_MMIO_QUEUE_DEVICE_LOW = 0x0A0,
    VIRTIO_MMIO_QUEUE_DEVICE_HIGH = 0x0A4,
    VIRTIO_MMIO_CONFIG_GENERATION = 0x0FC,
    VIRTIO_MMIO_CONFIG = 0x100,
};

/// MMIO v2 identification constants.
constexpr uint32_t VIRTIO_MMIO_MAGIC_VALUE = 0x74726976; // "virt"
constexpr uint32_t VIRTIO_MMIO_VERSION_MODERN = 2;
constexpr uint32_t VIRTIO_MMIO_VERSION_LEGACY = 1;
constexpr uint32_t VIRTIO_MMIO_REGION_SIZE = 0x200;
constexpr uint32_t VIRTIO_MMIO_DEVICE_NET = 1;
constexpr uint32_t VIRTIO_MMIO_DEVICE_BLOCK = 2;
constexpr uint32_t VIRTIO_MMIO_DEVICE_GPU = 16; // issue #394
constexpr uint16_t VIRTIO_MMIO_REG_NONE = 0xFFFF;

/// Legacy (v1) queue chunk geometry (issue #350): one contiguous chunk
/// holding descriptors at +0, the avail ring immediately after, and the
/// used ring at the aligned offset below. The allocator (driver) and the
/// fit check (setup) both derive addresses from this single definition —
/// they cannot disagree.
constexpr uint64_t VIRTIO_LEGACY_ALIGN = 0x1000;
constexpr uint64_t VIRTIO_LEGACY_CHUNK_PAGES = 2;
constexpr uint64_t virtio_legacy_used_off(uint16_t queue_size) {
    uint64_t end = static_cast<uint64_t>(queue_size) * 16 + 6 +
                   static_cast<uint64_t>(queue_size) * 2;
    return (end + VIRTIO_LEGACY_ALIGN - 1) & ~(VIRTIO_LEGACY_ALIGN - 1);
}

/// Architected QEMU `virt` MMIO discovery windows (issue #350). The DTB
/// is primary where firmware provides it (riscv64 boot passes it; the
/// aarch64 `-kernel` path provably does not — GDB: x0 == 0 at
/// higherhalf_entry and no FDT magic anywhere in RAM). The slot table
/// covers the stable board layout for the fallback probe — the same
/// class of board knowledge as the hardcoded UART/GIC/PLIC/ECAM maps.
/// DTB windows must lie inside a boot-mapped leaf (aarch64: the 2MiB
/// Device leaf at 0x0A000000; riscv64: the UART leaf at 0x10000000).
#if defined(CONFIG_ARCH_AARCH64)
constexpr uint64_t VIRTIO_MMIO_DTB_BASE = 0x0A000000;
constexpr uint64_t VIRTIO_MMIO_DTB_WINDOW = 0x200000;
constexpr uint64_t VIRTIO_MMIO_SLOT_BASE = 0x0A000000;
constexpr uint32_t VIRTIO_MMIO_SLOT_STRIDE = 0x200;
constexpr uint32_t VIRTIO_MMIO_SLOT_COUNT = 32;
#elif defined(CONFIG_ARCH_RISCV64)
constexpr uint64_t VIRTIO_MMIO_DTB_BASE = 0x10000000;
constexpr uint64_t VIRTIO_MMIO_DTB_WINDOW = 0x200000;
constexpr uint64_t VIRTIO_MMIO_SLOT_BASE = 0x10001000;
constexpr uint32_t VIRTIO_MMIO_SLOT_STRIDE = 0x1000;
constexpr uint32_t VIRTIO_MMIO_SLOT_COUNT = 8;
#endif

/// Map a PCI common-cfg register to its MMIO v2 offset for reads, or
/// VIRTIO_MMIO_REG_NONE when the register has no MMIO equivalent.
constexpr uint16_t virtio_mmio_read_offset(uint16_t pci_reg) {
    switch (pci_reg) {
    case VIRTIO_COMMON_DEVICE_FEATURE_SEL:
        return VIRTIO_MMIO_DEVICE_FEATURES_SEL;
    case VIRTIO_COMMON_DEVICE_FEATURE:
        return VIRTIO_MMIO_DEVICE_FEATURES;
    case VIRTIO_COMMON_DRIVER_FEATURE_SEL:
        return VIRTIO_MMIO_DRIVER_FEATURES_SEL;
    case VIRTIO_COMMON_DRIVER_FEATURE:
        return VIRTIO_MMIO_DRIVER_FEATURES;
    case VIRTIO_COMMON_DEVICE_STATUS:
        return VIRTIO_MMIO_STATUS;
    case VIRTIO_COMMON_QUEUE_SEL:
        return VIRTIO_MMIO_QUEUE_SEL;
    case VIRTIO_COMMON_QUEUE_SIZE:
        return VIRTIO_MMIO_QUEUE_NUM_MAX;
    case VIRTIO_COMMON_QUEUE_DESC_LO:
        return VIRTIO_MMIO_QUEUE_DESC_LOW;
    case VIRTIO_COMMON_QUEUE_DESC_HI:
        return VIRTIO_MMIO_QUEUE_DESC_HIGH;
    case VIRTIO_COMMON_QUEUE_DRIVER_LO:
        return VIRTIO_MMIO_QUEUE_DRIVER_LOW;
    case VIRTIO_COMMON_QUEUE_DRIVER_HI:
        return VIRTIO_MMIO_QUEUE_DRIVER_HIGH;
    case VIRTIO_COMMON_QUEUE_DEVICE_LO:
        return VIRTIO_MMIO_QUEUE_DEVICE_LOW;
    case VIRTIO_COMMON_QUEUE_DEVICE_HI:
        return VIRTIO_MMIO_QUEUE_DEVICE_HIGH;
    case VIRTIO_COMMON_QUEUE_ENABLE:
        return VIRTIO_MMIO_QUEUE_READY;
    default:
        return VIRTIO_MMIO_REG_NONE;
    }
}

/// Map a PCI common-cfg register to its MMIO v2 offset for writes.
/// QUEUE_SIZE writes target QUEUE_NUM (not NUM_MAX); per-queue MSI-X
/// and notify-multiplier registers have no MMIO equivalent.
constexpr uint16_t virtio_mmio_write_offset(uint16_t pci_reg) {
    if (pci_reg == VIRTIO_COMMON_QUEUE_SIZE)
        return VIRTIO_MMIO_QUEUE_NUM;
    return virtio_mmio_read_offset(pci_reg);
}

/// Map a PCI modern/legacy device ID to its MMIO device ID (0 = unknown).
constexpr uint16_t virtio_mmio_device_id(uint16_t pci_id) {
    if (pci_id == VIRTIO_DEVICE_NET || pci_id == VIRTIO_DEVICE_NET_LEGACY)
        return VIRTIO_MMIO_DEVICE_NET;
    if (pci_id == VIRTIO_DEVICE_BLOCK || pci_id == VIRTIO_DEVICE_BLOCK_LEGACY)
        return VIRTIO_MMIO_DEVICE_BLOCK;
    if (pci_id == VIRTIO_DEVICE_GPU || pci_id == VIRTIO_DEVICE_GPU_LEGACY)
        return VIRTIO_MMIO_DEVICE_GPU;
    return 0;
}

/// Virtual alias for an MMIO physical address (boot-provided mappings).
inline uint64_t virtio_mmio_virt(uint64_t phys) {
#if defined(CONFIG_ARCH_AARCH64)
    return HHDM_OFFSET + phys;
#elif defined(CONFIG_ARCH_RISCV64)
    return phys; // identity L0[0] window covers the MMIO range
#else
    (void)phys;
    return 0;
#endif
}

// --- Transport API ---

/// Check if a PCI device is a Virtio device (vendor 0x1AF4).
inline bool virtio_is_virtio_device(const PciDeviceInfo &info) {
    return info.vendor_id == VIRTIO_PCI_VENDOR;
}

/// Find the first Virtio device with the given device ID.
/// @return true if found, populates @p transport.
bool virtio_find_device(uint16_t device_id, VirtioTransport &transport);

/// Initialize Virtio transport: parse capabilities, reset device, negotiate
/// features, set up queues.
/// @return true on success.
bool virtio_init_transport(VirtioTransport &transport);

/// Map a BAR-based MMIO region for a Virtio capability.
bool virtio_map_mmio(uint8_t bar, uint32_t offset, uint32_t length,
                     VirtioMmio &mmio_out, PciBdf bdf);

/// Read a 32-bit register from the Virtio common config.
inline uint32_t virtio_read_common(const VirtioTransport &t, uint16_t reg) {
#if defined(CONFIG_ARCH_X86_64)
    auto *ptr =
        reinterpret_cast<volatile uint32_t *>(t.common_cfg.virt_addr + reg);
    return *ptr;
#else
    if (t.mmio) {
        // MMIO registers are 32-bit; registers without an MMIO
        // equivalent read as 0 (fail-closed).
        uint16_t mapped = virtio_mmio_read_offset(reg);
        if (mapped == VIRTIO_MMIO_REG_NONE) return 0;
        auto *ptr = reinterpret_cast<volatile uint32_t *>(
            t.common_cfg.virt_addr + mapped);
        return *ptr;
    }
    auto *ptr =
        reinterpret_cast<volatile uint32_t *>(t.common_cfg.virt_addr + reg);
    return *ptr;
#endif
}

/// Write a 32-bit register to the Virtio common config.
inline void virtio_write_common(const VirtioTransport &t, uint16_t reg,
                                uint32_t val) {
#if defined(CONFIG_ARCH_X86_64)
    auto *ptr =
        reinterpret_cast<volatile uint32_t *>(t.common_cfg.virt_addr + reg);
    *ptr = val;
#else
    if (t.mmio) {
        uint16_t mapped = virtio_mmio_write_offset(reg);
        if (mapped == VIRTIO_MMIO_REG_NONE) return;
        auto *ptr = reinterpret_cast<volatile uint32_t *>(
            t.common_cfg.virt_addr + mapped);
        *ptr = val;
        return;
    }
    auto *ptr =
        reinterpret_cast<volatile uint32_t *>(t.common_cfg.virt_addr + reg);
    *ptr = val;
#endif
}

/// Read a 16-bit register from the Virtio common config.
inline uint16_t virtio_read_common16(const VirtioTransport &t, uint16_t reg) {
#if defined(CONFIG_ARCH_X86_64)
    auto *ptr =
        reinterpret_cast<volatile uint16_t *>(t.common_cfg.virt_addr + reg);
    return *ptr;
#else
    // MMIO has no 16-bit registers: 32-bit read, low half.
    if (t.mmio)
        return static_cast<uint16_t>(virtio_read_common(t, reg) & 0xFFFF);
    auto *ptr =
        reinterpret_cast<volatile uint16_t *>(t.common_cfg.virt_addr + reg);
    return *ptr;
#endif
}

/// Write a 16-bit register to the Virtio common config.
inline void virtio_write_common16(const VirtioTransport &t, uint16_t reg,
                                  uint16_t val) {
#if defined(CONFIG_ARCH_X86_64)
    auto *ptr =
        reinterpret_cast<volatile uint16_t *>(t.common_cfg.virt_addr + reg);
    *ptr = val;
#else
    // MMIO takes 32-bit accesses (reserved upper bytes stay zero).
    if (t.mmio) {
        virtio_write_common(t, reg, val);
        return;
    }
    auto *ptr =
        reinterpret_cast<volatile uint16_t *>(t.common_cfg.virt_addr + reg);
    *ptr = val;
#endif
}

/// Read an 8-bit register from the Virtio common config.
inline uint8_t virtio_read_common8(const VirtioTransport &t, uint16_t reg) {
#if defined(CONFIG_ARCH_X86_64)
    auto *ptr =
        reinterpret_cast<volatile uint8_t *>(t.common_cfg.virt_addr + reg);
    return *ptr;
#else
    // MMIO has no 8-bit registers: 32-bit read, low byte.
    if (t.mmio)
        return static_cast<uint8_t>(virtio_read_common(t, reg) & 0xFF);
    auto *ptr =
        reinterpret_cast<volatile uint8_t *>(t.common_cfg.virt_addr + reg);
    return *ptr;
#endif
}

/// Write an 8-bit register to the Virtio common config.
inline void virtio_write_common8(const VirtioTransport &t, uint16_t reg,
                                 uint8_t val) {
#if defined(CONFIG_ARCH_X86_64)
    auto *ptr =
        reinterpret_cast<volatile uint8_t *>(t.common_cfg.virt_addr + reg);
    *ptr = val;
#else
    // MMIO takes 32-bit accesses (reserved upper bytes stay zero).
    if (t.mmio) {
        virtio_write_common(t, reg, val);
        return;
    }
    auto *ptr =
        reinterpret_cast<volatile uint8_t *>(t.common_cfg.virt_addr + reg);
    *ptr = val;
#endif
}

/// Read device status (8-bit register at 0x14).
inline uint8_t virtio_read_status(const VirtioTransport &t) {
    return virtio_read_common8(t, VIRTIO_COMMON_DEVICE_STATUS);
}

/// Write device status (8-bit register at 0x14).
inline void virtio_write_status(const VirtioTransport &t, uint8_t status) {
    virtio_write_common8(t, VIRTIO_COMMON_DEVICE_STATUS, status);
}

/// Negotiate features: offer device features, accept driver features.
/// @return true if feature negotiation succeeded.
bool virtio_negotiate_features(VirtioTransport &t, uint64_t driver_features);

/// Set up a virtqueue.
/// @param t         Transport
/// @param queue_idx Queue index (0-based)
/// @param queue_size Number of descriptors (must be power of 2)
/// @param desc_phys Physical address of descriptor ring
/// @param avail_phys Physical address of available ring
/// @param used_phys Physical address of used ring
/// @return true on success
bool virtio_setup_queue(VirtioTransport &t, uint16_t queue_idx,
                        uint16_t queue_size, uint64_t desc_phys,
                        uint64_t avail_phys, uint64_t used_phys);

/// Notify the device that new descriptors are available.
inline void virtio_notify(const VirtioTransport &t, uint16_t queue_idx) {
#if defined(CONFIG_ARCH_X86_64)
    uint32_t off = t.notify_off_multiplier * queue_idx;
    auto *ptr =
        reinterpret_cast<volatile uint32_t *>(t.notify_cfg.virt_addr + off);
    *ptr = queue_idx;
#else
    // MMIO QueueNotify is a direct queue-index write (no multiplier).
    if (t.mmio) {
        auto *ptr = reinterpret_cast<volatile uint32_t *>(
            t.common_cfg.virt_addr + VIRTIO_MMIO_QUEUE_NOTIFY);
        *ptr = queue_idx;
        return;
    }
    uint32_t off = t.notify_off_multiplier * queue_idx;
    auto *ptr =
        reinterpret_cast<volatile uint32_t *>(t.notify_cfg.virt_addr + off);
    *ptr = queue_idx;
#endif
}

} // namespace arch
// NOLINTEND(performance-no-int-to-ptr,bugprone-easily-swappable-parameters)
