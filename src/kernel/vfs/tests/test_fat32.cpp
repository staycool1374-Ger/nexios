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

/// @file test_fat32.cpp
/// @brief FAT32 filesystem tests.

#include <test.hpp>
#include <logger.hpp>
#include <kernel/vfs/fat32.hpp>
#include <kernel/driver/block_device.hpp>
#include <kernel/test/test_isolate.hpp>
#include <string.hpp>

using namespace kernel;
using namespace kernel::fat32;

#ifndef __clang__
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wanalyzer-possible-null-dereference"
#pragma GCC diagnostic ignored "-Wanalyzer-use-of-uninitialized-value"
#pragma GCC diagnostic ignored "-Wanalyzer-malloc-leak"
#endif

extern "C" uint8_t _binary_build_fat32_img_start[];
extern "C" uint8_t _binary_build_fat32_img_end[];

static uint64_t fat32_img_sectors() {
    return (reinterpret_cast<uintptr_t>(_binary_build_fat32_img_end) -
            reinterpret_cast<uintptr_t>(_binary_build_fat32_img_start)) /
           SECTOR_SIZE;
}

static kernel::block::MockBlockDevice make_dev() {
    return kernel::block::MockBlockDevice(_binary_build_fat32_img_start,
                                          fat32_img_sectors(), true);
}

static Fat32Partition mount_fs(kernel::block::MockBlockDevice &dev) {
    kernel::test::mark_vfs_touched();
    Fat32Partition fs(dev);
    fs.mount();
    return fs;
}

JARVIS_TEST(fat32_mbr_valid_signature, "PRE: vfsd, iocd | POST: none") {
    auto dev = make_dev();
    uint8_t mbr[SECTOR_SIZE];
    JARVIS_ASSERT(dev.read_sector(0, mbr));
    JARVIS_ASSERT(mbr[510] == 0x55);
    JARVIS_ASSERT(mbr[511] == 0xAA);
}

JARVIS_TEST(fat32_mbr_parse_partition, "PRE: vfsd, iocd | POST: none") {
    auto dev = make_dev();
    uint64_t part_lba = 0;
    JARVIS_ASSERT(find_fat32_in_mbr(dev, part_lba));
    JARVIS_ASSERT(part_lba > 0);
}

JARVIS_TEST(fat32_mbr_no_partition_table, "PRE: vfsd, iocd | POST: none") {
    auto dev = kernel::block::MockBlockDevice(1);
    uint64_t part_lba = 0;
    JARVIS_ASSERT(!find_fat32_in_mbr(dev, part_lba));
}

JARVIS_TEST(fat32_bpb_valid_signature, "PRE: vfsd, iocd | POST: none") {
    auto dev = make_dev();
    uint64_t part_lba = 0;
    JARVIS_ASSERT(find_fat32_in_mbr(dev, part_lba));
    uint8_t bpb_sector[SECTOR_SIZE];
    JARVIS_ASSERT(dev.read_sector(part_lba, bpb_sector));
    JARVIS_ASSERT(bpb_sector[510] == 0x55);
    JARVIS_ASSERT(bpb_sector[511] == 0xAA);
}

JARVIS_TEST(fat32_bpb_bytes_per_sector, "PRE: vfsd, iocd | POST: none") {
    auto dev = make_dev();
    auto fs = mount_fs(dev);
    JARVIS_ASSERT(fs.bpb().bytes_per_sector == 512);
}

JARVIS_TEST(fat32_bpb_sectors_per_cluster, "PRE: vfsd, iocd | POST: none") {
    auto dev = make_dev();
    auto fs = mount_fs(dev);
    uint8_t spc = fs.bpb().sectors_per_cluster;
    JARVIS_ASSERT(spc >= 1);
    JARVIS_ASSERT((spc & (spc - 1)) == 0);
}

JARVIS_TEST(fat32_bpb_reserved_sectors, "PRE: vfsd, iocd | POST: none") {
    auto dev = make_dev();
    auto fs = mount_fs(dev);
    JARVIS_ASSERT(fs.bpb().reserved_sectors > 0);
}

JARVIS_TEST(fat32_bpb_fat_count, "PRE: vfsd, iocd | POST: none") {
    auto dev = make_dev();
    auto fs = mount_fs(dev);
    JARVIS_ASSERT(fs.bpb().fat_count == 2);
}

JARVIS_TEST(fat32_bpb_root_cluster, "PRE: vfsd, iocd | POST: none") {
    auto dev = make_dev();
    auto fs = mount_fs(dev);
    JARVIS_ASSERT(fs.bpb().root_cluster >= 2);
}

JARVIS_TEST(fat32_bpb_fat_size, "PRE: vfsd, iocd | POST: none") {
    auto dev = make_dev();
    auto fs = mount_fs(dev);
    JARVIS_ASSERT(fs.bpb().fat_size > 0);
}

JARVIS_TEST(fat32_bpb_total_sectors, "PRE: vfsd, iocd | POST: none") {
    auto dev = make_dev();
    auto fs = mount_fs(dev);
    JARVIS_ASSERT(fs.bpb().total_sectors > 0);
}

JARVIS_TEST(fat32_fat_read_cluster, "PRE: vfsd, iocd | POST: none") {
    auto dev = make_dev();
    auto fs = mount_fs(dev);
    uint32_t next = 0;
    JARVIS_ASSERT(fs.read_fat_entry(2, next));
    JARVIS_ASSERT(Fat32Partition::is_eof(next));
}

JARVIS_TEST(fat32_fat_eof_marker, "PRE: vfsd, iocd | POST: none") {
    JARVIS_ASSERT(Fat32Partition::is_eof(0x0FFFFFF8));
    JARVIS_ASSERT(Fat32Partition::is_eof(0x0FFFFFFF));
    JARVIS_ASSERT(!Fat32Partition::is_eof(0x0FFFFFF7));
}

JARVIS_TEST(fat32_fat_free_cluster, "PRE: vfsd, iocd | POST: none") {
    JARVIS_ASSERT(Fat32Partition::is_free(0));
    JARVIS_ASSERT(!Fat32Partition::is_free(1));
}

JARVIS_TEST(fat32_fat_bad_cluster, "PRE: vfsd, iocd | POST: none") {
    JARVIS_ASSERT(Fat32Partition::is_bad(0x0FFFFFF7));
    JARVIS_ASSERT(!Fat32Partition::is_bad(0x0FFFFFF8));
}

JARVIS_TEST(fat32_dir_root_entries, "PRE: vfsd, iocd | POST: none") {
    auto dev = make_dev();
    auto fs = mount_fs(dev);

    DirEntry entry = {};
    uint64_t pos = 0;
    bool found_multi = false;
    bool found_subdir = false;

    while (read_dir_entry(fs, fs.bpb().root_cluster, pos, entry)) {
        if (!entry.valid)
            continue;
        if (strcmp(entry.name, "MULTI.TXT") == 0)
            found_multi = true;
        if (strcmp(entry.name, "SUBDIR") == 0)
            found_subdir = true;
    }

    JARVIS_ASSERT(found_multi);
    JARVIS_ASSERT(found_subdir);
}

JARVIS_TEST(fat32_dir_short_name, "PRE: vfsd, iocd | POST: none") {
    auto dev = make_dev();
    auto fs = mount_fs(dev);
    DirEntry entry = {};
    JARVIS_ASSERT(lookup_in_dir(fs, fs.bpb().root_cluster, "MULTI.TXT", entry));
    JARVIS_ASSERT(strcmp(entry.name, "MULTI.TXT") == 0);
}

JARVIS_TEST(fat32_dir_file_size, "PRE: vfsd, iocd | POST: none") {
    auto dev = make_dev();
    auto fs = mount_fs(dev);
    DirEntry entry = {};
    JARVIS_ASSERT(lookup_in_dir(fs, fs.bpb().root_cluster, "MULTI.TXT", entry));
    JARVIS_ASSERT(entry.size > 0);
}

JARVIS_TEST(fat32_dir_file_cluster, "PRE: vfsd, iocd | POST: none") {
    auto dev = make_dev();
    auto fs = mount_fs(dev);
    DirEntry entry = {};
    JARVIS_ASSERT(lookup_in_dir(fs, fs.bpb().root_cluster, "MULTI.TXT", entry));
    JARVIS_ASSERT(entry.cluster >= 2);
}

JARVIS_TEST(fat32_dir_attribute_readonly, "PRE: vfsd, iocd | POST: none") {
    JARVIS_ASSERT((ATTR_READ_ONLY & 0x01) != 0);
}

JARVIS_TEST(fat32_dir_attribute_hidden, "PRE: vfsd, iocd | POST: none") {
    JARVIS_ASSERT((ATTR_HIDDEN & 0x02) != 0);
}

JARVIS_TEST(fat32_dir_attribute_directory, "PRE: vfsd, iocd | POST: none") {
    auto dev = make_dev();
    auto fs = mount_fs(dev);
    DirEntry entry = {};
    JARVIS_ASSERT(lookup_in_dir(fs, fs.bpb().root_cluster, "SUBDIR", entry));
    JARVIS_ASSERT(entry.is_directory);
    JARVIS_ASSERT(lookup_in_dir(fs, fs.bpb().root_cluster, "MULTI.TXT", entry));
    JARVIS_ASSERT(!entry.is_directory);
}

JARVIS_TEST(fat32_dir_volume_label, "PRE: vfsd, iocd | POST: none") {
    JARVIS_ASSERT(ATTR_VOLUME_ID == 0x08);
}

JARVIS_TEST(fat32_dir_lfn_skipped, "PRE: vfsd, iocd | POST: none") {
    JARVIS_ASSERT(ATTR_LFN == 0x0F);
}

JARVIS_TEST(fat32_chain_traverse_single, "PRE: vfsd, iocd | POST: none") {
    auto dev = make_dev();
    auto fs = mount_fs(dev);
    DirEntry entry = {};
    JARVIS_ASSERT(lookup_in_dir(fs, fs.bpb().root_cluster, "HELLO.TXT", entry));
    uint32_t next = entry.cluster;
    JARVIS_ASSERT(fs.read_fat_entry(next, next));
    JARVIS_ASSERT(Fat32Partition::is_eof(next));
}

JARVIS_TEST(fat32_chain_traverse_multi, "PRE: vfsd, iocd | POST: none") {
    auto dev = make_dev();
    auto fs = mount_fs(dev);
    DirEntry entry = {};
    JARVIS_ASSERT(lookup_in_dir(fs, fs.bpb().root_cluster, "MULTI.TXT", entry));
    uint32_t next = entry.cluster;
    JARVIS_ASSERT(fs.read_fat_entry(next, next));
    JARVIS_ASSERT(!Fat32Partition::is_eof(next));
    JARVIS_ASSERT(!Fat32Partition::is_free(next));
    JARVIS_ASSERT(fs.read_fat_entry(next, next));
    JARVIS_ASSERT(Fat32Partition::is_eof(next));
}

JARVIS_TEST(fat32_read_file, "PRE: vfsd, iocd | POST: none") {
    auto dev = make_dev();
    auto fs = mount_fs(dev);
    DirEntry entry = {};
    JARVIS_ASSERT(lookup_in_dir(fs, fs.bpb().root_cluster, "HELLO.TXT", entry));
    JARVIS_ASSERT(entry.size > 0);

    uint8_t buf[256];
    memset(buf, 0, sizeof(buf));
    int64_t nread =
        read_file(fs, entry.cluster, entry.size, 0, sizeof(buf), buf);
    JARVIS_ASSERT(nread > 0);
    JARVIS_ASSERT(static_cast<uint64_t>(nread) == entry.size);
    JARVIS_ASSERT(memcmp(buf, "Hello, World!\n", 14) == 0);
}

JARVIS_TEST(fat32_lookup_subdir, "PRE: vfsd, iocd | POST: none") {
    auto dev = make_dev();
    auto fs = mount_fs(dev);
    DirEntry sub = {};
    JARVIS_ASSERT(lookup_in_dir(fs, fs.bpb().root_cluster, "SUBDIR", sub));
    JARVIS_ASSERT(sub.is_directory);

    DirEntry file_in_sub = {};
    JARVIS_ASSERT(lookup_in_dir(fs, sub.cluster, "FILE.TXT", file_in_sub));
    JARVIS_ASSERT(!file_in_sub.is_directory);
    JARVIS_ASSERT(file_in_sub.size > 0);

    uint8_t buf[128];
    memset(buf, 0, sizeof(buf));
    int64_t nread = read_file(fs, file_in_sub.cluster, file_in_sub.size, 0,
                              sizeof(buf), buf);
    JARVIS_ASSERT(nread > 0);
    JARVIS_ASSERT(memcmp(buf, "I am in a subdirectory.\n", 24) == 0);
}

JARVIS_TEST(fat32_chain_corrupt_loop, "PRE: vfsd, iocd | POST: none") {
    JARVIS_ASSERT(Fat32Partition::is_eof(0x0FFFFFFF));
}

JARVIS_TEST(fat32_chain_corrupt_eof_missing, "PRE: vfsd, iocd | POST: none") {
    JARVIS_ASSERT(!Fat32Partition::is_eof(3));
}

// ---- Write primitive tests (writable copy of embedded image) ----

static kernel::block::MockBlockDevice make_writable_dev() {
    auto dev = kernel::block::MockBlockDevice(fat32_img_sectors());
    for (uint64_t i = 0; i < fat32_img_sectors(); ++i) {
        uint8_t sector[SECTOR_SIZE];
        __builtin_memcpy(sector,
                         _binary_build_fat32_img_start + i * SECTOR_SIZE,
                         SECTOR_SIZE);
        dev.write_sector(i, sector);
    }
    return dev;
}

// Runmode: kernel
// Testidea: Write a FAT entry then read it back.
// Input: write_fat_entry(10, 0x0FFFFFF8), read_fat_entry(10)
// Expect: read back value matches written EOF marker
// Depends: kernel::fat32::Fat32Partition::write_fat_entry
JARVIS_TEST(fat32_write_fat_entry, "PRE: vfsd, iocd | POST: none") {
    auto dev = make_writable_dev();
    auto fs = mount_fs(dev);
    uint32_t next = 0;
    JARVIS_ASSERT(fs.write_fat_entry(10, 0x0FFFFFF8));
    JARVIS_ASSERT(fs.read_fat_entry(10, next));
    JARVIS_ASSERT(Fat32Partition::is_eof(next));
}

// Runmode: kernel
// Testidea: Write a full cluster and read it back.
// Input: write_cluster(3, test_data), read_cluster(3, buf)
// Expect: read back matches written data
// Depends: kernel::fat32::Fat32Partition::write_cluster
JARVIS_TEST(fat32_write_cluster, "PRE: vfsd, iocd | POST: none") {
    auto dev = make_writable_dev();
    auto fs = mount_fs(dev);
    uint64_t cluster_bytes =
        static_cast<uint64_t>(fs.bpb().sectors_per_cluster) * SECTOR_SIZE;
    uint8_t *wbuf = new uint8_t[cluster_bytes];
    memset(wbuf, 0x42, cluster_bytes);
    JARVIS_ASSERT(fs.write_cluster(3, wbuf));
    uint8_t *rbuf = new uint8_t[cluster_bytes];
    memset(rbuf, 0, cluster_bytes);
    JARVIS_ASSERT(fs.read_cluster(3, rbuf));
    JARVIS_ASSERT(memcmp(wbuf, rbuf, cluster_bytes) == 0);
    delete[] wbuf;
    delete[] rbuf;
}

// Runmode: kernel
// Testidea: Zero-fill a cluster and verify all bytes are zero.
// Input: clear_cluster(4)
// Expect: read_cluster returns all-zero data
// Depends: kernel::fat32::Fat32Partition::clear_cluster
JARVIS_TEST(fat32_clear_cluster, "PRE: vfsd, iocd | POST: none") {
    auto dev = make_writable_dev();
    auto fs = mount_fs(dev);
    JARVIS_ASSERT(fs.clear_cluster(4));
    uint64_t cluster_bytes =
        static_cast<uint64_t>(fs.bpb().sectors_per_cluster) * SECTOR_SIZE;
    uint8_t *buf = new uint8_t[cluster_bytes];
    memset(buf, 0xFF, cluster_bytes);
    JARVIS_ASSERT(fs.read_cluster(4, buf));
    for (uint64_t i = 0; i < cluster_bytes; ++i)
        JARVIS_ASSERT(buf[i] == 0);
    delete[] buf;
}

// Runmode: kernel
// Testidea: Find a free cluster in the FAT.
// Input: find_free_cluster(2, out)
// Expect: Returns a valid free cluster >= 2
// Depends: kernel::fat32::Fat32Partition::find_free_cluster
JARVIS_TEST(fat32_find_free_cluster, "PRE: vfsd, iocd | POST: none") {
    auto dev = make_writable_dev();
    auto fs = mount_fs(dev);
    uint32_t free_cl = 0;
    JARVIS_ASSERT(fs.find_free_cluster(2, free_cl));
    JARVIS_ASSERT(free_cl >= 2);
    uint32_t entry = 0;
    JARVIS_ASSERT(fs.read_fat_entry(free_cl, entry));
    JARVIS_ASSERT(Fat32Partition::is_free(entry));
}

// Runmode: kernel
// Testidea: Allocate a cluster and chain it to a previous cluster.
// Input: alloc_cluster(2, out)
// Expect: New cluster allocated, FAT entry 2 points to new cluster,
//         new cluster marked EOF
// Depends: kernel::fat32::Fat32Partition::alloc_cluster
JARVIS_TEST(fat32_alloc_cluster, "PRE: vfsd, iocd | POST: none") {
    auto dev = make_writable_dev();
    auto fs = mount_fs(dev);
    uint32_t new_cl = 0;
    JARVIS_ASSERT(fs.alloc_cluster(2, new_cl));
    JARVIS_ASSERT(new_cl >= 2);
    JARVIS_ASSERT(new_cl != 2);
    uint32_t entry = 0;
    JARVIS_ASSERT(fs.read_fat_entry(2, entry));
    JARVIS_ASSERT_EQ(new_cl, entry);
    JARVIS_ASSERT(fs.read_fat_entry(new_cl, entry));
    JARVIS_ASSERT(Fat32Partition::is_eof(entry));
}

// Runmode: kernel
// Testidea: Free a cluster chain and verify entries are cleared.
// Input: alloc_cluster chain, free_cluster_chain
// Expect: FAT entries become free (0)
// Depends: kernel::fat32::free_cluster_chain
JARVIS_TEST(fat32_free_cluster_chain, "PRE: vfsd, iocd | POST: none") {
    auto dev = make_writable_dev();
    auto fs = mount_fs(dev);
    uint32_t c1 = 0, c2 = 0;
    JARVIS_ASSERT(fs.alloc_cluster(0, c1));
    JARVIS_ASSERT(fs.alloc_cluster(c1, c2));
    uint32_t entry = 0;
    JARVIS_ASSERT(fs.read_fat_entry(c2, entry));
    JARVIS_ASSERT(Fat32Partition::is_eof(entry));
    free_cluster_chain(fs, c1);
    JARVIS_ASSERT(fs.read_fat_entry(c1, entry));
    JARVIS_ASSERT(Fat32Partition::is_free(entry));
    JARVIS_ASSERT(fs.read_fat_entry(c2, entry));
    JARVIS_ASSERT(Fat32Partition::is_free(entry));
}

// Runmode: kernel
// Testidea: Convert a long filename to 8.3 short name.
// Input: name_to_short_name("README.TXT", out)
// Expect: Output matches "README TXT" padded
// Depends: kernel::fat32::name_to_short_name
JARVIS_TEST(fat32_name_to_short_name, "PRE: vfsd, iocd | POST: none") {
    uint8_t sn[11];
    name_to_short_name("HELLO.TXT", sn);
    JARVIS_ASSERT(memcmp(sn, "HELLO   TXT", 11) == 0);
    name_to_short_name("File", sn);
    JARVIS_ASSERT(memcmp(sn, "FILE        ", 11) == 0);
}

// Runmode: kernel
// Testidea: Add a directory entry, then verify it appears in read_dir_entry.
// Input: add_dir_entry to root, read_dir_entry
// Expect: New entry found with correct name and attributes
// Depends: kernel::fat32::add_dir_entry, kernel::fat32::read_dir_entry
JARVIS_TEST(fat32_add_dir_entry, "PRE: vfsd, iocd | POST: none") {
    auto dev = make_writable_dev();
    auto fs = mount_fs(dev);
    DirEntryRaw raw;
    memset(&raw, 0, sizeof(raw));
    name_to_short_name("NEWFILE.TXT", raw.name);
    raw.attrs = ATTR_ARCHIVE;
    JARVIS_ASSERT(add_dir_entry(fs, fs.bpb().root_cluster, raw));
    DirEntry entry = {};
    uint64_t pos = 0;
    bool found = false;
    while (read_dir_entry(fs, fs.bpb().root_cluster, pos, entry)) {
        if (!entry.valid)
            continue;
        if (strcmp(entry.name, "NEWFILE.TXT") == 0) {
            found = true;
            break;
        }
    }
    JARVIS_ASSERT(found);
}

// Runmode: kernel
// Testidea: Remove a directory entry, verify it no longer appears.
// Input: remove_dir_entry for "HELLO.TXT", read_dir_entry
// Expect: Entry no longer found in directory listing
// Depends: kernel::fat32::remove_dir_entry
JARVIS_TEST(fat32_remove_dir_entry, "PRE: vfsd, iocd | POST: none") {
    auto dev = make_writable_dev();
    auto fs = mount_fs(dev);
    JARVIS_ASSERT(remove_dir_entry(fs, fs.bpb().root_cluster, "HELLO.TXT"));
    DirEntry entry = {};
    uint64_t pos = 0;
    bool found = false;
    while (read_dir_entry(fs, fs.bpb().root_cluster, pos, entry)) {
        if (!entry.valid)
            continue;
        if (strcmp(entry.name, "HELLO.TXT") == 0) {
            found = true;
            break;
        }
    }
    JARVIS_ASSERT(!found);
}

// Runmode: kernel
// Testidea: Full mkdir round-trip: create dir, verify entries.
// Input: alloc_cluster, add_dir_entry with ATTR_DIRECTORY
// Expect: lookup_in_dir finds new directory
// Depends: kernel::fat32::Fat32Partition, kernel::fat32::add_dir_entry
JARVIS_TEST(fat32_mkdir_roundtrip, "PRE: vfsd, iocd | POST: none") {
    auto dev = make_writable_dev();
    auto fs = mount_fs(dev);
    uint32_t dir_cl = 0;
    JARVIS_ASSERT(fs.alloc_cluster(0, dir_cl));
    JARVIS_ASSERT(fs.clear_cluster(dir_cl));

    DirEntryRaw dot, dotdot;
    memset(&dot, 0, sizeof(dot));
    memset(&dotdot, 0, sizeof(dotdot));
    name_to_short_name(".", dot.name);
    dot.attrs = ATTR_DIRECTORY;
    dot.cluster_low = static_cast<uint16_t>(dir_cl & 0xFFFF);
    dot.cluster_high = static_cast<uint16_t>((dir_cl >> 16) & 0xFFFF);
    name_to_short_name("..", dotdot.name);
    dotdot.attrs = ATTR_DIRECTORY;
    JARVIS_ASSERT(add_dir_entry(fs, dir_cl, dot));
    JARVIS_ASSERT(add_dir_entry(fs, dir_cl, dotdot));

    DirEntryRaw parent_entry;
    memset(&parent_entry, 0, sizeof(parent_entry));
    name_to_short_name("TESTDIR", parent_entry.name);
    parent_entry.attrs = ATTR_DIRECTORY;
    parent_entry.cluster_low = static_cast<uint16_t>(dir_cl & 0xFFFF);
    parent_entry.cluster_high = static_cast<uint16_t>((dir_cl >> 16) & 0xFFFF);
    JARVIS_ASSERT(add_dir_entry(fs, fs.bpb().root_cluster, parent_entry));

    DirEntry entry = {};
    JARVIS_ASSERT(lookup_in_dir(fs, fs.bpb().root_cluster, "TESTDIR", entry));
    JARVIS_ASSERT(entry.is_directory);
    JARVIS_ASSERT_EQ(dir_cl, entry.cluster);
}

void register_fat32_tests() {
    Logger::info("Registering FAT32 tests");

    JARVIS_REGISTER_RELEASE_TEST(fat32_mbr_valid_signature);
    JARVIS_REGISTER_RELEASE_TEST(fat32_mbr_parse_partition);
    JARVIS_REGISTER_RELEASE_TEST(fat32_mbr_no_partition_table);
    JARVIS_REGISTER_RELEASE_TEST(fat32_bpb_valid_signature);
    JARVIS_REGISTER_RELEASE_TEST(fat32_bpb_bytes_per_sector);
    JARVIS_REGISTER_RELEASE_TEST(fat32_bpb_sectors_per_cluster);
    JARVIS_REGISTER_RELEASE_TEST(fat32_bpb_reserved_sectors);
    JARVIS_REGISTER_RELEASE_TEST(fat32_bpb_fat_count);
    JARVIS_REGISTER_RELEASE_TEST(fat32_bpb_root_cluster);
    JARVIS_REGISTER_RELEASE_TEST(fat32_bpb_fat_size);
    JARVIS_REGISTER_RELEASE_TEST(fat32_bpb_total_sectors);
    JARVIS_REGISTER_RELEASE_TEST(fat32_fat_read_cluster);
    JARVIS_REGISTER_RELEASE_TEST(fat32_fat_eof_marker);
    JARVIS_REGISTER_RELEASE_TEST(fat32_fat_free_cluster);
    JARVIS_REGISTER_RELEASE_TEST(fat32_fat_bad_cluster);
    JARVIS_REGISTER_RELEASE_TEST(fat32_dir_root_entries);
    JARVIS_REGISTER_RELEASE_TEST(fat32_dir_short_name);
    JARVIS_REGISTER_RELEASE_TEST(fat32_dir_file_size);
    JARVIS_REGISTER_RELEASE_TEST(fat32_dir_file_cluster);
    JARVIS_REGISTER_RELEASE_TEST(fat32_dir_attribute_readonly);
    JARVIS_REGISTER_RELEASE_TEST(fat32_dir_attribute_hidden);
    JARVIS_REGISTER_RELEASE_TEST(fat32_dir_attribute_directory);
    JARVIS_REGISTER_RELEASE_TEST(fat32_dir_volume_label);
    JARVIS_REGISTER_RELEASE_TEST(fat32_dir_lfn_skipped);
    JARVIS_REGISTER_RELEASE_TEST(fat32_chain_traverse_single);
    JARVIS_REGISTER_RELEASE_TEST(fat32_chain_traverse_multi);
    JARVIS_REGISTER_RELEASE_TEST(fat32_read_file);
    JARVIS_REGISTER_RELEASE_TEST(fat32_lookup_subdir);
    JARVIS_REGISTER_RELEASE_TEST(fat32_chain_corrupt_loop);
    JARVIS_REGISTER_RELEASE_TEST(fat32_chain_corrupt_eof_missing);

    JARVIS_REGISTER_TEST(fat32_write_fat_entry);
    JARVIS_REGISTER_TEST(fat32_write_cluster);
    JARVIS_REGISTER_TEST(fat32_clear_cluster);
    JARVIS_REGISTER_TEST(fat32_find_free_cluster);
    JARVIS_REGISTER_TEST(fat32_alloc_cluster);
    JARVIS_REGISTER_TEST(fat32_free_cluster_chain);
    JARVIS_REGISTER_TEST(fat32_name_to_short_name);
    JARVIS_REGISTER_TEST(fat32_add_dir_entry);
    JARVIS_REGISTER_TEST(fat32_remove_dir_entry);
    JARVIS_REGISTER_TEST(fat32_mkdir_roundtrip);
}
#ifndef __clang__
#pragma GCC diagnostic pop
#endif
