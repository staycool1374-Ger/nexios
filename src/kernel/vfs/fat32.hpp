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

/// @file fat32.hpp
/// @brief FAT32 on-disk structures, partition class, and directory helpers.

#pragma once

#include <types.hpp>
#include <kernel/driver/block_device.hpp>

namespace kernel {
namespace fat32 {

static constexpr uint64_t SECTOR_SIZE = 512;

/// @brief Parsed MBR partition entry.
struct MbrPartition {
    uint8_t status;         ///< Partition status byte (0x80 = active).
    uint8_t partition_type; ///< Partition type code.
    uint32_t lba_start;     ///< LBA of the first sector.
    uint32_t sector_count;  ///< Number of sectors in the partition.
    bool valid;             ///< Whether this entry is valid.
};

/// @brief Parsed FAT32 BPB fields.
struct Fat32Bpb {
    uint16_t bytes_per_sector;   ///< BPB_BytsPerSec
    uint8_t sectors_per_cluster; ///< BPB_SecPerClus
    uint16_t reserved_sectors;   ///< BPB_RsvdSecCnt
    uint8_t fat_count;           ///< BPB_NumFATs
    uint32_t fat_size;           ///< BPB_FATSz32
    uint32_t root_cluster;       ///< BPB_RootClus
    uint32_t total_sectors;      ///< BPB_TotSec32
    uint16_t fs_info_sector;     ///< BPB_FSInfo
    bool valid;                  ///< Whether the BPB was parsed successfully.
};

/// @brief A mounted FAT32 partition, backed by a BlockDevice.
class Fat32Partition {
  public:
    explicit Fat32Partition(block::BlockDevice &device);

    /// @brief Attempt to mount the device by scanning for a FAT32 partition.
    /// @return true if a valid FAT32 partition was found and BPB parsed.
    bool mount();

    /// @brief Mount at a specific partition LBA (for mock/test use).
    bool mount_at(uint64_t partition_lba);

    /// @brief Convert a cluster number to the first sector's LBA.
    uint64_t cluster_to_lba(uint32_t cluster) const;

    /// @brief Read the FAT entry for a given cluster.
    bool read_fat_entry(uint32_t cluster, uint32_t &next_cluster) const;

    /// @brief Read a full cluster's data.
    bool read_cluster(uint32_t cluster, uint8_t *buffer) const;

    /// @brief Write a FAT entry for a given cluster.
    bool write_fat_entry(uint32_t cluster, uint32_t value);

    /// @brief Write a full cluster's data.
    bool write_cluster(uint32_t cluster, const uint8_t *buffer);

    /// @brief Zero-fill a cluster.
    bool clear_cluster(uint32_t cluster);

    /// @brief Find a free cluster in the FAT.
    /// @param start_cluster Hint where to start searching (0 = from beginning).
    /// @param out_cluster Output: the free cluster number.
    /// @return true if found.
    bool find_free_cluster(uint32_t start_cluster, uint32_t &out_cluster);

    /// @brief Allocate a cluster and append it to a chain.
    /// @param prev_cluster The last cluster in the chain (0 = no chain).
    /// @param out_cluster Output: the newly allocated cluster number.
    /// @return true on success. The new cluster is zero-filled and marked EOF.
    bool alloc_cluster(uint32_t prev_cluster, uint32_t &out_cluster);

    /// @brief Check if a FAT entry is end-of-chain.
    static bool is_eof(uint32_t entry) {
        return entry >= 0x0FFFFFF8;
    }

    /// @brief Check if a cluster is free.
    static bool is_free(uint32_t entry) {
        return entry == 0;
    }

    /// @brief Check if a cluster is bad.
    static bool is_bad(uint32_t entry) {
        return entry == 0x0FFFFFF7;
    }

    const Fat32Bpb &bpb() const {
        return bpb_;
    }
    uint64_t partition_lba() const {
        return partition_start_;
    }
    block::BlockDevice &device() const {
        return device_;
    }

  private:
    bool parse_mbr();
    bool parse_bpb(uint64_t lba);
    bool find_fat32_partition();

    block::BlockDevice &device_;
    uint64_t partition_start_ = 0;
    Fat32Bpb bpb_ = {};
};

static constexpr uint8_t ATTR_READ_ONLY = 0x01;
static constexpr uint8_t ATTR_HIDDEN = 0x02;
static constexpr uint8_t ATTR_SYSTEM = 0x04;
static constexpr uint8_t ATTR_VOLUME_ID = 0x08;
static constexpr uint8_t ATTR_DIRECTORY = 0x10;
static constexpr uint8_t ATTR_ARCHIVE = 0x20;
static constexpr uint8_t ATTR_LFN = 0x0F;

/// @brief Raw 32-byte FAT32 directory entry.
struct DirEntryRaw {
    uint8_t name[11];        ///< 8.3 short name (space-padded).
    uint8_t attrs;           ///< File attributes.
    uint8_t nt_reserved;     ///< Reserved for Windows NT.
    uint8_t creation_tenths; ///< Creation time, tenths of second.
    uint16_t creation_time;  ///< Creation time.
    uint16_t creation_date;  ///< Creation date.
    uint16_t access_date;    ///< Last access date.
    uint16_t cluster_high;   ///< High 16 bits of first cluster.
    uint16_t modify_time;    ///< Last modification time.
    uint16_t modify_date;    ///< Last modification date.
    uint16_t cluster_low;    ///< Low 16 bits of first cluster.
    uint32_t file_size;      ///< File size in bytes.
} __attribute__((packed));

/// @brief Parsed directory entry.
struct DirEntry {
    char name[13];     ///< 8.3 null-terminated (8+1+3+1).
    uint8_t attrs;     ///< File attributes.
    uint32_t cluster;  ///< First cluster number.
    uint32_t size;     ///< File size in bytes.
    bool is_directory; ///< True if this is a directory.
    bool valid;        ///< Whether this entry is valid.
};

/// @brief Read directory entries from a cluster chain.
/// @param fs       Mounted partition.
/// @param cluster  Starting cluster of the directory.
/// @param pos      Entry index (0-based), updated after read.
/// @param entry    Output parsed entry.
/// @return true if an entry was read, false at end of directory.
bool read_dir_entry(Fat32Partition &fs, uint32_t cluster, uint64_t &pos,
                    DirEntry &entry);

/// @brief Look up a name in a directory cluster chain.
/// @param fs      Mounted partition.
/// @param dir_cluster  Starting cluster of the directory.
/// @param name    8.3 short name to find.
/// @param entry   Output parsed entry.
/// @return true if found, false if not found or error.
bool lookup_in_dir(Fat32Partition &fs, uint32_t dir_cluster, const char *name,
                   DirEntry &entry);

/// @brief Look up a name, also reporting the on-disk slot index.
/// Same as lookup_in_dir, but stores the 0-based directory-entry slot
/// of the match in out_index (issue #333: stable identity key input).
/// Slots are never compacted (remove marks 0xE5), so the index is
/// stable until an unlink+reuse cycle recycles the slot.
/// @param[out] out_index Slot index of the matched entry (valid iff
///             the return value is true).
/// @return true if found, false if not found or error.
bool lookup_in_dir_at(Fat32Partition &fs, uint32_t dir_cluster,
                      const char *name, DirEntry &entry, uint64_t &out_index);

/// @brief Read data from a file (cluster chain).
/// @param fs        Mounted partition.
/// @param cluster   First cluster of the file.
/// @param file_size Total file size.
/// @param offset    Byte offset to start reading.
/// @param count     Number of bytes to read.
/// @param buffer    Output buffer.
/// @return Number of bytes read, or -1 on error.
int64_t read_file(Fat32Partition &fs, uint32_t cluster, uint32_t file_size,
                  uint64_t offset, uint64_t count, uint8_t *buffer);

/// @brief Format the short name from a raw directory entry into "NAME.EXT".
void format_short_name(const uint8_t raw_name[11], char *out, size_t out_size);

/// @brief Find the first FAT32 partition in the MBR.
/// @return true if partition_lba_out has been set.
bool find_fat32_in_mbr(block::BlockDevice &device, uint64_t &partition_lba_out);

/// @brief Convert a filename (e.g. "MyFile.txt") to 8.3 short name bytes.
///        Pads with spaces, uppercases, strips dots.
void name_to_short_name(const char *name, uint8_t out[11]);

/// @brief Add a new directory entry to a directory cluster chain.
/// @param fs         Mounted partition.
/// @param dir_cluster Starting cluster of the target directory.
/// @param raw        The raw 32-byte entry to write.
/// @return true on success, false if directory is full (no free entry).
bool add_dir_entry(Fat32Partition &fs, uint32_t dir_cluster,
                   const DirEntryRaw &raw);

/// @brief Remove (mark as free) a directory entry by name.
/// @param fs         Mounted partition.
/// @param dir_cluster Starting cluster of the target directory.
/// @param name       8.3 short name to remove.
/// @return true if found and removed, false if not found.
bool remove_dir_entry(Fat32Partition &fs, uint32_t dir_cluster,
                      const char *name);

/// @brief Free an entire cluster chain starting from a given cluster.
void free_cluster_chain(Fat32Partition &fs, uint32_t first_cluster);

} // namespace fat32
} // namespace kernel
