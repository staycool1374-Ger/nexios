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

/// @file vfs.hpp
/// @brief Core VFS types, structures, and public API declarations.

#pragma once

#include <types.hpp>
#include <kernel/memory/checked_ptr.hpp>
#include <kernel/vfs/vfs_errors.hpp>

namespace kernel {
namespace vfs {

static constexpr size_t MAX_FDS = CONFIG_MAX_FDS;
static constexpr size_t MAX_MOUNTS = CONFIG_MAX_MOUNTS;
static constexpr size_t MAX_PATH = CONFIG_VFS_MAX_PATH;
static constexpr size_t MAX_PATH_DEPTH = 16;

enum OpenFlags : uint16_t {
    O_RDONLY = 0,
    O_WRONLY = 1,
    O_RDWR = 2,
    O_NONBLOCK = 0x800,
    O_CREAT = 0x200,
};

enum SeekWhence : int8_t {
    SEEK_SET = 0,
    SEEK_CUR = 1,
    SEEK_END = 2,
};

enum class VfsSentinel : int8_t {
    INVALID_FD = -4,
};
constexpr int64_t VFS_INVALID = static_cast<int64_t>(VfsSentinel::INVALID_FD);

using errors::VfsError;

enum FileMode : uint16_t {
    S_IFREG = 0x8000,
    S_IFDIR = 0x4000,
    S_IFCHR = 0x2000,
};

struct VfsStat {
    uint64_t st_size; ///< File size in bytes.
    uint16_t st_mode; ///< File mode / type flags.
};

struct Dirent {
    char d_name[64]; ///< Entry name (null-terminated).
    uint64_t d_ino;  ///< Inode number.
};

struct Vnode;
struct VnodeOps {
    int64_t (*read)(Vnode &self, uint8_t *buffer, uint64_t count,
                    uint64_t offset); ///< Read data from vnode.
    int64_t (*write)(Vnode &self, const uint8_t *buffer, uint64_t count,
                     uint64_t offset);        ///< Write data to vnode.
    int (*open)(Vnode &self, uint64_t flags); ///< Open the vnode.
    void (*close)(Vnode &self);               ///< Close the vnode.
    int64_t (*lseek)(Vnode &self, int64_t offset, int whence,
                     uint64_t *out_pos);    ///< Seek to a position.
    int (*fstat)(Vnode &self, VfsStat &st); ///< Get file status.
    int (*ioctl)(Vnode &self, uint64_t request,
                 kernel::CheckedPtr<uint8_t> arg); ///< Device I/O control.
    int (*readdir)(Vnode &self, uint64_t &pos,
                   Dirent &dent);                    ///< Read directory entry.
    Vnode *(*lookup)(Vnode &self, const char *name); ///< Look up child by name.
    int (*mkdir)(Vnode &self, const char *name,
                 uint16_t mode);                  ///< Create subdirectory.
    int (*unlink)(Vnode &self, const char *name); ///< Remove entry.
    int (*create)(Vnode &self, const char *name,
                  uint16_t mode); ///< Create file.
};

struct Vnode {
    const VnodeOps *ops; ///< Vnode operations table.
    uint64_t ino;        ///< Inode number.
    uint64_t size;       ///< File size in bytes.
    uint16_t mode;       ///< File mode / type flags.
    void *private_data;  ///< Filesystem-specific private data.
    uint64_t refcount;   ///< Reference count (atomic-intended — see below).
    Vnode *parent;       ///< Parent directory vnode (for `..` resolution).
};

/// @brief Atomically increment a vnode's refcount (VULN-C5/C6).
/// @note `std::atomic` is unavailable under `-nostdinc`; `__atomic_*` builtins
///       give identical lock-free semantics on the existing int field.
static inline void vnode_ref_inc(Vnode *vn) noexcept {
    if (vn)
        __atomic_fetch_add(&vn->refcount, 1ULL, __ATOMIC_RELAXED);
}

/// @brief Atomically decrement a vnode's refcount.
/// @return true if this call brought the count from 1 to 0 (owner must close).
static inline bool vnode_ref_dec(Vnode *vn) noexcept {
    if (!vn)
        return false;
    return __atomic_fetch_sub(&vn->refcount, 1ULL, __ATOMIC_ACQ_REL) == 1;
}

/// @brief Release a vnode reference (issue #268: uniform OWNED convention).
///        `resolve()` / `resolve_parent()` / `lookup()` results are OWNED —
///        the caller releases exactly once via this helper when done.
///        Zero-count vnodes (tmpfs cache entries, static roots, devfs
///        statics) are borrowed: release is a no-op for them, so sharing
///        one helper across filesystems can never free cache/static
///        storage.  Never call `ops->close` directly (except `FdTable`).
static inline void release(Vnode *vn) noexcept {
    if (!vn)
        return;
    // UP: no VFS refcount traffic in IRQ context, so the acquire-load
    // below is stable for the check-then-dec that follows (SMP will need
    // a locked or RCU reclamation pass — see vfs.md ownership notes).
    if (__atomic_load_n(&vn->refcount, __ATOMIC_ACQUIRE) == 0)
        return;
    if (vnode_ref_dec(vn) && vn->ops && vn->ops->close)
        vn->ops->close(*vn);
}

struct FileDescription {
    Vnode *vnode;    ///< The vnode this descriptor refers to.
    uint64_t offset; ///< Current read/write offset.
    uint64_t flags;  ///< Open flags (e.g. O_NONBLOCK).
    bool used;       ///< Whether this descriptor slot is in use.
};

struct FdTable {
    FileDescription fds[MAX_FDS]; ///< Array of file descriptor entries.

    FdTable() {
        __builtin_memset(fds, 0, sizeof(fds));
    }

    /// @brief Allocate a file descriptor entry.
    /// @return The fd index, or VFS_INVALID if full.
    int alloc();
    /// @brief Allocate a file descriptor entry with error code.
    /// @param[out] out_fd The allocated fd on success.
    /// @return VfsError code.
    VfsError alloc_err(int &out_fd);
    /// @brief Release a file descriptor entry.
    void free(int file_descriptor);
    /// @brief Release a file descriptor entry with error code.
    /// @return VfsError code.
    VfsError free_err(int file_descriptor);
    /// @brief Look up a file descriptor by index.
    /// @return Pointer to the entry, or nullptr if invalid.
    FileDescription *get(int file_descriptor);
    /// @brief Look up a file descriptor by index with error code.
    /// @param[out] out_fd Pointer to the entry on success.
    /// @return VfsError code.
    VfsError get_err(int file_descriptor, FileDescription *&out_fd);
};

struct Filesystem {
    const char *name;     ///< Filesystem driver name.
    Vnode *(*get_root)(); ///< Get the root vnode of this filesystem.
};

struct Mount {
    const char *mount_point; ///< Mount path (e.g. "/", "/dev").
    Filesystem *fs;          ///< The mounted filesystem.
    Vnode *root_vnode;       ///< Root vnode of the mounted fs.
    bool used;               ///< Whether this mount slot is occupied.
};

/// @brief Resolve an absolute path to a vnode.
/// @return The vnode (OWNED — release via `vfs::release()`), or nullptr
///         if not found.  Traversal intermediates are freed inside.
Vnode *resolve(const char *path);
/// @brief Resolve an absolute path to a vnode with error code.
/// @param[out] out_vnode The resolved vnode (OWNED) on success.
/// @return VfsError code.
VfsError resolve_err(const char *path, Vnode *&out_vnode);
/// @brief Resolve the parent directory and leaf name of a path.
/// @param[out] out_name Leaf component of the path (points into `path`).
/// @return The parent vnode (OWNED — release via `vfs::release()`), or
///         nullptr if not found.
Vnode *resolve_parent(const char *path, const char *&out_name);
/// @brief Mount a filesystem at a given mount point.
/// @return 0 on success, or VFS_INVALID on failure.
int mount(Filesystem &filesystem, const char *mount_point);
/// @brief Mount a filesystem at a given mount point with error code.
/// @return VfsError code.
VfsError mount_err(Filesystem &filesystem, const char *mount_point);
/// @brief Initialize the VFS subsystem (root vnode, mounts, etc.).
void init();
/// @brief Initialize the VFS subsystem with error code.
/// @return VfsError code.
VfsError init_err();
/// @brief Reset VFS globals and re-mount standard filesystems.
/// Called from snapshot_restore() after MemPool/PMM restoration to
/// clear stale mount-table entries and vnode pointers that survived
/// test execution.
void reset_and_remount();
/// @brief Find a registered filesystem driver by name.
/// @return The filesystem, or nullptr if not found.
Filesystem *find_fs(const char *name);
/// @brief Find a registered filesystem driver by name with error code.
/// @param[out] out_fs The filesystem on success.
/// @return VfsError code.
VfsError find_fs_err(const char *name, Filesystem *&out_fs);
/// @brief Get the root vnode of the VFS tree.
Vnode *get_root_vnode();
/// @brief Set the root vnode of the VFS tree.
void set_root_vnode(Vnode &vnode);
/// @brief Set the root vnode of the VFS tree with error code.
/// @return VfsError code.
VfsError set_root_vnode_err(Vnode &vnode);

/// @brief Create a subdirectory at the given path.
/// @return 0 on success, VFS_INVALID on failure.
int mkdir(const char *path, uint16_t mode);
/// @brief Create a subdirectory at the given path with error code.
/// @return VfsError code.
VfsError mkdir_err(const char *path, uint16_t mode);

/// @brief Create a regular file at the given path.
/// @return 0 on success, VFS_INVALID on failure.
int create(const char *path, uint16_t mode);
/// @brief Create a regular file at the given path with error code.
/// @return VfsError code.
VfsError create_err(const char *path, uint16_t mode);

/// @brief Remove a file or empty directory at the given path.
/// @return 0 on success, VFS_INVALID on failure.
int unlink(const char *path);
/// @brief Remove a file or empty directory at the given path with error code.
/// @return VfsError code.
VfsError unlink_err(const char *path);

} // namespace vfs
} // namespace kernel
