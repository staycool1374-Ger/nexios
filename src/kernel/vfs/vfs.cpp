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

/// @file vfs.cpp
/// @brief VFS core implementation: path resolution, mounting, file/directory
/// ops.

#include <kernel/vfs/vfs.hpp>
#include <kernel/vfs/tmpfs.hpp>
#include <kernel/vfs/ramdisk_fs.hpp>
#include <kernel/vfs/devfs.hpp>
#include <kernel/vfs/procfs.hpp>
#include <kernel/vfs/initrd_fs.hpp>
#include <kernel/vfs/fat32_fs.hpp>
#include <kernel/task/scheduler.hpp>
#include <kernel/task/task.hpp>
#include <kernel/test/resource_tracker.hpp>
#ifdef CONFIG_TEST
#include <kernel/test/test_isolate.hpp>
#endif
#include <string.hpp>
#include <utils.hpp>
#include <kernel/vfs/vfs_errors.hpp>

using namespace kernel::errors;

namespace kernel {
namespace vfs {

static Mount mount_table[MAX_MOUNTS];
static size_t mount_count = 0;
static Vnode *root_vnode_global = nullptr;

/// @brief Get the global root vnode.
Vnode *get_root_vnode() {
    return root_vnode_global;
}

/// @brief Set the global root vnode.
void set_root_vnode(Vnode &vnode) {
    root_vnode_global = &vnode;
}

/// @brief Allocate a file descriptor entry.
/// @return The fd index, or VFS_INVALID if full.
int FdTable::alloc() {
    for (size_t i = 0; i < MAX_FDS; ++i) {
        if (!fds[i].used) {
            fds[i].used = true;
            fds[i].vnode = nullptr;
            fds[i].offset = 0;
            fds[i].flags = 0;
            kernel::test::ResourceTracker::instance().track_fd_add();
            return static_cast<int>(i);
        }
    }
    return VFS_INVALID;
}

/// @brief Allocate a file descriptor entry with error code.
/// @return VfsError code.
VfsError FdTable::alloc_err(int &out_fd) {
    for (size_t i = 0; i < MAX_FDS; ++i) {
        if (!fds[i].used) {
            fds[i].used = true;
            fds[i].vnode = nullptr;
            fds[i].offset = 0;
            fds[i].flags = 0;
            kernel::test::ResourceTracker::instance().track_fd_add();
            out_fd = static_cast<int>(i);
            return VFS_ERR_OK;
        }
    }
    return VFS_ERR_FD_TABLE_FULL;
}

/// @brief Release a file descriptor entry.
void FdTable::free(int file_descriptor) {
    if (file_descriptor < 0 || static_cast<size_t>(file_descriptor) >= MAX_FDS)
        return;
    if (fds[file_descriptor].used && fds[file_descriptor].vnode) {
        if (vnode_ref_dec(fds[file_descriptor].vnode)) {
            if (fds[file_descriptor].vnode->ops->close)
                fds[file_descriptor].vnode->ops->close(
                    *fds[file_descriptor].vnode);
        }
    }
    if (fds[file_descriptor].used) {
        kernel::test::ResourceTracker::instance().track_fd_remove();
    }
    fds[file_descriptor].used = false;
    fds[file_descriptor].vnode = nullptr;
    fds[file_descriptor].offset = 0;
    fds[file_descriptor].flags = 0;
}

/// @brief Release a file descriptor entry with error code.
/// @return VfsError code.
VfsError FdTable::free_err(int file_descriptor) {
    if (file_descriptor < 0 || static_cast<size_t>(file_descriptor) >= MAX_FDS)
        return VFS_ERR_INVALID_FD;
    if (fds[file_descriptor].used && fds[file_descriptor].vnode) {
        if (vnode_ref_dec(fds[file_descriptor].vnode)) {
            if (fds[file_descriptor].vnode->ops->close)
                fds[file_descriptor].vnode->ops->close(
                    *fds[file_descriptor].vnode);
        }
    }
    if (fds[file_descriptor].used) {
        kernel::test::ResourceTracker::instance().track_fd_remove();
    }
    fds[file_descriptor].used = false;
    fds[file_descriptor].vnode = nullptr;
    fds[file_descriptor].offset = 0;
    fds[file_descriptor].flags = 0;
    return VFS_ERR_OK;
}

/// @brief Look up a file descriptor by index.
/// @return Pointer to the entry, or nullptr if invalid.
FileDescription *FdTable::get(int file_descriptor) {
    if (file_descriptor < 0 || static_cast<size_t>(file_descriptor) >= MAX_FDS)
        return nullptr;
    if (!fds[file_descriptor].used)
        return nullptr;
    return &fds[file_descriptor];
}

/// @brief Look up a file descriptor by index with error code.
/// @return VfsError code.
VfsError FdTable::get_err(int file_descriptor, FileDescription *&out_fd) {
    if (file_descriptor < 0 || static_cast<size_t>(file_descriptor) >= MAX_FDS)
        return VFS_ERR_INVALID_FD;
    if (!fds[file_descriptor].used)
        return VFS_ERR_INVALID_FD;
    out_fd = &fds[file_descriptor];
    return VFS_ERR_OK;
}

/// @brief Resolve an absolute or relative path to a vnode.
/// @return The vnode, or nullptr if not found.
Vnode *resolve(const char *path) {
#ifdef CONFIG_TEST
    kernel::test::mark_vfs_touched();
#endif
    if (!path || !*path)
        return nullptr;

    TaskControlBlock *task = Scheduler::current_task();

    bool is_absolute = (path[0] == '/');
    const char *search = path;
    Vnode *current = nullptr;
    // Issue #268: ownership of `current`.  The initial node (mount root /
    // cwd / global root) is BORROWED; every adopted `lookup()` result is
    // OWNED and must be released before reassignment or on failure.  The
    // final return is always OWNED (borrowed finals are inc'd on exit).
    bool owned = false;

    if (is_absolute) {
        Mount *best = nullptr;
        size_t best_len = 0;
        for (size_t i = 0; i < mount_count; ++i) {
            if (!mount_table[i].used)
                continue;
            const char *mp = mount_table[i].mount_point;
            size_t mplen = strlen(mp);
            if (strncmp(path, mp, mplen) == 0) {
                // Live-session finding (#77 report): the root mount "/"
                // (mplen 1) must match every absolute path — requiring
                // path[1] to be '/' or NUL meant no direct child of /
                // ("/hey.c.elf", "/etc/rc") ever resolved, while submount
                // paths ("/tmp/x", "/dev/tty") worked. The separator
                // guard still applies to longer mount points ("/devXYZ"
                // must not match "/dev").
                if (mplen == 1 || path[mplen] == '/' ||
                    path[mplen] == '\0') {
                    if (mplen > best_len) {
                        best = &mount_table[i];
                        best_len = mplen;
                    }
                }
            }
        }
        if (!best)
            return nullptr;
        current = best->root_vnode;
        search = path + best_len;
        while (*search == '/')
            ++search;
    } else {
        current =
            (task && task->cwd_vnode) ? task->cwd_vnode : root_vnode_global;
    }

    if (!current)
        return nullptr;

    char comp[MAX_PATH];
    while (*search) {
        while (*search == '/')
            ++search;
        if (!*search)
            break;

        size_t i = 0;
        while (*search && *search != '/' && i < MAX_PATH - 1) {
            comp[i++] = *search++;
        }
        comp[i] = '\0';

        if (comp[0] == '.' && comp[1] == '\0')
            continue;

        if (comp[0] == '.' && comp[1] == '.' && comp[2] == '\0') {
            if (current->parent) {
                // Parent links are borrowed: drop an owned `current`
                // first, then adopt borrowed (owned = false).
                if (owned) {
                    release(current);
                    owned = false;
                }
                current = current->parent;
                continue;
            }
            for (size_t i = 0; i < mount_count; ++i) {
                if (!mount_table[i].used)
                    continue;
                if (mount_table[i].root_vnode != current)
                    continue;
                const char *mp = mount_table[i].mount_point;
                const char *last_slash = nullptr;
                for (const char *p = mp; *p; ++p) {
                    if (*p == '/')
                        last_slash = p;
                }
                if (last_slash && last_slash > mp) {
                    size_t len = static_cast<size_t>(last_slash - mp);
                    if (len >= MAX_PATH)
                        len = MAX_PATH - 1;
                    char parent_path[MAX_PATH];
                    __builtin_memcpy(parent_path, mp, len);
                    parent_path[len] = '\0';
                    if (owned) {
                        release(current);
                        owned = false;
                    }
                    current = resolve(parent_path);
                    owned = true;
                } else if (last_slash == mp) {
                    if (owned) {
                        release(current);
                        owned = false;
                    }
                    current = resolve("/");
                    owned = true;
                }
                if (!current)
                    return nullptr; // inner resolve failed: nothing owned
                break;
            }
            continue;
        }

        Vnode *child = nullptr;
        bool child_from_mount = false;
        for (size_t i = 0; i < mount_count; ++i) {
            if (!mount_table[i].used)
                continue;
            const char *mp = mount_table[i].mount_point;
            const char *name_start = mp;
            for (const char *p = mp; *p; ++p) {
                if (*p == '/')
                    name_start = p + 1;
            }
            if (strcmp(name_start, comp) == 0) {
                child = mount_table[i].root_vnode;
                child_from_mount = true;
                break;
            }
        }
        if (!child) {
            child =
                current->ops ? current->ops->lookup(*current, comp) : nullptr;
        }
        if (!child) {
            if (owned)
                release(current);
            return nullptr;
        }
        if (owned)
            release(current);
        // Mount roots are borrowed (owned = false); lookup results are
        // callee-owned (owned = true) — borrowed ones (tmpfs cache,
        // devfs statics) are refcount-0, so release() no-ops on them.
        owned = !child_from_mount;
        current = child;
    }

    if (current && !owned) {
        // Uniform OWNED return: the initial node (mount root / cwd /
        // global root) is borrowed during traversal — take the caller's
        // reference here so every resolve() result releases exactly once.
        vnode_ref_inc(current);
    }
    return current;
}

/// @brief Mount a filesystem at a mount point.
/// @return 0 on success, VFS_INVALID on failure.
int mount(Filesystem &filesystem, const char *mount_point) {
#ifdef CONFIG_TEST
    kernel::test::mark_vfs_touched();
#endif
    if (!filesystem.get_root || mount_count >= MAX_MOUNTS)
        return VFS_INVALID;

    Vnode *root = filesystem.get_root();
    if (!root)
        return VFS_INVALID;

    mount_table[mount_count].mount_point = mount_point;
    mount_table[mount_count].fs = &filesystem;
    mount_table[mount_count].root_vnode = root;
    mount_table[mount_count].used = true;
    ++mount_count;

    if (mount_count == 1) {
        root_vnode_global = root;
    }

    return 0;
}

/// @brief Initialize the VFS subsystem (clear mount table).
void init() {
    mount_count = 0;
    for (size_t i = 0; i < MAX_MOUNTS; ++i) {
        mount_table[i].used = false;
        mount_table[i].mount_point = nullptr;
        mount_table[i].fs = nullptr;
        mount_table[i].root_vnode = nullptr;
    }
}

/// @brief Reset VFS globals and re-mount standard filesystems.
void reset_and_remount() {
#ifdef CONFIG_TEST
    kernel::test::mark_vfs_touched();
#endif
    // Clear stale mount-table entries and root-vnode pointers from
    // prior test execution.  The underlying MemPool blocks have been
    // restored by this point, but mount_table entries still point to
    // pre-restore vnode addresses that now contain snapshot-era data.
    mount_count = 0;
    root_vnode_global = nullptr;
    for (size_t i = 0; i < MAX_MOUNTS; ++i) {
        mount_table[i].used = false;
        mount_table[i].mount_point = nullptr;
        mount_table[i].fs = nullptr;
        mount_table[i].root_vnode = nullptr;
    }

    // Re-mount standard filesystems.  get_root() allocates fresh
    // vnodes from MemPool, which is now in snapshot-era state.
    mount(initrd_fs, "/");
    mount(dev_fs, "/dev");
    mount(proc_fs, "/proc");
    mount(tmpfs_fs, "/tmp");
    // Issue #314: best-effort ramdisk mount (fail-closed when ungranted —
    // boot and tests never block on it; absence just means /mnt/ramdisk
    // does not resolve).
    ramdisk_fs_try_mount();
}

/// @brief Find a mounted filesystem by name.
/// @return The filesystem, or nullptr if not found.
Filesystem *find_fs(const char *name) {
    for (size_t i = 0; i < mount_count; ++i) {
        if (mount_table[i].fs && mount_table[i].fs->name &&
            strcmp(mount_table[i].fs->name, name) == 0) {
            return mount_table[i].fs;
        }
    }
    return nullptr;
}

/// @brief Resolve the parent vnode and leaf name from a path.
/// Handles both absolute ("/foo/bar") and relative ("bar") paths by
/// delegating sub-path resolution to resolve(), which already handles
/// CWD, mount points, .., . etc.
/// @param path  The full path to parse.
/// @param[out] out_name  Set to the final path component (leaf name).
/// @return The parent vnode, or nullptr if it cannot be resolved.
Vnode *resolve_parent(const char *path, const char *&out_name) {
    const char *slash = nullptr;
    for (const char *p = path; *p; ++p) {
        if (*p == '/')
            slash = p;
    }

    if (slash) {
        size_t parent_len = static_cast<size_t>(slash - path);
        char parent_buf[MAX_PATH];
        for (size_t i = 0; i < parent_len && i < MAX_PATH - 1; ++i)
            parent_buf[i] = path[i];
        parent_buf[parent_len] = '\0';
        out_name = slash + 1;
        if (!*out_name)
            return nullptr;
        return resolve(parent_buf);
    }

    out_name = path;
    if (!*out_name)
        return nullptr;
    auto *task = Scheduler::current_task();
    Vnode *cwd =
        (task && task->cwd_vnode) ? task->cwd_vnode : root_vnode_global;
    // Uniform OWNED return (issue #268): take the caller's reference on
    // the borrowed cwd/global root; slash-case resolve() is already owned.
    if (cwd)
        vnode_ref_inc(cwd);
    return cwd;
}

/// @brief Create a subdirectory at the given path.
/// @return 0 on success, VFS_INVALID on failure.
int mkdir(const char *path, uint16_t mode) {
#ifdef CONFIG_TEST
    kernel::test::mark_vfs_touched();
#endif
    const char *name = nullptr;
    Vnode *parent = resolve_parent(path, name);
    if (!parent || !(parent->mode & S_IFDIR)) {
        release(parent);
        return VFS_INVALID;
    }
    if (!parent->ops || !parent->ops->mkdir) {
        release(parent);
        return VFS_INVALID;
    }
    int rc = parent->ops->mkdir(*parent, name, mode);
    release(parent);
    return rc;
}

/// @brief Remove a file or empty directory at the given path.
/// @return 0 on success, VFS_INVALID on failure.
int unlink(const char *path) {
#ifdef CONFIG_TEST
    kernel::test::mark_vfs_touched();
#endif
    const char *name = nullptr;
    Vnode *parent = resolve_parent(path, name);
    if (!parent || !(parent->mode & S_IFDIR)) {
        release(parent);
        return VFS_INVALID;
    }
    if (!parent->ops || !parent->ops->unlink) {
        release(parent);
        return VFS_INVALID;
    }
    int rc = parent->ops->unlink(*parent, name);
    release(parent);
    return rc;
}

/// @brief Create a regular file at the given path.
/// @return 0 on success, VFS_INVALID on failure.
int create(const char *path, uint16_t mode) {
    const char *name = nullptr;
    Vnode *parent = resolve_parent(path, name);
    if (!parent || !(parent->mode & S_IFDIR)) {
        release(parent);
        return VFS_INVALID;
    }
    if (!parent->ops || !parent->ops->create) {
        release(parent);
        return VFS_INVALID;
    }
    int rc = parent->ops->create(*parent, name, mode);
    release(parent);
    return rc;
}

/// @brief Mount a filesystem with error handling.
/// @return VfsError code.
VfsError mount_err(Filesystem &filesystem, const char *mount_point) {
    if (!filesystem.get_root || mount_count >= MAX_MOUNTS) {
        return VFS_ERR_INVALID_ARGS;
    }

    Vnode *root = filesystem.get_root();
    if (!root) {
        return VFS_ERR_NO_DEVICE;
    }

    mount_table[mount_count].mount_point = mount_point;
    mount_table[mount_count].fs = &filesystem;
    mount_table[mount_count].root_vnode = root;
    mount_table[mount_count].used = true;
    ++mount_count;

    if (mount_count == 1) {
        root_vnode_global = root;
    }

    return VFS_ERR_OK;
}

/// @brief Initialize the VFS subsystem with error code return.
/// @return VfsError code.
VfsError init_err() {
    mount_count = 0;
    for (size_t i = 0; i < MAX_MOUNTS; ++i) {
        mount_table[i].used = false;
        mount_table[i].mount_point = nullptr;
        mount_table[i].fs = nullptr;
        mount_table[i].root_vnode = nullptr;
    }
    root_vnode_global = nullptr;
    return VFS_ERR_OK;
}

/// @brief Find a mounted filesystem by name with error code return.
/// @return VfsError code.
VfsError find_fs_err(const char *name, Filesystem *&out_fs) {
    if (!name) {
        return VFS_ERR_INVALID_ARGS;
    }
    for (size_t i = 0; i < mount_count; ++i) {
        if (mount_table[i].fs && mount_table[i].fs->name &&
            strcmp(mount_table[i].fs->name, name) == 0) {
            out_fs = mount_table[i].fs;
            return VFS_ERR_OK;
        }
    }
    return VFS_ERR_NO_SUCH_FS;
}

/// @brief Set the global root vnode with error code return.
/// @return VfsError code.
VfsError set_root_vnode_err(Vnode &vnode) {
    root_vnode_global = &vnode;
    return VFS_ERR_OK;
}

/// @brief Resolve an absolute path to a vnode with error code return.
/// @return VfsError code.
VfsError resolve_err(const char *path, Vnode *&out_vnode) {
    Vnode *result = resolve(path);
    if (!result) {
        return VFS_ERR_NOT_FOUND;
    }
    out_vnode = result;
    return VFS_ERR_OK;
}

/// @brief Create a subdirectory with error code return.
/// @return VfsError code.
VfsError mkdir_err(const char *path, uint16_t mode) {
    const char *name = nullptr;
    Vnode *parent = resolve_parent(path, name);
    if (!parent)
        return VFS_ERR_NOT_FOUND;
    if (!(parent->mode & S_IFDIR)) {
        release(parent); // issue #268
        return VFS_ERR_NOT_DIR;
    }
    if (!parent->ops || !parent->ops->mkdir) {
        release(parent); // issue #268
        return VFS_ERR_NOT_SUPPORTED;
    }
    int result = parent->ops->mkdir(*parent, name, mode);
    release(parent); // issue #268
    if (result == 0)
        return VFS_ERR_OK;
    // Positive codes are specific VfsError values (e.g. VFS_ERR_EXISTS);
    // negative sentinels (VFS_INVALID) fall back to a generic I/O error.
    if (result > 0)
        return static_cast<VfsError>(result);
    return VFS_ERR_IO_ERROR;
}

/// @brief Create a regular file with error code return.
/// @return VfsError code.
VfsError create_err(const char *path, uint16_t mode) {
    const char *name = nullptr;
    Vnode *parent = resolve_parent(path, name);
    if (!parent)
        return VFS_ERR_NOT_FOUND;
    if (!(parent->mode & S_IFDIR)) {
        release(parent); // issue #268
        return VFS_ERR_NOT_DIR;
    }
    if (!parent->ops || !parent->ops->create) {
        release(parent); // issue #268
        return VFS_ERR_NOT_SUPPORTED;
    }
    int result = parent->ops->create(*parent, name, mode);
    release(parent); // issue #268
    return result == 0 ? VFS_ERR_OK : VFS_ERR_IO_ERROR;
}

/// @brief Remove a file or empty directory with error code return.
/// @return VfsError code.
VfsError unlink_err(const char *path) {
    const char *name = nullptr;
    Vnode *parent = resolve_parent(path, name);
    if (!parent)
        return VFS_ERR_NOT_FOUND;
    if (!(parent->mode & S_IFDIR)) {
        release(parent); // issue #268
        return VFS_ERR_NOT_DIR;
    }
    if (!parent->ops || !parent->ops->unlink) {
        release(parent); // issue #268
        return VFS_ERR_NOT_SUPPORTED;
    }
    int result = parent->ops->unlink(*parent, name);
    release(parent); // issue #268
    return result == 0 ? VFS_ERR_OK : VFS_ERR_IO_ERROR;
}

} // namespace vfs
} // namespace kernel
