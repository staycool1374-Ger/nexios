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
/// @brief AArch64 PCIe ECAM config space access (memory-mapped, no CF8/CFC).

#pragma once

#include <types.hpp>
#include <kernel/arch/hal/io.hpp>

// NOTE: included inside namespace arch {} in pci.hpp — do not re-wrap

/// QEMU virt aarch64 ECAM base address (PCIe configuration space)
/// This is memory-mapped, not I/O port based like x86 CF8/CFC
#ifndef CONFIG_PCI_ECAM_BASE
#define CONFIG_PCI_ECAM_BASE 0x3f000000ULL // QEMU virt aarch64 PCIe ECAM
#endif

constexpr uint64_t PCI_ECAM_BASE = CONFIG_PCI_ECAM_BASE;
constexpr uint64_t PCI_ECAM_SIZE =
    0x100000000ULL; // 256 buses * 32 devices * 8 functions * 4KB = 256MB (ECAM window at 0x3f000000)

/// @brief Build ECAM address from BDF + register offset.
/// ECAM layout: base | (bus << 20) | (device << 15) | (function << 12) | reg
/// @param[in] bdf PCI bus/device/function identifier.
/// @param[in] reg Configuration space register offset.
/// @return Physical memory address for ECAM access.
inline uint64_t pci_ecam_addr(PciBdf bdf, uint8_t reg) {
    return PCI_ECAM_BASE | (static_cast<uint64_t>(bdf.bus) << 20) |
           (static_cast<uint64_t>(bdf.device) << 15) |
           (static_cast<uint64_t>(bdf.function) << 12) | reg;
}

/// @brief ECAM config space read (32-bit).
/// @param[in] bdf PCI bus/device/function identifier.
/// @param[in] reg Register offset.
/// @return 32-bit value read from config space.
inline uint32_t pci_ecam_readl(PciBdf bdf, uint8_t reg) {
    volatile uint32_t *addr =
        reinterpret_cast<volatile uint32_t *>(pci_ecam_addr(bdf, reg));
    return *addr;
}

/// @brief ECAM config space read (16-bit).
/// @param[in] bdf PCI bus/device/function identifier.
/// @param[in] reg Register offset.
/// @return 16-bit value read from config space.
inline uint16_t pci_ecam_readw(PciBdf bdf, uint8_t reg) {
    volatile uint16_t *addr =
        reinterpret_cast<volatile uint16_t *>(pci_ecam_addr(bdf, reg));
    return *addr;
}

/// @brief ECAM config space read (8-bit).
/// @param[in] bdf PCI bus/device/function identifier.
/// @param[in] reg Register offset.
/// @return 8-bit value read from config space.
inline uint8_t pci_ecam_readb(PciBdf bdf, uint8_t reg) {
    volatile uint8_t *addr =
        reinterpret_cast<volatile uint8_t *>(pci_ecam_addr(bdf, reg));
    return *addr;
}

/// @brief ECAM config space write (32-bit).
/// @param[in] bdf PCI bus/device/function identifier.
/// @param[in] reg Register offset.
/// @param[in] val 32-bit value to write.
inline void pci_ecam_writel(PciBdf bdf, uint8_t reg, uint32_t val) {
    volatile uint32_t *addr =
        reinterpret_cast<volatile uint32_t *>(pci_ecam_addr(bdf, reg));
    *addr = val;
}

/// @brief ECAM config space write (16-bit).
/// @param[in] bdf PCI bus/device/function identifier.
/// @param[in] reg Register offset.
/// @param[in] val 16-bit value to write.
inline void pci_ecam_writew(PciBdf bdf, uint8_t reg, uint16_t val) {
    volatile uint16_t *addr =
        reinterpret_cast<volatile uint16_t *>(pci_ecam_addr(bdf, reg));
    *addr = val;
}

/// @brief ECAM config space write (8-bit).
/// @param[in] bdf PCI bus/device/function identifier.
/// @param[in] reg Register offset.
/// @param[in] val 8-bit value to write.
inline void pci_ecam_writeb(PciBdf bdf, uint8_t reg, uint8_t val) {
    volatile uint8_t *addr =
        reinterpret_cast<volatile uint8_t *>(pci_ecam_addr(bdf, reg));
    *addr = val;
}

/// @brief Read PCI vendor ID via ECAM.
/// @param[in] bdf PCI bus/device/function identifier.
/// @return Vendor ID (16-bit).
inline uint16_t pci_read_vendor_ecam(PciBdf bdf) {
    return pci_ecam_readw(bdf, PCI_VENDOR_ID);
}

/// @brief Read PCI device ID via ECAM.
/// @param[in] bdf PCI bus/device/function identifier.
/// @return Device ID (16-bit).
inline uint16_t pci_read_device_ecam(PciBdf bdf) {
    return pci_ecam_readw(bdf, PCI_DEVICE_ID);
}

/// @brief Check whether a PCI device exists at the given BDF.
/// @param[in] bdf PCI bus/device/function identifier.
/// @return true if vendor ID is not 0xFFFF.
inline bool pci_device_exists_ecam(PciBdf bdf) {
    return pci_read_vendor_ecam(bdf) != 0xFFFF;
}

/// @brief ECAM-aware config space read (32-bit, API compatibility stub).
/// Not used for ECAM — retained for API compatibility with x86 CF8/CFC path.
/// @param[in] addr Legacy address.
/// @return Always 0.
inline uint32_t pci_config_readl_ecam(uint32_t addr) {
    // Not used for ECAM - kept for API compatibility
    (void)addr;
    return 0;
}

/// @brief ECAM-aware config space read (8-bit, API compatibility stub).
/// @param[in] addr Legacy address.
/// @return Always 0.
inline uint8_t pci_config_readb_ecam(uint32_t addr) {
    (void)addr;
    return 0;
}

/// @brief ECAM-aware config space write (32-bit, API compatibility stub).
/// @param[in] addr Legacy address.
/// @param[in] val Value (ignored).
inline void pci_config_writel_ecam(uint32_t addr, uint32_t val) {
    (void)addr;
    (void)val;
}

/// @brief ECAM-aware config space read (16-bit, API compatibility stub).
/// @param[in] addr Legacy address.
/// @return Always 0.
inline uint16_t pci_config_readw_ecam(uint32_t addr) {
    (void)addr;
    return 0;
}

/// @brief Build ECAM physical address (API compatibility with x86 make_addr).
/// @param[in] bdf PCI bus/device/function identifier.
/// @param[in] reg Register offset.
/// @return ECAM physical address.
inline uint64_t pci_make_addr_ecam(PciBdf bdf, uint8_t reg) {
    return pci_ecam_addr(bdf, reg);
}