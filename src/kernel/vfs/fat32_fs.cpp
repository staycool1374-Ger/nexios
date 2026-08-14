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

/// @file fat32_fs.cpp
/// @brief FAT32 VFS filesystem implementation (file/dir vnode ops, mounting).

#include <kernel/vfs/fat32_fs.hpp>
#include <kernel/core/global_state.hpp>
#include <kernel/memory/mempool.hpp>
#include <kernel/test/resource_tracker.hpp>
#include <string.hpp>

namespace kernel {
namespace vfs {

/// @brief Per-vnode private data for FAT32 vnodes.
struct Fat32VnodeData {
    fat32::Fat32Partition *fs; ///< Backing FAT32 partition.
    uint32_t cluster;          ///< First cluster of the file/directory.
    uint32_t size;             ///< File size in bytes.
    Vnode *parent;             ///< Parent directory vnode.
};

// Forward declarations
static Vnode fat32_root_vnode = {};
static Vnode *fat32_dir_lookup(Vnode &self, const char *name);
static Vnode *fat32_file_lookup(Vnode &, const char *);
static int fat32_dir_mkdir(Vnode &self, const char *name, uint16_t mode);
static int fat32_dir_unlink(Vnode &self, const char *name);

// Convert a path name to the 8.3 short name format string produced by
// format_short_name (strip trailing spaces, add '.' before extension).
// This ensures lookups match the name actually stored on disk.
/// @brief Convert a name to its 8.3 formatted short name string.
static void to_short_name_str(const char *name, char *out, size_t out_size) {
    if (!name || !out || out_size < 13)
        return;
    uint8_t raw[11];
    fat32::name_to_short_name(name, raw);
    size_t j = 0;
    for (size_t i = 0; i < 8 && raw[i] && raw[i] != ' '; ++i) {
        if (j < out_size - 1)
            out[j++] = static_cast<char>(raw[i]);
    }
    if (raw[8] && raw[8] != ' ') {
        if (j < out_size - 1)
            out[j++] = '.';
        for (size_t i = 8; i < 11 && raw[i] && raw[i] != ' '; ++i) {
            if (j < out_size - 1)
                out[j++] = static_cast<char>(raw[i]);
        }
    }
    out[j] = '\0';
}

// -------------------------------------------------------------------
// File operations
// -------------------------------------------------------------------

/// @brief Read data from a FAT32 file vnode.
static int64_t fat32_file_read(Vnode &self, uint8_t *buffer, uint64_t count,
                               uint64_t offset) {
    auto *data = static_cast<Fat32VnodeData *>(self.private_data);
    if (!data || !data->fs)
        return VFS_INVALID;
    return fat32::read_file(*data->fs, data->cluster, data->size, offset, count,
                            buffer);
}

/// @brief Write to a FAT32 file (not supported, read-only).
static int64_t fat32_file_write(Vnode &, const uint8_t *, uint64_t, uint64_t) {
    return VFS_INVALID;
}

/// @brief Open a FAT32 file vnode.
static int fat32_file_open(Vnode &, uint64_t) {
    return 0;
}

/// @brief Close a FAT32 file vnode, freeing private data.
static void fat32_file_close(Vnode &self) {
    if (self.private_data) {
        auto *data = static_cast<Fat32VnodeData *>(self.private_data);
        MemPool::free(data);
        self.private_data = nullptr;
    }
    test::ResourceTracker::instance().track_vnode_remove();
    MemPool::free(&self);
}

/// @brief Seek within a FAT32 file.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
static int64_t fat32_file_lseek(Vnode &self, int64_t offset, int whence,
                                uint64_t *out_pos) {
    auto *data = static_cast<Fat32VnodeData *>(self.private_data);
    if (!data)
        return VFS_INVALID;
    uint64_t new_pos = 0;
    switch (whence) {
    case SEEK_SET:
        new_pos = static_cast<uint64_t>(offset);
        break;
    case SEEK_CUR:
        new_pos = *out_pos + static_cast<uint64_t>(offset);
        break;
    case SEEK_END:
        new_pos = data->size + static_cast<uint64_t>(offset);
        break;
    default:
        return VFS_INVALID;
    }
    if (new_pos > data->size)
        new_pos = data->size;
    *out_pos = new_pos;
    return static_cast<int64_t>(new_pos);
}

/// @brief Get FAT32 file status.
static int fat32_file_fstat(Vnode &self, VfsStat &st) {
    auto *data = static_cast<Fat32VnodeData *>(self.private_data);
    if (!data)
        return VFS_INVALID;
    st.st_size = data->size;
    st.st_mode = S_IFREG;
    return 0;
}

/// @brief I/O control on FAT32 file (not supported).
static int fat32_file_ioctl(Vnode &, uint64_t, kernel::CheckedPtr<uint8_t>) {
    return VFS_INVALID;
}

/// @brief Read directory on FAT32 file (not supported).
static int fat32_file_readdir(Vnode &, uint64_t &, Dirent &) {
    return VFS_INVALID;
}

/// @brief Look up child in FAT32 file (not supported).
static Vnode *fat32_file_lookup(Vnode &, const char *) {
    return nullptr;
}

static const VnodeOps fat32_file_ops = {
    fat32_file_read,   fat32_file_write, fat32_file_open,  fat32_file_close,
    fat32_file_lseek,  fat32_file_fstat, fat32_file_ioctl, fat32_file_readdir,
    fat32_file_lookup, nullptr,          nullptr,
    nullptr, // create
};

// -------------------------------------------------------------------
// Directory operations
// -------------------------------------------------------------------

/// @brief Read from a FAT32 directory (not supported).
static int64_t fat32_dir_read(Vnode &, uint8_t *, uint64_t, uint64_t) {
    return VFS_INVALID;
}

/// @brief Write to a FAT32 directory (not supported).
static int64_t fat32_dir_write(Vnode &, const uint8_t *, uint64_t, uint64_t) {
    return VFS_INVALID;
}

/// @brief Open a FAT32 directory vnode.
static int fat32_dir_open(Vnode &, uint64_t) {
    return 0;
}

/// @brief Close a FAT32 directory vnode, freeing private data.
static void fat32_dir_close(Vnode &self) {
    if (&self == &fat32_root_vnode) {
        self.refcount = 1;
        return;
    }
    if (self.private_data) {
        auto *data = static_cast<Fat32VnodeData *>(self.private_data);
        MemPool::free(data);
        self.private_data = nullptr;
    }
    test::ResourceTracker::instance().track_vnode_remove();
    MemPool::free(&self);
}

/// @brief Seek within a FAT32 directory.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
static int64_t fat32_dir_lseek(Vnode &self, int64_t offset, int whence,
                               uint64_t *out_pos) {
    auto *data = static_cast<Fat32VnodeData *>(self.private_data);
    if (!data)
        return VFS_INVALID;
    uint64_t new_pos = 0;
    switch (whence) {
    case SEEK_SET:
        new_pos = static_cast<uint64_t>(offset);
        break;
    case SEEK_CUR:
        new_pos = *out_pos + static_cast<uint64_t>(offset);
        break;
    case SEEK_END:
    default:
        return VFS_INVALID;
    }
    *out_pos = new_pos;
    return static_cast<int64_t>(new_pos);
}

/// @brief Get FAT32 directory status.
static int fat32_dir_fstat(Vnode &, VfsStat &st) {
    st.st_size = 0;
    st.st_mode = S_IFDIR;
    return 0;
}

/// @brief I/O control on FAT32 directory (not supported).
static int fat32_dir_ioctl(Vnode &, uint64_t, kernel::CheckedPtr<uint8_t>) {
    return VFS_INVALID;
}

/// @brief Read a directory entry from a FAT32 directory.
static int fat32_dir_readdir(Vnode &self, uint64_t &pos, Dirent &dent) {
    auto *data = static_cast<Fat32VnodeData *>(self.private_data);
    if (!data || !data->fs)
        return VFS_INVALID;

    fat32::DirEntry entry = {};
    if (!fat32::read_dir_entry(*data->fs, data->cluster, pos, entry))
        return VFS_INVALID;

    size_t idx = 0;
    while (entry.name[idx] && idx < 63) {
        dent.d_name[idx] = entry.name[idx];
        ++idx;
    }
    dent.d_name[idx] = '\0';
    dent.d_ino = 1;
    return 0;
}

static const VnodeOps fat32_dir_ops = {
    fat32_dir_read,   fat32_dir_write, fat32_dir_open,   fat32_dir_close,
    fat32_dir_lseek,  fat32_dir_fstat, fat32_dir_ioctl,  fat32_dir_readdir,
    fat32_dir_lookup, fat32_dir_mkdir, fat32_dir_unlink,
    nullptr, // create
};

// -------------------------------------------------------------------
// mkdir / unlink
// -------------------------------------------------------------------

/// @brief Create a subdirectory in a FAT32 directory.
static int fat32_dir_mkdir(Vnode &self, const char *name, uint16_t) {
    auto *data = static_cast<Fat32VnodeData *>(self.private_data);
    if (!data || !data->fs)
        return VFS_INVALID;

    // Check if entry already exists (using short name format)
    char short_name[13];
    to_short_name_str(name, short_name, sizeof(short_name));
    fat32::DirEntry existing{};
    if (fat32::lookup_in_dir(*data->fs, data->cluster, short_name, existing))
        return errors::VFS_ERR_EXISTS; // already exists

    // Allocate a cluster for the new directory
    uint32_t new_cluster = 0;
    if (!data->fs->alloc_cluster(0, new_cluster))
        return VFS_INVALID;

    // Write "." entry pointing to self
    fat32::DirEntryRaw dot_entry{};
    __builtin_memset(&dot_entry, 0, sizeof(dot_entry));
    fat32::name_to_short_name(".", dot_entry.name);
    dot_entry.attrs = fat32::ATTR_DIRECTORY;
    dot_entry.cluster_low = static_cast<uint16_t>(new_cluster & 0xFFFF);
    dot_entry.cluster_high =
        static_cast<uint16_t>((new_cluster >> 16) & 0xFFFF);

    // Write ".." entry pointing to parent
    fat32::DirEntryRaw dotdot_entry{};
    __builtin_memset(&dotdot_entry, 0, sizeof(dotdot_entry));
    fat32::name_to_short_name("..", dotdot_entry.name);
    dotdot_entry.attrs = fat32::ATTR_DIRECTORY;
    uint32_t parent_cluster = data->cluster;
    dotdot_entry.cluster_low = static_cast<uint16_t>(parent_cluster & 0xFFFF);
    dotdot_entry.cluster_high =
        static_cast<uint16_t>((parent_cluster >> 16) & 0xFFFF);

    // Write entries into the new directory's first data area.
    // Allocate the scratch buffer from the heap: a 32 KiB *stack* buffer here
    // overflowed the shell's 64 KiB kernel stack (RSP dropped below its base
    // during `mkdir /mnt/<name>`, wedging the scheduler). Only `cluster_size`
    // bytes are ever used, so size the allocation to that.
    uint32_t cluster_size =
        data->fs->bpb().sectors_per_cluster * data->fs->bpb().bytes_per_sector;
    uint8_t *dir_data = static_cast<uint8_t *>(MemPool::alloc(cluster_size));
    if (!dir_data) {
        fat32::free_cluster_chain(*data->fs, new_cluster);
        return VFS_INVALID;
    }
    __builtin_memset(dir_data, 0, cluster_size);
    __builtin_memcpy(dir_data, &dot_entry, sizeof(dot_entry));
    __builtin_memcpy(dir_data + 32, &dotdot_entry, sizeof(dotdot_entry));
    if (!data->fs->write_cluster(new_cluster, dir_data)) {
        MemPool::free(dir_data);
        fat32::free_cluster_chain(*data->fs, new_cluster);
        return VFS_INVALID;
    }
    MemPool::free(dir_data);

    // Add entry for new directory in the parent
    fat32::DirEntryRaw new_entry{};
    __builtin_memset(&new_entry, 0, sizeof(new_entry));
    fat32::name_to_short_name(name, new_entry.name);
    new_entry.attrs = fat32::ATTR_DIRECTORY;
    new_entry.cluster_low = static_cast<uint16_t>(new_cluster & 0xFFFF);
    new_entry.cluster_high =
        static_cast<uint16_t>((new_cluster >> 16) & 0xFFFF);

    if (!fat32::add_dir_entry(*data->fs, data->cluster, new_entry)) {
        fat32::free_cluster_chain(*data->fs, new_cluster);
        return VFS_INVALID;
    }

    return 0;
}

/// @brief Remove a file or empty directory from a FAT32 directory.
static int fat32_dir_unlink(Vnode &self, const char *name) {
    auto *data = static_cast<Fat32VnodeData *>(self.private_data);
    if (!data || !data->fs)
        return VFS_INVALID;

    char short_name[13];
    to_short_name_str(name, short_name, sizeof(short_name));

    fat32::DirEntry entry{};
    if (!fat32::lookup_in_dir(*data->fs, data->cluster, short_name, entry))
        return VFS_INVALID;

    // If it's a directory, check that it is empty
    if (entry.is_directory && entry.cluster != 0) {
        // Count entries in the target directory (skip . and ..)
        uint64_t count_pos = 0;
        fat32::DirEntry child_entry{};
        uint64_t real_entries = 0;
        while (fat32::read_dir_entry(*data->fs, entry.cluster, count_pos,
                                     child_entry)) {
            if (child_entry.valid && strcmp(child_entry.name, ".") != 0 &&
                strcmp(child_entry.name, "..") != 0) {
                ++real_entries;
            }
        }
        // A new directory has only . and .. — so real_entries should be 0
        if (real_entries > 0)
            return VFS_INVALID; // not empty

        // Free the directory's cluster chain
        fat32::free_cluster_chain(*data->fs, entry.cluster);
    } else if (!entry.is_directory && entry.cluster != 0) {
        // Free file's cluster chain
        fat32::free_cluster_chain(*data->fs, entry.cluster);
    }

    // Remove the entry from the parent
    if (!fat32::remove_dir_entry(*data->fs, data->cluster, short_name))
        return VFS_INVALID;

    return 0;
}

/// @brief Look up a child entry by name in a FAT32 directory.
static Vnode *fat32_dir_lookup(Vnode &self, const char *name) {
    auto *data = static_cast<Fat32VnodeData *>(self.private_data);
    if (!data || !data->fs)
        return nullptr;

    char short_name[13];
    to_short_name_str(name, short_name, sizeof(short_name));

    fat32::DirEntry entry = {};
    if (!fat32::lookup_in_dir(*data->fs, data->cluster, short_name, entry))
        return nullptr;

    auto *vdata =
        static_cast<Fat32VnodeData *>(MemPool::alloc(sizeof(Fat32VnodeData)));
    if (!vdata)
        return nullptr;
    vdata->fs = data->fs;
    vdata->cluster = entry.cluster;
    vdata->size = entry.size;
    vdata->parent = &self;

    auto *vnode = static_cast<Vnode *>(MemPool::alloc(sizeof(Vnode)));
    if (!vnode) {
        MemPool::free(vdata);
        return nullptr;
    }

    test::ResourceTracker::instance().track_vnode_add();
    vnode->ops = entry.is_directory ? &fat32_dir_ops : &fat32_file_ops;
    vnode->ino = 1;
    vnode->size = entry.size;
    vnode->mode = entry.is_directory ? S_IFDIR : S_IFREG;
    vnode->private_data = vdata;
    vnode->refcount = 1;
    vnode->parent = &self;
    return vnode;
}

// -------------------------------------------------------------------
// Filesystem root
// -------------------------------------------------------------------

static Fat32VnodeData fat32_root_vdata{};
static bool root_initialized = false;

/// @brief Get the FAT32 root vnode (lazily initialised).
static Vnode *fat32_get_root() {
    if (!kernel::gs::get_fat32_partition() ||
        !kernel::gs::get_fat32_partition()->bpb().valid)
        return nullptr;

    if (!root_initialized ||
        fat32_root_vdata.fs != kernel::gs::get_fat32_partition()) {
        fat32_root_vdata.fs = kernel::gs::get_fat32_partition();
        fat32_root_vdata.cluster =
            kernel::gs::get_fat32_partition()->bpb().root_cluster;
        fat32_root_vdata.size = 0;

        fat32_root_vnode.ops = &fat32_dir_ops;
        fat32_root_vnode.ino = 0;
        fat32_root_vnode.size = 0;
        fat32_root_vnode.mode = S_IFDIR;
        fat32_root_vnode.private_data = &fat32_root_vdata;
        fat32_root_vnode.refcount = 1;
        fat32_root_vnode.parent = nullptr;
        root_initialized = true;
    }
    return &fat32_root_vnode;
}

Filesystem fat32_fs = {
    "fat32",
    fat32_get_root,
};

/// @brief Mount the FAT32 filesystem at the given mount point.
/// @return 0 on success, VFS_INVALID on error.
int mount_fat32(const char *mount_point) {
    if (!kernel::gs::get_fat32_partition() ||
        !kernel::gs::get_fat32_partition()->bpb().valid)
        return VFS_INVALID;
    return mount(fat32_fs, mount_point);
}

} // namespace vfs
} // namespace kernel
