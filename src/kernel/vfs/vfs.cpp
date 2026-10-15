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

/// @brief Maximum underlying names remembered for mount dedup (#346).
/// Bounds the phase-2 re-walk name table. Entries beyond the cap are
/// still counted for the phase boundary but skipped for dedup: a mount
/// colliding with an over-cap entry then double-lists instead of hiding
/// (fail-visible; resolve() shadowing already defines that ambiguity).
static constexpr size_t VFS_MERGED_MAX_SEEN = 64;

/// @brief Leaf component of a mount path ("" for "/" itself).
static const char *mount_leaf_name(const char *mount_point) {
    const char *leaf_name = mount_point;
    for (const char *scan = mount_point; *scan != '\0'; ++scan) {
        if (*scan == '/')
            leaf_name = scan + 1;
    }
    return leaf_name;
}

/// @brief Parent path of a mount point into out_parent.
/// @return false for "/" itself, non-absolute paths, or truncation.
static bool mount_parent_path(const char *mount_point, char *out_parent,
                              size_t parent_size) {
    if (mount_point == nullptr || mount_point[0] != '/' ||
        mount_point[1] == '\0' || parent_size == 0) {
        return false;
    }
    const char *last_slash = nullptr;
    for (const char *scan = mount_point + 1; *scan != '\0'; ++scan) {
        if (*scan == '/')
            last_slash = scan;
    }
    if (last_slash == nullptr) {
        // Direct child of root ("/mnt"): parent is "/".
        if (parent_size < 2)
            return false;
        out_parent[0] = '/';
        out_parent[1] = '\0';
        return true;
    }
    const size_t parent_len = static_cast<size_t>(last_slash - mount_point);
    if (parent_len + 1 > parent_size)
        return false;
    for (size_t idx = 0; idx < parent_len; ++idx)
        out_parent[idx] = mount_point[idx];
    out_parent[parent_len] = '\0';
    return true;
}

/// @brief Read a directory entry, overlaying child mount points (#346).
int readdir_merged(Vnode &dir_vnode, uint64_t &pos, Dirent &dent) {
#ifdef CONFIG_TEST
    kernel::test::mark_vfs_touched();
#endif
    // Phase 1: the filesystem's own entries. A missing ops table or
    // readdir hook (test fakes) means an empty underlying phase. The
    // hooks own the cursor units (initrd: raw cpio index; fat32: raw dir
    // slot — both advance past skipped entries), so pos passes through
    // untouched into the phase boundary below.
    const VnodeOps *ops = dir_vnode.ops;
    if (ops != nullptr && ops->readdir != nullptr) {
        if (ops->readdir(dir_vnode, pos, dent) == 0)
            return 0;
    }
    // Phase 2: mount-table overlay. Re-walk the underlying entries to
    // learn the phase boundary as an EOF CURSOR (not a yielded count —
    // issue #358: raw-slot strides inflate pos past yielded counts, so a
    // yielded-count boundary skips overlay candidates) and the names
    // already yielded (exact-match dedup). Walk-until-EOF matches the
    // cmd_ls loop shape; the stuck-hook break keeps it terminating.
    // Deterministic hooks re-walk to the caller's EOF cursor, making the
    // first overlay index 0; concurrent mutation fails closed below.
    uint64_t underlying_eof = 0;
    char seen_names[VFS_MERGED_MAX_SEEN][sizeof(Dirent::d_name)];
    size_t seen_count = 0;
    if (ops != nullptr && ops->readdir != nullptr) {
        uint64_t walk_pos = 0;
        Dirent walk_dent{};
        while (true) {
            const uint64_t before_pos = walk_pos;
            if (ops->readdir(dir_vnode, walk_pos, walk_dent) != 0)
                break;
            if (walk_pos <= before_pos)
                break; // stuck hook: fail closed with what we learned
            if (seen_count < VFS_MERGED_MAX_SEEN) {
                size_t name_idx = 0;
                while (walk_dent.d_name[name_idx] != '\0' &&
                       name_idx < sizeof(seen_names[0]) - 1) {
                    seen_names[seen_count][name_idx] =
                        walk_dent.d_name[name_idx];
                    ++name_idx;
                }
                seen_names[seen_count][name_idx] = '\0';
                ++seen_count;
            }
        }
        underlying_eof = walk_pos;
    }
    if (pos < underlying_eof)
        return VFS_INVALID; // shrunk mid-listing: fail closed, no guess
    const uint64_t mount_index = pos - underlying_eof;
    // Holds no locks: per-call ops locking only, mount_table is read
    // unlocked under the UP assumption (same as resolve(), spec §1.4).
    uint64_t candidate_index = 0;
    for (size_t mount_idx = 0; mount_idx < mount_count; ++mount_idx) {
        if (!mount_table[mount_idx].used)
            continue;
        const char *mount_point = mount_table[mount_idx].mount_point;
        if (mount_point == nullptr)
            continue;
        const char *leaf_name = mount_leaf_name(mount_point);
        if (leaf_name[0] == '\0')
            continue; // "/" itself is never a child entry
        char parent_path[MAX_PATH];
        parent_path[0] = '\0';
        if (!mount_parent_path(mount_point, parent_path, sizeof(parent_path)))
            continue;
        Vnode *parent_vnode = resolve(parent_path);
        if (parent_vnode == nullptr)
            continue;
        const bool is_child = vnode_identity_match(&dir_vnode, parent_vnode,
                                                   dir_vnode.ino);
        release(parent_vnode); // issue #268: owned resolve result
        if (!is_child)
            continue;
        bool already_listed = false;
        for (size_t seen_idx = 0; seen_idx < seen_count; ++seen_idx) {
            if (strcmp(seen_names[seen_idx], leaf_name) == 0) {
                already_listed = true;
                break;
            }
        }
        if (already_listed)
            continue;
        if (candidate_index != mount_index) {
            ++candidate_index;
            continue;
        }
        size_t name_idx = 0;
        while (leaf_name[name_idx] != '\0' &&
               name_idx < sizeof(dent.d_name) - 1) {
            dent.d_name[name_idx] = leaf_name[name_idx];
            ++name_idx;
        }
        dent.d_name[name_idx] = '\0';
        const Vnode *child_root = mount_table[mount_idx].root_vnode;
        dent.d_ino = (child_root != nullptr) ? child_root->ino : 0;
        ++pos;
        return 0;
    }
    return VFS_INVALID;
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
