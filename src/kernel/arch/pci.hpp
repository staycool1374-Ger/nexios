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

/// @file pci.hpp
/// @brief PCI enumeration — arch-independent interface using HAL config space
/// access.

#pragma once

#include <types.hpp>
#include <kernel/arch/hal/io.hpp>
#include <kernel/arch/pci_errors.hpp>
#include <kernel/arch/hal/pci.hpp>

namespace arch {

/// Max devices per bus
constexpr uint8_t PCI_MAX_DEVICES = 32;
constexpr uint8_t PCI_MAX_FUNCTIONS = 8;
constexpr uint16_t PCI_MAX_BUSES = 256;

/// Standard PCI config space register offsets
enum PciRegister : uint8_t {
    PCI_VENDOR_ID = 0x00,
    PCI_DEVICE_ID = 0x02,
    PCI_COMMAND = 0x04,
    PCI_STATUS = 0x06,
    PCI_REVISION = 0x08,
    PCI_PROG_IF = 0x09,
    PCI_SUBCLASS = 0x0A,
    PCI_CLASS = 0x0B,
    PCI_HEADER_TYPE = 0x0E,
    PCI_BIST = 0x0F,
    PCI_BAR0 = 0x10,
    PCI_BAR1 = 0x14,
    PCI_BAR2 = 0x18,
    PCI_BAR3 = 0x1C,
    PCI_BAR4 = 0x20,
    PCI_BAR5 = 0x24,
    PCI_SECONDARY_BUS = 0x19,
};

/// PCI status register bits
// NOLINTNEXTLINE(performance-enum-size)
enum PciStatus : uint16_t {
    PCI_STATUS_CAP_LIST = 1 << 4,
};

/// PCI command register bits
// NOLINTNEXTLINE(performance-enum-size)
enum PciCommand : uint16_t {
    PCI_CMD_IO_SPACE = 1 << 0,
    PCI_CMD_MEM_SPACE = 1 << 1,
    PCI_CMD_BUS_MASTER = 1 << 2,
};

/// PCI header type flags
constexpr uint8_t PCI_HEADER_TYPE_DEVICE = 0x00;
constexpr uint8_t PCI_HEADER_TYPE_BRIDGE = 0x01;
constexpr uint8_t PCI_HEADER_TYPE_MULTI = 0x80;

/// Capability pointer offset (for header type 0)
constexpr uint8_t PCI_CAP_PTR = 0x34;

/// Capability IDs
constexpr uint8_t PCI_CAP_ID_MSI = 0x05;
constexpr uint8_t PCI_CAP_ID_MSIX = 0x11;

/// MSI Message Control register bits
constexpr uint16_t PCI_MSI_CTRL_ENABLE = 1 << 0;
constexpr uint16_t PCI_MSI_CTRL_MMC_MASK = 0x0E; // bits 3:1
constexpr uint16_t PCI_MSI_CTRL_MME_MASK = 0x70; // bits 6:4
constexpr uint16_t PCI_MSI_CTRL_64BIT = 1 << 7;
constexpr uint16_t PCI_MSI_CTRL_PVM = 1 << 8;

/// MSI-X Message Control register bits
constexpr uint16_t PCI_MSIX_CTRL_ENABLE = 1 << 15;
constexpr uint16_t PCI_MSIX_CTRL_FUNCMASK = 1 << 14;
constexpr uint16_t PCI_MSIX_CTRL_TSIZE_MASK = 0x7FF;

/// MSI-X table entry vector-control mask bit (bit 0 of the 4th dword).
constexpr uint32_t MSIX_ENTRY_MASK_BIT = 1U << 0;

/// One 16-byte MSI-X table entry (TableOffset + entry*16).  Accessed through
/// the mapped table KVA only (never a raw physical write — CODING_STYLE §4).
struct MsixTableEntry {
    uint32_t msg_addr_low = 0;   ///< Message Address low (bits 31:0)
    uint32_t msg_addr_high = 0;  ///< Message Address high (bits 63:32)
    uint32_t msg_data = 0;       ///< Message Data (vector etc.)
    uint32_t vector_control = 0; ///< bit 0 = mask; 1 = masked
};

/// Parsed MSI-X table location (from the capability's Table BIR/Offset and
/// Message Control TSIZE field).
struct PciMsixTableInfo {
    uint8_t bir = 0;          ///< BAR index holding the table
    uint16_t entry_count = 0; ///< number of entries (TSIZE+1)
    uint64_t table_phys = 0;  ///< physical address of the table start
    uint64_t table_kva = 0;   ///< kernel virtual address (HHDM alias) of table
    uint64_t bar_size = 0;    ///< size of the backing BAR (bounds check)
};

/// MSI address: xAPIC base (must be written to device's Message Address reg)
constexpr uint32_t PCI_MSI_ADDR_BASE = 0xFEE00000U;

/// MSI data: fixed delivery, edge-triggered, physical destination
constexpr uint16_t PCI_MSI_DATA_FIXED = 0x0000;

/// BAR type
enum class PciBarType : uint8_t {
    MEMORY_32 = 0,
    IO = 1,
    MEMORY_64 = 2,
};

/// Parsed BAR descriptor
struct PciBar {
    uint64_t address;
    uint64_t size;
    PciBarType type;
    bool prefetchable;
};

/// Discovered device info
struct PciDeviceInfo {
    PciBdf bdf;
    uint16_t vendor_id;
    uint16_t device_id;
    uint8_t class_code;
    uint8_t subclass;
    uint8_t prog_if;
    uint8_t revision;
    uint8_t header_type;
    PciBar bars[6];
    uint8_t bar_count;
};

// --- Unified (arch-independent) config space access wrappers ---

/// Read vendor ID (return 0xFFFF if no device)
inline uint16_t pci_read_vendor(PciBdf bdf) {
    return pci_config_readw(pci_make_addr(bdf, PCI_VENDOR_ID));
}

/// Read device ID
inline uint16_t pci_read_device(PciBdf bdf) {
    return pci_config_readw(pci_make_addr(bdf, PCI_DEVICE_ID));
}

/// Check if a BDF has a valid device
inline bool pci_device_exists(PciBdf bdf) {
    return pci_read_vendor(bdf) != 0xFFFF;
}

// --- Enumeration ---

constexpr size_t PCI_MAX_DEVICES_FOUND = 128;

size_t pci_scan_all();
const PciDeviceInfo *pci_devices();
size_t pci_device_count();
const PciDeviceInfo *pci_find_device(uint8_t class_code, uint8_t subclass);
void pci_parse_bars(PciDeviceInfo &info);
PciDeviceInfo pci_read_device_info(PciBdf bdf);

void pci_dump_tree();
void pci_print_tree(char *buffer, size_t size);

// --- Capabilities & MSI/MSI-X ---

/// Walk the capability list and return the config space offset of the
/// first capability matching @p cap_id, or 0 if not found.
uint8_t pci_find_capability(PciBdf bdf, uint8_t cap_id);

/// Program MSI for a device. Allocates a free interrupt vector and
/// sets up the Message Address + Data registers.
/// @param bdf        Device BDF
/// @param apic_id    Destination APIC ID (0 for BSP)
/// @return           The allocated vector number, or 0 on failure.
uint8_t pci_enable_msi(PciBdf bdf, uint8_t apic_id);

/// Program MSI-X for a device entry. Allocates a free interrupt vector
/// and writes the MSI-X table entry in MMIO.
/// @param bdf        Device BDF
/// @param entry      MSI-X table entry index
/// @param apic_id    Destination APIC ID (0 for BSP)
/// @return           The allocated vector number, or 0 on failure.
uint8_t pci_enable_msix(PciBdf bdf, uint16_t entry, uint8_t apic_id);

/// @brief Locates and maps the MSI-X table of @p bdf.
/// @param bdf   Device BDF (MSI-X capability required).
/// @param out   Filled with the parsed table info on success.
/// @return true when the device has an MSI-X capability, the table lies in a
///         memory BAR that fits the BAR size, and the table pages are mapped
///         at a kernel VA (out.table_kva).
bool pci_msix_table_info(PciBdf bdf, PciMsixTableInfo &out);

/// @brief Pre-maps the MSI-X tables of every scanned MSI-X-capable device and
///        caches the result (iommu probe-once pattern, issue #10).  Called
///        from pci_scan_all() so the mapping pages are established at boot,
///        BEFORE the test-isolation baseline — a test-time MsixCap::create
///        then reuses the cached KVA instead of allocating a fresh page-table
///        page (ResourceTracker-clean).  Idempotent.
void pci_msix_premap_all();

/// @brief Programs one MSI-X table entry: message address/data + vector
///        control.  The entry is left MASKED (create-time fail-closed state);
///        the caller unmasks via pci_msix_entry_set_masked when arming.
/// @param bdf     Device BDF (validated against @p tbl.bir/@p tbl.entry_count).
/// @param tbl     Parsed table info (from pci_msix_table_info).
/// @param entry   Entry index (< tbl.entry_count).
/// @param vector  Allocated MSI-X vector (48–255, != 0x80).
/// @param apic_id Destination APIC ID (0 for BSP).
/// @return true on success.
bool pci_program_msix_entry(PciBdf bdf, const PciMsixTableInfo &tbl,
                            uint16_t entry, uint8_t vector, uint8_t apic_id);

/// @brief Sets/clears the mask bit of one MSI-X table entry (MMIO write to
///        the mapped table KVA).  @p masked = true blocks delivery.
/// @return true on success (table mapped, entry in range).
bool pci_msix_entry_set_masked(const PciMsixTableInfo &tbl, uint16_t entry,
                               bool masked);

/// @brief Reads the mask state of one MSI-X table entry.
/// @return true when the entry is currently masked.
bool pci_msix_entry_masked(const PciMsixTableInfo &tbl, uint16_t entry);

/// Allocate a free interrupt vector for MSI/MSI-X use.
/// @return Vector number (48-0xFF, excluding 0x80), or 0 if none free.
uint8_t pci_alloc_vector();

/// Free a previously allocated interrupt vector.
void pci_free_vector(uint8_t vec);

} // namespace arch
