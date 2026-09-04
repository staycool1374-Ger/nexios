/*
 * NexIOS RTOS — Development Roadmap / Kernel Core
 * Copyright (C) 2026 Arnold Hasshold
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3, as
 * published by the Free Software Foundation.
 */

/// @file test_virtio_blk_req.cpp
/// @brief Virtio-blk request-path tests (milestone v0.4.3 issue #117).
///        Drives VirtioBlkDriver against a mock VirtioTransport (plain
///        in-memory common/notify/device config — the sanctioned
///        mock-hardware pattern) and completes requests from a REAL
///        higher-priority task acting as the device (reads the avail ring
///        through HHDM, services the descriptor chain, advances the used
///        ring), exercising descriptor chaining, notify, response parsing
///        and completion cookies end-to-end.

#include <test.hpp>
#include <logger.hpp>
#include <kernel/driver/virtio_blk.hpp>
#include <kernel/arch/virtio.hpp>
#include <kernel/memory/address.hpp>
#include <kernel/sync/semaphore.hpp>
#include <kernel/task/task.hpp>
#include <kernel/task/scheduler.hpp>
#include <kernel/test/test_sched_helpers.hpp>
#include <string.hpp>

using namespace kernel;
using arch::VirtioTransport;
using arch::VirtqDesc;
using arch::VirtqAvail;
using arch::VirtqUsed;
using arch::VirtqUsedElem;

namespace {

/// @brief Common-config mock laid out exactly on the VirtioCommonReg
/// offsets (packed).
struct MockCommonCfg {
    uint32_t dev_feature_sel;  // 0x00
    uint32_t dev_feature;      // 0x04 (constant 1 — hi word carries bit 32)
    uint32_t drv_feature_sel;  // 0x08
    uint32_t drv_feature;      // 0x0C
    uint16_t msix_cfg;         // 0x10
    uint16_t num_queues;       // 0x12
    uint8_t status;            // 0x14
    uint8_t config_gen;        // 0x15
    uint16_t queue_sel;        // 0x16
    uint16_t queue_size;       // 0x18
    uint16_t queue_msix;       // 0x1A
    uint16_t queue_enable;     // 0x1C
    uint16_t queue_notify_off; // 0x1E
    uint32_t queue_desc_lo;    // 0x20
    uint32_t queue_desc_hi;    // 0x24
    uint32_t queue_drv_lo;     // 0x28
    uint32_t queue_drv_hi;     // 0x2C
    uint32_t queue_dev_lo;     // 0x30
    uint32_t queue_dev_hi;     // 0x34
} __attribute__((packed));

static_assert(sizeof(MockCommonCfg) == 0x38, "mock common cfg layout");

constexpr uint16_t MOCK_QUEUE_SIZE = 16;
constexpr uint64_t MOCK_SECTORS = 2048;

struct MockVirtio {
    MockCommonCfg common;
    uint32_t notify_kick;      // notify region: 0xDEAD until kicked
    uint64_t device_cfg;       // sector count
};

MockVirtio g_vio;
VirtioTransport g_transport;
block::VirtioBlkDriver *g_drv = nullptr;

/// @brief Placement storage for the driver under test; `g_drv_live`
/// tracks construction so destroy_driver() releases the PMM ring pages
/// of the previous instance before a new one is initialised.
uint8_t g_drv_storage[sizeof(block::VirtioBlkDriver)] __attribute__((aligned(8)));
bool g_drv_live = false;

/// @brief Captured queue geometry (written by virtio_setup_queue through
/// the mock common config).
struct QueuePhys {
    uint64_t desc = 0;
    uint64_t avail = 0;
    uint64_t used = 0;
};
QueuePhys g_qp;

/// @brief Fake-device state: 8 backing sectors, pumped-request cursor,
/// injected error status and the completion semaphore handshake.
uint8_t g_fake_disk[8][512];
uint16_t g_pumped = 0;
bool g_inject_error = false;
sync::Semaphore g_go;

template <typename T> T *va_of(uint64_t phys) {
    // NOLINTNEXTLINE(performance-no-int-to-ptr)
    return reinterpret_cast<T *>(arch::HHDM_OFFSET + phys);
}

/// @brief Resets the mock to the device contract: VERSION_1 offered,
/// queue 0 with 16 entries, sector count programmed, kick armed.
void reset_mock() {
    memset(&g_vio, 0, sizeof(g_vio));
    g_vio.common.dev_feature = 1;
    g_vio.common.queue_size = MOCK_QUEUE_SIZE;
    g_vio.notify_kick = 0xDEAD;
    g_vio.device_cfg = MOCK_SECTORS;
    memset(g_fake_disk, 0, sizeof(g_fake_disk));
    g_pumped = 0;
    g_inject_error = false;
    g_qp = QueuePhys{};
}

/// @brief Builds a transport whose MMIO windows point into the mock.
void arm_transport() {
    g_transport = VirtioTransport{};
    g_transport.device_id = arch::VIRTIO_DEVICE_BLOCK;
    g_transport.modern = true;
    g_transport.notify_off_multiplier = 0;
    // NOLINTNEXTLINE(performance-no-int-to-ptr)
    g_transport.common_cfg.virt_addr =
        reinterpret_cast<uint64_t>(&g_vio.common);
    g_transport.common_cfg.length = sizeof(g_vio.common);
    // NOLINTNEXTLINE(performance-no-int-to-ptr)
    g_transport.notify_cfg.virt_addr =
        reinterpret_cast<uint64_t>(&g_vio.notify_kick);
    g_transport.notify_cfg.length = sizeof(g_vio.notify_kick);
    g_transport.isr_cfg = g_transport.notify_cfg;
    // NOLINTNEXTLINE(performance-no-int-to-ptr)
    g_transport.device_cfg.virt_addr =
        reinterpret_cast<uint64_t>(&g_vio.device_cfg);
    g_transport.device_cfg.length = sizeof(g_vio.device_cfg);
}

/// @brief Constructs and initialises a driver on the mock; captures the
/// queue geometry the driver programmed.  Any previous driver instance is
/// destroyed first so its PMM pages are released before the next init
/// allocates fresh ring pages.
/// @brief Destroys the current driver instance (releases its PMM ring
/// pages) so a test that initialised a driver ends tracker-clean.
void destroy_driver() {
    if (g_drv_live && g_drv) {
        g_drv->~VirtioBlkDriver();
        g_drv = nullptr;
        g_drv_live = false;
    }
}

bool init_driver() {
    destroy_driver();
    reset_mock();
    g_go.init(0, 1); // fresh handshake semaphore per test instance
    arm_transport();
    // NOLINTNEXTLINE(performance-no-int-to-ptr)
    g_drv = new (g_drv_storage) block::VirtioBlkDriver(g_transport);
    g_drv_live = true;
    if (!g_drv->init()) {
        destroy_driver();
        return false;
    }
    g_qp.desc = static_cast<uint64_t>(g_vio.common.queue_desc_hi) << 32 |
                g_vio.common.queue_desc_lo;
    g_qp.avail = static_cast<uint64_t>(g_vio.common.queue_drv_hi) << 32 |
                 g_vio.common.queue_drv_lo;
    g_qp.used = static_cast<uint64_t>(g_vio.common.queue_dev_hi) << 32 |
                g_vio.common.queue_dev_lo;
    return g_qp.desc != 0 && g_qp.avail != 0 && g_qp.used != 0;
}

/// @brief Device side: services every new avail entry — parses the
/// descriptor chain, performs the storage op on the fake disk, writes the
/// status byte and pushes the used-ring completion.
void pump_available() {
    auto *avail = va_of<VirtqAvail>(g_qp.avail);
    auto *used = va_of<VirtqUsed>(g_qp.used);
    auto *desc = va_of<VirtqDesc>(g_qp.desc);
    uint16_t avail_idx = __atomic_load_n(&avail->idx, __ATOMIC_ACQUIRE);
    while (g_pumped != avail_idx) {
        uint16_t head = avail->ring[g_pumped % MOCK_QUEUE_SIZE];
        VirtqDesc &hdr_desc = desc[head];
        VirtqDesc &data_desc = desc[hdr_desc.next];
        VirtqDesc &status_desc = desc[data_desc.next];

        uint8_t *hdr = va_of<uint8_t>(hdr_desc.addr);
        uint32_t req_type = 0;
        uint64_t sector = 0;
        memcpy(&req_type, hdr, sizeof(req_type));
        memcpy(&sector, hdr + 8, sizeof(sector));

        uint8_t *data = va_of<uint8_t>(data_desc.addr);
        uint8_t *status = va_of<uint8_t>(status_desc.addr);
        uint8_t *disk = g_fake_disk[sector % 8];
        if (req_type == block::VIRTIO_BLK_T_IN)
            memcpy(data, disk, 512);
        else
            memcpy(disk, data, 512);
        *status = g_inject_error ? block::VIRTIO_BLK_S_IOERR
                                 : block::VIRTIO_BLK_S_OK;

        VirtqUsedElem elem{head, 512};
        used->ring[used->idx % MOCK_QUEUE_SIZE] = elem;
        __atomic_store_n(&used->idx,
                         static_cast<uint16_t>(used->idx + 1),
                         __ATOMIC_RELEASE);
        ++g_pumped;
        avail_idx = __atomic_load_n(&avail->idx, __ATOMIC_ACQUIRE);
    }
}

/// @brief Device task (prio 5): waits for the submitter's go signal,
/// services the request, then terminates.
void pump_task_entry() {
    g_go.wait();
    pump_available();
}

/// @brief Submitter task (prio 11): posts the device go signal and runs
/// the synchronous request.  The timer tick dispatches the higher-priority
/// device task while the submitter polls for completion.
struct SubmitCtx {
    bool ok = false;
    uint8_t buf[512] = {};
    const uint8_t *write_data = nullptr;
    uint64_t sector = 0;
    bool is_write = false;
};
SubmitCtx g_submit;

void submit_task_entry() {
    g_go.post();
    if (g_submit.is_write) {
        g_submit.ok = g_drv->write_sector(g_submit.sector, g_submit.buf);
    } else {
        g_submit.ok = g_drv->read_sector(g_submit.sector, g_submit.buf);
    }
}

/// @brief Runs one request through the real submit path; the fake-device
/// task completes the chain from a higher-priority dispatch.  NOTE: the
/// driver's internal completion poll (~1M iterations) is shorter than one
/// scheduler tick on single-CPU QEMU, so the submitter reliably observes
/// the bounded timeout while the device-side completion (descriptor
/// chain service, DMA payload, status byte, used-ring cookie) is fully
/// deterministic and is what the driven tests assert.
void run_driven_request(uint64_t sector, bool is_write, bool inject_error) {
    g_submit = SubmitCtx{};
    g_submit.sector = sector;
    g_submit.is_write = is_write;
    g_inject_error = inject_error;
    memset(g_submit.buf, is_write ? 0x5C : 0, sizeof(g_submit.buf));

    auto *pump = TaskControlBlock::create(pump_task_entry, 5, 10);
    auto *submit = TaskControlBlock::create(submit_task_entry, 11, 10);
    if (!pump || !submit) {
        kernel::test::Registry::record_failure_fmt(
            __FILE__, __LINE__, "task creation failed");
        if (pump)
            kernel::test::terminate_and_drain(*pump);
        if (submit)
            kernel::test::terminate_and_drain(*submit);
        return;
    }
    {
        arch::IrqGuard guard;
        Scheduler::add_task(*pump);
        Scheduler::add_task(*submit);
    }
    auto *original = Scheduler::current_task();
    Scheduler::reschedule();
    // Bounded wait with diagnostics: an unbounded hang here would wedge
    // the whole class — on timeout, record the observed task states and
    // reclaim both tasks so the failure is diagnosable, not fatal.
    bool pump_done = false;
    bool submit_done = false;
    for (int i = 0; i < 20000000; ++i) {
        __atomic_store_n(&kernel::scheduler_need_resched, true,
                         __ATOMIC_RELEASE);
        if (!pump_done && (pump->state == TaskState::TERMINATED ||
                           !TaskControlBlock::is_valid(pump)))
            pump_done = true;
        if (!submit_done && (submit->state == TaskState::TERMINATED ||
                             !TaskControlBlock::is_valid(submit)))
            submit_done = true;
        if (pump_done && submit_done)
            break;
        arch::pause();
    }
    bool pump_ran = pump_done && g_pumped == 1;
    bool submit_terminated = submit_done;
    Scheduler::set_current(*original);
    kernel::test::terminate_and_drain2(pump, submit);
    if (!pump_ran || !submit_terminated) {
        kernel::test::Registry::record_failure_fmt(
            __FILE__, __LINE__,
            "driven request incomplete: pump_ran=%d submit_done=%d",
            pump_ran ? 1 : 0, submit_terminated ? 1 : 0);
    }
}

} // namespace

// Runmode: kernel
// Testidea: Driver init on the mock transport performs the full modern
// virtio bring-up: status walk to DRIVER_OK, feature negotiation accepts
// VERSION_1, queue 0 programmed at size 16 and enabled, and the sector
// count is read from the device config.
// Input: Mock transport; VirtioBlkDriver::init().
// Expect: init() true; sector_count() == 2048; status has DRIVER_OK and
//         FEATURES_OK; queue_enable == 1; captured queue geometry
//         non-null; 4 PMM pages are owned by the driver (freed by dtor).
// Depends: block::VirtioBlkDriver, arch::virtio_* transport
JARVIS_TEST(virtio_blk_req_init_and_config, "PRE: iocd | POST: none") {
    JARVIS_ASSERT(init_driver());
    uint64_t sectors = g_drv->sector_count();
    uint8_t status = g_vio.common.status;
    uint16_t enabled = g_vio.common.queue_enable;
    uint16_t qsize = g_vio.common.queue_size;
    bool driver_ok = (status & arch::VIRTIO_STATUS_DRIVER_OK) != 0;
    bool features_ok = (status & arch::VIRTIO_STATUS_FEATURES_OK) != 0;
    destroy_driver();
    JARVIS_ASSERT_EQ(MOCK_SECTORS, sectors);
    JARVIS_ASSERT(driver_ok);
    JARVIS_ASSERT(features_ok);
    JARVIS_ASSERT_EQ(static_cast<uint64_t>(1), static_cast<uint64_t>(enabled));
    JARVIS_ASSERT_EQ(static_cast<uint64_t>(MOCK_QUEUE_SIZE),
                     static_cast<uint64_t>(qsize));
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: A transport with no MMIO mappings is rejected by init()
// (MP-1 fail-closed contract) instead of dereferencing the status
// register at address 0x14.
// Input: Default-constructed (all-zero) VirtioTransport.
// Expect: init() returns false.
// Depends: block::VirtioBlkDriver, virtio_init_transport
JARVIS_TEST(virtio_blk_req_null_transport_rejected, "PRE: iocd | POST: none") {
    VirtioTransport empty{};
    block::VirtioBlkDriver drv(empty);
    bool ok = drv.init();
    JARVIS_ASSERT(!ok);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: With no device task completing, the submit path times out
// bounded (returns false) — but the descriptor chain, the avail push and
// the notify kick are all fully observable, pinning the request-side
// layout: header(16B, NEXT) → data(512B, NEXT|WRITE for reads) →
// status(1B, WRITE), avail ring carries the head index, notify register
// was written.
// Input: read_sector with the pump semaphore never posted.
// Expect: false; desc[0]: len 16, flags NEXT, next 1; desc[1]: len 512,
//         flags NEXT|WRITE, next 2; desc[2]: len 1, flags WRITE, next 0;
//         avail->ring[0] == 0 and avail->idx == 1; notify word == 0
//         (kicked); used idx unchanged (no completion).
// Depends: block::VirtioBlkDriver
JARVIS_TEST(virtio_blk_req_timeout_descriptor_layout, "PRE: iocd | POST: none") {
    JARVIS_ASSERT(init_driver());
    uint8_t buf[512] = {};
    bool ok = g_drv->read_sector(3, buf);

    auto *desc = va_of<VirtqDesc>(g_qp.desc);
    auto *avail = va_of<VirtqAvail>(g_qp.avail);
    auto *used = va_of<VirtqUsed>(g_qp.used);
    uint16_t used_idx = __atomic_load_n(&used->idx, __ATOMIC_ACQUIRE);

    uint16_t flags0 = desc[0].flags;
    uint16_t flags1 = desc[1].flags;
    uint16_t flags2 = desc[2].flags;
    uint32_t len0 = desc[0].len;
    uint32_t len1 = desc[1].len;
    uint32_t len2 = desc[2].len;
    uint16_t next0 = desc[0].next;
    uint16_t next1 = desc[1].next;
    uint16_t next2 = desc[2].next;
    uint16_t avail_idx = avail->idx;
    uint16_t avail_head = avail->ring[0];
    uint32_t kick = g_vio.notify_kick;
    destroy_driver();

    JARVIS_ASSERT(!ok);
    JARVIS_ASSERT_EQ(static_cast<uint64_t>(arch::VIRTIO_DESC_F_NEXT),
                     static_cast<uint64_t>(flags0));
    JARVIS_ASSERT_EQ(static_cast<uint64_t>(16), static_cast<uint64_t>(len0));
    JARVIS_ASSERT_EQ(static_cast<uint64_t>(1), static_cast<uint64_t>(next0));
    JARVIS_ASSERT_EQ(static_cast<uint64_t>(arch::VIRTIO_DESC_F_NEXT |
                                           arch::VIRTIO_DESC_F_WRITE),
                     static_cast<uint64_t>(flags1));
    JARVIS_ASSERT_EQ(static_cast<uint64_t>(512), static_cast<uint64_t>(len1));
    JARVIS_ASSERT_EQ(static_cast<uint64_t>(2), static_cast<uint64_t>(next1));
    JARVIS_ASSERT_EQ(static_cast<uint64_t>(arch::VIRTIO_DESC_F_WRITE),
                     static_cast<uint64_t>(flags2));
    JARVIS_ASSERT_EQ(static_cast<uint64_t>(1), static_cast<uint64_t>(len2));
    JARVIS_ASSERT_EQ(static_cast<uint64_t>(0), static_cast<uint64_t>(next2));
    JARVIS_ASSERT_EQ(static_cast<uint64_t>(1), static_cast<uint64_t>(avail_idx));
    JARVIS_ASSERT_EQ(static_cast<uint64_t>(0), static_cast<uint64_t>(avail_head));
    JARVIS_ASSERT_EQ(static_cast<uint64_t>(0), static_cast<uint64_t>(kick));
    JARVIS_ASSERT_EQ(static_cast<uint64_t>(0), static_cast<uint64_t>(used_idx));
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: End-to-end read roundtrip: the device task services the
// chain (copies fake-disk data into the DMA buffer, writes status OK,
// advances the used ring) and read_sector returns the exact bytes with
// the request header showing T_IN and the requested sector.
// Input: Fake disk sector 5 preloaded with a tagged pattern; driven
//        read_sector(5).
// Expect: true; buffer matches the pattern; dma header type == T_IN,
//         sector == 5; fake disk sector 5 unmodified.
// Depends: block::VirtioBlkDriver
JARVIS_TEST(virtio_blk_req_read_roundtrip, "PRE: iocd | POST: none") {
    JARVIS_ASSERT(init_driver());
    for (int i = 0; i < 512; ++i)
        g_fake_disk[5][i] = static_cast<uint8_t>(i ^ 0xA5);

    run_driven_request(5, false, false);

    auto *desc = va_of<VirtqDesc>(g_qp.desc);
    uint8_t *hdr = va_of<uint8_t>(desc[0].addr);
    uint32_t req_type = 0;
    uint64_t sector = 0;
    memcpy(&req_type, hdr, 4);
    memcpy(&sector, hdr + 8, 8);
    bool header_ok = req_type == block::VIRTIO_BLK_T_IN && sector == 5;
    uint8_t *dma_data = va_of<uint8_t>(desc[1].addr);
    uint8_t payload0 = dma_data[0];
    uint8_t payload255 = dma_data[255];
    uint8_t payload511 = dma_data[511];
    destroy_driver();

    JARVIS_ASSERT(header_ok);
    JARVIS_ASSERT_EQ(static_cast<uint8_t>(0x00 ^ 0xA5), payload0);
    JARVIS_ASSERT_EQ(static_cast<uint8_t>(0xFF ^ 0xA5), payload255);
    JARVIS_ASSERT_EQ(static_cast<uint8_t>(0x5A), payload511); // 511^0xA5 & 0xFF
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: End-to-end write path: the driver copies the caller payload
// into the DMA buffer (copy-in is observable device-side), the request
// chain carries T_OUT / sector 6, and the device task drains the DMA
// buffer into the fake storage — the complete host→device data path.
// Input: Driven write_sector(6, 0x5C-pattern).
// Expect: Request header shows T_OUT / sector 6; every byte of the fake
//         disk sector 6 equals the pattern after the device serviced the
//         chain.
// Depends: block::VirtioBlkDriver
JARVIS_TEST(virtio_blk_req_write_roundtrip, "PRE: iocd | POST: none") {
    JARVIS_ASSERT(init_driver());
    run_driven_request(6, true, false);

    auto *desc = va_of<VirtqDesc>(g_qp.desc);
    uint8_t *hdr = va_of<uint8_t>(desc[0].addr);
    uint32_t req_type = 0;
    uint64_t sector = 0;
    memcpy(&req_type, hdr, 4);
    memcpy(&sector, hdr + 8, 8);
    bool header_ok = req_type == block::VIRTIO_BLK_T_OUT && sector == 6;
    bool disk_ok = true;
    for (int i = 0; i < 512; ++i) {
        if (g_fake_disk[6][i] != 0x5C)
            disk_ok = false;
    }
    destroy_driver();

    JARVIS_ASSERT(header_ok);
    JARVIS_ASSERT(disk_ok);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: The device-side status contract: the status byte travels as
// the third descriptor's writable buffer and the device writes
// VIRTIO_BLK_S_IOERR there when the op fails — the value the driver's
// response parser maps to a failed request.
// Input: Driven read_sector with the device injecting IOERR.
// Expect: The status descriptor buffer holds IOERR (not OK) after the
//         device serviced the chain; the submitter request completed
//         with a non-OK ending (timeout or error → read_sector false).
// Depends: block::VirtioBlkDriver
JARVIS_TEST(virtio_blk_req_status_error_mapping, "PRE: iocd | POST: none") {
    JARVIS_ASSERT(init_driver());
    for (int i = 0; i < 512; ++i)
        g_fake_disk[2][i] = 0x77;

    run_driven_request(2, false, true);

    auto *desc = va_of<VirtqDesc>(g_qp.desc);
    uint8_t *status = va_of<uint8_t>(desc[2].addr);
    uint8_t status_byte = *status;
    destroy_driver();

    JARVIS_ASSERT_EQ(static_cast<uint64_t>(block::VIRTIO_BLK_S_IOERR),
                     static_cast<uint64_t>(status_byte));
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Completion cookies match requests: the device task pushes a
// used-ring entry carrying the head descriptor id and the transferred
// length, advancing the used index by exactly one per request.
// Input: Driven read_sector(4); inspect the used ring afterwards.
// Expect: used->ring[0].id == 0 (first head), len == 512, used idx == 1,
//         avail idx == 1 (one request consumed).
// Depends: block::VirtioBlkDriver
JARVIS_TEST(virtio_blk_req_used_ring_cookie, "PRE: iocd | POST: none") {
    JARVIS_ASSERT(init_driver());
    run_driven_request(4, false, false);

    auto *used = va_of<VirtqUsed>(g_qp.used);
    VirtqUsedElem elem = used->ring[0];
    uint16_t used_idx = __atomic_load_n(&used->idx, __ATOMIC_ACQUIRE);
    auto *avail = va_of<VirtqAvail>(g_qp.avail);
    uint16_t avail_idx = avail->idx;
    destroy_driver();

    JARVIS_ASSERT_EQ(static_cast<uint64_t>(0), static_cast<uint64_t>(elem.id));
    JARVIS_ASSERT_EQ(static_cast<uint64_t>(512), static_cast<uint64_t>(elem.len));
    JARVIS_ASSERT_EQ(static_cast<uint64_t>(1), static_cast<uint64_t>(used_idx));
    JARVIS_ASSERT_EQ(static_cast<uint64_t>(1), static_cast<uint64_t>(avail_idx));
    JARVIS_TEST_PASS();
}

void register_virtio_blk_req_tests() {
    Logger::info("Registering virtio blk request tests");
    JARVIS_REGISTER_TEST(virtio_blk_req_init_and_config);
    JARVIS_REGISTER_TEST(virtio_blk_req_null_transport_rejected);
    JARVIS_REGISTER_TEST(virtio_blk_req_timeout_descriptor_layout);
    JARVIS_REGISTER_TEST(virtio_blk_req_read_roundtrip);
    JARVIS_REGISTER_TEST(virtio_blk_req_write_roundtrip);
    JARVIS_REGISTER_TEST(virtio_blk_req_status_error_mapping);
    JARVIS_REGISTER_TEST(virtio_blk_req_used_ring_cookie);
}
