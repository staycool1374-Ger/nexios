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

/// @file syscall_handlers_misc.cpp
/// @brief Syscall handlers for miscellaneous operations: yield, print,
/// get_ticks, exit, etc.

#include <kernel/syscall/syscall.hpp>
#include <kernel/syscall/syscall_helpers.hpp>
#include <kernel/log/dmesg.hpp>
#include <kernel/random.hpp>
#include <kernel/task/scheduler.hpp>
#include <kernel/task/task.hpp>
#include <kernel/memory/checked_ptr.hpp>
#include <kernel/arch/timer.hpp>
#include <string.hpp>

#if defined(CONFIG_ARCH_X86_64)
#define TASK_STACK_PTR(t) ((t)->context.rsp)
#elif defined(CONFIG_ARCH_AARCH64)
#define TASK_STACK_PTR(t) ((t)->context.sp_el0)
#elif defined(CONFIG_ARCH_RISCV64)
#define TASK_STACK_PTR(t) ((t)->context.sp)
#endif
#include <kernel/arch/io.hpp>
#include <kernel/arch/rtc.hpp>
#include <kernel/sync/spinlock.hpp>
#include <kernel/sync/spinlock_guard.hpp>
#include <kernel/memory/checked_ptr.hpp>
#include <kernel/memory/pmm.hpp>
#include <kernel/memory/vmm.hpp>
#include <string.hpp>
#include <version.hpp>
#include <constants.hpp>

namespace kernel {

struct Timeval {
    int64_t tv_sec;
    int64_t tv_usec;
};

struct Utsname {
    char sysname[65];
    char nodename[65];
    char release[65];
    char version[65];
    char machine[65];
    char domainname[65];
};

uint64_t Syscall::sys_yield(uint64_t, uint64_t, uint64_t, uint64_t,
                            uint64_t *) {
    arch::io_wait();
    Scheduler::reschedule();
    return 0;
}

uint64_t Syscall::sys_print(uint64_t, uint64_t, uint64_t, uint64_t,
                            uint64_t *) {
    return 0;
}

uint64_t Syscall::sys_get_ticks(uint64_t, uint64_t, uint64_t, uint64_t,
                                uint64_t *) {
    return arch::Timer::ticks();
}

uint64_t Syscall::sys_getpid(uint64_t, uint64_t, uint64_t, uint64_t,
                             uint64_t *) {
    auto *t = syscall_task();
    return t ? t->id : 0;
}

uint64_t Syscall::sys_exit(uint64_t arg0, uint64_t, uint64_t, uint64_t,
                           uint64_t *) {
    auto *t = syscall_task();
    if (t) {
        t->state = TaskState::TERMINATED;
        t->exit_code = arg0;

        // If the exiting task is the current running task (the normal case for
        // sys_exit), we must switch the CPU off it BEFORE returning to user
        // space, otherwise the syscall epilogue iret's back into a TERMINATED
        // task and executes freed/garbage code (#UD -> SIGILL loop).  Use the
        // ISR-safe switch (publishes the deferred-switch slot with the exiting
        // task's own context.rsp as a dead save target); do NOT call
        // Scheduler::terminate, which invokes switch_to_task and resolves the
        // live ISR RSP as the save owner, corrupting a live task's context.
        if (Scheduler::current_task() == t) {
            Scheduler::switch_away_from_terminating(*t);
        } else {
            Scheduler::terminate(*t, arg0);
        }

        if (t->first_child) {
            TaskControlBlock *init_task = Scheduler::task_at(0);
            if (init_task) {
                auto *child = t->first_child;
                while (child) {
                    auto *next = child->next_sibling;
                    t->remove_child(child);
                    init_task->add_child(child);
                    child = next;
                }
            }
        }

        if (t->parent_id) {
            uint64_t count = Scheduler::task_count();
            for (uint64_t i = 0; i < count; ++i) {
                auto *p = Scheduler::task_at(i);
                if (p && p->id == t->parent_id &&
                    (p->waiting_child_pid == t->id ||
                     p->waiting_child_pid == static_cast<uint64_t>(-1))) {
                    // Write exit code to parent's user-space status pointer.
                    // We must switch to the parent's page table because
                    // the status address belongs to the parent's address space.
                    if (p->waiting_child_status) {
                        uint64_t old_cr3 = 0;
                        bool switched = false;
                        if (p->page_table_ &&
                            arch::read_cr3() != p->page_table_) {
                            old_cr3 = arch::read_cr3();
                            arch::write_cr3(p->page_table_);
                            switched = true;
                        }
                        // MP-4 (SMAP): the status page is a user page in the
                        // parent's address space, pre-certified MAPPED by
                        // waitpid's virt_to_phys_in_pml4 check.  The write
                        // runs with AC set (stac) — the page is present so no
                        // recover_ip is needed; clac restores AC for the
                        // kernel after the user-page store.
                        arch::stac();
                        *p->waiting_child_status = t->exit_code;
                        arch::clac();
                        if (switched)
                            arch::write_cr3(old_cr3);
                        p->waiting_child_status = nullptr;
                    }
                    p->waiting_child_pid = 0;
                    // Orphan the child so reap_orphans can clean it up
                    p->remove_child(t);
                    t->parent_id = 0;
                    // Wake the parent and override its saved RAX to return
                    // the child's PID instead of -1 (the value set when
                    // waitpid blocked).
                    if (p->state != TaskState::TERMINATED) {
                        Scheduler::set_task_ready(*p);
                        if (TASK_STACK_PTR(p)) {
                            // NOLINTNEXTLINE(performance-no-int-to-ptr)
                            auto *stack =
                                reinterpret_cast<uint64_t *>(TASK_STACK_PTR(p));
                            stack[0] = t->id;
                        }
                    }
                    break;
                }
            }
        }
    }
    return 0;
}

uint64_t Syscall::sys_gettod(uint64_t arg0, uint64_t, uint64_t, uint64_t,
                             uint64_t *) {
    // NOLINTNEXTLINE(performance-no-int-to-ptr)
    auto tv_ptr = checked(reinterpret_cast<Timeval *>(arg0));
    if (syscall_is_user_task() && !tv_ptr.valid())
        return static_cast<uint64_t>(-1);

    uint64_t secs = arch::RTC::read_seconds();
    Timeval tv = {};
    tv.tv_sec = static_cast<int64_t>(secs);
    tv.tv_usec = 0;
    // MP-4 (SMAP): local + safe_copy_to_user (stac-wrapped).
    if (syscall_is_user_task() &&
        !safe_copy_to_user(tv_ptr.unsafe_ptr(), &tv, 1))
        return static_cast<uint64_t>(-1);
    else
        *tv_ptr.unsafe_ptr() = tv;
    return 0;
}

uint64_t Syscall::sys_uname(uint64_t arg0, uint64_t, uint64_t, uint64_t,
                            uint64_t *) {
    // v0.4.0 MP-1 hardening: a null buffer must be rejected for ANY caller —
    // pre-MP-1 the boot identity map masked the write to address 0x0.
    if (arg0 == 0)
        return static_cast<uint64_t>(-1);
    // NOLINTNEXTLINE(performance-no-int-to-ptr)
    auto uts_ptr = checked(reinterpret_cast<Utsname *>(arg0));
    if (syscall_is_user_task() && !uts_ptr.valid())
        return static_cast<uint64_t>(-1);

    Utsname uts{};
    strlcpy(uts.sysname, "NexIOS", sizeof(uts.sysname));
    strlcpy(uts.nodename, "nexios", sizeof(uts.nodename));
    strlcpy(uts.release, Version::string(), sizeof(uts.release));
    strlcpy(uts.version, Version::build_date(), sizeof(uts.version));
    strlcpy(uts.machine, "x86_64", sizeof(uts.machine));
    strlcpy(uts.domainname, "(none)", sizeof(uts.domainname));
    // MP-4 (SMAP): local + safe_copy_to_user (stac-wrapped).
    if (syscall_is_user_task() &&
        !safe_copy_to_user(uts_ptr.unsafe_ptr(), &uts, 1))
        return static_cast<uint64_t>(-1);
    else
        *uts_ptr.unsafe_ptr() = uts;
    return 0;
}

uint64_t Syscall::sys_pause(uint64_t, uint64_t, uint64_t, uint64_t,
                            uint64_t *) {
    arch::hlt();
    return 0;
}

uint64_t Syscall::sys_reboot(uint64_t, uint64_t, uint64_t, uint64_t,
                             uint64_t *) {
    // PS/2 controller CPU reset: write 0xFE to port 0x64
    arch::outb(0x64, 0xFE);
    arch::io_wait();
    // If reset fails, halt
    arch::cli();
    for (;;) {
        arch::hlt();
    }
}

uint64_t Syscall::sys_halt(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t *) {
    arch::cli();
    for (;;) {
        arch::hlt();
    }
}

uint64_t Syscall::sys_brk(uint64_t arg0, uint64_t, uint64_t, uint64_t,
                          uint64_t *) {
    static sync::SpinLock brk_lock{};
    SpinLockGuard<sync::SpinLock> guard(brk_lock);
    auto *t = syscall_task();
    if (!t)
        return static_cast<uint64_t>(-1);

    // Query mode
    if (arg0 == 0)
        return t->program_break;

    // Cannot shrink below start
    if (arg0 < t->program_break_start)
        return t->program_break;

    // Cannot expand beyond stack area
    if (arg0 > mem::STACK_VADDR)
        return static_cast<uint64_t>(-1);
    // v0.4.0 MP-2: the guard page at STACK_VADDR must never become the heap
    // end — brk(STACK_VADDR) would make the next heap page the unmapped
    // guard, so reject it explicitly.
    if (arg0 == mem::STACK_VADDR)
        return static_cast<uint64_t>(-1);

    uint64_t old_break = t->program_break;

    // Expand: map new pages
    if (arg0 > old_break) {
        uint64_t start_page =
            (old_break + arch::PAGE_SIZE - 1) & ~(arch::PAGE_SIZE - 1);
        uint64_t end_page =
            (arg0 + arch::PAGE_SIZE - 1) & ~(arch::PAGE_SIZE - 1);
        for (uint64_t vaddr = start_page; vaddr < end_page;
             vaddr += arch::PAGE_SIZE) {
            // Check if already mapped
            if (VMM::virt_to_phys_in_pml4(vaddr, t->page_table_) != 0)
                continue;
            uint64_t phys = PMM::alloc_user_page();
            if (!phys)
                return static_cast<uint64_t>(-1);
            VMM::map_page_in_pml4(vaddr, phys, true, t->page_table_);
            // NOLINTNEXTLINE(performance-no-int-to-ptr)
            __builtin_memset(reinterpret_cast<void *>(arch::HHDM_OFFSET + phys),
                             0, arch::PAGE_SIZE);
        }
    }

    // Contract: could unmap pages but it's optional — leave them mapped
    t->program_break = arg0;

    // v0.4.0 MP-3: re-arm the heap-after canary at the new break top.
    {
        const uint64_t heap = TaskControlBlock::SEG_HEAP;
        if (t->canary_installed & (1u << heap)) {
            uint64_t new_top =
                ((arg0 + arch::PAGE_SIZE - 1) & ~(arch::PAGE_SIZE - 1)) - 8;
            if (new_top >= mem::HEAP_VADDR) {
                t->canary_after[heap] = new_top;
                canary_write_at(new_top,
                                TaskControlBlock::CANARY_MAGIC ^ (heap + 1),
                                t->page_table_);
            }
        }
    }
    return t->program_break;
}

struct Rlimit {
    uint64_t rlim_cur;
    uint64_t rlim_max;
};

enum RlimitResource : uint8_t {
    RLIMIT_DATA = 0,
    RLIMIT_STACK = 1,
    RLIMIT_NOFILE = 2,
};

uint64_t Syscall::sys_getrlimit(uint64_t arg0, uint64_t arg1, uint64_t,
                                uint64_t, uint64_t *) {
    auto *t = syscall_task();
    if (!t)
        return static_cast<uint64_t>(-1);

    Rlimit rl = {};
    switch (arg0) {
    case RLIMIT_DATA:
        rl.rlim_cur = mem::STACK_VADDR - t->program_break_start;
        rl.rlim_max = mem::STACK_VADDR - t->program_break_start;
        break;
    case RLIMIT_STACK:
        rl.rlim_cur = mem::STACK_SIZE;
        rl.rlim_max = mem::STACK_SIZE;
        break;
    case RLIMIT_NOFILE:
        rl.rlim_cur = vfs::MAX_FDS;
        rl.rlim_max = vfs::MAX_FDS;
        break;
    default:
        return static_cast<uint64_t>(-1);
    }

    // NOLINTNEXTLINE(performance-no-int-to-ptr)
    auto rl_ptr = checked(reinterpret_cast<Rlimit *>(arg1));
    if (syscall_is_user_task() && !rl_ptr.valid())
        return static_cast<uint64_t>(-1);
    // MP-4 (SMAP): local + safe_copy_to_user (stac-wrapped).
    if (syscall_is_user_task() &&
        !safe_copy_to_user(rl_ptr.unsafe_ptr(), &rl, 1))
        return static_cast<uint64_t>(-1);
    else
        *rl_ptr.unsafe_ptr() = rl;
    return 0;
}

uint64_t Syscall::sys_setrlimit(uint64_t arg0, uint64_t arg1, uint64_t,
                                uint64_t, uint64_t *) {
    auto *t = syscall_task();
    if (!t)
        return static_cast<uint64_t>(-1);

    // NOLINTNEXTLINE(performance-no-int-to-ptr)
    auto rl_ptr = checked(reinterpret_cast<const Rlimit *>(arg1));
    if (syscall_is_user_task() && !rl_ptr.valid())
        return static_cast<uint64_t>(-1);
    // MP-4 (SMAP): safe_copy_from_user (stac-wrapped).
    Rlimit rl{};
    if (syscall_is_user_task()) {
        if (!safe_copy_from_user(&rl, rl_ptr.unsafe_ptr(), 1))
            return static_cast<uint64_t>(-1);
    } else {
        rl = *rl_ptr.unsafe_ptr();
    }
    (void)rl;
    (void)arg0;
    return 0;
}

uint64_t Syscall::sys_getrandom(uint64_t arg0, uint64_t arg1, uint64_t arg2,
                                uint64_t, uint64_t *) {
    // arg0 = buffer, arg1 = length, arg2 = flags (reserved, must be 0)
    if (arg2 != 0)
        return static_cast<uint64_t>(-1);
    if (arg1 == 0)
        return 0;
    // v0.4.0 MP-1 hardening: reject a null buffer for ANY caller (the boot
    // identity map previously masked writes to address 0x0).
    if (arg0 == 0)
        return static_cast<uint64_t>(-1);

    if (syscall_is_user_task()) {
        // NOLINTNEXTLINE(performance-no-int-to-ptr)
        auto buf = checked(reinterpret_cast<uint8_t *>(arg0), arg1);
        if (!buf.valid())
            return static_cast<uint64_t>(-1);
        // MP-4 (SMAP): random_fill is generic (kernel buffers elsewhere); the
        // user write is stac-wrapped with fault recovery.
        g_user_access_recover_ip =
            reinterpret_cast<uint64_t>(&&recover_rand);
        arch::stac();
        random_fill(buf.unsafe_ptr(), static_cast<size_t>(arg1));
        arch::clac();
        g_user_access_recover_ip = 0;
    } else {
        // NOLINTNEXTLINE(performance-no-int-to-ptr)
        random_fill(reinterpret_cast<uint8_t *>(arg0),
                    static_cast<size_t>(arg1));
    }
    return arg1;

recover_rand:
    arch::clac();
    g_user_access_recover_ip = 0;
    return static_cast<uint64_t>(-1);
}

uint64_t Syscall::sys_klog(uint64_t arg0, uint64_t arg1, uint64_t arg2,
                           uint64_t, uint64_t *) {
    // arg0 = buffer, arg1 = size, arg2 = flags (0=read, 1=clear)
    if (arg2 == 1) {
        kernel::log::g_dmesg.clear();
        return 0;
    }
    if (arg0 == 0 || arg1 == 0)
        return 0;

    // NOLINTNEXTLINE(performance-no-int-to-ptr)
    char *user_buf = reinterpret_cast<char *>(arg0);
    size_t user_size = static_cast<size_t>(arg1);
    size_t written = 0;

    if (syscall_is_user_task()) {
        auto buf = checked(user_buf, user_size);
        if (!buf.valid())
            return static_cast<uint64_t>(-1);
        user_buf = buf.unsafe_ptr();
        // MP-4 (SMAP): arm fault recovery for the user writes inside the
        // for_each callback below.
        g_user_access_recover_ip = reinterpret_cast<uint64_t>(&&recover_klog);
    }

    kernel::log::g_dmesg.for_each([&](const kernel::log::LogEntry &e) {
        if (written >= user_size)
            return;
        char entry_buf[256];
        char *p = entry_buf;
        char *end = entry_buf + sizeof(entry_buf) - 1;

        const char *prefix = "[DMESG] TS=";
        while (*prefix && p < end)
            *p++ = *prefix++;

        uint64_t ts = e.timestamp;
        char tsbuf[24];
        int tlen = 0;
        if (ts == 0)
            tsbuf[tlen++] = '0';
        else {
            while (ts > 0 && tlen < 23) {
                tsbuf[tlen++] = static_cast<char>('0' + (ts % 10));
                ts /= 10;
            }
        }
        for (int i = 0; i < tlen / 2; ++i) {
            char c = tsbuf[i];
            tsbuf[i] = tsbuf[tlen - 1 - i];
            tsbuf[tlen - 1 - i] = c;
        }
        for (int i = 0; i < tlen && p < end; ++i)
            *p++ = tsbuf[i];

        const char *task_str = " TASK=";
        while (*task_str && p < end)
            *p++ = *task_str++;
        uint64_t tid = e.task_id;
        char tidbuf[24];
        int tidlen = 0;
        if (tid == 0)
            tidbuf[tidlen++] = '0';
        else {
            while (tid > 0 && tidlen < 23) {
                tidbuf[tidlen++] = static_cast<char>('0' + (tid % 10));
                tid /= 10;
            }
        }
        for (int i = 0; i < tidlen / 2; ++i) {
            char c = tidbuf[i];
            tidbuf[i] = tidbuf[tidlen - 1 - i];
            tidbuf[tidlen - 1 - i] = c;
        }
        for (int i = 0; i < tidlen && p < end; ++i)
            *p++ = tidbuf[i];

        const char *err_str = " ERR=";
        while (*err_str && p < end)
            *p++ = *err_str++;
        const char *sub = kernel::log::subsystem_name(e.subsystem);
        while (*sub && p < end)
            *p++ = *sub++;
        *p++ = ':';
        const char *err_name =
            kernel::log::error_string(e.subsystem, e.error_code);
        while (*err_name && p < end)
            *p++ = *err_name++;

        const char *ctx_str = " CTX=";
        while (*ctx_str && p < end)
            *p++ = *ctx_str++;
        uintptr_t ctx = e.context;
        *p++ = '0';
        *p++ = 'x';
        for (int i = (sizeof(uintptr_t) * 2) - 1; i >= 0 && p < end; --i) {
            uint8_t nib = (ctx >> (i * 4)) & 0xF;
            *p++ = static_cast<char>(nib < 10 ? '0' + nib : 'a' + (nib - 10));
        }

        const char *msg_str = ": ";
        while (*msg_str && p < end)
            *p++ = *msg_str++;
        const char *msg = e.message ? e.message : "(null)";
        while (*msg && p < end)
            *p++ = *msg++;

        *p++ = '\n';
        *p = '\0';

        size_t entry_len = p - entry_buf;
        size_t copy_len = (written + entry_len <= user_size)
                              ? entry_len
                              : (user_size - written);
        if (copy_len > 0) {
            // MP-4 (SMAP): user write inside a generic for_each callback.
            // stac/recover armed for the whole walk; a fault redirects to
            // recover_klog with AC cleared and returns the partial count.
            arch::stac();
            memcpy(user_buf + written, entry_buf, copy_len);
            arch::clac();
            written += copy_len;
        }
    });

    // Clear the armed recovery on the success path.
    g_user_access_recover_ip = 0;
    return written;

recover_klog:
    arch::clac();
    g_user_access_recover_ip = 0;
    return written;
}

} // namespace kernel
