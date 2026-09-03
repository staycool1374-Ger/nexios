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

/// @file syscall.cpp
/// @brief System call dispatcher — init (MSR setup), handle (table dispatch),
/// helpers.

#include <kernel/syscall/syscall.hpp>
#include <kernel/syscall/syscall_helpers.hpp>
#include <kernel/core/global_state.hpp>
#include <kernel/task/scheduler.hpp>
#include <kernel/task/task.hpp>
#include <kernel/vfs/vfs.hpp>
#include <kernel/arch/msr.hpp>
#include <kernel/arch/io.hpp>
#include <constants.hpp>

namespace kernel {

/// @brief Initialise the syscall interface (syscall-table built at compile
/// time).
void Syscall::init() {
#if defined(CONFIG_ARCH_X86_64)
    // P7 / gs-base-swapgs-audit F-1 (issue #6): the LSTAR/sysret path is
    // REMOVED.  MSR_KERNEL_GS_BASE was never written, so syscall_entry's
    // swapgs left GS base 0 and `mov [gs:0],rsp` faulted on phys 0 — a
    // guaranteed panic on any ring-3 `syscall` (0F 05).  The sole live syscall
    // path is `int $0x80` (isr_128 trap gate, GS-free).  The fastpath redesign
    // lives in docs/specs/syscall-fastpath.md (v0.4.3).
#elif defined(CONFIG_ARCH_AARCH64)
    // AArch64 syscalls use SVC #0; VBAR_EL1 entry point handles dispatch.
    // No MSR-based syscall setup needed.
#endif
}

/// @brief Get the currently running task from the scheduler.
TaskControlBlock *syscall_task() {
    return Scheduler::current_task();
}

/// @brief MP-3 canary verification for the FULL syscall path (issue #92).
///        Checks the current user task's segment canaries; a mismatch latches
///        g_canary_trip in test mode (returns false → the caller returns -1)
///        or panics in production.  Runs ONLY on the FULL path — the FAST
///        subset dereferences no user memory, so it is covered instead by the
///        scheduler's context-switch / timer-tick canary sampling.
/// @param regs Register save area (for the fault RIP in test-mode latch).
/// @return true if canaries verified or no user task; false on a trip (caller
///         returns -1).  Always true when CONFIG_CANARY_GUARD is disabled.
bool canary_check_on_full_path(uint64_t *regs) {
#if CONFIG_CANARY_GUARD
    auto *t = syscall_task();
    if (t && t->is_user_) {
        uint8_t bad_seg = 0;
        uint64_t bad_va = 0;
        if (!canary_verify_user_segments(t, bad_seg, bad_va)) {
            uint64_t rip = regs ? regs[17] : 0;
            if (kernel::Scheduler::is_test_active()) {
                kernel::gs::set_canary_trip(t->id, bad_seg, rip);
                return false;
            }
            kernel::Logger::fatal(
                "CANARY TRIP: task '%s' id=%u segment=%u va=0x%lx rip=0x%lx",
                t->name, static_cast<unsigned>(t->id),
                static_cast<unsigned>(bad_seg), bad_va, rip);
            panic("software sentinel canary violated");
        }
    }
#endif
    return true;
}

/// @brief Check if the current task is a user-space task (is_user_ flag —
///        MP-1: page_table_ != 0 no longer discriminates, every task owns a
///        private kernel-half PML4).
bool syscall_is_user_task() {
    auto *t = syscall_task();
    return t && t->is_user_;
}

/// @brief Open a vnode as a file descriptor in the current task's fd table.
int syscall_task_open(vfs::Vnode *vn, uint64_t flags) {
    int fd = syscall_task()->fd_table.alloc();
    if (fd < 0)
        return -1;
    syscall_task()->fd_table.fds[fd].vnode = vn;
    syscall_task()->fd_table.fds[fd].offset = 0;
    syscall_task()->fd_table.fds[fd].flags = flags;
    if (vn->ops->open)
        vn->ops->open(*vn, flags);
    return fd;
}

/// @brief Resolve a path and open it as a file descriptor.
int syscall_path_open(const char *path, uint64_t flags) {
    vfs::Vnode *vn = vfs::resolve(path);
    if (!vn) {
        if (flags & vfs::O_CREAT) {
            if (vfs::create(path, vfs::S_IFREG) != 0)
                return -1;
            vn = vfs::resolve(path);
        }
        if (!vn)
            return -1;
    }
    return syscall_task_open(vn, flags);
}

 uint64_t Syscall::handle(uint64_t number, uint64_t arg0, uint64_t arg1,
                         uint64_t arg2, uint64_t arg3, uint64_t *regs) {
    if (number >= static_cast<uint64_t>(SyscallNumber::MAX_SYSCALL))
        return static_cast<uint64_t>(-1);

#if defined(CONFIG_DEBUG) && defined(CONFIG_ARCH_X86_64)
    // MP-4 (SMAP) AC-leak detector: AC must be 0 on syscall entry.  A leaked
    // AC=1 (a missed clac in a user-access path) would otherwise be handed to
    // user mode via sysret (IA32_FMASK masks IF only, not AC), letting ring-3
    // access supervisor pages.  Fail fast in debug.  Kept on BOTH paths (FAST
    // and FULL) — the ~2-cycle debug-only read preserves the MP-4 leak
    // detector (issue #92 deviates from the paper's "bounds check only" here).
    if ((arch::read_rflags() & (1ULL << 18)) != 0)
        panic("MP-4: AC flag leaked into syscall entry (missing clac)");
#endif

    // Issue #92 tiered dispatch: the FAST subset (pointer-free members of
    // SYSCALL_FAST_MASK) skips the canary walk entirely — it dereferences no
    // user memory, so there is nothing for the canary to protect.  FULL-path
    // syscalls keep the canary verify (see canary_check_on_full_path below).
    // Bounds are already checked above — exactly one check before indexing.
    if (s_fastpath_enabled_ && (SYSCALL_FAST_MASK & (1ULL << number)))
        return syscall_table_[number](arg0, arg1, arg2, arg3, regs);

    // FULL path: MP-3 canary verify of user segments (page-table reads, no
    // locks).  Mismatch → controlled panic in production; test mode latches
    // g_canary_trip and the syscall returns -1 so the harness survives.
    // Canary relocation (issue #92): this check now runs ONLY on the FULL
    // path; FAST members are covered by the context-switch / timer-tick
    // sampling in the scheduler (canary_check_in_scheduler_hooks).
    if (!canary_check_on_full_path(regs))
        return static_cast<uint64_t>(-1);

    return syscall_table_[number](arg0, arg1, arg2, arg3, regs);
}

 uint64_t Syscall::handle_fast(uint64_t number, uint64_t arg0, uint64_t arg1,
                              uint64_t arg2, uint64_t arg3, uint64_t *regs) {
    if (number >= static_cast<uint64_t>(SyscallNumber::MAX_SYSCALL))
        return static_cast<uint64_t>(-1);
    // Lean path: bounds check only, then direct table dispatch.  The caller
    // guarantees `number` is a FAST member (no user-pointer dereference).
    return syscall_table_[number](arg0, arg1, arg2, arg3, regs);
}

} // namespace kernel
