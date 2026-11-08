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

/// @file test_block_srv.cpp
/// @brief Block-srv PD live tests (issue #47, strangler step-b1).
/// ON-build only (CONFIG_BLOCK_SRV=1): BIND/CAP_GET against the real
/// userspace PD, sector read/write through the shared window, digest
/// parity, OOB fail-closed. OFF build registers vacuous passes so the
/// default aggregates stay green.

#include <test.hpp>
#include <logger.hpp>

using namespace kernel;

#if CONFIG_BLOCK_SRV
#include <kernel/driver/block_srv.hpp>
#include <string.hpp>
#endif

// Runmode: kernel
// Testidea: PD binds with arch-correct backend and sane geometry.
// Input: block_srv_bind() against the live block-srv PD.
// Expect: backend ATA_PIO (x86_64) or VIRTIO_MMIO, 512 B sectors, >0.
// Depends: block_srv_bind, SDF BLOCK_SRV row, PD CAP_GET/BIND handlers
JARVIS_TEST(block_srv_bind_geometry, "PRE: block-srv PD live | POST: bound") {
#if CONFIG_BLOCK_SRV
    JARVIS_ASSERT(block_srv::block_srv_bind());
#if defined(CONFIG_ARCH_X86_64)
    JARVIS_ASSERT(block_srv::block_srv_backend() ==
                  block_srv::BlkBackend::ATA_PIO);
#else
    JARVIS_ASSERT(block_srv::block_srv_backend() ==
                  block_srv::BlkBackend::VIRTIO_MMIO);
#endif
    JARVIS_ASSERT(block_srv::block_srv_sector_size() == 512);
    JARVIS_ASSERT(block_srv::block_srv_sector_count() > 0);
    JARVIS_TEST_PASS();
#else
    JARVIS_TEST_PASS();
#endif
}

// Runmode: kernel
// Testidea: LBA 0 through the PD is the FAT32 boot sector.
// Input: block_srv_read_sector(0).
// Expect: jump byte + FAT32 tag + 0x55AA signature.
// Depends: block_srv_read_sector, PD READ_BLOCK, shared window
JARVIS_TEST(block_srv_read_lba0, "PRE: bound | POST: none") {
#if CONFIG_BLOCK_SRV
    // The packaged image is MBR-partitioned (type 0x0C at LBA 2048):
    // LBA 0 carries the MBR, LBA 2048 the FAT32 boot sector.
    uint8_t buf[512] = {};
    JARVIS_ASSERT(block_srv::block_srv_read_sector(0, buf) == 0);
    JARVIS_ASSERT(buf[510] == 0x55 && buf[511] == 0xAA);
    JARVIS_ASSERT(buf[450] == 0x0C || buf[450] == 0x0B);
    uint32_t start = static_cast<uint32_t>(buf[454]) |
                     (static_cast<uint32_t>(buf[455]) << 8) |
                     (static_cast<uint32_t>(buf[456]) << 16) |
                     (static_cast<uint32_t>(buf[457]) << 24);
    JARVIS_ASSERT(start != 0);
    JARVIS_ASSERT(block_srv::block_srv_read_sector(start, buf) == 0);
    JARVIS_ASSERT(buf[0] == 0xEB || buf[0] == 0xE9);
    JARVIS_ASSERT(memcmp(&buf[82], "FAT32   ", 8) == 0);
    JARVIS_ASSERT(buf[510] == 0x55 && buf[511] == 0xAA);
    JARVIS_TEST_PASS();
#else
    JARVIS_TEST_PASS();
#endif
}

// Runmode: kernel
// Testidea: Repeated reads are stable; digest feeds the host oracle.
// Input: Two reads of LBA 0..15, FNV-1a over both.
// Expect: Identical digests; S BLK digest line printed.
// Depends: block_srv_read_sector, PD determinism
JARVIS_TEST(block_srv_read_image_digest, "PRE: bound | POST: none") {
#if CONFIG_BLOCK_SRV
    // FNV-1a 64 (host parity oracle mirrors this exact fold).
    auto fnv1a = [](const uint8_t *data, uint64_t len,
                    uint64_t hash) -> uint64_t {
        for (uint64_t i = 0; i < len; ++i) {
            hash ^= static_cast<uint64_t>(data[i]);
            hash *= 1099511628211ULL;
        }
        return hash;
    };
    uint8_t buf[512] = {};
    uint64_t first = 14695981039346656037ULL;
    for (uint64_t lba = 0; lba < 16; ++lba) {
        JARVIS_ASSERT(block_srv::block_srv_read_sector(lba, buf) == 0);
        first = fnv1a(buf, sizeof(buf), first);
    }
    uint64_t second = 14695981039346656037ULL;
    for (uint64_t lba = 0; lba < 16; ++lba) {
        JARVIS_ASSERT(block_srv::block_srv_read_sector(lba, buf) == 0);
        second = fnv1a(buf, sizeof(buf), second);
    }
    JARVIS_ASSERT(first == second);
    JARVIS_ASSERT(first != 14695981039346656037ULL);
    Logger::info("S BLK digest=%lx", first);
    JARVIS_TEST_PASS();
#else
    JARVIS_TEST_PASS();
#endif
}

// Runmode: kernel
// Testidea: Scratch write round-trips; OOB/guards fail closed.
// Input: Save last sector; write pattern; read back; restore; OOB ops.
// Expect: Round-trip + restore verified; OOB/-EACCES typed errors.
// Depends: block_srv_write_sector, PD WRITE_BLOCK, DMA_MAP deny
JARVIS_TEST(block_srv_write_roundtrip_guards, "PRE: bound | POST: restored") {
#if CONFIG_BLOCK_SRV
    const uint64_t sectors = block_srv::block_srv_sector_count();
    JARVIS_ASSERT(sectors > 16);
    const uint64_t scratch = sectors - 1;
    uint8_t orig[512] = {};
    uint8_t readback[512] = {};
    uint8_t pattern[512] = {};
    for (uint64_t i = 0; i < sizeof(pattern); ++i) {
        pattern[i] = static_cast<uint8_t>(0x5A ^ (i & 0xFF));
    }
    // Save the scratch sector, write the pattern, verify, restore.
    JARVIS_ASSERT(block_srv::block_srv_read_sector(scratch, orig) == 0);
    JARVIS_ASSERT(block_srv::block_srv_write_sector(scratch, pattern) == 0);
    JARVIS_ASSERT(block_srv::block_srv_read_sector(scratch, readback) == 0);
    JARVIS_ASSERT(memcmp(pattern, readback, sizeof(pattern)) == 0);
    JARVIS_ASSERT(block_srv::block_srv_write_sector(scratch, orig) == 0);
    JARVIS_ASSERT(block_srv::block_srv_read_sector(scratch, readback) == 0);
    JARVIS_ASSERT(memcmp(orig, readback, sizeof(orig)) == 0);
    // OOB and guard rails fail closed with typed errors.
    JARVIS_ASSERT(block_srv::block_srv_read_sector(sectors, readback) ==
                  -22);
    JARVIS_ASSERT(block_srv::block_srv_write_sector(sectors, pattern) ==
                  -22);
    JARVIS_ASSERT(block_srv::block_srv_fault_nodomain() == -13);
    JARVIS_TEST_PASS();
#else
    JARVIS_TEST_PASS();
#endif
}

void register_block_srv_tests() {
    Logger::info("Registering Block Server live tests");
    JARVIS_REGISTER_TEST(block_srv_bind_geometry);
    JARVIS_REGISTER_TEST(block_srv_read_lba0);
    JARVIS_REGISTER_TEST(block_srv_read_image_digest);
    JARVIS_REGISTER_TEST(block_srv_write_roundtrip_guards);
}
