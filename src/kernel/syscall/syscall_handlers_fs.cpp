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

/// @file syscall_handlers_fs.cpp
/// @brief Syscall handlers for file-system operations: open, read, write,
/// close, stat, etc.

#include <kernel/syscall/syscall.hpp>
#include <kernel/syscall/syscall_helpers.hpp>
#include <kernel/task/task.hpp>
#include <kernel/task/scheduler.hpp>
#include <kernel/vfs/vfs.hpp>
#include <kernel/vfs/vfsd.hpp>
#include <kernel/vfs/pipe.hpp>
#include <kernel/ipc/ipc.hpp>
#include <kernel/memory/checked_ptr.hpp>
#include <kernel/memory/mempool.hpp>
#include <kernel/memory/vmm.hpp>
#include <kernel/test/test_isolate.hpp>
#include <string.hpp>

namespace kernel {

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
static bool vfsd_authorize(uint64_t op_type, uint64_t pid, const char *path,
                           uint64_t ino = 0) {
    // Kernel tasks (is_user_ == false) are trusted — bypass IPC authorization
    auto *cur = kernel::Scheduler::current_task();
    if (cur && !cur->is_user_)
        return true;

    uint64_t vfsd_pid = vfsd::get_vfsd_pid();
    if (vfsd_pid == 0)
        return false;
    if (vfsd::is_vfsd_task())
        return true;

    vfsd::Msg msg{};
    msg.sender_id = pid;
    msg.type = op_type;
    msg.arg0 = 0;
    // VULN-C4: carry the resolved object's inode so vfsd authorizes against
    // the specific resolved object, not just the path string.  `arg1` is
    // unused by every existing vfsd handler and by userspace/vfsd.c, so no
    // layout change is required on either side.
    msg.arg1 = ino;
    size_t i = 0;
    if (path) {
        while (path[i] && i < sizeof(msg.path) - 1) {
            msg.path[i] = path[i];
            ++i;
        }
    }
    msg.path[i] = '\0';

    Message send_msg{};
    send_msg.sender_id = pid;
    send_msg.type = op_type;
    send_msg.data_size = sizeof(vfsd::Msg);
    __builtin_memcpy(send_msg.data, &msg, sizeof(vfsd::Msg));

    Message reply_msg{};
    if (!IPC::send_sync(vfsd_pid, send_msg, reply_msg))
        return false;
    if (reply_msg.data_size < sizeof(vfsd::Reply))
        return false;

    vfsd::Reply reply{};
    __builtin_memcpy(&reply, reply_msg.data, sizeof(vfsd::Reply));
    return reply.result >= 0;
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
static bool vfsd_authorize_fd_op(uint64_t op_type, uint64_t pid, int fd) {
    char fd_str[16] = {0};
    int len = 0;
    int n = fd;
    if (n == 0) {
        fd_str[len++] = '0';
    } else {
        char tmp[16];
        int tmp_len = 0;
        while (n > 0) {
            tmp[tmp_len++] = static_cast<char>('0' + (n % 10));
            n /= 10;
        }
        for (int j = tmp_len - 1; j >= 0; --j)
            fd_str[len++] = tmp[j];
    }
    fd_str[len] = '\0';

    // Kernel tasks (is_user_ == false) are trusted — bypass IPC authorization
    auto *cur = kernel::Scheduler::current_task();
    if (cur && !cur->is_user_)
        return true;

    uint64_t vfsd_pid = vfsd::get_vfsd_pid();
    if (vfsd_pid == 0)
        return false;
    if (vfsd::is_vfsd_task())
        return true;

    vfsd::Msg msg{};
    msg.sender_id = pid;
    msg.type = op_type;
    msg.arg0 = static_cast<uint64_t>(fd);
    size_t i = 0;
    while (fd_str[i] && i < sizeof(msg.path) - 1) {
        msg.path[i] = fd_str[i];
        ++i;
    }
    msg.path[i] = '\0';

    Message send_msg{};
    send_msg.sender_id = pid;
    send_msg.type = op_type;
    send_msg.data_size = sizeof(vfsd::Msg);
    __builtin_memcpy(send_msg.data, &msg, sizeof(vfsd::Msg));

    Message reply_msg{};
    if (!IPC::send_sync(vfsd_pid, send_msg, reply_msg))
        return false;
    if (reply_msg.data_size < sizeof(vfsd::Reply))
        return false;

    vfsd::Reply reply{};
    __builtin_memcpy(&reply, reply_msg.data, sizeof(vfsd::Reply));
    return reply.result >= 0;
}

/// @brief Resolve-first authorization for path syscalls (VULN-C4).
/// Resolves the target BEFORE the (blocking) vfsd_authorize IPC, captures the
/// resolved object's identity (ino + fs-instance), authorizes, then re-resolves
/// and compares identity.  If the object changed while the CPU was yielded to
/// vfsd, the syscall fails instead of operating on a different object.
/// @param op_type vfsd operation type (VFS_OPEN, VFS_STAT, ...).
/// @param pid Sender task id.
/// @param path Path to authorize + operate on.
/// @param[out] out_vn The re-validated resolved vnode on success.
/// @return true if authorized AND object identity unchanged.
static bool resolve_then_authorize(uint64_t op_type, uint64_t pid,
                                   const char *path, vfs::Vnode *&out_vn) {
    vfs::Vnode *vn = vfs::resolve(path);
    if (!vn)
        return false;
    uint64_t ino = vn->ino;
    if (!vfsd_authorize(op_type, pid, path, ino))
        return false;
    vfs::Vnode *vn2 = vfs::resolve(path);
    if (!vn2 || vn2 != vn || vn2->ino != ino)
        return false;
    out_vn = vn2;
    return true;
}

/// @brief Resolve-first authorization for create/mkdir/unlink/rmdir, which
/// operate on the parent directory + leaf name.
static bool resolve_parent_then_authorize(uint64_t op_type, uint64_t pid,
                                          const char *path, const char *&out_name,
                                          vfs::Vnode *&out_parent) {
    const char *name = nullptr;
    vfs::Vnode *parent = vfs::resolve_parent(path, name);
    if (!parent || !name || !*name)
        return false;
    uint64_t ino = parent->ino;
    if (!vfsd_authorize(op_type, pid, path, ino))
        return false;
    const char *name2 = nullptr;
    vfs::Vnode *parent2 = vfs::resolve_parent(path, name2);
    if (!parent2 || parent2 != parent || parent2->ino != ino || !name2 ||
        *name2 == '\0')
        return false;
    out_name = name2;
    out_parent = parent2;
    return true;
}

uint64_t Syscall::sys_open(uint64_t arg0, uint64_t arg1, uint64_t, uint64_t,
                           uint64_t *) {
    kernel::test::mark_vfs_touched();
    // NOLINTNEXTLINE(performance-no-int-to-ptr)
    const char *user_path = reinterpret_cast<const char *>(arg0);
    uint64_t pid = syscall_task() ? syscall_task()->id : 0;
    char path_buf[SYSCALL_MAX_PATH];
    const char *path = user_path;
    if (syscall_is_user_task()) {
        if (!strncpy_from_user(path_buf, user_path, SYSCALL_MAX_PATH))
            return static_cast<uint64_t>(-1);
        path = path_buf;
    }
    vfs::Vnode *vn = nullptr;
    if (resolve_then_authorize(vfsd::VFS_OPEN, pid, path, vn))
        return static_cast<uint64_t>(syscall_task_open(vn, arg1));
    if (arg1 & vfs::O_CREAT) {
        // Target does not exist yet — authorize + create in the parent dir.
        const char *name = nullptr;
        vfs::Vnode *parent = nullptr;
        if (resolve_parent_then_authorize(vfsd::VFS_OPEN, pid, path, name,
                                          parent)) {
            if (parent->ops && parent->ops->create &&
                parent->ops->create(*parent, name, vfs::S_IFREG) == 0) {
                vfs::Vnode *created = vfs::resolve(path);
                if (created)
                    return static_cast<uint64_t>(syscall_task_open(created,
                                                                  arg1));
            }
        }
    }
    return static_cast<uint64_t>(-1);
}

uint64_t Syscall::sys_read(uint64_t arg0, uint64_t arg1, uint64_t arg2,
                           uint64_t, uint64_t *) {
    kernel::test::mark_vfs_touched();
    auto *cur = syscall_task();
    if (!cur)
        return static_cast<uint64_t>(-1);
    if (!vfsd_authorize_fd_op(vfsd::VFS_READ, cur->id, static_cast<int>(arg0)))
        return static_cast<uint64_t>(-1);
    auto *f = cur->fd_table.get(static_cast<int>(arg0));
    if (!f)
        return static_cast<uint64_t>(-1);
    uint64_t count = arg2;
    // NOLINTNEXTLINE(performance-no-int-to-ptr)
    auto buf = checked(reinterpret_cast<uint8_t *>(arg1), count);
    if (syscall_is_user_task() && !buf.valid())
        return static_cast<uint64_t>(-1);
    if (!f->vnode || !f->vnode->ops->read)
        return static_cast<uint64_t>(-1);
    int64_t r =
        f->vnode->ops->read(*f->vnode, buf.unsafe_ptr(), count, f->offset);
    if (r > 0)
        f->offset += static_cast<uint64_t>(r);
    return static_cast<uint64_t>(r >= 0 ? r : -1);
}

uint64_t Syscall::sys_close(uint64_t arg0, uint64_t, uint64_t, uint64_t,
                            uint64_t *) {
    kernel::test::mark_vfs_touched();
    auto *cur = syscall_task();
    if (!cur)
        return static_cast<uint64_t>(-1);
    if (!vfsd_authorize_fd_op(vfsd::VFS_CLOSE, cur->id, static_cast<int>(arg0)))
        return static_cast<uint64_t>(-1);
    int fd = static_cast<int>(arg0);
    cur->fd_table.free(fd);
    return 0;
}

uint64_t Syscall::sys_fstat(uint64_t arg0, uint64_t arg1, uint64_t, uint64_t,
                            uint64_t *) {
    kernel::test::mark_vfs_touched();
    auto *cur = syscall_task();
    if (!cur)
        return static_cast<uint64_t>(-1);
    if (!vfsd_authorize_fd_op(vfsd::VFS_FSTAT, cur->id, static_cast<int>(arg0)))
        return static_cast<uint64_t>(-1);
    auto *f = cur->fd_table.get(static_cast<int>(arg0));
    if (!f || !f->vnode || !f->vnode->ops->fstat)
        return static_cast<uint64_t>(-1);
    // NOLINTNEXTLINE(performance-no-int-to-ptr)
    auto st = checked(reinterpret_cast<vfs::VfsStat *>(arg1));
    if (syscall_is_user_task() && !st.valid())
        return static_cast<uint64_t>(-1);
    return static_cast<uint64_t>(f->vnode->ops->fstat(*f->vnode, *st.unsafe_ptr()));
}

uint64_t Syscall::sys_write(uint64_t arg0, uint64_t arg1, uint64_t arg2,
                            uint64_t, uint64_t *) {
    kernel::test::mark_vfs_touched();
    auto *cur = syscall_task();
    if (!cur)
        return static_cast<uint64_t>(-1);
    if (!vfsd_authorize_fd_op(vfsd::VFS_WRITE, cur->id, static_cast<int>(arg0)))
        return static_cast<uint64_t>(-1);
    auto *f = cur->fd_table.get(static_cast<int>(arg0));
    if (!f || !f->vnode || !f->vnode->ops->write)
        return static_cast<uint64_t>(-1);
    uint64_t count = arg2;
    // NOLINTNEXTLINE(performance-no-int-to-ptr)
    auto buf = checked(reinterpret_cast<const uint8_t *>(arg1), count);
    if (syscall_is_user_task() && !buf.valid())
        return static_cast<uint64_t>(-1);

    // Kernel tasks (no page table): buffer is already in kernel space, no
    // bounce buffer needed (mirrors the vfsd_authorize kernel bypass pattern).
    if (count == 0)
        return 0;
    if (!syscall_is_user_task()) {
        int64_t r = f->vnode->ops->write(*f->vnode, buf.unsafe_ptr(), count,
                                          f->offset);
        if (r > 0)
            f->offset += static_cast<uint64_t>(r);
        return static_cast<uint64_t>(r >= 0 ? r : 0);
    }
    // Fast path: single-page user write via HHDM (no alloc, no copy).
    // The HHDM maps all physical RAM 1:1 at arch::HHDM_OFFSET, so translating
    // the user VA to a physical address gives a directly-readable kernel pointer.
    {
        uint64_t user_va = reinterpret_cast<uint64_t>(buf.unsafe_ptr());
        uint64_t page_va = user_va & ~0xFFFULL;
        uint64_t page_off = user_va & 0xFFFULL;
        if (page_off + count <= 4096) {
            uint64_t phys = VMM::virt_to_phys_in_pml4(page_va, cur->page_table_);
            if (phys == 0)
                return static_cast<uint64_t>(-1);
            auto *kaddr = reinterpret_cast<uint8_t *>(arch::HHDM_OFFSET +
                                                      phys + page_off);
            int64_t r = f->vnode->ops->write(*f->vnode, kaddr, count,
                                              f->offset);
            if (r > 0)
                f->offset += static_cast<uint64_t>(r);
            return static_cast<uint64_t>(r >= 0 ? r : 0);
        }
    }
    // Slow path: multi-page write — kernel bounce buffer with fault recovery.
    // This path is rare (most writes fit in a single page).
    constexpr uint64_t kMaxWriteBounce = 1 << 20; // 1 MiB
    if (count > kMaxWriteBounce)
        return static_cast<uint64_t>(-1);
    uint8_t *kb = static_cast<uint8_t *>(MemPool::alloc(count));
    if (!kb)
        return static_cast<uint64_t>(-1);
    if (!safe_copy_from_user(kb, buf.unsafe_ptr(), count)) {
        MemPool::free(kb);
        return static_cast<uint64_t>(-1);
    }
    int64_t r = f->vnode->ops->write(*f->vnode, kb, count, f->offset);
    MemPool::free(kb);
    if (r > 0)
        f->offset += static_cast<uint64_t>(r);
    return static_cast<uint64_t>(r >= 0 ? r : 0);
}

uint64_t Syscall::sys_lseek(uint64_t arg0, uint64_t arg1, uint64_t arg2,
                            uint64_t, uint64_t *) {
    kernel::test::mark_vfs_touched();
    auto *cur = syscall_task();
    if (!cur)
        return static_cast<uint64_t>(-1);
    auto *f = cur->fd_table.get(static_cast<int>(arg0));
    if (!f || !f->vnode || !f->vnode->ops->lseek)
        return static_cast<uint64_t>(-1);
    int64_t r = f->vnode->ops->lseek(*f->vnode, static_cast<int64_t>(arg1),
                                     static_cast<int>(arg2), &f->offset);
    return static_cast<uint64_t>(r >= 0 ? r : -1);
}

uint64_t Syscall::sys_ioctl(uint64_t arg0, uint64_t arg1, uint64_t arg2,
                            uint64_t, uint64_t *) {
    kernel::test::mark_vfs_touched();
    auto *cur = syscall_task();
    if (!cur)
        return static_cast<uint64_t>(-1);
    auto *f = cur->fd_table.get(static_cast<int>(arg0));
    if (!f || !f->vnode || !f->vnode->ops->ioctl)
        return static_cast<uint64_t>(-1);
    // NOLINTNEXTLINE(performance-no-int-to-ptr)
    auto arg_chk = checked(reinterpret_cast<uint8_t *>(arg2), sizeof(uint64_t));
    if (syscall_is_user_task() && arg2 != 0 && !arg_chk.valid())
        return static_cast<uint64_t>(-1);
    return static_cast<uint64_t>(
        f->vnode->ops->ioctl(*f->vnode, arg1, arg_chk));
}

uint64_t Syscall::sys_readdir(uint64_t arg0, uint64_t arg1, uint64_t arg2,
                              uint64_t, uint64_t *) {
    kernel::test::mark_vfs_touched();
    auto *cur = syscall_task();
    if (!cur)
        return static_cast<uint64_t>(-1);
    auto *f = cur->fd_table.get(static_cast<int>(arg0));
    if (!f || !f->vnode || !f->vnode->ops->readdir)
        return static_cast<uint64_t>(-1);
    // NOLINTNEXTLINE(performance-no-int-to-ptr)
    auto pos_chk = checked(reinterpret_cast<uint64_t *>(arg1));
    // NOLINTNEXTLINE(performance-no-int-to-ptr)
    auto dent_chk = checked(reinterpret_cast<vfs::Dirent *>(arg2));
    if (syscall_is_user_task() && (!pos_chk.valid() || !dent_chk.valid()))
        return static_cast<uint64_t>(-1);
    uint64_t position = pos_chk.read();
    int r = f->vnode->ops->readdir(*f->vnode, position, *dent_chk.unsafe_ptr());
    if (r == 0)
        pos_chk.write(position);
    return static_cast<uint64_t>(r == 0 ? 0 : -1);
}

uint64_t Syscall::sys_stat(uint64_t arg0, uint64_t arg1, uint64_t, uint64_t,
                           uint64_t *) {
    kernel::test::mark_vfs_touched();
    // NOLINTNEXTLINE(performance-no-int-to-ptr)
    const char *user_path = reinterpret_cast<const char *>(arg0);
    uint64_t pid = syscall_task() ? syscall_task()->id : 0;
    char path_buf[SYSCALL_MAX_PATH];
    const char *path = user_path;
    if (syscall_is_user_task()) {
        if (!strncpy_from_user(path_buf, user_path, SYSCALL_MAX_PATH))
            return static_cast<uint64_t>(-1);
        path = path_buf;
    }
    vfs::Vnode *vn = nullptr;
    if (!resolve_then_authorize(vfsd::VFS_STAT, pid, path, vn) || !vn ||
        !vn->ops->fstat)
        return static_cast<uint64_t>(-1);
    // NOLINTNEXTLINE(performance-no-int-to-ptr)
    auto st = checked(reinterpret_cast<vfs::VfsStat *>(arg1));
    if (syscall_is_user_task() && !st.valid())
        return static_cast<uint64_t>(-1);
    return static_cast<uint64_t>(vn->ops->fstat(*vn, *st.unsafe_ptr()));
}

uint64_t Syscall::sys_dup(uint64_t arg0, uint64_t, uint64_t, uint64_t,
                          uint64_t *) {
    kernel::test::mark_vfs_touched();
    auto *cur = syscall_task();
    if (!cur)
        return static_cast<uint64_t>(-1);
    int old_fd = static_cast<int>(arg0);
    auto *old = cur->fd_table.get(old_fd);
    if (!old)
        return static_cast<uint64_t>(-1);
    int new_fd = cur->fd_table.alloc();
    if (new_fd < 0)
        return static_cast<uint64_t>(-1);
    cur->fd_table.fds[new_fd] = cur->fd_table.fds[old_fd];
    vfs::vnode_ref_inc(old->vnode);
    return static_cast<uint64_t>(new_fd);
}

uint64_t Syscall::sys_chdir(uint64_t arg0, uint64_t, uint64_t, uint64_t,
                            uint64_t *) {
    kernel::test::mark_vfs_touched();
    // NOLINTNEXTLINE(performance-no-int-to-ptr)
    const char *user_path = reinterpret_cast<const char *>(arg0);
    uint64_t pid = syscall_task() ? syscall_task()->id : 0;
    char path_buf[SYSCALL_MAX_PATH];
    const char *path = user_path;
    const char *resolved_path = nullptr;
    if (syscall_is_user_task()) {
        if (!strncpy_from_user(path_buf, user_path, SYSCALL_MAX_PATH))
            return static_cast<uint64_t>(-1);
        path = path_buf;
        resolved_path = path_buf;
    } else {
        resolved_path = user_path;
    }
    vfs::Vnode *vn = nullptr;
    if (!resolve_then_authorize(vfsd::VFS_CHDIR, pid, path, vn) || !vn)
        return static_cast<uint64_t>(-1);
    auto *cur = syscall_task();
    if (!cur)
        return static_cast<uint64_t>(-1);
    if (!(vn->mode & vfs::S_IFDIR))
        return static_cast<uint64_t>(-1);
    cur->cwd_lock_.lock();
    if (cur->cwd_vnode)
        vfs::vnode_ref_dec(cur->cwd_vnode);
    cur->cwd_vnode = vn;
    vfs::vnode_ref_inc(vn);
    cur->cwd_lock_.unlock();
    size_t i = 0;
    while (resolved_path[i] && i < 255) {
        cur->cwd[i] = resolved_path[i];
        ++i;
    }
    cur->cwd[i] = '\0';
    return 0;
}

uint64_t Syscall::sys_pipe(uint64_t arg0, uint64_t, uint64_t, uint64_t,
                           uint64_t *) {
    kernel::test::mark_vfs_touched();
    int fds[2];
    int result = vfs::create_pipe(fds);
    if (result < 0)
        return static_cast<uint64_t>(-1);
    // NOLINTNEXTLINE(performance-no-int-to-ptr)
    auto *out = reinterpret_cast<int *>(arg0);
    if (syscall_is_user_task()) {
        auto fds_buf = checked(out, 2);
        if (!fds_buf.valid())
            return static_cast<uint64_t>(-1);
        if (!fds_buf.write(fds[0], 0))
            return static_cast<uint64_t>(-1);
        if (!fds_buf.write(fds[1], 1))
            return static_cast<uint64_t>(-1);
    } else {
        out[0] = fds[0];
        out[1] = fds[1];
    }
    return 0;
}

uint64_t Syscall::sys_dup2(uint64_t arg0, uint64_t arg1, uint64_t, uint64_t,
                           uint64_t *) {
    kernel::test::mark_vfs_touched();
    int old_fd = static_cast<int>(arg0);
    int new_fd = static_cast<int>(arg1);
    auto *t = syscall_task();
    if (!t)
        return static_cast<uint64_t>(-1);
    auto *old_desc = t->fd_table.get(old_fd);
    if (!old_desc)
        return static_cast<uint64_t>(-1);
    if (old_fd == new_fd)
        return static_cast<uint64_t>(new_fd);
    if (new_fd < 0 || static_cast<size_t>(new_fd) >= vfs::MAX_FDS)
        return static_cast<uint64_t>(-1);
    t->fd_table.free(new_fd);
    t->fd_table.fds[new_fd] = *old_desc;
    t->fd_table.fds[new_fd].used = true;
    vfs::vnode_ref_inc(old_desc->vnode);
    return static_cast<uint64_t>(new_fd);
}

uint64_t Syscall::sys_mkdir(uint64_t arg0, uint64_t arg1, uint64_t, uint64_t,
                            uint64_t *) {
    kernel::test::mark_vfs_touched();
    // NOLINTNEXTLINE(performance-no-int-to-ptr)
    const char *user_path = reinterpret_cast<const char *>(arg0);
    uint64_t pid = syscall_task() ? syscall_task()->id : 0;
    char path_buf[SYSCALL_MAX_PATH];
    const char *path = user_path;
    if (syscall_is_user_task()) {
        if (!strncpy_from_user(path_buf, user_path, SYSCALL_MAX_PATH))
            return static_cast<uint64_t>(-1);
        path = path_buf;
    }
    const char *name = nullptr;
    vfs::Vnode *parent = nullptr;
    if (!resolve_parent_then_authorize(vfsd::VFS_MKDIR, pid, path, name,
                                       parent))
        return static_cast<uint64_t>(-1);
    if (!parent->ops || !parent->ops->mkdir)
        return static_cast<uint64_t>(-1);
    int r = parent->ops->mkdir(*parent, name, static_cast<uint16_t>(arg1));
    return static_cast<uint64_t>(r == 0 ? 0 : -1);
}

uint64_t Syscall::sys_unlink(uint64_t arg0, uint64_t, uint64_t, uint64_t,
                             uint64_t *) {
    kernel::test::mark_vfs_touched();
    // NOLINTNEXTLINE(performance-no-int-to-ptr)
    const char *user_path = reinterpret_cast<const char *>(arg0);
    uint64_t pid = syscall_task() ? syscall_task()->id : 0;
    char path_buf[SYSCALL_MAX_PATH];
    const char *path = user_path;
    if (syscall_is_user_task()) {
        if (!strncpy_from_user(path_buf, user_path, SYSCALL_MAX_PATH))
            return static_cast<uint64_t>(-1);
        path = path_buf;
    }
    const char *name = nullptr;
    vfs::Vnode *parent = nullptr;
    if (!resolve_parent_then_authorize(vfsd::VFS_UNLINK, pid, path, name,
                                       parent))
        return static_cast<uint64_t>(-1);
    if (!parent->ops || !parent->ops->unlink)
        return static_cast<uint64_t>(-1);
    int r = parent->ops->unlink(*parent, name);
    return static_cast<uint64_t>(r == 0 ? 0 : -1);
}

uint64_t Syscall::sys_rmdir(uint64_t arg0, uint64_t, uint64_t, uint64_t,
                            uint64_t *) {
    kernel::test::mark_vfs_touched();
    // rmdir is just unlink with directory semantics (enforced by FS)
    // NOLINTNEXTLINE(performance-no-int-to-ptr)
    const char *user_path = reinterpret_cast<const char *>(arg0);
    uint64_t pid = syscall_task() ? syscall_task()->id : 0;
    char path_buf[SYSCALL_MAX_PATH];
    const char *path = user_path;
    if (syscall_is_user_task()) {
        if (!strncpy_from_user(path_buf, user_path, SYSCALL_MAX_PATH))
            return static_cast<uint64_t>(-1);
        path = path_buf;
    }
    const char *name = nullptr;
    vfs::Vnode *parent = nullptr;
    if (!resolve_parent_then_authorize(vfsd::VFS_RMDIR, pid, path, name,
                                       parent))
        return static_cast<uint64_t>(-1);
    if (!parent->ops || !parent->ops->unlink)
        return static_cast<uint64_t>(-1);
    int r = parent->ops->unlink(*parent, name);
    return static_cast<uint64_t>(r == 0 ? 0 : -1);
}

} // namespace kernel
