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
/// @brief PCI config space access — CF8/CFC port I/O (x86_64) and ECAM
/// (AArch64, RISC-V).

#pragma once

#include <types.hpp>

namespace arch {

/// @brief PCI Bus:Device.Function address.
struct PciBdf {
    uint8_t bus;      ///< PCI bus number.
    uint8_t device;   ///< Device number on the bus.
    uint8_t function; ///< Function number within the device.

    bool operator==(const PciBdf &o) const {
        return bus == o.bus && device == o.device && function == o.function;
    }
    bool operator!=(const PciBdf &o) const {
        return !(*this == o);
    }
};

} // namespace arch

// ---------------------------------------------------------------------------
// Architecture-specific PCI config space access
//   x86_64: CF8/CFC port I/O (0xCF8 / 0xCFC)
//   aarch64 / riscv64: ECAM memory-mapped (QEMU virt aarch64 at 0x3f000000)
//
// All access functions work on dword-aligned addresses internally, matching
// the x86 CF8/CFC semantics where bits [1:0] of the register offset select
// the byte/word within the dword.  This ensures identical behavior across
// all architectures.
// ---------------------------------------------------------------------------

#if defined(CONFIG_ARCH_X86_64)

#include <kernel/arch/hal/io.hpp>

// NOLINTBEGIN(bugprone-easily-swappable-parameters,performance-no-int-to-ptr)
namespace arch {

/// @brief PCI configuration address port (0xCF8).
constexpr uint16_t PCI_CONFIG_ADDR = 0xCF8;
/// @brief PCI configuration data port (0xCFC).
constexpr uint16_t PCI_CONFIG_DATA = 0xCFC;

/// @brief Build a PCI configuration address from BDF and register offset (x86
/// CF8 format).
/// @param bdf Bus:Device.Function address.
/// @param reg Register offset.
/// @return Encoded address for use with pci_config_{read,write}*.
inline uint64_t pci_make_addr(PciBdf bdf, uint8_t reg) {
    return 0x80000000U | (static_cast<uint32_t>(bdf.bus) << 16) |
           (static_cast<uint32_t>(bdf.device) << 11) |
           (static_cast<uint32_t>(bdf.function) << 8) | reg;
}

/// @brief Read a 32-bit dword from PCI config space (x86 port-I/O).
/// @param addr Address built by pci_make_addr.
/// @return The dword value.
inline uint32_t pci_config_readl(uint64_t addr) {
    outl(PCI_CONFIG_ADDR, static_cast<uint32_t>(addr & ~3ULL));
    return inl(PCI_CONFIG_DATA);
}

/// @brief Read an 8-bit byte from PCI config space (x86 port-I/O).
/// @param addr Address built by pci_make_addr.
/// @return The byte value.
inline uint8_t pci_config_readb(uint64_t addr) {
    uint32_t val = pci_config_readl(addr);
    return static_cast<uint8_t>((val >> ((addr & 3) * 8)) & 0xFF);
}

/// @brief Read a 16-bit word from PCI config space (x86 port-I/O).
/// @param addr Address built by pci_make_addr.
/// @return The word value.
inline uint16_t pci_config_readw(uint64_t addr) {
    uint32_t val = pci_config_readl(addr);
    return static_cast<uint16_t>((val >> ((addr & 2) * 8)) & 0xFFFF);
}

/// @brief Write a 32-bit dword to PCI config space (x86 port-I/O).
/// @param addr Address built by pci_make_addr.
/// @param val Value to write.
inline void pci_config_writel(uint64_t addr, uint32_t val) {
    outl(PCI_CONFIG_ADDR, static_cast<uint32_t>(addr & ~3ULL));
    outl(PCI_CONFIG_DATA, val);
}

/// @brief Write a 16-bit word to PCI config space (x86 port-I/O,
/// read-modify-write).
/// @param addr Address built by pci_make_addr.
/// @param val Value to write.
inline void pci_config_writew(uint64_t addr, uint16_t val) {
    uint32_t old = pci_config_readl(addr);
    uint32_t shift = (addr & 2) * 8;
    uint32_t new_val =
        (old & ~(0xFFFF << shift)) | (static_cast<uint32_t>(val) << shift);
    pci_config_writel(addr, new_val);
}

/// @brief Write an 8-bit byte to PCI config space (x86 port-I/O,
/// read-modify-write).
/// @param addr Address built by pci_make_addr.
/// @param val Value to write.
inline void pci_config_writeb(uint64_t addr, uint8_t val) {
    uint32_t old = pci_config_readl(addr);
    uint32_t shift = (addr & 3) * 8;
    uint32_t new_val =
        (old & ~(0xFF << shift)) | (static_cast<uint32_t>(val) << shift);
    pci_config_writel(addr, new_val);
}

} // namespace arch
// NOLINTEND(bugprone-easily-swappable-parameters,performance-no-int-to-ptr)

#elif defined(CONFIG_ARCH_AARCH64) || defined(CONFIG_ARCH_RISCV64)

#include <kernel/arch/hal/io.hpp>

// NOLINTBEGIN(bugprone-easily-swappable-parameters,performance-no-int-to-ptr)
namespace arch {

#ifndef CONFIG_PCI_ECAM_BASE
#if defined(CONFIG_ARCH_AARCH64)
#define CONFIG_PCI_ECAM_BASE 0x3f000000ULL // QEMU virt aarch64 PCIe ECAM
#else
#define CONFIG_PCI_ECAM_BASE 0x100000000ULL // riscv64 fallback (per-platform)
#endif
#endif
/// @brief ECAM base address for memory-mapped PCI config space.
constexpr uint64_t PCI_ECAM_BASE = CONFIG_PCI_ECAM_BASE;

/// @brief Build a PCI configuration address from BDF and register offset (ECAM
/// format).
/// @param bdf Bus:Device.Function address.
/// @param reg Register offset.
/// @return Memory-mapped address for use with pci_config_{read,write}*.
inline uint64_t pci_make_addr(PciBdf bdf, uint8_t reg) {
    return PCI_ECAM_BASE | (static_cast<uint64_t>(bdf.bus) << 20) |
           (static_cast<uint64_t>(bdf.device) << 15) |
           (static_cast<uint64_t>(bdf.function) << 12) | reg;
}

/// @brief Read a 32-bit dword from PCI config space (ECAM memory-mapped).
/// @param addr Address built by pci_make_addr.
/// @return The dword value.
inline uint32_t pci_config_readl(uint64_t addr) {
    volatile uint32_t *ptr =
        reinterpret_cast<volatile uint32_t *>(addr & ~3ULL);
    return *ptr;
}

/// @brief Read an 8-bit byte from PCI config space (ECAM memory-mapped).
/// @param addr Address built by pci_make_addr.
/// @return The byte value.
inline uint8_t pci_config_readb(uint64_t addr) {
    uint32_t val = pci_config_readl(addr);
    return static_cast<uint8_t>((val >> ((addr & 3) * 8)) & 0xFF);
}

/// @brief Read a 16-bit word from PCI config space (ECAM memory-mapped).
/// @param addr Address built by pci_make_addr.
/// @return The word value.
inline uint16_t pci_config_readw(uint64_t addr) {
    uint32_t val = pci_config_readl(addr);
    return static_cast<uint16_t>((val >> ((addr & 2) * 8)) & 0xFFFF);
}

/// @brief Write a 32-bit dword to PCI config space (ECAM memory-mapped).
/// @param addr Address built by pci_make_addr.
/// @param val Value to write.
inline void pci_config_writel(uint64_t addr, uint32_t val) {
    volatile uint32_t *ptr =
        reinterpret_cast<volatile uint32_t *>(addr & ~3ULL);
    *ptr = val;
}

/// @brief Write a 16-bit word to PCI config space (ECAM memory-mapped,
/// read-modify-write).
/// @param addr Address built by pci_make_addr.
/// @param val Value to write.
inline void pci_config_writew(uint64_t addr, uint16_t val) {
    uint32_t old = pci_config_readl(addr);
    uint32_t shift = (addr & 2) * 8;
    uint32_t new_val =
        (old & ~(0xFFFF << shift)) | (static_cast<uint32_t>(val) << shift);
    pci_config_writel(addr, new_val);
}

/// @brief Write an 8-bit byte to PCI config space (ECAM memory-mapped,
/// read-modify-write).
/// @param addr Address built by pci_make_addr.
/// @param val Value to write.
inline void pci_config_writeb(uint64_t addr, uint8_t val) {
    uint32_t old = pci_config_readl(addr);
    uint32_t shift = (addr & 3) * 8;
    uint32_t new_val =
        (old & ~(0xFF << shift)) | (static_cast<uint32_t>(val) << shift);
    pci_config_writel(addr, new_val);
}

} // namespace arch
// NOLINTEND(bugprone-easily-swappable-parameters,performance-no-int-to-ptr)

/// @cond
#else
#error "pci.hpp: unsupported architecture"
#endif
/// @endcond
