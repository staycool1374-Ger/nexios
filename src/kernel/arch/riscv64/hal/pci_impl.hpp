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

/// @file pci_impl.hpp
/// @brief RISC-V64 PCI ECAM (Enhanced Configuration Access Mechanism)
///        implementation — memory-mapped PCI configuration space access.

#pragma once

#include <types.hpp>

// NOTE: included inside namespace arch {} in pci.hpp — do not re-wrap

/// @brief QEMU virt riscv64 ECAM base address (PCIe configuration space)
/// NOTE: the live base is owned by kernel/arch/hal/pci.hpp (issue #29:
/// 0x30000000, `info mtree` proof); this #ifndef is a dead fallback that
/// never fires (pci.hpp defines the macro first).  Raw phys is valid:
/// boot.S identity-maps 0x30000000-0x3FFFFFFF (L2_page[384..511)),
/// following the in-tree PLIC/UART precedent.
#ifndef CONFIG_PCI_ECAM_BASE
#define CONFIG_PCI_ECAM_BASE 0x100000000ULL // 4GB default
#endif

/// @brief Base physical address of the PCI ECAM region.
constexpr uint64_t PCI_ECAM_BASE = CONFIG_PCI_ECAM_BASE;
/// @brief Size of the PCI ECAM region (256MB: 256 buses x 32 dev x 8 fn x 4KB).
constexpr uint64_t PCI_ECAM_SIZE = 0x10000000ULL;

/// @brief Compute the memory-mapped ECAM address for a given BDF and register.
/// @param bdf PCI Bus/Device/Function identifier.
/// @param reg Register offset within the config space.
/// @return Physical address of the register in ECAM space.
inline uint64_t pci_ecam_addr(PciBdf bdf, uint8_t reg) {
    return PCI_ECAM_BASE | (static_cast<uint64_t>(bdf.bus) << 20) |
           (static_cast<uint64_t>(bdf.device) << 15) |
           (static_cast<uint64_t>(bdf.function) << 12) | reg;
}

/// @brief Read a 32-bit value from PCI config space via ECAM.
/// @param bdf PCI BDF identifier.
/// @param reg Register offset.
/// @return 32-bit value read.
inline uint32_t pci_ecam_readl(PciBdf bdf, uint8_t reg) {
    volatile uint32_t *addr =
        reinterpret_cast<volatile uint32_t *>(pci_ecam_addr(bdf, reg));
    return *addr;
}

/// @brief Read a 16-bit value from PCI config space via ECAM.
/// @param bdf PCI BDF identifier.
/// @param reg Register offset.
/// @return 16-bit value read.
inline uint16_t pci_ecam_readw(PciBdf bdf, uint8_t reg) {
    volatile uint16_t *addr =
        reinterpret_cast<volatile uint16_t *>(pci_ecam_addr(bdf, reg));
    return *addr;
}

/// @brief Read an 8-bit value from PCI config space via ECAM.
/// @param bdf PCI BDF identifier.
/// @param reg Register offset.
/// @return 8-bit value read.
inline uint8_t pci_ecam_readb(PciBdf bdf, uint8_t reg) {
    volatile uint8_t *addr =
        reinterpret_cast<volatile uint8_t *>(pci_ecam_addr(bdf, reg));
    return *addr;
}

/// @brief Write a 32-bit value to PCI config space via ECAM.
/// @param bdf PCI BDF identifier.
/// @param reg Register offset.
/// @param val Value to write.
inline void pci_ecam_writel(PciBdf bdf, uint8_t reg, uint32_t val) {
    volatile uint32_t *addr =
        reinterpret_cast<volatile uint32_t *>(pci_ecam_addr(bdf, reg));
    *addr = val;
}

/// @brief Write a 16-bit value to PCI config space via ECAM.
/// @param bdf PCI BDF identifier.
/// @param reg Register offset.
/// @param val Value to write.
inline void pci_ecam_writew(PciBdf bdf, uint8_t reg, uint16_t val) {
    volatile uint16_t *addr =
        reinterpret_cast<volatile uint16_t *>(pci_ecam_addr(bdf, reg));
    *addr = val;
}

/// @brief Write an 8-bit value to PCI config space via ECAM.
/// @param bdf PCI BDF identifier.
/// @param reg Register offset.
/// @param val Value to write.
inline void pci_ecam_writeb(PciBdf bdf, uint8_t reg, uint8_t val) {
    volatile uint8_t *addr =
        reinterpret_cast<volatile uint8_t *>(pci_ecam_addr(bdf, reg));
    *addr = val;
}

/// @brief Read the PCI Vendor ID via ECAM.
/// @param bdf PCI BDF identifier.
/// @return Vendor ID (0xFFFF if no device present).
inline uint16_t pci_read_vendor_ecam(PciBdf bdf) {
    return pci_ecam_readw(bdf, PCI_VENDOR_ID);
}

/// @brief Read the PCI Device ID via ECAM.
/// @param bdf PCI BDF identifier.
/// @return Device ID.
inline uint16_t pci_read_device_ecam(PciBdf bdf) {
    return pci_ecam_readw(bdf, PCI_DEVICE_ID);
}

/// @brief Check if a PCI device exists by reading its Vendor ID.
/// @param bdf PCI BDF identifier.
/// @return true if Vendor ID != 0xFFFF.
inline bool pci_device_exists_ecam(PciBdf bdf) {
    return pci_read_vendor_ecam(bdf) != 0xFFFF;
}

/// @brief Stub: read 32-bit PCI config via old-style address (no-op).
/// @param addr Config address (ignored).
/// @return Always 0.
inline uint32_t pci_config_readl_ecam(uint32_t addr) {
    (void)addr;
    return 0;
}

/// @brief Stub: read 8-bit PCI config via old-style address (no-op).
/// @param addr Config address (ignored).
/// @return Always 0.
inline uint8_t pci_config_readb_ecam(uint32_t addr) {
    (void)addr;
    return 0;
}

/// @brief Stub: write 32-bit PCI config via old-style address (no-op).
/// @param addr Config address (ignored).
/// @param val  Value to write (ignored).
inline void pci_config_writel_ecam(uint32_t addr, uint32_t val) {
    (void)addr;
    (void)val;
}

/// @brief Stub: read 16-bit PCI config via old-style address (no-op).
/// @param addr Config address (ignored).
/// @return Always 0.
inline uint16_t pci_config_readw_ecam(uint32_t addr) {
    (void)addr;
    return 0;
}

/// @brief Build an ECAM address for a given BDF and register.
/// @param bdf PCI BDF identifier.
/// @param reg Register offset.
/// @return ECAM physical address.
inline uint64_t pci_make_addr_ecam(PciBdf bdf, uint8_t reg) {
    return pci_ecam_addr(bdf, reg);
}
