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

/// @file fat32.cpp
/// @brief FAT32 partition parsing, directory reading, and write primitives.

#include <kernel/vfs/fat32.hpp>
#include <kernel/memory/mempool.hpp>
#include <string.hpp>
#include <utils.hpp>

namespace kernel {
namespace fat32 {

static constexpr uint8_t MBR_SIGNATURE_LO = 0x55;
static constexpr uint8_t MBR_SIGNATURE_HI = 0xAA;
static constexpr uint8_t BPB_SIGNATURE_LO = 0x55;
static constexpr uint8_t BPB_SIGNATURE_HI = 0xAA;
static constexpr uint8_t PART_FAT32_LBA = 0x0C;
static constexpr uint8_t PART_FAT32_CHS = 0x0B;
static constexpr int64_t FAT_READ_ERROR = -1;

// -------------------------------------------------------------------
// MBR parsing
// -------------------------------------------------------------------

/// @brief Find the first FAT32 partition in the MBR.
/// @return true if a FAT32 partition was found.
bool find_fat32_in_mbr(block::BlockDevice &device,
                       uint64_t &partition_lba_out) {
    uint8_t sector[SECTOR_SIZE];
    if (!device.read_sector(0, sector))
        return false;

    if (sector[510] != MBR_SIGNATURE_LO || sector[511] != MBR_SIGNATURE_HI)
        return false;

    // Partition entries are at offset 0x1BE (446), each 16 bytes, 4 entries
    for (int i = 0; i < 4; ++i) {
        uint16_t off = static_cast<uint16_t>(446 + i * 16);
        uint8_t type = sector[off + 4];
        if (type == PART_FAT32_LBA || type == PART_FAT32_CHS) {
            partition_lba_out = static_cast<uint32_t>(
                (static_cast<uint32_t>(sector[off + 8])) |
                (static_cast<uint32_t>(sector[off + 9]) << 8) |
                (static_cast<uint32_t>(sector[off + 10]) << 16) |
                (static_cast<uint32_t>(sector[off + 11]) << 24));
            return true;
        }
    }

    return false;
}

// -------------------------------------------------------------------
// BPB parsing
// -------------------------------------------------------------------

/// @brief Parse the FAT32 BPB from a raw sector buffer.
/// @return true if the BPB is valid.
static bool parse_bpb_from_sector(const uint8_t *sector, Fat32Bpb &bpb) {
    if (sector[510] != BPB_SIGNATURE_LO || sector[511] != BPB_SIGNATURE_HI)
        return false;

    bpb.bytes_per_sector = static_cast<uint16_t>(
        sector[11] | (static_cast<uint16_t>(sector[12]) << 8));
    if (bpb.bytes_per_sector != 512)
        return false; // Only support 512

    bpb.sectors_per_cluster = sector[13];
    if (bpb.sectors_per_cluster == 0 ||
        (bpb.sectors_per_cluster & (bpb.sectors_per_cluster - 1)) != 0)
        return false;

    bpb.reserved_sectors = static_cast<uint16_t>(
        sector[14] | (static_cast<uint16_t>(sector[15]) << 8));
    if (bpb.reserved_sectors == 0)
        return false;

    bpb.fat_count = sector[16];
    if (bpb.fat_count == 0)
        return false;

    bpb.fat_size =
        static_cast<uint32_t>((static_cast<uint32_t>(sector[36])) |
                              (static_cast<uint32_t>(sector[37]) << 8) |
                              (static_cast<uint32_t>(sector[38]) << 16) |
                              (static_cast<uint32_t>(sector[39]) << 24));
    if (bpb.fat_size == 0)
        return false;

    bpb.root_cluster =
        static_cast<uint32_t>((static_cast<uint32_t>(sector[44])) |
                              (static_cast<uint32_t>(sector[45]) << 8) |
                              (static_cast<uint32_t>(sector[46]) << 16) |
                              (static_cast<uint32_t>(sector[47]) << 24));
    if (bpb.root_cluster < 2)
        return false;

    bpb.fs_info_sector = static_cast<uint16_t>(
        sector[48] | (static_cast<uint16_t>(sector[49]) << 8));

    bpb.total_sectors =
        static_cast<uint32_t>((static_cast<uint32_t>(sector[32])) |
                              (static_cast<uint32_t>(sector[33]) << 8) |
                              (static_cast<uint32_t>(sector[34]) << 16) |
                              (static_cast<uint32_t>(sector[35]) << 24));
    if (bpb.total_sectors == 0) {
        // Try 16-bit field
        bpb.total_sectors = static_cast<uint32_t>(
            sector[19] | (static_cast<uint16_t>(sector[20]) << 8));
    }

    bpb.valid = true;
    return true;
}

// -------------------------------------------------------------------
// Fat32Partition
// -------------------------------------------------------------------

/// @brief Construct a Fat32Partition backed by the given block device.
Fat32Partition::Fat32Partition(block::BlockDevice &device) : device_(device) {
}

/// @brief Mount by scanning the MBR for a FAT32 partition.
/// @return true if a valid FAT32 partition was found.
bool Fat32Partition::mount() {
    if (find_fat32_in_mbr(device_, partition_start_)) {
        return parse_bpb(partition_start_);
    }
    return false;
}

/// @brief Mount at a specific partition LBA.
/// @return true if the BPB at the given LBA parsed successfully.
bool Fat32Partition::mount_at(uint64_t partition_lba) {
    partition_start_ = partition_lba;
    return parse_bpb(partition_lba);
}

/// @brief Parse the MBR to find the partition start.
/// @return true if a FAT32 partition was found.
bool Fat32Partition::parse_mbr() {
    return find_fat32_in_mbr(device_, partition_start_);
}

/// @brief Parse the BPB at the given LBA.
/// @return true if the BPB is valid.
bool Fat32Partition::parse_bpb(uint64_t lba) {
    uint8_t sector[SECTOR_SIZE];
    if (!device_.read_sector(lba, sector))
        return false;
    return parse_bpb_from_sector(sector, bpb_);
}

/// @brief Find the FAT32 partition in the MBR (private helper).
bool Fat32Partition::find_fat32_partition() {
    return find_fat32_in_mbr(device_, partition_start_);
}

/// @brief Convert a cluster number to the first sector's LBA.
uint64_t Fat32Partition::cluster_to_lba(uint32_t cluster) const {
    uint64_t data_start = partition_start_ + bpb_.reserved_sectors +
                          static_cast<uint64_t>(bpb_.fat_count) * bpb_.fat_size;
    return data_start +
           (static_cast<uint64_t>(cluster) - 2) * bpb_.sectors_per_cluster;
}

/// @brief Read a FAT entry for a given cluster.
/// @return true on success.
bool Fat32Partition::read_fat_entry(uint32_t cluster,
                                    uint32_t &next_cluster) const {
    uint64_t fat_offset = static_cast<uint64_t>(cluster) * 4;
    uint64_t fat_sector = fat_offset / bpb_.bytes_per_sector;
    uint64_t sector_lba = partition_start_ + bpb_.reserved_sectors + fat_sector;

    uint8_t sector[SECTOR_SIZE];
    if (!device_.read_sector(sector_lba, sector))
        return false;

    uint64_t byte_offset = fat_offset % bpb_.bytes_per_sector;
    next_cluster = static_cast<uint32_t>(
        (static_cast<uint32_t>(sector[byte_offset])) |
        (static_cast<uint32_t>(sector[byte_offset + 1]) << 8) |
        (static_cast<uint32_t>(sector[byte_offset + 2]) << 16) |
        (static_cast<uint32_t>(sector[byte_offset + 3]) << 24));
    next_cluster &= 0x0FFFFFFF;
    return true;
}

/// @brief Read a full cluster's data.
/// @return true on success.
bool Fat32Partition::read_cluster(uint32_t cluster, uint8_t *buffer) const {
    uint64_t lba = cluster_to_lba(cluster);
    uint64_t sectors = bpb_.sectors_per_cluster;
    for (uint64_t i = 0; i < sectors; ++i) {
        if (!device_.read_sector(lba + i, buffer + i * bpb_.bytes_per_sector))
            return false;
    }
    return true;
}

// -------------------------------------------------------------------
// Directory entry helpers
// -------------------------------------------------------------------

/// @brief Format a raw 8.3 directory entry name into "NAME.EXT".
void format_short_name(const uint8_t raw_name[11], char *out, size_t out_size) {
    if (!raw_name || !out || out_size < 13)
        return;

    // Name part (8 bytes)
    size_t i = 0, j = 0;
    while (i < 8 && raw_name[i] && raw_name[i] != ' ') {
        if (j < out_size - 1)
            out[j++] = static_cast<char>(raw_name[i]);
        ++i;
    }

    // Extension (3 bytes)
    if (raw_name[8] && raw_name[8] != ' ') {
        if (j < out_size - 1)
            out[j++] = '.';
        i = 8;
        while (i < 11 && raw_name[i] && raw_name[i] != ' ') {
            if (j < out_size - 1)
                out[j++] = static_cast<char>(raw_name[i]);
            ++i;
        }
    }

    out[j] = '\0';
}

/// @brief Check if a raw entry is a Long File Name entry.
static bool is_lfn_entry(const DirEntryRaw &raw) {
    return raw.attrs == ATTR_LFN;
}

/// @brief Check if a raw entry is the end-of-directory marker.
static bool is_end_marker(const DirEntryRaw &raw) {
    return raw.name[0] == 0x00;
}

/// @brief Check if a raw entry is free (deleted).
static bool is_free_entry(const DirEntryRaw &raw) {
    return raw.name[0] == 0xE5;
}

/// @brief Extract the cluster number from a raw directory entry.
static uint32_t get_entry_cluster(const DirEntryRaw &raw) {
    return (static_cast<uint32_t>(raw.cluster_high) << 16) |
           static_cast<uint32_t>(raw.cluster_low);
}

// -------------------------------------------------------------------
// Directory reading
// -------------------------------------------------------------------

/// @brief Read the next directory entry from a cluster chain.
/// @return true if a valid entry was read.
bool read_dir_entry(Fat32Partition &fs, uint32_t dir_cluster, uint64_t &pos,
                    DirEntry &entry) {
    entry.valid = false;

    uint32_t cluster_chain[256];
    uint32_t chain_len = 0;
    uint32_t current = dir_cluster;
    uint32_t max_clusters = 256;

    while (!Fat32Partition::is_eof(current) && chain_len < max_clusters) {
        if (Fat32Partition::is_bad(current) || Fat32Partition::is_free(current))
            return false;
        cluster_chain[chain_len++] = current;
        if (!fs.read_fat_entry(current, current))
            return false;
    }

    if (chain_len == 0)
        return false;

    uint32_t entries_per_cluster =
        fs.bpb().sectors_per_cluster * fs.bpb().bytes_per_sector / 32;
    uint64_t total_entries =
        static_cast<uint64_t>(chain_len) * entries_per_cluster;
    if (pos >= total_entries)
        return false;

    uint64_t entry_idx = pos;
    uint32_t cluster_idx =
        static_cast<uint32_t>(entry_idx / entries_per_cluster);
    uint32_t entry_in_cluster =
        static_cast<uint32_t>(entry_idx % entries_per_cluster);

    if (cluster_idx >= chain_len)
        return false;

    // Read the cluster containing the entry.  Use a stack buffer for the
    // common case (cluster ≤ 4 KiB).  Oversized clusters fall back to heap.
    uint32_t rd_cluster_size =
        fs.bpb().sectors_per_cluster * fs.bpb().bytes_per_sector;
    uint8_t cluster_data_stack[4096];
    uint8_t *cluster_data = cluster_data_stack;
    bool heap_alloc = false;
    if (rd_cluster_size > sizeof(cluster_data_stack)) {
        cluster_data = static_cast<uint8_t *>(MemPool::alloc(rd_cluster_size));
        if (!cluster_data)
            return false;
        heap_alloc = true;
    }
    if (!fs.read_cluster(cluster_chain[cluster_idx], cluster_data)) {
        if (heap_alloc) MemPool::free(cluster_data);
        return false;
    }

    const auto &raw = reinterpret_cast<const DirEntryRaw &>(
        cluster_data[static_cast<size_t>(entry_in_cluster) * 32]);

    // Skip empty, freed, and LFN entries
    if (is_end_marker(raw)) {
        if (heap_alloc) MemPool::free(cluster_data);
        return false;
    }
    if (is_free_entry(raw) || is_lfn_entry(raw)) {
        // cluster_data is stack-allocated or heap-freed before recursion
        if (heap_alloc) MemPool::free(cluster_data);
        ++pos;
        return read_dir_entry(fs, dir_cluster, pos, entry);
    }

    format_short_name(raw.name, entry.name, sizeof(entry.name));
    entry.attrs = raw.attrs;
    entry.cluster = get_entry_cluster(raw);
    entry.size = raw.file_size;
    entry.is_directory = (raw.attrs & ATTR_DIRECTORY) != 0;
    entry.valid = true;

    if (heap_alloc) MemPool::free(cluster_data);
    ++pos;
    return true;
}

/// @brief Look up a name in a directory cluster chain.
/// @return true if found.
bool lookup_in_dir(Fat32Partition &fs, uint32_t dir_cluster, const char *name,
                   DirEntry &entry) {
    uint64_t ignored_index = 0;
    return lookup_in_dir_at(fs, dir_cluster, name, entry, ignored_index);
}

/// @brief Look up a name, reporting the matched on-disk slot index.
/// read_dir_entry leaves pos one past the returned slot (it increments
/// before every true return, including via the skip-recursion), so the
/// matched slot is pos - 1. pos > 0 whenever we return true.
bool lookup_in_dir_at(Fat32Partition &fs, uint32_t dir_cluster,
                      const char *name, DirEntry &entry, uint64_t &out_index) {
    uint64_t pos = 0;
    while (read_dir_entry(fs, dir_cluster, pos, entry)) {
        if (entry.valid && strcmp(entry.name, name) == 0) {
            out_index = pos - 1;
            return true;
        }
    }
    return false;
}

// -------------------------------------------------------------------
// File reading
// -------------------------------------------------------------------

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
/// @brief Read data from a file's cluster chain.
/// @return Number of bytes read, or -1 on error.
int64_t read_file(Fat32Partition &fs, uint32_t first_cluster,
                  uint32_t file_size, uint64_t offset, uint64_t count,
                  uint8_t *buffer) {
    if (!buffer || offset >= file_size)
        return 0;

    if (count > file_size - offset)
        count = file_size - offset;
    if (count == 0)
        return 0;

    uint32_t cluster_size =
        fs.bpb().sectors_per_cluster * fs.bpb().bytes_per_sector;
    uint32_t current = first_cluster;
    uint64_t pos = 0;
    int64_t total_read = 0;

    // Skip to cluster containing offset
    while (pos + cluster_size <= offset) {
        pos += cluster_size;
        if (!fs.read_fat_entry(current, current))
            return FAT_READ_ERROR;
        if (Fat32Partition::is_eof(current))
            return static_cast<int64_t>(total_read);
        if (Fat32Partition::is_bad(current))
            return FAT_READ_ERROR;
    }

    // Read data.  Use a stack buffer for the common case (cluster ≤ 4 KiB).
    // Oversized clusters fall back to heap.  The previous comment warned about
    // 32 KiB stack buffer overflow — a single cluster is at most 4 KiB on
    // typical images (8 sectors × 512 B).
    uint8_t cluster_data_stack[4096];
    uint8_t *cluster_data = cluster_data_stack;
    bool heap_alloc = false;
    if (cluster_size > sizeof(cluster_data_stack)) {
        cluster_data = static_cast<uint8_t *>(MemPool::alloc(cluster_size));
        if (!cluster_data)
            return FAT_READ_ERROR;
        heap_alloc = true;
    }
    while (total_read < static_cast<int64_t>(count)) {
        if (!fs.read_cluster(current, cluster_data)) {
            if (heap_alloc) MemPool::free(cluster_data);
            return FAT_READ_ERROR;
        }

        uint64_t cluster_off = offset - pos;
        uint64_t to_read = cluster_size - cluster_off;
        if (to_read > count - static_cast<uint64_t>(total_read))
            to_read = count - static_cast<uint64_t>(total_read);

        memcpy(buffer + total_read, cluster_data + cluster_off, to_read);
        total_read += static_cast<int64_t>(to_read);
        pos += cluster_size;
        offset += to_read;

        if (total_read >= static_cast<int64_t>(count))
            break;

        if (!fs.read_fat_entry(current, current)) {
            if (heap_alloc) MemPool::free(cluster_data);
            return FAT_READ_ERROR;
        }
        if (Fat32Partition::is_eof(current))
            break;
        if (Fat32Partition::is_bad(current)) {
            if (heap_alloc) MemPool::free(cluster_data);
            return FAT_READ_ERROR;
        }
    }

    if (heap_alloc) MemPool::free(cluster_data);
    return total_read;
}

// -------------------------------------------------------------------
// Write primitives
// -------------------------------------------------------------------

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
/// @brief Write a FAT entry, mirroring to secondary FATs.
/// @return true on success.
bool Fat32Partition::write_fat_entry(uint32_t cluster, uint32_t value) {
    value &= 0x0FFFFFFF;
    uint64_t fat_offset = static_cast<uint64_t>(cluster) * 4;
    uint64_t fat_sector = fat_offset / bpb_.bytes_per_sector;
    uint64_t sector_lba = partition_start_ + bpb_.reserved_sectors + fat_sector;

    uint8_t sector[SECTOR_SIZE];
    if (!device_.read_sector(sector_lba, sector))
        return false;

    uint64_t byte_offset = fat_offset % bpb_.bytes_per_sector;
    sector[byte_offset] = static_cast<uint8_t>(value);
    sector[byte_offset + 1] = static_cast<uint8_t>(value >> 8);
    sector[byte_offset + 2] = static_cast<uint8_t>(value >> 16);
    sector[byte_offset + 3] = static_cast<uint8_t>(value >> 24);

    // Write back to this FAT
    if (!device_.write_sector(sector_lba, sector))
        return false;

    // Mirror to secondary FAT(s)
    for (uint8_t fat_idx = 1; fat_idx < bpb_.fat_count; ++fat_idx) {
        uint64_t mirror_lba = partition_start_ + bpb_.reserved_sectors +
                              static_cast<uint64_t>(fat_idx) * bpb_.fat_size +
                              fat_sector;
        if (!device_.write_sector(mirror_lba, sector))
            return false;
    }
    return true;
}

/// @brief Write a full cluster's data.
/// @return true on success.
bool Fat32Partition::write_cluster(uint32_t cluster, const uint8_t *buffer) {
    uint64_t lba = cluster_to_lba(cluster);
    uint64_t sectors = bpb_.sectors_per_cluster;
    for (uint64_t i = 0; i < sectors; ++i) {
        if (!device_.write_sector(lba + i, buffer + i * bpb_.bytes_per_sector))
            return false;
    }
    return true;
}

/// @brief Zero-fill a cluster.
/// @return true on success.
bool Fat32Partition::clear_cluster(uint32_t cluster) {
    uint64_t lba = cluster_to_lba(cluster);
    uint64_t sectors = bpb_.sectors_per_cluster;
    uint8_t zero[SECTOR_SIZE];
    __builtin_memset(zero, 0, sizeof(zero));
    for (uint64_t i = 0; i < sectors; ++i) {
        if (!device_.write_sector(lba + i, zero))
            return false;
    }
    return true;
}

/// @brief Find a free cluster in the FAT.
/// @return true if a free cluster was found.
bool Fat32Partition::find_free_cluster(uint32_t start_hint,
                                       uint32_t &out_cluster) {
    uint32_t total_clusters =
        (bpb_.total_sectors -
         (bpb_.reserved_sectors +
          static_cast<uint64_t>(bpb_.fat_count) * bpb_.fat_size)) /
        bpb_.sectors_per_cluster;

    uint32_t search = (start_hint >= 2) ? start_hint : 2;
    uint32_t max_search = search + total_clusters;

    for (uint32_t c = search; c < max_search; ++c) {
        uint32_t idx = c;
        if (idx >= 2 + total_clusters)
            idx = 2 + (idx - 2 - total_clusters);
        uint32_t entry = 0;
        if (!read_fat_entry(idx, entry))
            return false;
        if (is_free(entry)) {
            out_cluster = idx;
            return true;
        }
    }
    return false;
}

/// @brief Allocate a cluster and append it to a chain.
/// @return true on success.
bool Fat32Partition::alloc_cluster(uint32_t prev_cluster,
                                   uint32_t &out_cluster) {
    if (!find_free_cluster(prev_cluster ? prev_cluster + 1 : 2, out_cluster))
        return false;

    // Mark new cluster as EOF
    if (!write_fat_entry(out_cluster, 0x0FFFFFFF))
        return false;

    // Clear the new cluster
    if (!clear_cluster(out_cluster))
        return false;

    // Link from previous cluster if present
    if (prev_cluster != 0) {
        if (!write_fat_entry(prev_cluster, out_cluster))
            return false;
    }

    return true;
}

/// @brief Free an entire cluster chain starting from a given cluster.
void free_cluster_chain(Fat32Partition &fs, uint32_t first_cluster) {
    uint32_t current = first_cluster;
    while (!Fat32Partition::is_eof(current) &&
           !Fat32Partition::is_free(current)) {
        uint32_t next = 0;
        if (!fs.read_fat_entry(current, next))
            break;
        fs.write_fat_entry(current, 0);
        current = next;
    }
}

// -------------------------------------------------------------------
// Short name conversion
// -------------------------------------------------------------------

/// @brief Convert a filename to 8.3 short name (uppercased, space-padded).
void name_to_short_name(const char *name, uint8_t out[11]) {
    // Initialize with spaces
    for (int i = 0; i < 11; ++i)
        out[i] = ' ';

    // Handle special "." and ".." entries
    if (name[0] == '.' && name[1] == '\0') {
        out[0] = '.';
        return;
    }
    if (name[0] == '.' && name[1] == '.' && name[2] == '\0') {
        out[0] = '.';
        out[1] = '.';
        return;
    }

    size_t name_len = 0;
    while (name[name_len])
        ++name_len;

    // Find the dot for extension
    size_t dot_pos = name_len;
    for (size_t i = 0; i < name_len; ++i) {
        if (name[i] == '.') {
            dot_pos = i;
            break;
        }
    }

    // Name part (up to 8 chars)
    size_t j = 0;
    for (size_t i = 0; i < dot_pos && j < 8; ++i) {
        char c = name[i];
        if (c >= 'a' && c <= 'z')
            c -= 32; // uppercase
        out[j++] = static_cast<uint8_t>(c);
    }

    // Extension part (up to 3 chars)
    if (dot_pos < name_len) {
        j = 8;
        for (size_t i = dot_pos + 1; i < name_len && j < 11; ++i) {
            char c = name[i];
            if (c >= 'a' && c <= 'z')
                c -= 32;
            out[j++] = static_cast<uint8_t>(c);
        }
    }
}

// -------------------------------------------------------------------
// Directory entry manipulation
// -------------------------------------------------------------------

/// @brief Add a new directory entry to a directory cluster chain.
/// @return true on success.
bool add_dir_entry(Fat32Partition &fs, uint32_t dir_cluster,
                   const DirEntryRaw &raw) {
    uint32_t cluster_chain[256];
    uint32_t chain_len = 0;
    uint32_t current = dir_cluster;
    while (!Fat32Partition::is_eof(current) && chain_len < 256) {
        if (Fat32Partition::is_bad(current) || Fat32Partition::is_free(current))
            return false;
        cluster_chain[chain_len++] = current;
        if (!fs.read_fat_entry(current, current))
            return false;
    }

    uint32_t entries_per_cluster =
        fs.bpb().sectors_per_cluster * fs.bpb().bytes_per_sector / 32;

    uint32_t entry_cluster_size =
        fs.bpb().sectors_per_cluster * fs.bpb().bytes_per_sector;
    uint8_t *cluster_data =
        static_cast<uint8_t *>(MemPool::alloc(entry_cluster_size));
    if (!cluster_data)
        return false;

    for (uint32_t ci = 0; ci < chain_len; ++ci) {
        if (!fs.read_cluster(cluster_chain[ci], cluster_data)) {
            MemPool::free(cluster_data);
            return false;
        }

        for (uint32_t ei = 0; ei < entries_per_cluster; ++ei) {
            auto &entry = reinterpret_cast<DirEntryRaw &>(
                cluster_data[static_cast<size_t>(ei) * 32]);
            if (entry.name[0] == 0xE5 || entry.name[0] == 0x00) {
                __builtin_memcpy(cluster_data + static_cast<size_t>(ei) * 32,
                                 &raw, sizeof(raw));
                bool ok = fs.write_cluster(cluster_chain[ci], cluster_data);
                MemPool::free(cluster_data);
                return ok;
            }
        }
    }

    uint32_t new_cluster = 0;
    uint32_t last_cluster = cluster_chain[chain_len - 1];
    if (!fs.alloc_cluster(last_cluster, new_cluster)) {
        MemPool::free(cluster_data);
        return false;
    }

    __builtin_memset(cluster_data, 0, entry_cluster_size);
    __builtin_memcpy(cluster_data, &raw, sizeof(raw));
    bool ok = fs.write_cluster(new_cluster, cluster_data);
    MemPool::free(cluster_data);
    return ok;
}

/// @brief Remove a directory entry by name (mark as free).
/// @return true if found and removed.
bool remove_dir_entry(Fat32Partition &fs, uint32_t dir_cluster,
                      const char *name) {
    uint32_t cluster_chain[256];
    uint32_t chain_len = 0;
    uint32_t current = dir_cluster;
    while (!Fat32Partition::is_eof(current) && chain_len < 256) {
        if (Fat32Partition::is_bad(current) || Fat32Partition::is_free(current))
            return false;
        cluster_chain[chain_len++] = current;
        if (!fs.read_fat_entry(current, current))
            return false;
    }

    uint32_t entries_per_cluster =
        fs.bpb().sectors_per_cluster * fs.bpb().bytes_per_sector / 32;

    uint8_t short_name[11];
    name_to_short_name(name, short_name);

    uint32_t rm_cluster_size =
        fs.bpb().sectors_per_cluster * fs.bpb().bytes_per_sector;
    uint8_t *cluster_data =
        static_cast<uint8_t *>(MemPool::alloc(rm_cluster_size));
    if (!cluster_data)
        return false;

    for (uint32_t ci = 0; ci < chain_len; ++ci) {
        if (!fs.read_cluster(cluster_chain[ci], cluster_data)) {
            MemPool::free(cluster_data);
            return false;
        }

        for (uint32_t ei = 0; ei < entries_per_cluster; ++ei) {
            auto &entry = reinterpret_cast<DirEntryRaw &>(
                cluster_data[static_cast<size_t>(ei) * 32]);
            if (entry.name[0] == 0xE5 || entry.name[0] == 0x00)
                continue;
            if (entry.attrs == ATTR_LFN)
                continue;

            bool match = true;
            for (int i = 0; i < 11; ++i) {
                if (entry.name[i] != short_name[i]) {
                    match = false;
                    break;
                }
            }
            if (!match)
                continue;

            entry.name[0] = 0xE5;
            bool ok = fs.write_cluster(cluster_chain[ci], cluster_data);
            MemPool::free(cluster_data);
            return ok;
        }
    }
    MemPool::free(cluster_data);
    return false;
}

} // namespace fat32
} // namespace kernel
