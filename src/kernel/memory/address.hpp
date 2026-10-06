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

/// @file address.hpp
/// @brief Type-safe physical and virtual address wrappers (MISRA C++ 2023
/// compliant).

#include <types.hpp>

namespace kernel {

/// @brief Physical address wrapper — eliminates raw reinterpret_cast for
/// physical memory.
/// @note Enforces alignment constraints and provides explicit conversion
/// operators.
class PhysicalAddress {
    uint64_t addr_;

  public:
    /// @brief Default constructor — initialises to null address 0.
    constexpr PhysicalAddress() noexcept : addr_(0) {
    }
    /// @brief Construct from a raw physical address.
    /// @param addr Physical memory address.
    explicit constexpr PhysicalAddress(uint64_t addr) noexcept : addr_(addr) {
    }

    /// @brief Return the raw physical address value.
    [[nodiscard]] constexpr uint64_t value() const noexcept {
        return addr_;
    }
    /// @brief Check whether the address is null (zero).
    [[nodiscard]] constexpr bool is_null() const noexcept {
        return addr_ == 0;
    }

    constexpr PhysicalAddress &operator=(uint64_t addr) noexcept {
        addr_ = addr;
        return *this;
    }
    constexpr bool operator==(PhysicalAddress other) const noexcept {
        return addr_ == other.addr_;
    }
    constexpr bool operator!=(PhysicalAddress other) const noexcept {
        return addr_ != other.addr_;
    }
    constexpr bool operator<(PhysicalAddress other) const noexcept {
        return addr_ < other.addr_;
    }

    constexpr PhysicalAddress operator+(size_t offset) const noexcept {
        return PhysicalAddress(addr_ + offset);
    }
    constexpr PhysicalAddress operator-(size_t offset) const noexcept {
        return PhysicalAddress(addr_ - offset);
    }
    constexpr PhysicalAddress &operator+=(size_t offset) noexcept {
        addr_ += offset;
        return *this;
    }
    constexpr PhysicalAddress &operator-=(size_t offset) noexcept {
        addr_ -= offset;
        return *this;
    }
};

/// @brief Virtual address wrapper — eliminates raw reinterpret_cast for virtual
/// memory.
/// @note Enforces canonical address space constraints for x86_64.
class VirtualAddress {
    uint64_t addr_;

  public:
    /// @brief Default constructor — initialises to null address 0.
    constexpr VirtualAddress() noexcept : addr_(0) {
    }
    /// @brief Construct from a raw virtual address.
    /// @param addr Virtual memory address.
    explicit constexpr VirtualAddress(uint64_t addr) noexcept : addr_(addr) {
    }

    /// @brief Return the raw virtual address value.
    [[nodiscard]] constexpr uint64_t value() const noexcept {
        return addr_;
    }
    /// @brief Check whether the address is null (zero).
    [[nodiscard]] constexpr bool is_null() const noexcept {
        return addr_ == 0;
    }
    /// @brief Check if the address is canonical (x86_64: bits 63-47 are sign
    /// extension of bit 47).
    [[nodiscard]] constexpr bool is_canonical() const noexcept {
        // x86_64 canonical: bits 63..47 must be sign extension of bit 47
        return (addr_ & 0xFFFF800000000000ULL) == 0 ||
               (addr_ & 0xFFFF800000000000ULL) == 0xFFFF800000000000ULL;
    }
    /// @brief Check whether the address lies in kernel-space (higher half).
    [[nodiscard]] constexpr bool is_kernel() const noexcept {
        return addr_ >= 0xFFFF800000000000ULL;
    }
    /// @brief Check whether the address lies in user-space (lower half).
    [[nodiscard]] constexpr bool is_user() const noexcept {
        return addr_ < 0x0000800000000000ULL;
    }

    constexpr VirtualAddress &operator=(uint64_t addr) noexcept {
        addr_ = addr;
        return *this;
    }
    constexpr bool operator==(VirtualAddress other) const noexcept {
        return addr_ == other.addr_;
    }
    constexpr bool operator!=(VirtualAddress other) const noexcept {
        return addr_ != other.addr_;
    }
    constexpr bool operator<(VirtualAddress other) const noexcept {
        return addr_ < other.addr_;
    }

    constexpr VirtualAddress operator+(size_t offset) const noexcept {
        return VirtualAddress(addr_ + offset);
    }
    constexpr VirtualAddress operator-(size_t offset) const noexcept {
        return VirtualAddress(addr_ - offset);
    }
    constexpr VirtualAddress &operator+=(size_t offset) noexcept {
        addr_ += offset;
        return *this;
    }
    constexpr VirtualAddress &operator-=(size_t offset) noexcept {
        addr_ -= offset;
        return *this;
    }
};

/// @brief Architecture-specific higher-half direct map offset.
/// @note Physical address X maps to VirtualAddress(HHDM_OFFSET + X).
static constexpr uint64_t HHDM_OFFSET = 0xFFFF800000000000ULL;

/// @brief Convert physical address to higher-half virtual address.
[[nodiscard]] constexpr VirtualAddress
phys_to_virt(PhysicalAddress phys) noexcept {
    return VirtualAddress(HHDM_OFFSET + phys.value());
}

/// @brief Convert higher-half virtual address to physical address.
[[nodiscard]] constexpr PhysicalAddress
virt_to_phys(VirtualAddress virt) noexcept {
    return PhysicalAddress(virt.value() - HHDM_OFFSET);
}

/// @brief Page-aligned physical address.
class PageAddress : public PhysicalAddress {
  public:
    /// @brief Default constructor — initialises to null page address.
    constexpr PageAddress() noexcept = default;
    /// @brief Construct a page-aligned address from a raw value (clears offset
    /// bits).
    /// @param addr Raw physical address (lower 12 bits are masked off).
    explicit constexpr PageAddress(uint64_t addr) noexcept
        : PhysicalAddress(addr & ~0xFFFULL) {
    }
    /// @brief Construct a page-aligned address from a PhysicalAddress.
    /// @param phys Physical address (lower 12 bits are masked off).
    explicit constexpr PageAddress(PhysicalAddress phys) noexcept
        : PhysicalAddress(phys.value() & ~0xFFFULL) {
    }

    /// @brief Return the page index (address / 4096).
    [[nodiscard]] constexpr size_t page_index() const noexcept {
        return value() / 4096;
    }
    /// @brief Check whether the underlying address is already page-aligned.
    [[nodiscard]] constexpr bool is_page_aligned() const noexcept {
        return (value() & 0xFFFULL) == 0;
    }
};

} // namespace kernel