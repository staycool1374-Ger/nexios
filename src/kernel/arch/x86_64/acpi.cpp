/*
 * NexIOS RTOS — IOMMU DMA protection
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

/// @file acpi.cpp
/// @brief ACPI DMAR discovery implementation (issue #9).  The boot page
/// tables identity-map only the first 128 MiB, but QEMU places ACPI tables
/// near the top of RAM, so every accessed page is explicitly mapped on
/// demand (HHDM + VMM::map_page, APIC/framebuffer pattern).  Every table
/// access is bounds-checked against the declared length and every signature
/// + checksum is validated before the data is trusted (fail-closed probe).

#include <kernel/arch/x86_64/acpi.hpp>

#include <kernel/multiboot2.hpp>
#include <kernel/memory/vmm.hpp>
#include <constants.hpp>

namespace kernel::iommu::acpi {

namespace {

/// @brief Maps the 4 KiB page containing @p phys at HHDM_OFFSET + page.
///        The boot page tables only identity-map the first 128 MiB; QEMU
///        places ACPI tables near the top of RAM, so each accessed page is
///        explicitly mapped (APIC/framebuffer pattern).  Returns the HHDM
///        virtual address of @p phys, or 0 when the mapping did not take
///        effect (fail-closed — a failed scan must never deref an unmapped
///        VA).
uint64_t acpi_map(uint64_t phys) {
    uint64_t page = phys & ~static_cast<uint64_t>(arch::PAGE_SIZE - 1);
    uint64_t va = arch::HHDM_OFFSET + page;
    kernel::VMM::map_page(va, page, false);
    // map_page is void; verify the leaf PTE landed (present + phys match).
    uint64_t pt = kernel::VMM::virt_to_phys_in_pml4(
        va, kernel::VMM::get_kernel_pml4());
    if (pt == 0)
        return 0; // mapping did not take effect — fail closed
    return arch::HHDM_OFFSET + phys;
}

/// @brief Reads a byte from physical memory, mapping the page on demand.
uint8_t phys_read8(uint64_t phys) {
    uint64_t va = acpi_map(phys);
    if (va == 0)
        return 0; // mapping failed — fail closed, never deref the null VA
    // NOLINTNEXTLINE(performance-no-int-to-ptr)
    const volatile uint8_t *p =
        reinterpret_cast<const volatile uint8_t *>(va);
    return *p;
}

uint16_t phys_read16(uint64_t phys) {
    return static_cast<uint16_t>(phys_read8(phys)) |
           static_cast<uint16_t>(phys_read8(phys + 1)) << 8;
}

uint32_t phys_read32(uint64_t phys) {
    return static_cast<uint32_t>(phys_read16(phys)) |
           static_cast<uint32_t>(phys_read16(phys + 2)) << 16;
}

uint64_t phys_read64(uint64_t phys) {
    return static_cast<uint64_t>(phys_read32(phys)) |
           static_cast<uint64_t>(phys_read32(phys + 4)) << 32;
}

/// @brief Reads a 4-byte signature (e.g. "DMAR", "XSDT") at @p phys.
uint32_t phys_sig(uint64_t phys) {
    return static_cast<uint32_t>(phys_read8(phys)) |
           (static_cast<uint32_t>(phys_read8(phys + 1)) << 8) |
           (static_cast<uint32_t>(phys_read8(phys + 2)) << 16) |
           (static_cast<uint32_t>(phys_read8(phys + 3)) << 24);
}

/// @brief "RSD PTR " signature.
constexpr uint64_t kRsdpSig = 0x2052545020445352ULL; // "RSD PTR "
/// @brief "XSDT" signature (4 bytes little-endian).
constexpr uint32_t kXsdtSig = 0x54445358U; // "XSDT"
/// @brief "RSDT" signature.
constexpr uint32_t kRsdtSig = 0x54445352U; // "RSDT"
/// @brief "DMAR" signature.
constexpr uint32_t kDmarSig = 0x52414D44U; // "DMAR"

/// @brief ACPI RSDP v2 layout (offset 0 = signature).
struct RsdpLayout {
    uint64_t signature;   // 0
    uint8_t checksum;     // 8
    uint8_t oem_id[6];    // 9
    uint8_t revision;     // 15
    uint32_t rsdt_addr;   // 16
    uint32_t length;      // 20
    uint64_t xsdt_addr;   // 24
    uint8_t ext_checksum; // 32
};

/// @brief Validates the RSDP v1 checksum (sum of bytes 0..19 == 0).
bool rsdp_checksum_ok_v1(uint64_t phys) {
    uint8_t sum = 0;
    for (size_t i = 0; i < 20; ++i)
        sum = static_cast<uint8_t>(sum + phys_read8(phys + i));
    return sum == 0;
}

/// @brief Validates the RSDP v2 extended checksum (sum of bytes 0..length).
bool rsdp_checksum_ok_v2(uint64_t phys, uint32_t length) {
    if (length < 36 || length > 4096)
        return false;
    uint8_t sum = 0;
    for (uint32_t i = 0; i < length; ++i)
        sum = static_cast<uint8_t>(sum + phys_read8(phys + i));
    return sum == 0;
}

/// @brief Locates the RSDP physical address from the multiboot2 ACPI tags.
///        The tag payload (RSDP structure) begins 8 bytes after the generic
///        tag header (type + size).
/// @return RSDP phys, or 0 when absent/invalid.
uint64_t locate_rsdp() {
    // Tag 15 = ACPI new (RSDP v2); tag 14 = ACPI old (RSDP v1).  Prefer v2.
    uint64_t tag15 = mb2_find_tag(15);
    if (tag15 != 0) {
        uint64_t rsdp = tag15 + 8;
        RsdpLayout layout{};
        layout.signature = phys_read64(rsdp);
        layout.checksum = phys_read8(rsdp + 8);
        layout.revision = phys_read8(rsdp + 15);
        layout.length = phys_read32(rsdp + 20);
        if (layout.signature == kRsdpSig &&
            rsdp_checksum_ok_v2(rsdp, layout.length))
            return rsdp;
    }
    uint64_t tag14 = mb2_find_tag(14);
    if (tag14 != 0) {
        uint64_t rsdp = tag14 + 8;
        uint64_t sig = phys_read64(rsdp);
        if (sig == kRsdpSig && rsdp_checksum_ok_v1(rsdp))
            return rsdp;
    }
    return 0;
}

/// @brief Walks the RSDT/XSDT entry list for the @p want signature.
/// @param table_phys  RSDT (32-bit entries) or XSDT (64-bit entries).
/// @param is_xsdt     True for XSDT (8-byte entries).
/// @param want        Signature to find ("DMAR").
/// @return Table phys, or 0 when absent.
uint64_t find_acpi_table(uint64_t table_phys, bool is_xsdt, uint32_t want) {
    uint32_t sig = phys_sig(table_phys);
    uint32_t expect = is_xsdt ? kXsdtSig : kRsdtSig;
    if (sig != expect)
        return 0;
    uint32_t length = phys_read32(table_phys + 4);
    if (length < 36 || length > 4096)
        return 0;
    // Checksum over the whole table header.
    uint8_t sum = 0;
    for (uint32_t i = 0; i < length; ++i)
        sum = static_cast<uint8_t>(sum + phys_read8(table_phys + i));
    if (sum != 0)
        return 0;
    size_t entry_size = is_xsdt ? 8U : 4U;
    uint32_t n = (length - 36) / static_cast<uint32_t>(entry_size);
    for (uint32_t i = 0; i < n; ++i) {
        uint64_t entry = is_xsdt
                             ? phys_read64(table_phys + 36 + i * 8)
                             : phys_read32(table_phys + 36 + i * 4);
        if (entry == 0)
            continue;
        if (phys_sig(entry) == want)
            return entry;
    }
    return 0;
}

/// @brief Parses the DMAR table for the first DRHD remapping unit.
/// @return DmarInfo: found + unit populated, or malformed on bad structure.
dmar::DmarInfo parse_dmar(uint64_t dmar_phys) {
    dmar::DmarInfo out{};
    uint32_t sig = phys_sig(dmar_phys);
    if (sig != kDmarSig)
        return out; // found=false — not a DMAR table
    uint32_t length = phys_read32(dmar_phys + 4);
    if (length < 48 || length > 4096) {
        out.malformed = true;
        return out;
    }
    uint8_t sum = 0;
    for (uint32_t i = 0; i < length; ++i)
        sum = static_cast<uint8_t>(sum + phys_read8(dmar_phys + i));
    if (sum != 0) {
        out.malformed = true;
        return out;
    }
    // Structures begin at offset 48 (36-byte ACPI header + 12-byte DMAR
    // header: host-address-width(1) + flags(1) + reserved(10)).
    uint64_t pos = dmar_phys + 48;
    uint64_t end = dmar_phys + length;
    while (pos + 4 <= end) {
        uint16_t type = phys_read16(pos);
        uint16_t struct_len = phys_read16(pos + 2);
        if (struct_len < 4 || pos + struct_len > end) {
            out.malformed = true;
            return out;
        }
        if (type == 0) { // DRHD — remapping unit
            if (struct_len < 16) {
                out.malformed = true;
                return out;
            }
            uint8_t flags = phys_read8(pos + 4);
            uint16_t segment = phys_read16(pos + 6);
            uint64_t base = phys_read64(pos + 8);
            if (base == 0 || (base & 0xFFFULL) != 0) {
                out.malformed = true;
                return out; // unaligned / zero base — never program it
            }
            out.unit.base_phys = base;
            out.unit.segment = segment;
            out.unit.include_pci_all = (flags & 0x1) != 0;
            out.unit.present = true;
            out.found = true;
            return out;
        }
        pos += struct_len;
    }
    return out; // DMAR valid but no DRHD — found=false
}

} // namespace

dmar::DmarInfo scan_dmar() {
    uint64_t rsdp = locate_rsdp();
    if (rsdp == 0)
        return {};
    // Prefer XSDT (revision 2) for 64-bit table addresses.
    uint8_t revision = phys_read8(rsdp + 15);
    uint64_t dmar = 0;
    if (revision >= 2) {
        uint64_t xsdt = phys_read64(rsdp + 24);
        if (xsdt != 0)
            dmar = find_acpi_table(xsdt, true, kDmarSig);
    }
    if (dmar == 0) {
        uint64_t rsdt = phys_read32(rsdp + 16);
        if (rsdt != 0)
            dmar = find_acpi_table(rsdt, false, kDmarSig);
    }
    if (dmar == 0)
        return {};
    return parse_dmar(dmar);
}

} // namespace kernel::iommu::acpi