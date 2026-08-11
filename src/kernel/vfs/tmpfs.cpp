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

/// @file tmpfs.cpp
/// @brief In-memory temporary filesystem implementation (file/dir ops, mkdir,
/// create).

#include <kernel/vfs/tmpfs.hpp>
#include <kernel/memory/mempool.hpp>
#include <kernel/memory/pmm.hpp>
#include <kernel/arch/io.hpp>
#include <kernel/sync/mutex.hpp>
#include <string.hpp>
#include <utils.hpp>
#include <constants.hpp>

namespace kernel {
namespace vfs {

/// @brief A directory entry in tmpfs (linked-list node).
struct TmpfsEntry {
    char name[64];    ///< Entry name (null-terminated).
    Vnode *vnode;     ///< Pointer to the entry's vnode.
    TmpfsEntry *next; ///< Next entry in the linked list.
};

static Vnode tmpfs_root{};

static uint64_t next_ino = 1;

// tmpfs file storage bounds (v0.3.12 G3-B): one contiguous allocation of
// TMPFS_MAX_FILE_PAGES pages, capped at TMPFS_MAX_FILE_SIZE.
static constexpr uint64_t TMPFS_MAX_FILE_PAGES = 16;
static constexpr uint64_t TMPFS_MAX_FILE_SIZE  = 64_KiB;

static sync::Mutex tmpfs_lock{};

/// @brief Find a directory entry by name in a tmpfs directory.
/// @return The entry, or nullptr if not found.
static TmpfsEntry *find_entry(Vnode &dir, const char *name) {
    auto *e = static_cast<TmpfsEntry *>(dir.private_data);
    while (e) {
        if (strcmp(e->name, name) == 0)
            return e;
        e = e->next;
    }
    return nullptr;
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
/// @brief Read data from a tmpfs file.
static int64_t tmpfs_file_read(Vnode &self, uint8_t *buffer, uint64_t count,
                               uint64_t offset) {
    tmpfs_lock.lock();
    if (offset >= self.size) {
        tmpfs_lock.unlock();
        return 0;
    }
    uint64_t avail = self.size - offset;
    if (count > avail)
        count = avail;
    uint64_t phys = reinterpret_cast<uint64_t>(self.private_data);
    if (!phys) {
        tmpfs_lock.unlock();
        return 0;
    }
    // NOLINTNEXTLINE(performance-no-int-to-ptr)
    const uint8_t *src =
        reinterpret_cast<const uint8_t *>(arch::HHDM_OFFSET + phys) + offset;
    __builtin_memcpy(buffer, src, count);
    tmpfs_lock.unlock();
    return static_cast<int64_t>(count);
}

/// @brief Write data to a tmpfs file (allocates pages on demand).
static int64_t tmpfs_file_write(Vnode &self, const uint8_t *buffer,
                                uint64_t count, uint64_t offset) {
    tmpfs_lock.lock();
    uint64_t needed = offset + count;
    if (needed > TMPFS_MAX_FILE_SIZE) {
        tmpfs_lock.unlock();
        return VFS_INVALID;
    }

    // Need to allocate pages if not already
    uint64_t phys = reinterpret_cast<uint64_t>(self.private_data);
    if (!phys) {
        phys = PMM::alloc_user_contiguous(TMPFS_MAX_FILE_PAGES);
        if (!phys) {
            tmpfs_lock.unlock();
            return VFS_INVALID;
        }
        // NOLINTNEXTLINE(performance-no-int-to-ptr)
        self.private_data = reinterpret_cast<void *>(phys);
        // NOLINTNEXTLINE(performance-no-int-to-ptr)
        __builtin_memset(reinterpret_cast<void *>(arch::HHDM_OFFSET + phys), 0,
                         TMPFS_MAX_FILE_SIZE);
    }

    // NOLINTNEXTLINE(performance-no-int-to-ptr)
    uint8_t *dst =
        reinterpret_cast<uint8_t *>(arch::HHDM_OFFSET + phys) + offset;
    __builtin_memcpy(dst, buffer, count);
    if (needed > self.size)
        self.size = needed;
    tmpfs_lock.unlock();
    return static_cast<int64_t>(count);
}

/// @brief Open a tmpfs file.
static int tmpfs_file_open(Vnode &, uint64_t) {
    return 0;
}
/// @brief Close a tmpfs file.
static void tmpfs_file_close(Vnode &) {
}

/// @brief Get tmpfs vnode status.
static int tmpfs_fstat(Vnode &self, VfsStat &st) {
    st.st_size = self.size;
    st.st_mode = self.mode;
    return 0;
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
/// @brief Seek within a tmpfs file.
static int64_t tmpfs_file_lseek(Vnode &self, int64_t offset, int whence,
                                uint64_t *out_pos) {
    (void)self;
    (void)offset;
    (void)whence;
    (void)out_pos;
    return 0;
}

/// @brief Read a directory entry from a tmpfs directory.
static int tmpfs_readdir(Vnode &self, uint64_t &pos, Dirent &dent) {
    tmpfs_lock.lock();
    uint64_t idx = 0;
    auto *e = static_cast<TmpfsEntry *>(self.private_data);
    while (e) {
        if (idx == pos) {
            size_t i = 0;
            while (e->name[i] && i < sizeof(dent.d_name) - 1) {
                dent.d_name[i] = e->name[i];
                ++i;
            }
            dent.d_name[i] = '\0';
            dent.d_ino = e->vnode->ino;
            ++pos;
            tmpfs_lock.unlock();
            return 0;
        }
        ++idx;
        e = e->next;
    }
    tmpfs_lock.unlock();
    return VFS_INVALID;
}

static int tmpfs_unlink(Vnode &self, const char *name);

static int tmpfs_create(Vnode &self, const char *name, uint16_t mode);

/// @brief Look up a child entry by name in a tmpfs directory.
static Vnode *tmpfs_lookup(Vnode &self, const char *name) {
    tmpfs_lock.lock();
    auto *e = find_entry(self, name);
    tmpfs_lock.unlock();
    return e ? e->vnode : nullptr;
}

/// @brief Create a subdirectory in tmpfs.
static int tmpfs_mkdir(Vnode &self, const char *name, uint16_t) {
    tmpfs_lock.lock();
    if (find_entry(self, name)) {
        tmpfs_lock.unlock();
        return VFS_INVALID;
    }
    // MemPool-only discipline (v0.3.12 G3-A): no heap path —
    // ResourceTracker-tracked via MemPool.
    auto *entry = static_cast<TmpfsEntry *>(MemPool::alloc(sizeof(TmpfsEntry)));
    if (!entry) {
        tmpfs_lock.unlock();
        return VFS_INVALID;
    }
    auto *vn = static_cast<Vnode *>(MemPool::alloc(sizeof(Vnode)));
    if (!vn) {
        MemPool::free(entry);
        tmpfs_lock.unlock();
        return VFS_INVALID;
    }

    size_t i = 0;
    while (name[i] && i < sizeof(entry->name) - 1) {
        entry->name[i] = name[i];
        ++i;
    }
    entry->name[i] = '\0';

    static const VnodeOps dir_ops = {
        nullptr, // read
        nullptr, // write
        nullptr, // open
        nullptr, // close
        nullptr, // lseek
        tmpfs_fstat,
        nullptr, // ioctl
        tmpfs_readdir, tmpfs_lookup, tmpfs_mkdir, tmpfs_unlink, tmpfs_create,
    };

    vn->ops = &dir_ops;
    vn->ino = next_ino++;
    vn->size = 0;
    vn->mode = S_IFDIR;
    vn->private_data = nullptr;
    vn->refcount = 0;
    vn->parent = &self;

    entry->vnode = vn;
    entry->next = static_cast<TmpfsEntry *>(self.private_data);
    self.private_data = entry;
    tmpfs_lock.unlock();
    return 0;
}

/// @brief Remove a file or empty directory from tmpfs.
static int tmpfs_unlink(Vnode &self, const char *name) {
    tmpfs_lock.lock();
    auto *prev = static_cast<TmpfsEntry *>(nullptr);
    auto *e = static_cast<TmpfsEntry *>(self.private_data);
    while (e) {
        if (strcmp(e->name, name) == 0) {
            if (e->vnode->mode & S_IFDIR) {
                if (e->vnode->private_data) {
                    tmpfs_lock.unlock();
                    return VFS_INVALID;
                }
            }
            if (prev)
                prev->next = e->next;
            else
                self.private_data = e->next;
            if (e->vnode->private_data && !(e->vnode->mode & S_IFDIR)) {
                uint64_t phys =
                    reinterpret_cast<uint64_t>(e->vnode->private_data);
                if (phys) {
                    // tmpfs_file_write always allocates
                    // TMPFS_MAX_FILE_PAGES contiguous pages
                    // (TMPFS_MAX_FILE_SIZE max per file).  Free all.
                    for (uint64_t i = 0; i < TMPFS_MAX_FILE_PAGES; ++i)
                        PMM::free_page(phys + i * arch::PAGE_SIZE);
                }
            }
            MemPool::free(e->vnode);
            MemPool::free(e);
            tmpfs_lock.unlock();
            return 0;
        }
        prev = e;
        e = e->next;
    }
    tmpfs_lock.unlock();
    return VFS_INVALID;
}

static const VnodeOps tmpfs_file_ops = {
    tmpfs_file_read,  tmpfs_file_write, tmpfs_file_open,
    tmpfs_file_close, tmpfs_file_lseek, tmpfs_fstat,
    nullptr, // ioctl
    nullptr, // readdir
    nullptr, // lookup
    nullptr, // mkdir
    nullptr, // unlink
    nullptr, // create
};

/// @brief Create a regular file in tmpfs.
static int tmpfs_create(Vnode &self, const char *name, uint16_t) {
    tmpfs_lock.lock();
    if (find_entry(self, name)) {
        tmpfs_lock.unlock();
        return VFS_INVALID;
    }
    // MemPool-only discipline (v0.3.12 G3-A): no heap path —
    // ResourceTracker-tracked via MemPool.
    auto *entry = static_cast<TmpfsEntry *>(MemPool::alloc(sizeof(TmpfsEntry)));
    if (!entry) {
        tmpfs_lock.unlock();
        return VFS_INVALID;
    }
    auto *vn = static_cast<Vnode *>(MemPool::alloc(sizeof(Vnode)));
    if (!vn) {
        MemPool::free(entry);
        tmpfs_lock.unlock();
        return VFS_INVALID;
    }

    size_t i = 0;
    while (name[i] && i < sizeof(entry->name) - 1) {
        entry->name[i] = name[i];
        ++i;
    }
    entry->name[i] = '\0';

    vn->ops = &tmpfs_file_ops;
    vn->ino = next_ino++;
    vn->size = 0;
    vn->mode = S_IFREG;
    vn->private_data = nullptr;
    vn->refcount = 0;
    vn->parent = &self;

    entry->vnode = vn;
    entry->next = static_cast<TmpfsEntry *>(self.private_data);
    self.private_data = entry;
    tmpfs_lock.unlock();
    return 0;
}

/// @brief Get the tmpfs root vnode (lazily initialised).
static Vnode *tmpfs_get_root() {
    static bool inited = false;
    if (!inited) {
        static const VnodeOps root_ops = {
            nullptr,      nullptr,     nullptr,      nullptr,
            nullptr,      tmpfs_fstat, nullptr,      tmpfs_readdir,
            tmpfs_lookup, tmpfs_mkdir, tmpfs_unlink, tmpfs_create,
        };
        tmpfs_root.ops = &root_ops;
        tmpfs_root.ino = next_ino++;
        tmpfs_root.size = 0;
        tmpfs_root.mode = S_IFDIR;
        tmpfs_root.private_data = nullptr;
        tmpfs_root.refcount = 0;
        tmpfs_root.parent = nullptr;
        inited = true;
    }
    return &tmpfs_root;
}

/// @brief Reset the tmpfs root directory (clear children).
void tmpfs_reset_root() {
    tmpfs_root.private_data = nullptr;
}

Filesystem tmpfs_fs = {
    "tmpfs",
    tmpfs_get_root,
};

} // namespace vfs
} // namespace kernel
