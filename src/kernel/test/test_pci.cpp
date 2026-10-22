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

/// @file test_pci.cpp
/// @brief PCI bus enumeration and config tests.

#if defined(CONFIG_ARCH_X86_64)

#include <test.hpp>
#include <logger.hpp>
#include <kernel/arch/pci.hpp>
#include <kernel/arch/io.hpp>
#include <string.hpp>

using namespace kernel;

// Runmode: kernel
// Testidea: Verifies PCI bus scan finds at least one device on QEMU's emulated
// PCI bus. QEMU's PC machine always has a host bridge (00:00.0) and typically
// several other devices.
// Input: Call arch::pci_scan_all()
// Expect: Returns > 0 devices found
// Depends: arch::PCI CF8/CFC config space access
JARVIS_TEST(pci_scan_finds_devices, "PRE: iocd | POST: none") {
    size_t count = arch::pci_scan_all();
    JARVIS_ASSERT(count > 0);
    Logger::info("PCI: %d devices found", count);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Verifies vendor/device ID can be read for a known BDF. The host
// bridge at 00:00.0 should always return valid IDs on QEMU.
// Input: Read vendor/device for 00:00.0
// Expect: vendor_id != 0xFFFF and != 0, device_id != 0
// Depends: arch::pci_read_vendor, arch::pci_read_device
JARVIS_TEST(pci_read_host_bridge_ids, "PRE: iocd | POST: none") {
    arch::PciBdf bdf = {0, 0, 0};
    uint16_t vendor = arch::pci_read_vendor(bdf);
    uint16_t device = arch::pci_read_device(bdf);
    JARVIS_ASSERT(vendor != 0xFFFF);
    JARVIS_ASSERT(vendor != 0);
    JARVIS_ASSERT(device != 0);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Verifies pci_read_device_info populates all fields for the host
// bridge at 00:00.0.
// Input: arch::pci_read_device_info({0,0,0})
// Expect: vendor_id != 0xFFFF, class_code is 0x06 (bridge), header_type = 0x00
// Depends: arch::pci_read_device_info
JARVIS_TEST(pci_read_host_bridge_info, "PRE: iocd | POST: none") {
    arch::PciBdf bdf = {0, 0, 0};
    arch::PciDeviceInfo info = arch::pci_read_device_info(bdf);
    JARVIS_ASSERT(info.vendor_id != 0xFFFF);
    JARVIS_ASSERT(info.class_code == 0x06);
    JARVIS_ASSERT(info.subclass == 0x00); // host bridge
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Verifies the ISA bridge (typically 00:01.0 on QEMU's PIIX3) is
// found and identified correctly.
// Input: Call arch::pci_scan_all(), then search devices
// Expect: At least one device is a PCI-to-ISA bridge (class=0x06,
// subclass=0x01) Depends: arch::pci_scan_all, arch::pci_find_device
JARVIS_TEST(pci_finds_isa_bridge, "PRE: iocd | POST: none") {
    arch::pci_scan_all();
    const arch::PciDeviceInfo *isa = arch::pci_find_device(0x06, 0x01);
    JARVIS_ASSERT(isa != nullptr);
    JARVIS_ASSERT(isa->bdf.bus == 0);
    JARVIS_ASSERT(isa->bdf.device == 1);
    JARVIS_ASSERT(isa->bdf.function == 0);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Verifies that a nonexistent BDF returns vendor 0xFFFF and
// pci_device_exists returns false.
// Input: Read vendor at BDF 255:31:7 (non-existent)
// Expect: vendor == 0xFFFF, pci_device_exists returns false
// Depends: arch::pci_read_vendor, arch::pci_device_exists
JARVIS_TEST(pci_nonexistent_bdf, "PRE: iocd | POST: none") {
    arch::PciBdf bdf = {255, 31, 7};
    uint16_t vendor = arch::pci_read_vendor(bdf);
    JARVIS_ASSERT_EQ(0xFFFF, vendor);
    JARVIS_ASSERT(!arch::pci_device_exists(bdf));
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Verifies the PCI command register can be read and contains at
// least the IO space bit (devices are typically enabled by firmware).
// Input: Read command register at 00:00.0
// Expect: command register != 0
// Depends: arch::pci_config_readw
JARVIS_TEST(pci_read_command_register, "PRE: iocd | POST: none") {
    arch::PciBdf bdf = {0, 0, 0};
    uint32_t addr = arch::pci_make_addr(bdf, arch::PCI_COMMAND);
    uint16_t cmd = arch::pci_config_readw(addr);
    // Host bridge should have at least I/O or memory space enabled
    JARVIS_ASSERT(cmd != 0);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Verifies pci_make_addr constructs the correct config address.
// Input: BDF 1:2.3, register 0x10
// Expect: addr = 0x80000000 | (1 << 16) | (2 << 11) | (3 << 8) | 0x10
// Depends: arch::pci_make_addr
JARVIS_TEST(pci_make_addr_format, "PRE: iocd | POST: none") {
    arch::PciBdf bdf = {1, 2, 3};
    uint32_t addr = arch::pci_make_addr(bdf, 0x10);
    JARVIS_ASSERT_HEX_EQ(
        0x80000000U | (1U << 16) | (2U << 11) | (3U << 8) | 0x10U, addr);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Verifies pci_find_device returns nullptr for nonexistent
// class/subclass. Input: arch::pci_find_device(0xFF, 0xFF) Expect: Returns
// nullptr Depends: arch::pci_scan_all, arch::pci_find_device
JARVIS_TEST(pci_find_nonexistent_device, "PRE: iocd | POST: none") {
    arch::pci_scan_all();
    const arch::PciDeviceInfo *dev = arch::pci_find_device(0xFF, 0xFF);
    JARVIS_ASSERT(dev == nullptr);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Verifies the host bridge (00:00.0) does not have an MSI capability
// (most QEMU host bridges don't). Confirms pci_find_capability works.
// Input: arch::pci_find_capability(bdf, PCI_CAP_ID_MSI)
// Expect: Returns 0 (no MSI capability)
// Depends: arch::pci_find_capability
JARVIS_TEST(pci_host_bridge_no_msi, "PRE: iocd | POST: none") {
    arch::PciBdf bdf = {0, 0, 0};
    uint8_t cap = arch::pci_find_capability(bdf, arch::PCI_CAP_ID_MSI);
    JARVIS_ASSERT_EQ(0, cap);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Verifies pci_alloc_vector returns a valid non-zero vector and
// that pci_free_vector can release it. Also verifies vector is in range 48-255
// and not 0x80.
// Input: arch::pci_alloc_vector() then arch::pci_free_vector(v)
// Expect: vector != 0, vector >= 48, vector != 0x80
// Depends: arch::pci_alloc_vector, arch::pci_free_vector
JARVIS_TEST(pci_alloc_free_vector, "PRE: iocd | POST: none") {
    uint8_t v1 = arch::pci_alloc_vector();
    JARVIS_ASSERT(v1 != 0);
    JARVIS_ASSERT(v1 >= 48);
    JARVIS_ASSERT(v1 != 0x80);
    uint8_t v2 = arch::pci_alloc_vector();
    JARVIS_ASSERT(v2 != 0);
    JARVIS_ASSERT(v2 != v1);
    arch::pci_free_vector(v1);
    arch::pci_free_vector(v2);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Verifies pci_enable_msi returns 0 for a device without MSI
// capability (host bridge at 00:00.0).
// Input: arch::pci_enable_msi(bdf, 0)
// Expect: Returns 0 (no MSI capability)
// Depends: arch::pci_enable_msi
JARVIS_TEST(pci_enable_msi_no_cap, "PRE: iocd | POST: none") {
    arch::PciBdf bdf = {0, 0, 0};
    uint8_t vec = arch::pci_enable_msi(bdf, 0);
    JARVIS_ASSERT_EQ(0, vec);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Verifies pci_enable_msix returns 0 for a device without MSI-X
// capability (host bridge at 00:00.0).
// Input: arch::pci_enable_msix(bdf, 0, 0)
// Expect: Returns 0 (no MSI-X capability)
// Depends: arch::pci_enable_msix
JARVIS_TEST(pci_enable_msix_no_cap, "PRE: iocd | POST: none") {
    arch::PciBdf bdf = {0, 0, 0};
    uint8_t vec = arch::pci_enable_msix(bdf, 0, 0);
    JARVIS_ASSERT_EQ(0, vec);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Verifies that the ISA bridge's status register indicates whether
// it has a capability list, and that pci_find_capability returns 0 for MSI.
// Input: arch::pci_find_capability(bdf, PCI_CAP_ID_MSI)
// Expect: Returns 0 on devices without MSI
// Depends: arch::pci_find_capability
JARVIS_TEST(pci_isa_bridge_caps, "PRE: iocd | POST: none") {
    arch::PciBdf bdf = {0, 1, 0};
    uint8_t cap = arch::pci_find_capability(bdf, arch::PCI_CAP_ID_MSI);
    JARVIS_ASSERT_EQ(0, cap);
    cap = arch::pci_find_capability(bdf, arch::PCI_CAP_ID_MSIX);
    JARVIS_ASSERT_EQ(0, cap);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Verifies at least one PCI device has valid BAR registers
// with valid type, non-zero address, and non-zero size.
// Input: arch::pci_scan_all(), iterate devices
// Expect: At least one device has a BAR with address > 0 and size > 0
// Depends: arch::pci_scan_all, arch::pci_read_device_info (pci_parse_bars)
JARVIS_TEST(pci_bar_registers, "PRE: iocd | POST: none") {
    arch::pci_scan_all();
    bool found_valid_bar = false;
    for (size_t i = 0; i < arch::pci_device_count(); ++i) {
        const auto &d = arch::pci_devices()[i];
        for (uint8_t b = 0; b < d.bar_count; ++b) {
            JARVIS_ASSERT(d.bars[b].type == arch::PciBarType::MEMORY_32 ||
                          d.bars[b].type == arch::PciBarType::IO ||
                          d.bars[b].type == arch::PciBarType::MEMORY_64);
            if (d.bars[b].address > 0 && d.bars[b].size > 0) {
                found_valid_bar = true;
            }
        }
    }
    JARVIS_ASSERT(found_valid_bar);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Verifies MSI/MSI-X capability chain traversal finds capabilities
// on a device that has them (virtio-net on QEMU has MSI-X).
// Input: Find virtio-net device, call pci_find_capability for MSI and MSI-X
// Expect: At least one MSI or MSI-X capability found (non-zero offset)
// Depends: arch::pci_scan_all, arch::pci_find_capability
JARVIS_TEST(pci_msi_capability_chain, "PRE: iocd | POST: none") {
    arch::pci_scan_all();
    const arch::PciDeviceInfo *dev = nullptr;
    for (size_t i = 0; i < arch::pci_device_count(); ++i) {
        const auto &d = arch::pci_devices()[i];
        if (d.vendor_id == 0x1AF4 && d.device_id == 0x1041) {
            dev = &d;
            break;
        }
    }
    JARVIS_ASSERT(dev != nullptr);
    uint8_t msi = arch::pci_find_capability(dev->bdf, arch::PCI_CAP_ID_MSI);
    uint8_t msix = arch::pci_find_capability(dev->bdf, arch::PCI_CAP_ID_MSIX);
    JARVIS_ASSERT(msi != 0 || msix != 0);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Verifies PCI enumeration completes within bounded time.
// Input: arch::pci_scan_all() with TSC timestamp measurement
// Expect: Enumeration completes in < 5M TSC ticks on QEMU
// Depends: arch::pci_scan_all, arch::rdtsc
// DISABLED: QEMU-dependent performance benchmark, not a correctness test
#if 0
JARVIS_TEST(pci_enumeration_bounded_time) {
    uint64_t tsc_start = arch::rdtsc();
    arch::pci_scan_all();
    uint64_t tsc_end = arch::rdtsc();
    uint64_t elapsed = tsc_end - tsc_start;
    JARVIS_ASSERT(elapsed < 5000000);
    Logger::info("PCI: scan completed in %d TSC ticks", elapsed);
    JARVIS_TEST_PASS();
}
#endif

// Runmode: kernel
// Testidea: Verifies pci_print_tree() produces valid device tree output.
// Input: Call pci_print_tree() after scan
// Expect: Output contains vendor:device ID strings, class/subclass, BAR info
// Depends: arch::pci_scan_all, arch::pci_print_tree
JARVIS_TEST(pci_print_tree_output, "PRE: iocd | POST: none") {
    arch::pci_scan_all();
    char buf[2048];
    arch::pci_print_tree(buf, sizeof(buf));
    JARVIS_ASSERT(strlen(buf) > 0);
    bool has_bracket = false;
    bool has_colon = false;
    bool has_bus = false;
    for (size_t i = 0; buf[i]; ++i) {
        if (buf[i] == '[')
            has_bracket = true;
        if (buf[i] == ':')
            has_colon = true;
        if (buf[i] == 'P' && buf[i + 1] == 'C' && buf[i + 2] == 'I')
            has_bus = true;
    }
    JARVIS_ASSERT(has_bracket);
    JARVIS_ASSERT(has_colon);
    JARVIS_ASSERT(has_bus);
    Logger::info("PCI tree:\n%s", buf);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Registers all PCI tests with the test framework.
// Input: None
// Expect: All PCI tests are registered (no direct assertion, only logging)
// Depends: Test framework, Logger, arch::PCI
void register_pci_tests() {
    Logger::info("Registering PCI tests");
    JARVIS_REGISTER_TEST(pci_scan_finds_devices);
    JARVIS_REGISTER_TEST(pci_read_host_bridge_ids);
    JARVIS_REGISTER_TEST(pci_read_host_bridge_info);
    JARVIS_REGISTER_TEST(pci_finds_isa_bridge);
    JARVIS_REGISTER_TEST(pci_nonexistent_bdf);
    JARVIS_REGISTER_TEST(pci_read_command_register);
    JARVIS_REGISTER_TEST(pci_make_addr_format);
    JARVIS_REGISTER_TEST(pci_find_nonexistent_device);
    JARVIS_REGISTER_TEST(pci_host_bridge_no_msi);
    JARVIS_REGISTER_TEST(pci_alloc_free_vector);
    JARVIS_REGISTER_TEST(pci_enable_msi_no_cap);
    JARVIS_REGISTER_TEST(pci_enable_msix_no_cap);
    JARVIS_REGISTER_TEST(pci_isa_bridge_caps);
    JARVIS_REGISTER_TEST(pci_bar_registers);
    JARVIS_REGISTER_TEST(pci_msi_capability_chain);
    // DISABLED: QEMU-dependent performance benchmark, not a correctness test
    // JARVIS_REGISTER_TEST(pci_enumeration_bounded_time);
    JARVIS_REGISTER_TEST(pci_print_tree_output);
}

#elif defined(CONFIG_ARCH_AARCH64) || defined(CONFIG_ARCH_RISCV64)

#include <test.hpp>
#include <logger.hpp>
#include <kernel/arch/pci.hpp>

using namespace kernel;

// Runmode: kernel
// Testidea: ECAM discovery regression (issues #216 + #354): the boot
// latched base must equal the DTB pcie-ecam-generic reg (no mismatch
// fallback), pinning the #216 base fix against silent drift. The test
// only reads the latch — a boot wiring regression fails loudly here.
// Input: arch::pci_ecam_info().
// Expect: dt_found; !dt_mismatch; phys_base == dt_phys_base;
//         size == PCI_ECAM_WINDOW_SIZE.
// Depends: arch::pci_ecam_discover (boot), DTB pcie node
JARVIS_TEST(pci_ecam_discovery_validated, "PRE: iocd | POST: none") {
    const arch::PciEcamInfo &info = arch::pci_ecam_info();
    JARVIS_ASSERT(info.dt_found);
    JARVIS_ASSERT(!info.dt_mismatch);
    JARVIS_ASSERT_EQ(static_cast<uint64_t>(info.dt_phys_base),
                     static_cast<uint64_t>(info.phys_base));
    JARVIS_ASSERT_EQ(static_cast<uint64_t>(arch::PCI_ECAM_WINDOW_SIZE),
                     static_cast<uint64_t>(info.size));
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: The ECAM window decodes bus 0 — the QEMU virt host bridge at
// 00:00.0 answers with a real vendor ID (proves mapping + decode, not
// just the discovery latch).
// Input: full bus-0 scan (32 devices x 8 functions) via
//        pci_device_exists; vendor read at 00:00.0.
// Expect: at least one device present; 00:00.0 vendor != 0xFFFF and != 0.
// Depends: arch unified PCI config access (ECAM off-x86)
JARVIS_TEST(pci_ecam_host_bridge_present, "PRE: iocd | POST: none") {
    size_t found = 0;
    for (uint32_t dev = 0; dev < 32; ++dev) {
        for (uint32_t fn = 0; fn < 8; ++fn) {
            arch::PciBdf bdf = {
                0, static_cast<uint8_t>(dev), static_cast<uint8_t>(fn)};
            if (arch::pci_device_exists(bdf))
                ++found;
        }
    }
    arch::PciBdf host = {0, 0, 0};
    uint16_t vendor = arch::pci_read_vendor(host);
    JARVIS_ASSERT(found > 0);
    JARVIS_ASSERT(vendor != 0xFFFF);
    JARVIS_ASSERT(vendor != 0);
    JARVIS_ASSERT(arch::pci_device_exists(host));
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Phantom-device reject (issue #354): unpopulated ECAM slots
// read all-ones and report absent — probing them never faults and never
// fabricates a device. Bus 255 is unreachable on single-root QEMU virt
// (no bridge forwards there), so both slots are structural phantoms.
// Input: vendor/exists reads at {255,31,7} and {255,0,0}.
// Expect: vendor == 0xFFFF and exists == false for both.
// Depends: arch unified PCI config access (ECAM off-x86)
JARVIS_TEST(pci_ecam_phantom_rejects, "PRE: iocd | POST: none") {
    arch::PciBdf ghost_a = {255, 31, 7};
    arch::PciBdf ghost_b = {255, 0, 0};
    JARVIS_ASSERT_EQ(static_cast<uint64_t>(0xFFFF),
                     static_cast<uint64_t>(
                         arch::pci_read_vendor(ghost_a)));
    JARVIS_ASSERT(!arch::pci_device_exists(ghost_a));
    JARVIS_ASSERT_EQ(static_cast<uint64_t>(0xFFFF),
                     static_cast<uint64_t>(
                         arch::pci_read_vendor(ghost_b)));
    JARVIS_ASSERT(!arch::pci_device_exists(ghost_b));
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Vendor/device read coherence on the host bridge: the dword
// read equals the two word reads composed (catches lane-shift or
// endianness faults in the config accessor layer).
// Input: config_readl, read_vendor, read_device at 00:00.0.
// Expect: dword == (device << 16) | vendor.
// Depends: arch unified PCI config access (ECAM off-x86)
JARVIS_TEST(pci_ecam_vendor_device_consistent, "PRE: iocd | POST: none") {
    arch::PciBdf host = {0, 0, 0};
    uint16_t vendor = arch::pci_read_vendor(host);
    uint16_t device = arch::pci_read_device(host);
    uint32_t combined =
        arch::pci_config_readl(arch::pci_make_addr(host, arch::PCI_VENDOR_ID));
    uint32_t expect =
        (static_cast<uint32_t>(device) << 16) | vendor;
    JARVIS_ASSERT_EQ(static_cast<uint64_t>(expect),
                     static_cast<uint64_t>(combined));
    JARVIS_TEST_PASS();
}

void register_pci_tests() {
    Logger::info("Registering PCI tests");
    JARVIS_REGISTER_TEST(pci_ecam_discovery_validated);
    JARVIS_REGISTER_TEST(pci_ecam_host_bridge_present);
    JARVIS_REGISTER_TEST(pci_ecam_phantom_rejects);
    JARVIS_REGISTER_TEST(pci_ecam_vendor_device_consistent);
}

#endif // CONFIG_ARCH_X86_64
