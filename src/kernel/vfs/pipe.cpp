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

/// @file pipe.cpp
/// @brief Anonymous pipe implementation (ring buffer, read/write vnode ops).

#include <kernel/vfs/pipe.hpp>
#include <kernel/vfs/vfs.hpp>
#include <kernel/task/task.hpp>
#include <kernel/task/scheduler.hpp>
#include <kernel/memory/mempool.hpp>
#include <kernel/memory/kernel_object.hpp>
#include <kernel/sync/semaphore.hpp>
#include <kernel/test/resource_tracker.hpp>
#include <string.hpp>

// Placement new (defined in lib/new.cpp, no <new> header in freestanding)
inline void *operator new(unsigned long, void *p) noexcept {
    return p;
}

namespace kernel {
namespace vfs {

static constexpr size_t PIPE_BUF_SIZE = 4096;

/// @brief Ring buffer shared between the read and write ends of a pipe.
///
/// Shared-ownership class (Class C): the two vnode endpoints each hold one
/// reference; the creator holds a transient reference during create_pipe().
/// The last reference invokes dispose() (KernelObject), which returns the
/// block to the MemPool.  Refcount is atomic (SMP-safe) — the previous plain
/// `int refcount` decrement raced on a multi-CPU target.
struct PipeBuffer : public KernelObject {
    uint8_t data[PIPE_BUF_SIZE]; ///< Circular buffer.
    size_t read_pos = 0;         ///< Read cursor position.
    size_t write_pos = 0;        ///< Write cursor position.
    size_t count = 0;            ///< Number of bytes currently in the buffer.
    bool read_closed = false;    ///< True when the read end is closed.
    bool write_closed = false;   ///< True when the write end is closed.
    sync::Semaphore data_avail;  ///< Semaphore signalled when data is written.

    /// @brief Final teardown on the last release: releases the pipe-buffer
    ///        ResourceTracker slot and returns the block to the MemPool.
    ///        Semaphore has no user-defined destructor (its members are
    ///        trivial), so freeing without invoking it is safe; waiters are
    ///        drained by the close paths (post() + closed flags) before the
    ///        last release.  On SMP, drain must be synchronized with dispose.
    void dispose() noexcept override {
        kernel::test::ResourceTracker::instance().track_pipe_buffer_remove();
        MemPool::free(this);
    }

    /// @brief Genuinely shared across the two pipe endpoints.
    bool is_shared() const noexcept override { return true; }
};

/// @brief Read data from the pipe.
static int64_t pipe_read(Vnode &self, uint8_t *buffer, uint64_t count,
                         uint64_t) {
    auto *pb = static_cast<PipeBuffer *>(self.private_data);
    if (!pb || pb->read_closed)
        return VFS_INVALID;
    if (pb->write_closed && pb->count == 0)
        return 0;
    while (pb->count == 0) {
        if (pb->write_closed)
            return 0;
        if (pb->read_closed)
            return VFS_INVALID;
        pb->data_avail.wait();
    }
    uint64_t total = 0;
    while (total < count && pb->count > 0) {
        buffer[total++] = pb->data[pb->read_pos];
        pb->read_pos = (pb->read_pos + 1) % PIPE_BUF_SIZE;
        --pb->count;
    }
    return static_cast<int64_t>(total);
}

/// @brief Write data to the pipe.
static int64_t pipe_write(Vnode &self, const uint8_t *buf, uint64_t count,
                          uint64_t) {
    auto *pb = static_cast<PipeBuffer *>(self.private_data);
    if (!pb || pb->read_closed)
        return VFS_INVALID;
    if (pb->write_closed)
        return VFS_INVALID;
    uint64_t total = 0;
    while (total < count) {
        if (pb->count >= PIPE_BUF_SIZE) {
            return static_cast<int64_t>(total);
        }
        pb->data[pb->write_pos] = buf[total++];
        pb->write_pos = (pb->write_pos + 1) % PIPE_BUF_SIZE;
        ++pb->count;
    }
    pb->data_avail.post();
    return static_cast<int64_t>(total);
}

/// @brief Open a pipe vnode.
static int pipe_open(Vnode &, uint64_t) {
    return 0;
}

/// @brief Close the read end of a pipe.
static void pipe_read_close(Vnode &self) {
    auto *pb = static_cast<PipeBuffer *>(self.private_data);
    if (!pb)
        return;
    pb->read_closed = true;
    // Drop this endpoint's reference.  The last release (both ends closed)
    // invokes dispose() -> track_remove + MemPool::free.  Never touch pb
    // after this — it may be freed here.
    pb->release();
    self.private_data = nullptr;
    kernel::test::ResourceTracker::instance().track_vnode_remove();
    MemPool::free(&self);
}

/// @brief Close the write end of a pipe.
static void pipe_write_close(Vnode &self) {
    auto *pb = static_cast<PipeBuffer *>(self.private_data);
    if (!pb)
        return;
    pb->write_closed = true;
    // Wake readers while pb is still alive (before release).
    pb->data_avail.post();
    // Drop this endpoint's reference; last release disposes the block.
    pb->release();
    self.private_data = nullptr;
    kernel::test::ResourceTracker::instance().track_vnode_remove();
    MemPool::free(&self);
}

/// @brief Seek on a pipe (not supported).
static int64_t pipe_lseek(Vnode &, int64_t, int, uint64_t *) {
    return VFS_INVALID;
}
/// @brief Get pipe status.
static int pipe_fstat(Vnode &, VfsStat &vfs_stat) {
    vfs_stat.st_size = 0;
    vfs_stat.st_mode = S_IFCHR;
    return 0;
}
/// @brief I/O control on pipe (not supported).
static int pipe_ioctl(Vnode &, uint64_t, kernel::CheckedPtr<uint8_t>) {
    return VFS_INVALID;
}
/// @brief Read directory on pipe (not supported).
static int pipe_readdir(Vnode &, uint64_t &, Dirent &) {
    return VFS_INVALID;
}
/// @brief Look up child in pipe (not supported).
static Vnode *pipe_lookup(Vnode &, const char *) {
    return nullptr;
}

static const VnodeOps pipe_read_ops = {
    pipe_read,   nullptr,    pipe_open,  pipe_read_close,
    pipe_lseek,  pipe_fstat, pipe_ioctl, pipe_readdir,
    pipe_lookup, nullptr,    nullptr,
    nullptr, // create
};

static const VnodeOps pipe_write_ops = {
    nullptr,     pipe_write, pipe_open,  pipe_write_close,
    pipe_lseek,  pipe_fstat, pipe_ioctl, pipe_readdir,
    pipe_lookup, nullptr,    nullptr,
    nullptr, // create
};

/// @brief Create a pair of connected pipe file descriptors.
/// @return 0 on success, VFS_INVALID on failure.
///
/// Refcount handoff (creator-ref -> two-end-refs):
///   1. alloc + placement-new: refcount=1 (creator), mark_pool_backed.
///   2. alloc rnode/wnode — failure -> pb->release() (1->0 -> dispose).
///   3. alloc rfd/wfd — failure -> pb->release() (creator) + free vnodes/fds.
///   4. set vnode fields incl. private_data = pb (NOT published yet).
///   5. pb->acquire() for read end, pb->acquire() for write end -> refcount 3.
///   6. publish both vnodes into the fd_table slots.
///   7. pb->release() (creator drop) -> refcount 2 (one per endpoint).
/// Every error path is a single symmetric release of the references taken so
/// far; no path can double-free or leak the block.
int create_pipe(int fds[2]) {
    auto *pb = static_cast<PipeBuffer *>(MemPool::alloc(sizeof(PipeBuffer)));
    if (!pb)
        return VFS_INVALID;
    new (pb) PipeBuffer;
    pb->mark_pool_backed();
    pb->data_avail.init(0, PIPE_BUF_SIZE);
    kernel::test::ResourceTracker::instance().track_pipe_buffer_add();

    auto *rnode = static_cast<Vnode *>(MemPool::alloc(sizeof(Vnode)));
    auto *wnode = static_cast<Vnode *>(MemPool::alloc(sizeof(Vnode)));
    if (!rnode || !wnode) {
        pb->release(); // creator -> dispose
        MemPool::free(rnode);
        MemPool::free(wnode);
        return VFS_INVALID;
    }
    kernel::test::ResourceTracker::instance().track_vnode_add();
    kernel::test::ResourceTracker::instance().track_vnode_add();

    rnode->ops = &pipe_read_ops;
    rnode->ino = 0;
    rnode->size = PIPE_BUF_SIZE;
    rnode->mode = S_IFCHR;
    rnode->private_data = pb;
    rnode->refcount = 1;

    wnode->ops = &pipe_write_ops;
    wnode->ino = 0;
    wnode->size = PIPE_BUF_SIZE;
    wnode->mode = S_IFCHR;
    wnode->private_data = pb;
    wnode->refcount = 1;

    auto *task = Scheduler::current_task();
    if (!task) {
        kernel::test::ResourceTracker::instance().track_vnode_remove();
        kernel::test::ResourceTracker::instance().track_vnode_remove();
        pb->release(); // creator -> dispose
        MemPool::free(rnode);
        MemPool::free(wnode);
        return VFS_INVALID;
    }

    int rfd = task->fd_table.alloc();
    int wfd = task->fd_table.alloc();
    if (rfd < 0 || wfd < 0) {
        if (rfd >= 0)
            task->fd_table.free(rfd);
        if (wfd >= 0)
            task->fd_table.free(wfd);
        kernel::test::ResourceTracker::instance().track_vnode_remove();
        kernel::test::ResourceTracker::instance().track_vnode_remove();
        pb->release(); // creator -> dispose
        MemPool::free(rnode);
        MemPool::free(wnode);
        return VFS_INVALID;
    }

    // Take the two endpoint references BEFORE publishing the vnodes so a
    // close on either fd can never observe an under-referenced pb.
    pb->acquire(); // read end
    pb->acquire(); // write end

    task->fd_table.fds[rfd].vnode = rnode;
    task->fd_table.fds[rfd].offset = 0;
    task->fd_table.fds[rfd].flags = 0;

    task->fd_table.fds[wfd].vnode = wnode;
    task->fd_table.fds[wfd].offset = 0;
    task->fd_table.fds[wfd].flags = 0;

    fds[0] = rfd;
    fds[1] = wfd;

    pb->release(); // creator drop -> refcount 2 (one per endpoint)
    return 0;
}

} // namespace vfs
} // namespace kernel