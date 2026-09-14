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

/// @file taskdefs.cpp
/// @brief Task definition table and reboot-from-table implementation.

#include <kernel/task/taskdefs.hpp>
#include <kernel/task/scheduler.hpp>
#include <kernel/task/sporadic_server.hpp>
#include <kernel/elf/elf.hpp>
#include <kernel/vfs/vfsd.hpp>
#include <kernel/driver/iocd.hpp>
#include <kernel/task/dmesg_task.hpp>
#include <initrd/initrd.hpp>
#include <services/shell.hpp>
#include <kernel/daemon/daemon_mgr.hpp>
#include <kernel/arch/io.hpp>
#include <kernel/arch/hal/irq_guard.hpp>
#if defined(CONFIG_ARCH_X86_64)
#include <kernel/arch/x86_64/hal/smp.hpp>
#endif
#include <kernel/nexios_config.h>
#include <logger.hpp>

extern "C" void debug_write(const char *s);
extern "C" void debug_write_hex(uint64_t value);

// Defined in kernel.cpp — the init-task entry point.
void init_task_main();

namespace kernel {
namespace task {
namespace {

// ── Task definition table ────────────────────────────────────────────────

constexpr TaskDef g_task_defs[] = {
    // init: PID 1 coordinator + reaper.  Background priority 0; raised to 10
    // (harness) only while the test runner is active, dropped back to 0 in
    // the reap loop.  Period 100 for RMS.
    {"init", TaskType::KERNEL, true, init_task_main, nullptr, 0, 100, 1, 0, 0,
     0, nullptr, nullptr, nullptr, 1, 0, false},
    // vfsd: sporadic server, fast IPC response, 2% worst-case CPU
    {"vfsd", TaskType::SPORADIC_SERVER, true, nullptr, "vfsd.c.elf", 20, 10, 0,
     2, 10, 0, "vfsd", vfsd::set_vfsd_pid, vfsd::get_vfsd_pid, 1, 0, false},
    // iocd: sporadic server for I/O, same prio as vfsd
    {"iocd", TaskType::SPORADIC_SERVER, true, nullptr, "iocd.c.elf", 20, 10, 0,
     3, 10, 0, "iocd", iocd::set_iocd_pid, iocd::get_iocd_pid, 1, 0, false},
    // user-app: generic userspace ELF placeholder — aperiodic (period 0)
    {"user-app", TaskType::USER_ELF, true, nullptr, "user-app.c.elf", 2, 0, 0,
     0, 0, 0, nullptr, nullptr, nullptr, 1, 0, false},
    // shell: interactive kernel debug shell.  Disabled in the table — the
    // real shell is created aperiodic by init_task_main at prio 2; values
    // kept here for documentation/validation parity.
    {"shell", TaskType::KERNEL, false, service::Shell::shell_task_main, nullptr,
     2, 0, 0, 0, 0, 0, nullptr, nullptr, nullptr, 1, 0, false},
    // dmesg: background logger with very long period
    {"dmesg", TaskType::KERNEL, false, dmesg_task_main, nullptr, 1, 500, 0, 0,
     0, 0, nullptr, nullptr, nullptr, 1, 0, false},
};

// ── Compile-time validation ──────────────────────────────────────────────

constexpr bool name_empty(const char *s) {
    return !s || !s[0];
}

constexpr size_t name_len(const char *s) {
    size_t n = 0;
    while (s && s[n])
        ++n;
    return n;
}

constexpr bool name_eq(const char *a, const char *b) {
    if (!a || !b)
        return a == b;
    for (; *a && *b; ++a, ++b)
        if (*a != *b)
            return false;
    return *a == *b;
}

template <size_t N> constexpr bool validate_all(const TaskDef (&t)[N]) {
    size_t enabled = 0;
    size_t ss = 0;

    for (size_t i = 0; i < N; ++i) {
        const auto &d = t[i];

        // name
        if (name_empty(d.name))
            return false;
        if (name_len(d.name) >= CONFIG_TASK_NAME_LEN)
            return false;

        // priority bounds
        if (d.priority > CONFIG_PRIORITY_CEILING)
            return false;

        // period must be positive for enabled periodic tasks; aperiodic
        // USER_ELF tasks (period 0) and disabled (informational) entries are
        // exempt.
        if (d.period_ticks == 0 && d.enabled && d.type != TaskType::USER_ELF)
            return false;

        // type-specific requirements
        switch (d.type) {
        case TaskType::KERNEL:
            if (!d.kernel_entry)
                return false;
            break;

        case TaskType::USER_ELF:
            if (name_empty(d.elf_path))
                return false;
            break;

        case TaskType::SPORADIC_SERVER:
            if (name_empty(d.elf_path))
                return false;
            if (d.ss_budget > d.ss_period)
                return false;
            if (name_empty(d.daemon_name))
                return false;
            if (!d.set_pid_fn || !d.get_pid_fn)
                return false;
            break;
        }

        // counts
        if (d.enabled) {
            ++enabled;
            if (d.type == TaskType::SPORADIC_SERVER)
                ++ss;
        }

        // no duplicate names among enabled entries
        for (size_t j = i + 1; j < N; ++j) {
            if (d.enabled && t[j].enabled && d.name && t[j].name &&
                name_eq(d.name, t[j].name)) {
                return false;
            }
        }
    }

    // idle takes one slot
    if (enabled + 1 > CONFIG_MAX_TASKS)
        return false;
    if (ss > CONFIG_MAX_DAEMONS)
        return false;

    return true;
}

static_assert(
    validate_all(g_task_defs),
    "TaskDef: priorities exceed CONFIG_PRIORITY_CEILING, periods are 0, "
    "SS params are invalid, names exceed CONFIG_TASK_NAME_LEN, "
    "or total enabled tasks / daemons exceed CONFIG_MAX_TASKS / CONFIG_MAX_DAEMONS "
    "(see nexios_config.h)");

} // anonymous namespace

// ── reboot_from_table ────────────────────────────────────────────────────

void reboot_from_table() {
    debug_write("[REBOOT] Rebuilding system from task-definition table\n");
    arch::IrqGuard guard{};

    auto *idle = Scheduler::get_idle_task();

    // Suppress [DAEMON] died and task terminated noise during teardown —
    // old daemons and tasks are intentionally killed here, not failing.
    daemon::set_suppress_death_msg(true);
    Scheduler::set_suppress_terminated_log(true);
    debug_write("[INFO]  daemon: restarting vfsd, iocd\n");

    // 1. Collect all non-idle, non-current tasks (can't mutate all_tasks_
    //    during iteration, and must not release our own TCB — we're running on
    //    its stack).
    static constexpr uint64_t MAX_KILL = 64;
    TaskControlBlock *to_kill[MAX_KILL];
    uint64_t num_to_kill = 0;
    auto *self = Scheduler::current_task();
    for (uint64_t i = 0; i < Scheduler::task_count() && num_to_kill < MAX_KILL;
         ++i) {
        auto *t = Scheduler::task_at(i);
        // Issue #25 C1: spare ANY idle (the AP idle cannot exist yet —
        // it is created post-gate — but this stays correct if that
        // ordering ever changes).
        if (t && !Scheduler::is_idle_task(t) && t != self)
            to_kill[num_to_kill++] = t;
    }
    // Release each into the zombie list.  IRQs are disabled (IrqGuard at
    // function entry), so no ISR concurrency — the scheduler lock is not
    // needed here.
    for (uint64_t i = 0; i < num_to_kill; ++i) {
        Scheduler::dequeue_ready(*to_kill[i]);
        to_kill[i]->state = TaskState::TERMINATED;
        to_kill[i]->exit_code = 0;
        Scheduler::release_zombie(*to_kill[i]);
    }
    Scheduler::drain_zombie_list();
    Scheduler::reset_ready_queue();

    // Re-enable death/terminated messages — cleanup is done, any from here
    // on are real failures.
    daemon::set_suppress_death_msg(false);
    Scheduler::set_suppress_terminated_log(false);

    // 2. Reset daemon manager so fresh SPORADIC_SERVER entries can register
    daemon::reset_clear_daemons();

    // Reset the task-ID counter so init gets PID 1 (daemons
    // send MSG_DAEMON_READY to PID 1 at boot).
    Scheduler::reset_next_task_id(1);

    // 3. Pre-scan: verify all enabled ELF files exist in initrd
    for (auto &def : g_task_defs) {
        if (!def.enabled)
            continue;
        if (def.type == TaskType::KERNEL)
            continue;

        auto f = initrd::find(def.elf_path);
        if (!f.data && def.elf_path[0] == '.') {
            f = initrd::find(def.elf_path + 2); // skip "./"
        }
        if (!f.data && def.elf_path[0] != '.') {
            // try with "./"
            char prefixed[160];
            prefixed[0] = '.';
            prefixed[1] = '/';
            size_t pi = 2;
            for (size_t si = 0; def.elf_path[si] && pi < 159; ++si)
                prefixed[pi++] = def.elf_path[si];
            prefixed[pi] = '\0';
            f = initrd::find(prefixed);
        }
        if (!f.data) {
            Logger::fatal("reboot: missing ELF '%s' for task '%s'",
                          def.elf_path, def.name);
            panic("reboot_from_table: missing ELF file");
        }
    }

    // 4. Spawn all enabled tasks from the table
    for (auto &def : g_task_defs) {
        if (!def.enabled)
            continue;

        TaskControlBlock *task = nullptr;

        switch (def.type) {
        case TaskType::KERNEL:
            task = TaskControlBlock::create(def.kernel_entry, def.priority,
                                            def.period_ticks);
            break;

        case TaskType::USER_ELF:
        case TaskType::SPORADIC_SERVER: {
            auto f = initrd::find(def.elf_path);
            if (!f.data && def.elf_path[0] == '.') {
                f = initrd::find(def.elf_path + 2);
            }
            if (!f.data && def.elf_path[0] != '.') {
                char prefixed[160];
                prefixed[0] = '.';
                prefixed[1] = '/';
                size_t pi = 2;
                for (size_t si = 0; def.elf_path[si] && pi < 159; ++si)
                    prefixed[pi++] = def.elf_path[si];
                prefixed[pi] = '\0';
                f = initrd::find(prefixed);
            }
            if (!f.data)
                break;

            auto *hdr =
                reinterpret_cast<const kernel::elf::ELF64Header *>(f.data);
            if (!kernel::elf::validate_header(hdr)) {
                Logger::warn("reboot: invalid ELF '%s' for task '%s'",
                             def.elf_path, def.name);
                break;
            }
            task = kernel::elf::load(hdr, f.data, f.size);
            if (task) {
                task->priority = def.priority;
                task->base_priority = def.priority;
                task->period_ticks = def.period_ticks;
                if (def.type == TaskType::SPORADIC_SERVER) {
                    task->init_sporadic_server(def.ss_budget, def.ss_period,
                                               def.ss_bg_prio,
                                               def.ss_budget_granularity);
                }
            }
            break;
        }
        }

        if (!task) {
            Logger::warn("reboot: failed to create task '%s', skipping",
                         def.name);
            continue;
        }

        {
            size_t i = 0;
            while (def.name[i] && i < CONFIG_TASK_NAME_LEN - 1) {
                task->name[i] = def.name[i];
                ++i;
            }
            task->name[i] = '\0';
        }
        task->wcet_ticks = def.wcet_ticks;

        Scheduler::add_task(*task);

        if (def.is_shell) {
            Scheduler::set_shell_task(task);
        }

        if (def.type == TaskType::SPORADIC_SERVER) {
            def.set_pid_fn(task->id);
            daemon::register_daemon(def.daemon_name, def.elf_path,
                                    def.set_pid_fn, def.get_pid_fn);
        }
    }

    // Re-set the harness-task pointer to the new init (PID 1) created by the
    // table loop above.  reboot_from_table() kills the original init task and
    // spawns a fresh TCB at a different address; the old harness_task_ptr_
    // (set during kernel_main) is now a dangling pointer.  Without this fix,
    // harness_nonpreempt in rate_monotonic_schedule() is always false, the
    // timer ISR calls next_task() during test execution, and dequeued test
    // tasks are stranded in READY+not-in-queue limbo — causing intermittent
    // failures in o1_scheduler and other test classes.
    {
        auto *init_task = Scheduler::find_task(1);
        if (init_task) {
            Scheduler::set_harness_task(init_task);
        }
    }

    debug_write("[REBOOT] after add_tasks task_count()=0x");
    debug_write_hex(Scheduler::task_count());
    debug_write("\n");

#if defined(CONFIG_ARCH_X86_64)
    // Issue #25 C1: publish the AP scheduler start-gate AFTER spawning
    // (tables quiescent from here on) and before the idle loop.  APs spin
    // parked until this lands; post-gate they create their idles and tick.
    kernel::smp::publish_scheduler_ready();
#endif

    // 5. Switch to idle-task stack and enter the idle loop
    if (idle) {
#if defined(CONFIG_ARCH_X86_64)
        asm volatile("mov %[sp], %%rsp\n" ::[sp] "r"(idle->kernel_stack_top)
                     : "memory");
#elif defined(CONFIG_ARCH_AARCH64)
        asm volatile("mov sp, %[sp]\n" ::[sp] "r"(idle->kernel_stack_top)
                     : "memory");
#elif defined(CONFIG_ARCH_RISCV64)
        asm volatile("mv sp, %[sp]\n" ::[sp] "r"(idle->kernel_stack_top)
                     : "memory");
#endif
    }

    debug_write("[DIAG] reboot idle loop entered\n");
    {
        static bool dumped = false;
        if (!dumped) {
            dumped = true;
            debug_write("[DIAG-TABLE] task_count=");
            debug_write_hex(Scheduler::task_count());
            auto *ct = Scheduler::current_task();
            debug_write(" cur_id=");
            debug_write_hex(ct ? ct->id : 0xFFFFFFFFu);
            debug_write("\n");
            for (uint64_t i = 0; i < Scheduler::task_count(); ++i) {
                auto *t = Scheduler::task_at(i);
                if (!t)
                    continue;
                debug_write("[DIAG-TABLE] idx=");
                debug_write_hex(i);
                debug_write(" id=");
                debug_write_hex(t->id);
                debug_write(" magic=0x");
                debug_write_hex(t->magic);
                debug_write(" state=");
                debug_write_hex(static_cast<uint8_t>(t->state));
                debug_write(" in_rq=");
                debug_write_hex(t->in_ready_queue_ ? 1u : 0u);
                debug_write(" prio=");
                debug_write_hex(t->priority);
                debug_write("\n");
            }
        }
    }
    arch::sti();
    for (;;) {
        static uint64_t _idle_count = 0;
        if (++_idle_count % 100000 == 0) {
            debug_write("[DIAG] idle loop count=");
            debug_write_hex(_idle_count);
            debug_write("\n");
        }
        arch::hlt();
    }
    __builtin_unreachable();
}

} // namespace task
} // namespace kernel
