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

/// @file syscall_handlers_process.cpp
/// @brief Syscall handlers for process operations: exec, fork, waitpid, getpid,
/// kill, etc.

#include <kernel/syscall/syscall.hpp>
#include <kernel/syscall/syscall_helpers.hpp>
#include <kernel/task/scheduler.hpp>
#include <kernel/task/task.hpp>
#include <kernel/vfs/vfs.hpp>
#include <kernel/elf/elf.hpp>
#include <kernel/memory/pmm.hpp>
#include <kernel/memory/mempool.hpp>
#include <kernel/memory/checked_ptr.hpp>
#include <kernel/test/test_isolate.hpp>
#include <signal.hpp>
#include <constants.hpp>
#include <string.hpp>

namespace kernel {

uint64_t Syscall::sys_fork(uint64_t, uint64_t, uint64_t, uint64_t,
                           uint64_t *regs) {
    if (!regs)
        return static_cast<uint64_t>(-1);
    auto *child = TaskControlBlock::clone(regs);
    if (!child)
        return static_cast<uint64_t>(-1);
    Scheduler::add_task(*child);
    return child->id;
}

uint64_t Syscall::sys_waitpid(uint64_t arg0, uint64_t arg1, uint64_t arg2,
                              uint64_t, uint64_t *) {
    uint64_t target_pid = arg0;
    // NOLINTNEXTLINE(performance-no-int-to-ptr)
    auto status = checked(reinterpret_cast<uint64_t *>(arg1));
    auto *status_ptr = (!syscall_is_user_task() || status.valid())
                           ? status.unsafe_ptr()
                           : nullptr;
    auto *cur = syscall_task();
    if (!cur)
        return static_cast<uint64_t>(-1);
    TaskControlBlock *child = nullptr;
    uint64_t count = Scheduler::task_count();
    for (uint64_t i = 0; i < count; ++i) {
        auto *t = Scheduler::task_at(i);
        if (t && t->parent_id == cur->id) {
            if (target_pid == static_cast<uint64_t>(-1) ||
                t->id == target_pid) {
                child = t;
                break;
            }
        }
    }
    if (!child)
        return static_cast<uint64_t>(-1);
    if (child->state == TaskState::TERMINATED) {
        if (status_ptr) {
            // MP-4 (SMAP): user write via safe_copy_to_user (stac-wrapped);
            // kernel-task callers write directly.
            uint64_t code = child->exit_code;
            if (syscall_is_user_task()) {
                if (!safe_copy_to_user(status.unsafe_ptr(), &code, 1))
                    return static_cast<uint64_t>(-1);
            } else {
                *status_ptr = code;
            }
        }
        uint64_t cid = child->id;
        cur->remove_child(child);
        child->cleanup();
        Scheduler::remove_task(*child);
        delete child;
        return cid;
    }
    if (arg2 & 1)
        return 0;
    cur->waiting_child_pid = target_pid;
    // MP-4 (SMAP): the stored status pointer is written later by
    // Scheduler::wake_waiting_parent under the parent's CR3.  For user tasks
    // pre-certify the page is MAPPED (range validity alone is insufficient —
    // an unmapped-but-valid VA would #PF in the wake path).  Kernel tasks pass
    // a kernel pointer (no user page check needed).
    if (syscall_is_user_task() && status.unsafe_ptr() &&
        VMM::virt_to_phys_in_pml4(
            reinterpret_cast<uint64_t>(status.unsafe_ptr()),
            cur->page_table_) == 0) {
        return static_cast<uint64_t>(-1);
    }
    cur->waiting_child_status = status_ptr;
    Scheduler::dequeue_ready(*cur);
    cur->state = TaskState::BLOCKED;
    return static_cast<uint64_t>(-1);
}

// VULN-H4/W1: hard bounds on the exec argv/envp scan.  Without these, a
// malicious task can place a non-NUL-terminated string abutting an unmapped
// page to trigger an unvalidated kernel-side page fault (DoS), or pass an
// unbounded entry count that stalls the syscall (WCET violation).
static constexpr size_t MAX_EXEC_ARGS = 64;
static constexpr size_t MAX_EXEC_ARG_LEN = SYSCALL_MAX_PATH; // 256

/// @brief Validate a user-space argv/envp array and bound every string.
/// @param ptr User pointer to the null-terminated pointer array.
/// @param is_user_task true if the caller is a Ring-3 task.
/// @param[in,out] out_total_len Accumulates the combined (argv+envp) string
///        byte total incl. terminators, for VULN-U2's stack reservation check.
/// @return true if the whole array (and every string window) is valid.
static bool validate_argv_envp(const char *const *ptr, bool is_user_task,
                               uint64_t *out_total_len = nullptr) {
    if (!ptr)
        return true;
    if (is_user_task) {
        auto arr = checked(ptr, static_cast<size_t>(1));
        if (!arr.valid())
            return false;
        const char *const *p = ptr;
        size_t arg_count = 0;
        g_user_access_recover_ip = reinterpret_cast<uint64_t>(&&recover_exec);
        arch::stac();
        while (*p) {
            if (++arg_count > MAX_EXEC_ARGS) {
                arch::clac();
                g_user_access_recover_ip = 0;
                return false;
            }
            // Validate the ENTIRE maximum-length window up front, so every
            // byte touched by the scan below is already certified mapped.
            auto s = checked(*p, static_cast<uint64_t>(MAX_EXEC_ARG_LEN));
            if (!s.valid()) {
                arch::clac();
                g_user_access_recover_ip = 0;
                return false;
            }
            size_t len = 0;
            for (; len < MAX_EXEC_ARG_LEN; ++len) {
                if (s.unsafe_ptr()[len] == '\0')
                    break;
                if (len == MAX_EXEC_ARG_LEN - 1) {
                    arch::clac();
                    g_user_access_recover_ip = 0;
                    return false; // unterminated within the window
                }
            }
            if (out_total_len)
                *out_total_len += len + 1;
            ++p;
            auto next = checked(p, static_cast<size_t>(1));
            if (!next.valid()) {
                arch::clac();
                g_user_access_recover_ip = 0;
                return false;
            }
        }
        arch::clac();
        g_user_access_recover_ip = 0;
        return true;
    }
    return true;

recover_exec:
    arch::clac();
    g_user_access_recover_ip = 0;
    return false;
}

uint64_t Syscall::sys_exec(uint64_t arg0, uint64_t arg1, uint64_t arg2,
                           uint64_t, uint64_t *regs) {
    kernel::test::mark_vfs_touched();
    if (!syscall_is_user_task())
        return static_cast<uint64_t>(-1);
    vfs::Vnode *vn = nullptr;
    char path_buf[SYSCALL_MAX_PATH];
    // NOLINTNEXTLINE(performance-no-int-to-ptr)
    if (!strncpy_from_user(path_buf, reinterpret_cast<const char *>(arg0),
                           SYSCALL_MAX_PATH))
        return static_cast<uint64_t>(-1);
    vn = vfs::resolve(path_buf);
    if (!vn)
        return static_cast<uint64_t>(-1);
    // NOLINTNEXTLINE(performance-no-int-to-ptr)
    auto *argv = reinterpret_cast<const char *const *>(arg1);
    // NOLINTNEXTLINE(performance-no-int-to-ptr)
    auto *envp = reinterpret_cast<const char *const *>(arg2);
    // VULN-H4/W1 + VULN-U2: validate both arrays with hard bounds and
    // accumulate the combined string length for the stack-reservation check.
    uint64_t str_total = 0;
    if (!validate_argv_envp(argv, true, &str_total))
        return static_cast<uint64_t>(-1);
    if (!validate_argv_envp(envp, true, &str_total))
        return static_cast<uint64_t>(-1);
    if (vn->size == 0 || vn->size > 512_KiB)
        return static_cast<uint64_t>(-1);
    size_t file_pages = (static_cast<size_t>(vn->size) + 4095) / 4096;
    uint64_t file_phys = PMM::alloc_contiguous(file_pages);
    if (!file_phys)
        return static_cast<uint64_t>(-1);
    // NOLINTNEXTLINE(performance-no-int-to-ptr)
    uint8_t *file_buf =
        reinterpret_cast<uint8_t *>(arch::HHDM_OFFSET + file_phys);
    int64_t r = vn->ops->read(*vn, file_buf, vn->size, 0);
    if (r <= 0 || static_cast<uint64_t>(r) != vn->size) {
        for (size_t i = 0; i < file_pages; ++i)
            PMM::free_page(file_phys + i * 4096);
        return static_cast<uint64_t>(-1);
    }
    auto *hdr = reinterpret_cast<const elf::ELF64Header *>(file_buf);
    // VULN-H2: pass the actual bytes read so validate_segment bounds
    // phdr->offset+filesz against the real file, not a constant.
    if (!elf::exec_into_current(hdr, file_buf, argv, envp, regs,
                                static_cast<uint64_t>(r))) {
        for (size_t i = 0; i < file_pages; ++i)
            PMM::free_page(file_phys + i * 4096);
        return static_cast<uint64_t>(-1);
    }
    for (size_t i = 0; i < file_pages; ++i)
        PMM::free_page(file_phys + i * 4096);
    return 0;
}

uint64_t Syscall::sys_kill(uint64_t arg0, uint64_t arg1, uint64_t, uint64_t,
                           uint64_t *) {
    uint64_t target_pid = arg0;
    uint64_t sig = arg1;
    if (sig >= MAX_SIGNAL_HANDLERS)
        return static_cast<uint64_t>(-1);
    // SIG_NONE (signal 0) is the null signal — POSIX existence check, no
    // delivery
    if (sig == static_cast<uint64_t>(kernel::Signal::SIG_NONE))
        return 0;
    auto *t = Scheduler::find_task(target_pid);
    if (!t)
        return static_cast<uint64_t>(-1);
    if (t == syscall_task()) {
        if (signal_is_fatal(sig) || !t->has_signal_handler(sig)) {
            t->state = TaskState::TERMINATED;
            t->exit_code = static_cast<uint64_t>(-static_cast<int64_t>(sig));
        } else {
            t->pending_signals |= (1ULL << sig);
        }
    } else {
        t->pending_signals |= (1ULL << sig);
    }
    return 0;
}

uint64_t Syscall::sys_signal(uint64_t arg0, uint64_t arg1, uint64_t, uint64_t,
                             uint64_t *) {
    auto *t = syscall_task();
    if (!t)
        return static_cast<uint64_t>(-1);
    uint64_t sig = arg0;
    if (sig >= MAX_SIGNAL_HANDLERS)
        return static_cast<uint64_t>(-1);
    if (signal_is_fatal(sig))
        return static_cast<uint64_t>(-1);
    // SIG_NONE cannot be caught or ignored
    if (sig == static_cast<uint64_t>(kernel::Signal::SIG_NONE))
        return 0;
    // NOLINTNEXTLINE(performance-no-int-to-ptr)
    auto handler = reinterpret_cast<sighandler_t>(arg1);
    t->set_signal_handler(sig, handler);
    return 0;
}

uint64_t Syscall::sys_sigreturn(uint64_t, uint64_t, uint64_t, uint64_t,
                                uint64_t *regs) {
    auto *t = syscall_task();
    if (!t || !regs)
        return static_cast<uint64_t>(-1);
    uint64_t user_rsp = regs[20];
    // NOLINTNEXTLINE(performance-no-int-to-ptr)
    auto *frame = reinterpret_cast<const SignalFrame *>(
        // NOLINTNEXTLINE(performance-no-int-to-ptr)
        reinterpret_cast<uint64_t *>(user_rsp));
    auto chk = checked(frame, 1);
    if (!chk.valid())
        return static_cast<uint64_t>(-1);
    // MP-4 (SMAP): copy the frame to a kernel local (stac-wrapped) instead of
    // dereferencing user memory directly.
    kernel::SignalFrame kf{};
    if (!safe_copy_from_user(&kf, frame, 1))
        return static_cast<uint64_t>(-1);
    regs[17] = kf.saved_rip;
    regs[20] = kf.saved_rsp;
    regs[19] = kf.saved_rflags;
    regs[18] = kf.saved_cs;
    regs[21] = kf.saved_ss;
    regs[0] = 0;
    return 0;
}

} // namespace kernel
