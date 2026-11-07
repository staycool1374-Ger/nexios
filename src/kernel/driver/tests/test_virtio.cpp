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

/// @file test_virtio.cpp
/// @brief VirtIO device driver tests.

#if defined(CONFIG_ARCH_X86_64)

#include <test.hpp>
#include <logger.hpp>
#include <kernel/arch/virtio.hpp>
#include <kernel/driver/virtio_blk.hpp>
#include <kernel/driver/virtio_net.hpp>
#include <kernel/memory/pmm.hpp>
#include <kernel/test/resource_tracker.hpp>

using namespace kernel;

// Runmode: kernel
// Testidea: Verifies that no Virtio block device is found on default QEMU
// (which only has IDE/SATA, no virtio-blk). Ensures virtio_find_device
// returns false gracefully.
// Input: arch::virtio_find_device(VIRTIO_DEVICE_BLOCK, transport)
// Expect: Returns false (no virtio device)
// Depends: arch::pci_scan_all, arch::virtio_find_device
JARVIS_TEST(virtio_no_device_found, "PRE: iocd | POST: none") {
    arch::VirtioTransport transport;
    bool found = arch::virtio_find_device(arch::VIRTIO_DEVICE_BLOCK, transport);
    JARVIS_ASSERT(!found);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Verifies virtio_is_virtio_device returns false for non-virtio
// devices like the host bridge.
// Input: arch::pci_read_device_info for 00:00.0, then check
// Expect: virtio_is_virtio_device returns false
// Depends: arch::pci_read_device_info, arch::virtio_is_virtio_device
JARVIS_TEST(virtio_not_virtio_device, "PRE: iocd | POST: none") {
    arch::PciBdf bdf = {0, 0, 0};
    arch::PciDeviceInfo info = arch::pci_read_device_info(bdf);
    JARVIS_ASSERT(!arch::virtio_is_virtio_device(info));
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Verifies virtio probe returns nullptr when no virtio-blk device
// is present.
// Input: kernel::block::VirtioBlkDriver::probe()
// Expect: Returns nullptr
// Depends: arch::pci_scan_all, arch::virtio_find_device
JARVIS_TEST(virtio_blk_probe_no_device, "PRE: iocd | POST: none") {
    auto *drv = kernel::block::VirtioBlkDriver::probe();
    JARVIS_ASSERT(drv == nullptr);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Verifies virtio_map_mmio returns false for invalid BAR.
// Input: BAR=6 (out of range), invalid BDF
// Expect: Returns false
// Depends: arch::virtio_map_mmio
JARVIS_TEST(virtio_map_mmio_invalid, "PRE: iocd | POST: none") {
    arch::VirtioMmio mmio;
    arch::PciBdf bdf = {0, 0, 0};
    bool ok = arch::virtio_map_mmio(6, 0, 4096, mmio, bdf);
    JARVIS_ASSERT(!ok);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: v0.4.0 MP-1 — virtio_init_transport rejects a transport with no
// MMIO mapping (null virt_addr) instead of dereferencing address 0x14.
// CHANGED (v0.4.0 MP-8): pre-MP-1 the boot identity map in the kernel PML4
// masked the null-deref; private kernel-half PML4s no longer carry it, so
// the hardened function must return false.
// Input: Default-constructed VirtioTransport
// Expect: Returns false (no MMIO mapping — rejected, no write to 0x14)
// Depends: arch::virtio_init_transport
JARVIS_TEST(virtio_init_transport_noop, "PRE: iocd | POST: none") {
    arch::VirtioTransport t = {};
    bool ok = arch::virtio_init_transport(t);
    JARVIS_ASSERT(!ok);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Verifies a virtio-net device can be reset via the status register.
// Finds the virtio-net device, writes RESET status, confirms device
// acknowledges. Input: arch::virtio_find_device(VIRTIO_DEVICE_NET),
// virtio_write_status(RESET) Expect: Device status reads back as RESET Depends:
// arch::virtio_find_device, arch::virtio_write/read_status
JARVIS_TEST(virtio_device_reset, "PRE: iocd | POST: none") {
    arch::VirtioTransport transport;
    bool found = arch::virtio_find_device(arch::VIRTIO_DEVICE_NET, transport);
    JARVIS_ASSERT(found);
    arch::virtio_write_status(transport, arch::VIRTIO_STATUS_RESET);
    uint8_t status = arch::virtio_read_status(transport);
    JARVIS_ASSERT_EQ(arch::VIRTIO_STATUS_RESET, status);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Verifies feature negotiation between host and guest succeeds
// on the virtio-net device. Offers VIRTIO_F_VERSION_1 and checks
// FEATURES_OK is set after negotiation.
// Input: Initialize transport, negotiate features with VIRTIO_F_VERSION_1
// Expect: Feature negotiation succeeds (virtio_negotiate_features returns true)
// Depends: arch::virtio_find_device, arch::virtio_init_transport,
//          arch::virtio_negotiate_features
JARVIS_TEST(virtio_feature_negotiation, "PRE: iocd | POST: none") {
    arch::VirtioTransport transport;
    bool found = arch::virtio_find_device(arch::VIRTIO_DEVICE_NET, transport);
    JARVIS_ASSERT(found);
    arch::virtio_init_transport(transport);
    bool ok =
        arch::virtio_negotiate_features(transport, arch::VIRTIO_F_VERSION_1);
    JARVIS_ASSERT(ok);
    arch::virtio_write_status(transport, arch::VIRTIO_STATUS_RESET);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Verifies a virtqueue can be configured on the virtio-net device.
// Sets up descriptor, available, and used rings with properly aligned
// physical addresses and verifies the device accepts the queue.
// Input: Initialize transport, allocate queue memory, call virtio_setup_queue
// Expect: virtio_setup_queue returns true
// Depends: arch::virtio_find_device, arch::virtio_init_transport,
//          arch::virtio_setup_queue, PMM
JARVIS_TEST(virtio_queue_configuration, "PRE: iocd | POST: none") {
    arch::VirtioTransport transport;
    bool found = arch::virtio_find_device(arch::VIRTIO_DEVICE_NET, transport);
    JARVIS_ASSERT(found);
    arch::virtio_init_transport(transport);
    arch::virtio_negotiate_features(transport, arch::VIRTIO_F_VERSION_1);
    // Allocate physically contiguous pages for descriptor, avail, and used
    // rings
    constexpr uint16_t QSIZE = 256;
    uint64_t desc_phys = PMM::alloc_contiguous(
        (QSIZE * sizeof(arch::VirtqDesc) + arch::PAGE_SIZE - 1) /
        arch::PAGE_SIZE);
    JARVIS_ASSERT(desc_phys != 0);
    uint64_t avail_phys = PMM::alloc_contiguous(
        (sizeof(arch::VirtqAvail) + QSIZE * 2 + arch::PAGE_SIZE - 1) /
        arch::PAGE_SIZE);
    JARVIS_ASSERT(avail_phys != 0);
    uint64_t used_phys = PMM::alloc_contiguous(
        (sizeof(arch::VirtqUsed) + QSIZE * sizeof(arch::VirtqUsedElem) +
         arch::PAGE_SIZE - 1) /
        arch::PAGE_SIZE);
    JARVIS_ASSERT(used_phys != 0);
    bool ok = arch::virtio_setup_queue(transport, 0, QSIZE, desc_phys,
                                       avail_phys, used_phys);
    JARVIS_ASSERT(ok);
    arch::virtio_write_status(transport, arch::VIRTIO_STATUS_RESET);
    PMM::free_page(desc_phys);
    PMM::free_page(avail_phys);
    PMM::free_page(used_phys);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Verifies the guest can notify the virtio device via MMIO write
// after a queue is configured.
// Input: Initialize transport, set up queue, call virtio_notify(queue_idx=0)
// Expect: No crash, MMIO write completes (no return value to assert, just
//         verifies the notify path doesn't fault)
// Depends: arch::virtio_find_device, arch::virtio_init_transport,
//          arch::virtio_setup_queue, arch::virtio_notify
JARVIS_TEST(virtio_guest_notify, "PRE: iocd | POST: none") {
    arch::VirtioTransport transport;
    bool found = arch::virtio_find_device(arch::VIRTIO_DEVICE_NET, transport);
    JARVIS_ASSERT(found);
    arch::virtio_init_transport(transport);
    arch::virtio_negotiate_features(transport, arch::VIRTIO_F_VERSION_1);
    constexpr uint16_t QSIZE = 64;
    uint64_t desc_phys = PMM::alloc_contiguous(
        (QSIZE * sizeof(arch::VirtqDesc) + arch::PAGE_SIZE - 1) /
        arch::PAGE_SIZE);
    JARVIS_ASSERT(desc_phys != 0);
    uint64_t avail_phys = PMM::alloc_contiguous(
        (sizeof(arch::VirtqAvail) + QSIZE * 2 + arch::PAGE_SIZE - 1) /
        arch::PAGE_SIZE);
    JARVIS_ASSERT(avail_phys != 0);
    uint64_t used_phys = PMM::alloc_contiguous(
        (sizeof(arch::VirtqUsed) + QSIZE * sizeof(arch::VirtqUsedElem) +
         arch::PAGE_SIZE - 1) /
        arch::PAGE_SIZE);
    JARVIS_ASSERT(used_phys != 0);
    arch::virtio_setup_queue(transport, 0, QSIZE, desc_phys, avail_phys,
                             used_phys);
    arch::virtio_notify(transport, 0);
    arch::virtio_write_status(transport, arch::VIRTIO_STATUS_RESET);
    PMM::free_page(desc_phys);
    PMM::free_page(avail_phys);
    PMM::free_page(used_phys);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: FLAW-03 — probing the virtio-net device, exercising the locked
//           RX poll path, then tearing down with zero ResourceTracker delta.
// Input: virtio_net_probe(nic); virtio_net_poll (no frames -> false);
//        virtio_net_destroy(); write VIRTIO_STATUS_RESET
// Expect: probe true; poll false (no frames); teardown clean
// Depends: kernel::net::virtio_net_probe/poll/destroy, arch::virtio
JARVIS_TEST(virtio_net_probe_poll_and_teardown, "PRE: iocd | POST: none") {
    auto &rt = kernel::test::ResourceTracker::instance();
    kernel::test::ResourceCounters before{};
    rt.capture(before);

    ::net::Nic nic;
    bool ok = kernel::net::virtio_net_probe(nic);
    JARVIS_ASSERT(ok);
    uint8_t buf[::net::MAX_PACKET_SIZE];
    size_t len = 0;
    bool got = kernel::net::virtio_net_poll(buf, len);
    JARVIS_ASSERT(!got); // no frames queued
    kernel::net::virtio_net_destroy();

    // Reset the device status so subsequent tests start clean.
    arch::VirtioTransport transport;
    if (arch::virtio_find_device(arch::VIRTIO_DEVICE_NET, transport)) {
        arch::virtio_write_status(transport, arch::VIRTIO_STATUS_RESET);
    }

    kernel::test::ResourceCounters after{};
    rt.capture(after);
    JARVIS_ASSERT_EQ(before.pmm_pages_used, after.pmm_pages_used);
    JARVIS_ASSERT_EQ(before.cap_objects, after.cap_objects);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: FLAW-03 — the TX path programs the descriptor under the lock,
//           notifies the device, and the bounded completion poll succeeds.
// Input: probe; nic.send_frame(frame, 64) twice; poll (no RX frames);
//        destroy; reset status
// Expect: both sends true; second send not rejected by tx_inflight_ after
//         the first completes; teardown clean
// Depends: kernel::net::virtio_net_probe/poll/destroy, net::Nic
JARVIS_TEST(virtio_net_send_frame_completes, "PRE: iocd | POST: none") {
    auto &rt = kernel::test::ResourceTracker::instance();
    kernel::test::ResourceCounters before{};
    rt.capture(before);

    ::net::Nic nic;
    bool ok = kernel::net::virtio_net_probe(nic);
    JARVIS_ASSERT(ok);

    uint8_t frame[64];
    for (size_t i = 0; i < sizeof(frame); ++i)
        frame[i] = static_cast<uint8_t>(i & 0xFF);
    bool sent1 = nic.send_frame(frame, sizeof(frame));
    JARVIS_ASSERT(sent1);
    // The completion poll must have consumed the descriptor, so a second
    // send is not rejected by tx_inflight_.
    bool sent2 = nic.send_frame(frame, sizeof(frame));
    JARVIS_ASSERT(sent2);

    uint8_t buf[::net::MAX_PACKET_SIZE];
    size_t len = 0;
    JARVIS_ASSERT(!kernel::net::virtio_net_poll(buf, len));
    kernel::net::virtio_net_destroy();

    arch::VirtioTransport transport;
    if (arch::virtio_find_device(arch::VIRTIO_DEVICE_NET, transport)) {
        arch::virtio_write_status(transport, arch::VIRTIO_STATUS_RESET);
    }

    kernel::test::ResourceCounters after{};
    rt.capture(after);
    JARVIS_ASSERT_EQ(before.pmm_pages_used, after.pmm_pages_used);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Registers all Virtio tests with the test framework.
// Input: None
// Expect: All Virtio tests are registered (no direct assertion, only logging)
// Depends: Test framework, Logger, arch::virtio
void register_virtio_tests() {
    Logger::info("Registering Virtio tests");
    JARVIS_REGISTER_TEST(virtio_no_device_found);
    JARVIS_REGISTER_TEST(virtio_not_virtio_device);
    JARVIS_REGISTER_TEST(virtio_blk_probe_no_device);
    JARVIS_REGISTER_TEST(virtio_map_mmio_invalid);
    JARVIS_REGISTER_TEST(virtio_init_transport_noop);
    JARVIS_REGISTER_TEST(virtio_device_reset);
    JARVIS_REGISTER_TEST(virtio_feature_negotiation);
    JARVIS_REGISTER_TEST(virtio_queue_configuration);
    JARVIS_REGISTER_TEST(virtio_guest_notify);
    JARVIS_REGISTER_TEST(virtio_net_probe_poll_and_teardown);
    JARVIS_REGISTER_TEST(virtio_net_send_frame_completes);
}

#endif // CONFIG_ARCH_X86_64
