/*
 * NexIOS RTOS — Development Roadmap / Kernel Core
 * Copyright (C) 2026 Arnold Hasshold
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3, as
 * published by the Free Software Foundation.
 */

/// @file test_ahci_deep.cpp
/// @brief AHCI deep-path tests (milestone v0.4.3 issue #108).
///        Two layers:
///        - drivers_ahci_deep (in `all`): protocol structure layout and
///          FIS/PRD/NCQ bit-encoding contracts the command path is built
///          from (pure, deterministic).
///        - ahci_live: drives the REAL AhciDriver command path against the
///          QEMU-emulated ICH9-AHCI (q35 variant, `-device ide-hd` on the
///          AHCI bus) — probe/init, raw LBA write/read roundtrip and error
///          paths.  Registered outside `all`; run with
///          `make execute-test x86_64 debug ahci_live`.

#if defined(CONFIG_ARCH_X86_64)
#include <test.hpp>
#include <logger.hpp>
#include <kernel/driver/ahci.hpp>
#include <kernel/driver/ahci_protocol.hpp>
#include <kernel/driver/block_device.hpp>
#include <kernel/memory/mempool.hpp>
#include <string.hpp>

namespace {
/// @brief Destroys a probe()d driver via the driver's own destroy() —
/// explicit destructor + PMM page release (probe() allocates from PMM,
/// never MemPool; see AhciDriver::probe).
void destroy_driver(kernel::block::AhciDriver *drv) {
    kernel::block::AhciDriver::destroy(drv);
}
} // namespace

using namespace kernel;
using namespace kernel::block;

// ============================================================================
// drivers_ahci_deep — protocol layout and encoding contracts (in `all`)
// ============================================================================

// Runmode: kernel
// Testidea: Command-header layout the driver programs (AHCI 1.3.1
// §4.2.2): 32 bytes with CFL+flags in DW0-low at 0, PRDTL in DW0-high
// at 2, 32-bit PRDBC at 4 and the 64-bit CTBA split at 8/12.  A wrong
// PRDTL offset starves DMA (QEMU reads PRDTL from DW0-high and refuses
// zero-length tables); a split PRDBC would corrupt the transfer count.
// Input: offsetof / sizeof on ahci::CmdHeader.
// Expect: Exact offsets and size — HBA DMA reads these fields by offset.
// Depends: ahci_protocol.hpp
JARVIS_TEST(ahci_deep_cmdheader_layout, "PRE: iocd | POST: none") {
    static_assert(sizeof(ahci::CmdHeader) == 32, "CmdHeader must be 32B");
    JARVIS_ASSERT_EQ(static_cast<uint64_t>(0),
                     static_cast<uint64_t>(__builtin_offsetof(ahci::CmdHeader, opts)));
    JARVIS_ASSERT_EQ(static_cast<uint64_t>(2),
                     static_cast<uint64_t>(__builtin_offsetof(ahci::CmdHeader, prdtl)));
    JARVIS_ASSERT_EQ(static_cast<uint64_t>(2),
                     static_cast<uint64_t>(sizeof(ahci::CmdHeader::prdtl)));
    JARVIS_ASSERT_EQ(static_cast<uint64_t>(4),
                     static_cast<uint64_t>(__builtin_offsetof(ahci::CmdHeader, prdbc)));
    JARVIS_ASSERT_EQ(static_cast<uint64_t>(4),
                     static_cast<uint64_t>(sizeof(ahci::CmdHeader::prdbc)));
    JARVIS_ASSERT_EQ(static_cast<uint64_t>(8),
                     static_cast<uint64_t>(__builtin_offsetof(ahci::CmdHeader, ctba)));
    JARVIS_ASSERT_EQ(static_cast<uint64_t>(12),
                     static_cast<uint64_t>(__builtin_offsetof(ahci::CmdHeader, ctbau)));
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Command-table layout: 20-byte command FIS at 0, 16-byte ATAPI
// window, 92 bytes padding, then 256 PRD entries at offset 128 (total
// 4224 bytes = one 2-page command table).
// Input: offsetof / sizeof on ahci::CmdTable and ahci::CmdFIS.
// Expect: sizeof(CmdFIS) == 20; offsetof(prd) == 128; sizeof(CmdTable) ==
//         128 + 256 * 16; AHCI_MAX_PRD == 256.
// Depends: ahci_protocol.hpp
JARVIS_TEST(ahci_deep_cmdtable_layout, "PRE: iocd | POST: none") {
    JARVIS_ASSERT_EQ(static_cast<uint64_t>(20),
                     static_cast<uint64_t>(sizeof(ahci::CmdFIS)));
    JARVIS_ASSERT_EQ(static_cast<uint64_t>(128),
                     static_cast<uint64_t>(__builtin_offsetof(ahci::CmdTable, prd)));
    JARVIS_ASSERT_EQ(static_cast<uint64_t>(128 + 256 * 16),
                     static_cast<uint64_t>(sizeof(ahci::CmdTable)));
    JARVIS_ASSERT_EQ(static_cast<uint64_t>(256),
                     static_cast<uint64_t>(ahci::AHCI_MAX_PRD));
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: PRD byte-count encoding for a single 512-byte sector PRD: the
// driver programs (BLOCK_SIZE - 1) | PRD_IOC — the HBA transfers
// byte_count+1 and raises an interrupt on completion.  Encode/extract
// round-trip via PRD_BYTE_COUNT_MASK.
// Input: ((BLOCK_SIZE - 1) | ahci::PRD_IOC), masked extraction.
// Expect: Encoded value 0x800001FF; masked byte count == 0x1FF (511 =
//         512-1); IOC bit is bit 31; PrdHbaEntry is 16 bytes.
// Depends: ahci_protocol.hpp, BLOCK_SIZE
JARVIS_TEST(ahci_deep_prd_encoding, "PRE: iocd | POST: none") {
    uint32_t encoded = static_cast<uint32_t>((BLOCK_SIZE - 1)) | ahci::PRD_IOC;
    uint32_t count = encoded & ahci::PRD_BYTE_COUNT_MASK;
    JARVIS_ASSERT_EQ(static_cast<uint64_t>(0x800001FF),
                     static_cast<uint64_t>(encoded));
    JARVIS_ASSERT_EQ(static_cast<uint64_t>(0x1FF), static_cast<uint64_t>(count));
    JARVIS_ASSERT_EQ(static_cast<uint64_t>(0x80000000),
                     static_cast<uint64_t>(ahci::PRD_IOC));
    JARVIS_ASSERT_EQ(static_cast<uint64_t>(16),
                     static_cast<uint64_t>(sizeof(ahci::PrdHbaEntry)));
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: NCQ tag placement contract: pm_port_c carries the C bit (7)
// plus the 5-bit tag shifted left by 3 — tags 0 and 31 (extremes) must
// encode without overlapping the command bit or wrapping into bit 6.
// Input: (0x80 | (tag & NCQ_TAG_MASK) << NCQ_TAG_SHIFT) for tags 0..31.
// Expect: Tag 0 → 0x80; tag 31 → 0x80 | 0xF8 = 0xF8 (bit 6 set is part of
//         the 5-bit field, bits 0-2 stay clear, bit 7 = C).
// Depends: ahci_protocol.hpp
JARVIS_TEST(ahci_deep_ncq_tag_encoding, "PRE: iocd | POST: none") {
    uint8_t tag0 = 0x80u | ((0u & ahci::NCQ_TAG_MASK) << ahci::NCQ_TAG_SHIFT);
    uint8_t tag31 =
        0x80u | ((31u & ahci::NCQ_TAG_MASK) << ahci::NCQ_TAG_SHIFT);
    JARVIS_ASSERT_EQ(static_cast<uint64_t>(0x80), static_cast<uint64_t>(tag0));
    JARVIS_ASSERT_EQ(static_cast<uint64_t>(0xF8), static_cast<uint64_t>(tag31));
    for (uint8_t tag = 0; tag < 32; ++tag) {
        uint8_t enc = 0x80u | ((tag & ahci::NCQ_TAG_MASK) << ahci::NCQ_TAG_SHIFT);
        if ((enc & 0x07) != 0) {
            JARVIS_FAIL("ncq tag %u leaks into bits 0-2 (enc=0x%x)", tag, enc);
        }
    }
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Register/FIS constants the wait path and FIS builder rely on:
// H2D FIS type 0x27, LBA device bit 0xE0, TFD status bits, TFES port
// interrupt status, 32 command slots and the FPDMA (NCQ) opcode pair.
// Input: Protocol constants.
// Expect: Documented values (AHCI 1.3.1 spec): FIS_TYPE_REG_H2D 0x27,
//         ATA_DEV_LBA 0xE0, TFD_BSY 0x80, TFD_DRQ 0x40, TFD_ERR 0x100,
//         PORT_IS_TFES (1 << 16), AHCI_MAX_CMDS 32, READ/WRITE
//         FPDMA_QUEUED 0x60/0x61, DMA EXT 0x25/0x35, IDENTIFY 0xEC.
// Depends: ahci_protocol.hpp
JARVIS_TEST(ahci_deep_register_constants, "PRE: iocd | POST: none") {
    JARVIS_ASSERT_EQ(static_cast<uint64_t>(0x27),
                     static_cast<uint64_t>(ahci::FIS_TYPE_REG_H2D));
    JARVIS_ASSERT_EQ(static_cast<uint64_t>(0xE0),
                     static_cast<uint64_t>(ahci::ATA_DEV_LBA));
    JARVIS_ASSERT_EQ(static_cast<uint64_t>(0x80),
                     static_cast<uint64_t>(ahci::TFD_BSY));
    JARVIS_ASSERT_EQ(static_cast<uint64_t>(0x40),
                     static_cast<uint64_t>(ahci::TFD_DRQ));
    JARVIS_ASSERT_EQ(static_cast<uint64_t>(0x100),
                     static_cast<uint64_t>(ahci::TFD_ERR));
    JARVIS_ASSERT_EQ(static_cast<uint64_t>(0x00010000),
                     static_cast<uint64_t>(ahci::PORT_IS_TFES));
    JARVIS_ASSERT_EQ(static_cast<uint64_t>(32),
                     static_cast<uint64_t>(ahci::AHCI_MAX_CMDS));
    JARVIS_ASSERT_EQ(static_cast<uint64_t>(0x60),
                     static_cast<uint64_t>(ahci::ATA_CMD_READ_FPDMA_QUEUED));
    JARVIS_ASSERT_EQ(static_cast<uint64_t>(0x61),
                     static_cast<uint64_t>(ahci::ATA_CMD_WRITE_FPDMA_QUEUED));
    JARVIS_ASSERT_EQ(static_cast<uint64_t>(0x25),
                     static_cast<uint64_t>(ahci::ATA_CMD_READ_DMA_EXT));
    JARVIS_ASSERT_EQ(static_cast<uint64_t>(0x35),
                     static_cast<uint64_t>(ahci::ATA_CMD_WRITE_DMA_EXT));
    JARVIS_ASSERT_EQ(static_cast<uint64_t>(0xEC),
                     static_cast<uint64_t>(ahci::ATA_CMD_IDENTIFY));
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: PORT_SIG device-signature contract the ATAPI skip relies on:
// SATA 0x00000101, ATAPI 0xEB140101 (AHCI 1.3.1 §3.3.8).
// Input: Protocol constants.
// Expect: Documented values; ATAPI != SATA.
// Depends: ahci_protocol.hpp
JARVIS_TEST(ahci_deep_signature_constants, "PRE: iocd | POST: none") {
    JARVIS_ASSERT_EQ(static_cast<uint64_t>(0x00000101),
                     static_cast<uint64_t>(ahci::PORT_SIG_SATA));
    JARVIS_ASSERT_EQ(static_cast<uint64_t>(0xEB140101),
                     static_cast<uint64_t>(ahci::PORT_SIG_ATAPI));
    JARVIS_ASSERT(ahci::PORT_SIG_SATA != ahci::PORT_SIG_ATAPI);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: match_completions basic contract: an armed slot whose CI bit
// cleared completes without error; a still-issued slot does not.
// Input: busy=0 (all retired), armed=bit 3, is=DHRS.
// Expect: done=bit 3, error=0.
// Depends: block::AhciDriver::match_completions
JARVIS_TEST(ahci_deep_compl_match_basic, "PRE: iocd | POST: none") {
    uint32_t done = 0xFFFFFFFF;
    uint32_t err = 0xFFFFFFFF;
    AhciDriver::match_completions(0, 1U << 3, ahci::PORT_IS_DHRS, &done,
                                  &err);
    JARVIS_ASSERT_EQ(static_cast<uint64_t>(1U << 3),
                     static_cast<uint64_t>(done));
    JARVIS_ASSERT_EQ(static_cast<uint64_t>(0), static_cast<uint64_t>(err));
    // Still-issued slot: no completion.
    done = 0xFFFFFFFF;
    err = 0xFFFFFFFF;
    AhciDriver::match_completions(1U << 3, 1U << 3, 0, &done, &err);
    JARVIS_ASSERT_EQ(static_cast<uint64_t>(0), static_cast<uint64_t>(done));
    JARVIS_ASSERT_EQ(static_cast<uint64_t>(0), static_cast<uint64_t>(err));
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: match_completions NCQ contract: busy = CI|SACT snapshot, so
// a slot with SACT set (device-side NCQ activity) is NOT done even when
// its CI bit cleared; both clear means retired.
// Input: armed=bit 5; busy=(1<<5) [SACT only] vs busy=0.
// Expect: SACT-only → done=0; neither → done=bit 5.
// Depends: block::AhciDriver::match_completions
JARVIS_TEST(ahci_deep_compl_match_ncq, "PRE: iocd | POST: none") {
    uint32_t done = 0xFFFFFFFF;
    uint32_t err = 0xFFFFFFFF;
    AhciDriver::match_completions(1U << 5, 1U << 5, ahci::PORT_IS_SDBS,
                                  &done, &err);
    JARVIS_ASSERT_EQ(static_cast<uint64_t>(0), static_cast<uint64_t>(done));
    AhciDriver::match_completions(0, 1U << 5, ahci::PORT_IS_SDBS, &done,
                                  &err);
    JARVIS_ASSERT_EQ(static_cast<uint64_t>(1U << 5),
                     static_cast<uint64_t>(done));
    JARVIS_ASSERT_EQ(static_cast<uint64_t>(0), static_cast<uint64_t>(err));
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: match_completions error contract: TFES marks only the armed
// slots that actually completed; unarmed slots are never reported even
// when their CI bit cleared (the ISR only wakes registered waiters).
// Input: busy=0, armed=bit 1, is=TFES|DHRS.
// Expect: done=bit 1, error=bit 1; armed=0 → done=0, error=0.
// Depends: block::AhciDriver::match_completions
JARVIS_TEST(ahci_deep_compl_match_error, "PRE: iocd | POST: none") {
    uint32_t done = 0xFFFFFFFF;
    uint32_t err = 0xFFFFFFFF;
    AhciDriver::match_completions(0, 1U << 1,
                                  ahci::PORT_IS_TFES | ahci::PORT_IS_DHRS,
                                  &done, &err);
    JARVIS_ASSERT_EQ(static_cast<uint64_t>(1U << 1),
                     static_cast<uint64_t>(done));
    JARVIS_ASSERT_EQ(static_cast<uint64_t>(1U << 1),
                     static_cast<uint64_t>(err));
    // Unarmed: nothing reported despite retired hardware.
    done = 0xFFFFFFFF;
    err = 0xFFFFFFFF;
    AhciDriver::match_completions(0, 0, ahci::PORT_IS_TFES, &done, &err);
    JARVIS_ASSERT_EQ(static_cast<uint64_t>(0), static_cast<uint64_t>(done));
    JARVIS_ASSERT_EQ(static_cast<uint64_t>(0), static_cast<uint64_t>(err));
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: match_completions fail-closed contract: null out-params must
// not crash (ISR-adjacent robustness).
// Input: Null out_done / out_error.
// Expect: Returns without writing (no fault).
// Depends: block::AhciDriver::match_completions
JARVIS_TEST(ahci_deep_compl_match_null, "PRE: iocd | POST: none") {
    uint32_t done = 0;
    AhciDriver::match_completions(0, 1, 0, nullptr, nullptr);
    AhciDriver::match_completions(0, 1, 0, &done, nullptr);
    AhciDriver::match_completions(0, 1, 0, nullptr, &done);
    JARVIS_ASSERT_EQ(static_cast<uint64_t>(0), static_cast<uint64_t>(done));
    JARVIS_TEST_PASS();
}

// ============================================================================
// ahci_live — real command path against the QEMU ICH9-AHCI (variant class)
// ============================================================================

// Runmode: kernel
// Testidea: The q35 variant exposes a real ICH9-AHCI controller with an
// attached disk — AhciDriver::probe() must find it, initialise the port
// DMA engine and report a sane sector count.  (Historical note: a prior
// MemPool-vs-driver-size mismatch gated these tests; probe() now
// allocates from PMM and destroy() mirrors it, so the gate is gone.)
// Input: AhciDriver::probe() on the q35+AHCI machine.
// Expect: Non-null driver; sector_count() > 0; sector_size() == 512;
//         not read-only.
// Depends: block::AhciDriver, QEMU q35 ahci_live variant
JARVIS_TEST(ahci_live_probe_finds_controller, "PRE: iocd | POST: none") {
    AhciDriver *drv = AhciDriver::probe();
    if (drv) {
        uint64_t sectors = drv->sector_count();
        uint64_t ssize = drv->sector_size();
        bool ro = drv->is_read_only();
        destroy_driver(drv);
        JARVIS_ASSERT(sectors > 0);
        JARVIS_ASSERT_EQ(static_cast<uint64_t>(512), ssize);
        JARVIS_ASSERT(!ro);
    }
    JARVIS_ASSERT(drv != nullptr);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Raw LBA write/read roundtrip through the real AHCI command
// path (FIS build, PRD DMA, CI issue, completion poll): write a tagged
// pattern to sector 100, read it back.
// Input: probe(); write_sector(100, pattern); read_sector(100).
// Expect: Both return true and the read-back matches the pattern.
// Depends: block::AhciDriver
JARVIS_TEST(ahci_live_write_read_roundtrip, "PRE: iocd | POST: none") {
    AhciDriver *drv = AhciDriver::probe();
    if (drv) {
        uint8_t wbuf[512];
        uint8_t rbuf[512] = {};
        for (int i = 0; i < 512; ++i)
            wbuf[i] = static_cast<uint8_t>(i ^ 0x3C);
        bool wrote = drv->write_sector(100, wbuf);
        bool read = drv->read_sector(100, rbuf);
        int cmp = memcmp(wbuf, rbuf, 512);
        destroy_driver(drv);
        JARVIS_ASSERT(wrote);
        JARVIS_ASSERT(read);
        JARVIS_ASSERT_EQ(0, cmp);
    }
    JARVIS_ASSERT(drv != nullptr);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Sector isolation on real hardware — writing sector 101 must
// not disturb sector 100 (no off-by-one in the LBA fields of the built
// FIS).
// Input: Write pattern A to sector 100, pattern B to sector 101, read
//        both back.
// Expect: Sector 100 matches A, sector 101 matches B.
// Depends: block::AhciDriver
JARVIS_TEST(ahci_live_sector_isolation, "PRE: iocd | POST: none") {
    AhciDriver *drv = AhciDriver::probe();
    if (drv) {
        uint8_t a[512];
        uint8_t b[512];
        memset(a, 0xAA, sizeof(a));
        memset(b, 0xBB, sizeof(b));
        bool w1 = drv->write_sector(100, a);
        bool w2 = drv->write_sector(101, b);

        uint8_t ra[512] = {};
        uint8_t rb[512] = {};
        bool r1 = drv->read_sector(100, ra);
        bool r2 = drv->read_sector(101, rb);
        int cmp_a = memcmp(a, ra, 512);
        int cmp_b = memcmp(b, rb, 512);
        destroy_driver(drv);
        JARVIS_ASSERT(w1);
        JARVIS_ASSERT(w2);
        JARVIS_ASSERT(r1);
        JARVIS_ASSERT(r2);
        JARVIS_ASSERT_EQ(0, cmp_a);
        JARVIS_ASSERT_EQ(0, cmp_b);
    }
    JARVIS_ASSERT(drv != nullptr);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: IRQ-driven completion (#64): with the MSI ISR armed, a
// write/read roundtrip must complete through ISR-recorded completions
// (isr_completions() > 0), proving wait_cmd took the scheduler-blocked
// path instead of the polling fallback.
// Input: probe(); assert msi_armed(); write/read sector 102.
// Expect: msi_armed() true; roundtrip true; isr_completions() > 0.
// Depends: block::AhciDriver, QEMU q35 ahci_live variant (ICH9 MSI)
JARVIS_TEST(ahci_live_irq_roundtrip, "PRE: iocd | POST: none") {
    AhciDriver *drv = AhciDriver::probe();
    if (drv) {
        bool armed = drv->msi_armed();
        uint8_t wbuf[512];
        uint8_t rbuf[512] = {};
        for (int i = 0; i < 512; ++i)
            wbuf[i] = static_cast<uint8_t>(i ^ 0x5A);
        bool wrote = drv->write_sector(102, wbuf);
        bool read = drv->read_sector(102, rbuf);
        int cmp = memcmp(wbuf, rbuf, 512);
        uint64_t completions = drv->isr_completions();
        destroy_driver(drv);
        JARVIS_ASSERT(armed);
        JARVIS_ASSERT(wrote);
        JARVIS_ASSERT(read);
        JARVIS_ASSERT_EQ(0, cmp);
        JARVIS_ASSERT(completions > 0);
    }
    JARVIS_ASSERT(drv != nullptr);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: IRQ error path (#64): an out-of-range LBA read must fail via
// the ISR TFES recording (not via timeout), preserving the poll path's
// error semantics on the blocked path.
// Input: probe(); read_sector(sector_count() + 1000).
// Expect: Returns false.
// Depends: block::AhciDriver, QEMU q35 ahci_live variant
JARVIS_TEST(ahci_live_irq_error_path, "PRE: iocd | POST: none") {
    AhciDriver *drv = AhciDriver::probe();
    if (drv) {
        uint8_t rbuf[512] = {};
        bool read = drv->read_sector(drv->sector_count() + 1000, rbuf);
        destroy_driver(drv);
        JARVIS_ASSERT(!read);
    }
    JARVIS_ASSERT(drv != nullptr);
    JARVIS_TEST_PASS();
}

void register_ahci_deep_tests() {
    Logger::info("Registering ahci deep tests");
    JARVIS_REGISTER_TEST(ahci_deep_cmdheader_layout);
    JARVIS_REGISTER_TEST(ahci_deep_cmdtable_layout);
    JARVIS_REGISTER_TEST(ahci_deep_prd_encoding);
    JARVIS_REGISTER_TEST(ahci_deep_ncq_tag_encoding);
    JARVIS_REGISTER_TEST(ahci_deep_register_constants);
    JARVIS_REGISTER_TEST(ahci_deep_signature_constants);
    JARVIS_REGISTER_TEST(ahci_deep_compl_match_basic);
    JARVIS_REGISTER_TEST(ahci_deep_compl_match_ncq);
    JARVIS_REGISTER_TEST(ahci_deep_compl_match_error);
    JARVIS_REGISTER_TEST(ahci_deep_compl_match_null);
}

void register_ahci_live_tests() {
    Logger::info("Registering ahci live tests");
    JARVIS_REGISTER_TEST(ahci_live_probe_finds_controller);
    JARVIS_REGISTER_TEST(ahci_live_write_read_roundtrip);
    JARVIS_REGISTER_TEST(ahci_live_sector_isolation);
    JARVIS_REGISTER_TEST(ahci_live_irq_roundtrip);
    JARVIS_REGISTER_TEST(ahci_live_irq_error_path);
}
#endif // CONFIG_ARCH_X86_64
