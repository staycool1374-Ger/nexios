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

/// @file initrd_fs.cpp
/// @brief Initial ramdisk filesystem implementation (read-only file vnodes).

#include <kernel/vfs/initrd_fs.hpp>
#include <kernel/memory/mempool.hpp>
#include <kernel/test/resource_tracker.hpp>
#include <initrd/initrd.hpp>
#include <string.hpp>
#include <utils.hpp>

namespace kernel {
namespace vfs {

/// @brief Per-vnode private data for initrd file vnodes.
struct InitrdFileNode {
    const uint8_t *data; ///< Pointer to file data in the initrd image.
    uint64_t size;       ///< File size in bytes.
    Vnode parent;        ///< Parent directory vnode.
};

static Vnode initrd_root = {};
static bool root_initialized = false;

/// @brief Read data from an initrd file vnode.
static int64_t initrd_file_read(Vnode &self, uint8_t *buffer, uint64_t count,
                                uint64_t offset) {
    auto *finfo = static_cast<InitrdFileNode *>(self.private_data);
    if (!finfo || !finfo->data)
        return VFS_INVALID;
    if (offset >= finfo->size)
        return 0;
    uint64_t avail = finfo->size - offset;
    if (count > avail)
        count = avail;
    memcpy(buffer, finfo->data + offset, count);
    return static_cast<int64_t>(count);
}

/// @brief Write to an initrd file (not supported, read-only).
static int64_t initrd_file_write(Vnode &, const uint8_t *, uint64_t, uint64_t) {
    return VFS_INVALID;
}

/// @brief Open an initrd file vnode.
static int initrd_file_open(Vnode &, uint64_t) {
    return 0;
}

/// @brief Close an initrd file vnode, freeing private data.
static void initrd_file_close(Vnode &self) {
    if (self.private_data) {
        auto *finfo = static_cast<InitrdFileNode *>(self.private_data);
        kernel::MemPool::free(finfo);
        self.private_data = nullptr;
    }
    kernel::test::ResourceTracker::instance().track_vnode_remove();
    kernel::MemPool::free(&self);
}

/// @brief Seek within an initrd file.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
static int64_t initrd_file_lseek(Vnode &self, int64_t offset, int whence,
                                 uint64_t *out_pos) {
    auto *finfo = static_cast<InitrdFileNode *>(self.private_data);
    if (!finfo)
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
        new_pos = finfo->size + static_cast<uint64_t>(offset);
        break;
    default:
        return VFS_INVALID;
    }
    if (new_pos > finfo->size)
        new_pos = finfo->size;
    *out_pos = new_pos;
    return static_cast<int64_t>(new_pos);
}

/// @brief Get initrd file status.
static int initrd_file_fstat(Vnode &self, VfsStat &st) {
    auto *finfo = static_cast<InitrdFileNode *>(self.private_data);
    if (!finfo)
        return VFS_INVALID;
    st.st_size = finfo->size;
    st.st_mode = S_IFREG;
    return 0;
}

/// @brief I/O control on initrd file (not supported).
static int initrd_file_ioctl(Vnode &, uint64_t, kernel::CheckedPtr<uint8_t>) {
    return VFS_INVALID;
}

/// @brief Read directory on initrd file (not supported).
static int initrd_file_readdir(Vnode &, uint64_t &, Dirent &) {
    return VFS_INVALID;
}

/// @brief Look up child in initrd file (not supported).
static Vnode *initrd_file_lookup(Vnode &, const char *) {
    return nullptr;
}

static const VnodeOps initrd_file_ops = {
    initrd_file_read,
    initrd_file_write,
    initrd_file_open,
    initrd_file_close,
    initrd_file_lseek,
    initrd_file_fstat,
    initrd_file_ioctl,
    initrd_file_readdir,
    initrd_file_lookup,
    nullptr,
    nullptr,
    nullptr, // create
};

// ── subdirectory support (issue #274) ──

/// @brief Bound for cpio entry scans when probing for a subdirectory.
static constexpr uint64_t INITRD_MAX_SCAN = 1024;
/// @brief Capacity of the cpio-relative subdir path in InitrdDirNode.
static constexpr size_t INITRD_MAX_SUBDIR = 128;
/// @brief Capacity of the joined "<subdir>/<name>" lookup buffer.
static constexpr size_t INITRD_MAX_JOINED = 256;

/// @brief Per-vnode private data for initrd subdirectory vnodes.
struct InitrdDirNode {
    char subdir[INITRD_MAX_SUBDIR]; ///< Cpio-relative subdir, e.g. "bin".
};

/// @brief Build a file vnode for an initrd image range.
/// Shared by the root and subdirectory lookups; the caller owns the
/// returned vnode (track_vnode_add) and releases it exactly once.
/// @return Owned file vnode, or nullptr on allocation failure.
static Vnode *initrd_make_file_vnode(const initrd::InitrdFile &file) {
    auto *finfo = static_cast<InitrdFileNode *>(
        kernel::MemPool::alloc(sizeof(InitrdFileNode)));
    if (!finfo)
        return nullptr;
    finfo->data = file.data;
    finfo->size = file.size;
    finfo->parent = initrd_root;

    auto *vnode = static_cast<Vnode *>(kernel::MemPool::alloc(sizeof(Vnode)));
    if (!vnode) {
        kernel::MemPool::free(finfo);
        return nullptr;
    }
    vnode->parent = &initrd_root;
    kernel::test::ResourceTracker::instance().track_vnode_add();
    vnode->ops = &initrd_file_ops;
    vnode->ino = 1;
    vnode->size = file.size;
    vnode->mode = S_IFREG;
    vnode->private_data = finfo;
    vnode->refcount = 1;
    return vnode;
}

/// @brief Strip a leading "./" cpio prefix in place (view only).
static const char *initrd_strip_dot(const char *name) {
    if (name[0] == '.' && name[1] == '/')
        return name + 2;
    return name;
}

/// @brief Probe the archive for subdirectory evidence under `name`.
/// A bounded readdir walk classifies `name` as an explicit directory
/// entry ("./<name>" with is_dir) or as an implied parent ("./<name>/..."
/// children exist). Either one makes `name` a subdirectory.
/// @param[out] is_dir True when `name` is an explicit directory entry.
static bool initrd_probe_subdir(const char *name, bool &is_dir) {
    is_dir = false;
    bool has_child = false;
    uint64_t pos = 0;
    initrd::InitrdEntry entry = {};
    for (uint64_t scanned = 0; scanned < INITRD_MAX_SCAN; ++scanned) {
        if (!initrd::readdir(&pos, &entry))
            break;
        const char *stripped = initrd_strip_dot(entry.name);
        size_t i = 0;
        while (name[i] && name[i] == stripped[i])
            ++i;
        if (name[i] != '\0')
            continue;
        if (stripped[i] == '\0') {
            if (entry.is_dir)
                is_dir = true;
            continue;
        }
        if (stripped[i] == '/')
            has_child = true;
    }
    return is_dir || has_child;
}

/// @brief Read from an initrd subdirectory (not supported).
static int64_t initrd_dir_read(Vnode &, uint8_t *, uint64_t, uint64_t) {
    return VFS_INVALID;
}

/// @brief Write to an initrd subdirectory (not supported, read-only).
static int64_t initrd_dir_write(Vnode &, const uint8_t *, uint64_t,
                                uint64_t) {
    return VFS_INVALID;
}

/// @brief Open an initrd subdirectory vnode.
static int initrd_dir_open(Vnode &, uint64_t) {
    return 0;
}

/// @brief Close an initrd subdirectory vnode, freeing private data.
/// Lifecycle mirrors initrd_file_close exactly: free the private node if
/// present, drop the tracker count, then free the vnode itself.
static void initrd_dir_close(Vnode &self) {
    if (self.private_data) {
        kernel::MemPool::free(self.private_data);
        self.private_data = nullptr;
    }
    kernel::test::ResourceTracker::instance().track_vnode_remove();
    kernel::MemPool::free(&self);
}

/// @brief Seek within an initrd subdirectory (not supported).
static int64_t initrd_dir_lseek(Vnode &, int64_t, int,
                                uint64_t *) {
    return VFS_INVALID;
}

/// @brief Get initrd subdirectory status.
static int initrd_dir_fstat(Vnode &, VfsStat &vfs_stat) {
    vfs_stat.st_size = 0;
    vfs_stat.st_mode = S_IFDIR;
    return 0;
}

/// @brief I/O control on initrd subdirectory (not supported).
static int initrd_dir_ioctl(Vnode &, uint64_t, kernel::CheckedPtr<uint8_t>) {
    return VFS_INVALID;
}

/// @brief Read a directory entry from an initrd subdirectory.
/// Lists immediate children ("./<subdir>/<child>" with no further '/').
static int initrd_dir_readdir(Vnode &self, uint64_t &pos, Dirent &dent) {
    auto *dinfo = static_cast<InitrdDirNode *>(self.private_data);
    if (!dinfo || dinfo->subdir[0] == '\0')
        return VFS_INVALID;
    size_t sublen = 0;
    while (sublen < INITRD_MAX_SUBDIR && dinfo->subdir[sublen])
        ++sublen;
    initrd::InitrdEntry entry = {};
    for (uint64_t scanned = 0; scanned < INITRD_MAX_SCAN; ++scanned) {
        if (!initrd::readdir(&pos, &entry))
            break;
        const char *stripped = initrd_strip_dot(entry.name);
        size_t i = 0;
        while (i < sublen && dinfo->subdir[i] == stripped[i])
            ++i;
        if (i != sublen || stripped[i] != '/')
            continue;
        const char *child = stripped + i + 1;
        if (child[0] == '\0')
            continue;
        size_t j = 0;
        while (child[j] && child[j] != '/')
            ++j;
        if (child[j] != '\0')
            continue;
        size_t k = 0;
        while (k < j && k < 63) {
            dent.d_name[k] = child[k];
            ++k;
        }
        dent.d_name[k] = '\0';
        dent.d_ino = 2;
        return 0;
    }
    return VFS_INVALID;
}

/// @brief Look up a file by name in an initrd subdirectory.
/// Joins "<subdir>/<name>" fail-closed (overlong input yields nullptr)
/// and delegates to the flat archive matcher.
static Vnode *initrd_dir_lookup(Vnode &self, const char *name) {
    auto *dinfo = static_cast<InitrdDirNode *>(self.private_data);
    if (!dinfo || !name)
        return nullptr;
    size_t sublen = 0;
    while (sublen < INITRD_MAX_SUBDIR && dinfo->subdir[sublen])
        ++sublen;
    size_t namelen = 0;
    while (name[namelen])
        ++namelen;
    if (sublen == 0 || sublen + 1 + namelen + 1 > INITRD_MAX_JOINED)
        return nullptr;
    char joined[INITRD_MAX_JOINED];
    for (size_t i = 0; i < sublen; ++i)
        joined[i] = dinfo->subdir[i];
    joined[sublen] = '/';
    for (size_t i = 0; i <= namelen; ++i)
        joined[sublen + 1 + i] = name[i];
    initrd::InitrdFile file = initrd::find(joined);
    if (!file.data)
        return nullptr;
    return initrd_make_file_vnode(file);
}

static const VnodeOps initrd_dir_ops = {
    initrd_dir_read,
    initrd_dir_write,
    initrd_dir_open,
    initrd_dir_close,
    initrd_dir_lseek,
    initrd_dir_fstat,
    initrd_dir_ioctl,
    initrd_dir_readdir,
    initrd_dir_lookup,
    nullptr,
    nullptr,
    nullptr, // create
};

/// @brief Build a subdirectory vnode for the cpio-relative `subdir`.
/// The parent link points at the static initrd_root (borrowed, never
/// freed) — transient dir vnodes are never stored as another vnode's
/// parent, so traversal release cannot dangle it.
/// @return Owned dir vnode, or nullptr on allocation failure.
static Vnode *initrd_make_dir_vnode(const char *subdir, size_t sublen) {
    if (!subdir || sublen == 0 || sublen >= INITRD_MAX_SUBDIR)
        return nullptr;
    auto *dinfo = static_cast<InitrdDirNode *>(
        kernel::MemPool::alloc(sizeof(InitrdDirNode)));
    if (!dinfo)
        return nullptr;
    for (size_t i = 0; i <= sublen; ++i)
        dinfo->subdir[i] = subdir[i];

    auto *vnode = static_cast<Vnode *>(kernel::MemPool::alloc(sizeof(Vnode)));
    if (!vnode) {
        kernel::MemPool::free(dinfo);
        return nullptr;
    }
    vnode->parent = &initrd_root;
    kernel::test::ResourceTracker::instance().track_vnode_add();
    vnode->ops = &initrd_dir_ops;
    vnode->ino = 2;
    vnode->size = 0;
    vnode->mode = S_IFDIR;
    vnode->private_data = dinfo;
    vnode->refcount = 1;
    return vnode;
}

// ── root directory ──

/// @brief Read from initrd root (not supported).
static int64_t initrd_root_read(Vnode &, uint8_t *, uint64_t, uint64_t) {
    return VFS_INVALID;
}
/// @brief Write to initrd root (not supported).
static int64_t initrd_root_write(Vnode &, const uint8_t *, uint64_t, uint64_t) {
    return VFS_INVALID;
}
/// @brief Open the initrd root.
static int initrd_root_open(Vnode &, uint64_t) {
    return 0;
}
/// @brief Close the initrd root.
static void initrd_root_close(Vnode &) {
}

/// @brief Seek within initrd root (delegates to file lseek).
static int64_t initrd_root_lseek(Vnode &self, int64_t offset, int whence,
                                 uint64_t *out_pos) {
    return initrd_file_lseek(self, offset, whence, out_pos);
}

/// @brief Get initrd root status.
static int initrd_root_fstat(Vnode &, VfsStat &vfs_stat) {
    vfs_stat.st_size = 0;
    vfs_stat.st_mode = S_IFDIR;
    return 0;
}

/// @brief I/O control on initrd root (not supported).
static int initrd_root_ioctl(Vnode &, uint64_t, kernel::CheckedPtr<uint8_t>) {
    return VFS_INVALID;
}

/// @brief Read a directory entry from initrd root.
static int initrd_root_readdir(Vnode &, uint64_t &pos, Dirent &dent) {
    initrd::InitrdEntry entry = {};
    if (!initrd::readdir(&pos, &entry))
        return VFS_INVALID;
    size_t idx = 0;
    while (entry.name[idx] && idx < 63) {
        dent.d_name[idx] = entry.name[idx];
        ++idx;
    }
    dent.d_name[idx] = '\0';
    dent.d_ino = 2;
    return 0;
}

/// @brief Look up a file by name in the initrd root directory.
/// Flat entries resolve exactly as before; on a miss the archive is
/// probed for subdirectory evidence, so multi-level paths ("/bin/...").
static Vnode *initrd_root_lookup(Vnode &, const char *name) {
    if (!name)
        return nullptr;
    bool is_dir = false;
    if (!initrd_probe_subdir(name, is_dir)) {
        initrd::InitrdFile file = initrd::find(name);
        if (!file.data)
            return nullptr;
        return initrd_make_file_vnode(file);
    }
    size_t sublen = 0;
    while (name[sublen])
        ++sublen;
    return initrd_make_dir_vnode(name, sublen);
}

static const VnodeOps initrd_root_ops = {
    initrd_root_read,
    initrd_root_write,
    initrd_root_open,
    initrd_root_close,
    initrd_root_lseek,
    initrd_root_fstat,
    initrd_root_ioctl,
    initrd_root_readdir,
    initrd_root_lookup,
    nullptr,
    nullptr,
    nullptr, // create
};

/// @brief Get the initrd root vnode (lazily initialised).
static Vnode *initrd_get_root() {
    if (!root_initialized) {
        initrd_root.ops = &initrd_root_ops;
        initrd_root.ino = 0;
        initrd_root.size = 0;
        initrd_root.mode = S_IFDIR;
        initrd_root.private_data = nullptr;
        initrd_root.parent = nullptr;
        root_initialized = true;
    }
    return &initrd_root;
}

Filesystem initrd_fs = {
    "initrd",
    initrd_get_root,
};

} // namespace vfs
} // namespace kernel
