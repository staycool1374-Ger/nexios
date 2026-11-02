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
#include <kernel/log/dmesg.hpp>
#include <kernel/task/scheduler.hpp>
#include <kernel/task/sporadic_server.hpp>
#include <kernel/elf/elf.hpp>
#include <kernel/vfs/vfsd.hpp>
#include <kernel/driver/iocd.hpp>
#include <kernel/ramdisk/ramdiskd.hpp>
#include <kernel/watchdog/watchdogd.hpp>
#include <kernel/time/timer_srv.hpp>
#include <kernel/vfs/vfs_srv.hpp>
#include <kernel/driver/block_srv.hpp>
#include <kernel/term/term_srv.hpp>
#include <kernel/log/log_srv.hpp>
#include <kernel/driver/driver_pds.hpp>
#include <kernel/task/dmesg_task.hpp>
#include <initrd/initrd.hpp>
#include <services/shell.hpp>
#include <kernel/daemon/daemon_mgr.hpp>
#include <kernel/arch/io.hpp>
#include <kernel/arch/hal/irq_guard.hpp>
#if defined(CONFIG_ARCH_X86_64)
#include <kernel/arch/x86_64/hal/smp.hpp>
#endif
#if defined(CONFIG_ARCH_AARCH64)
#include <kernel/arch/aarch64/hal/smp.hpp>
#endif
#if defined(CONFIG_ARCH_RISCV64)
#include <kernel/arch/riscv64/hal/smp.hpp>
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

// Issue #232: debugd PID cell for the disabled documentation row below.
// Real daemons use namespace-owned cells (vfsd::/iocd::); debugd never
// auto-starts, so a TU-local cell satisfies validate_all without
// implying a live daemon.
uint64_t g_debugd_pid_cell = 0;

// Driver-PD restart trampolines (issue #48): the DaemonEntry hook carries
// only a pid, so each role re-resolves its task and runs sdf_rebind
// (re-derive, never inherit) + the role PID cell bump. Each role triple
// is guarded by its own knob (a single-role ON build must not warn on
// the other roles' TU-local helpers under -Werror=unused-function).
namespace {
#if CONFIG_DRIVER_PD_SERIAL || CONFIG_DRIVER_PD_KBD ||                        \
    CONFIG_DRIVER_PD_RTC || CONFIG_DRIVER_PD_FB ||                            \
    CONFIG_DRIVER_PD_BLOCK || CONFIG_DRIVER_PD_NET
void pd_rebind_role(kernel::sdf::PdRole role, uint64_t pid) {
    auto *t = kernel::Scheduler::find_task(pid);
    if (t == nullptr || t->magic != TaskControlBlock::TCB_MAGIC) {
        return;
    }
    if (!kernel::sdf::sdf_rebind(*t, role)) {
        return;
    }
    kernel::driver_pds::pd_on_restart(role, pid);
}
#endif
} // namespace

#if CONFIG_DRIVER_PD_SERIAL
void pd_restart_serial(uint64_t pid) {
    pd_rebind_role(kernel::sdf::PdRole::SERIAL, pid);
}
uint64_t pd_pid_serial() {
    return kernel::driver_pds::get_pd_pid(kernel::sdf::PdRole::SERIAL);
}
void pd_set_serial(uint64_t pid) {
    kernel::driver_pds::set_pd_pid(kernel::sdf::PdRole::SERIAL, pid);
}
#endif
#if CONFIG_DRIVER_PD_KBD
void pd_restart_kbd(uint64_t pid) {
    pd_rebind_role(kernel::sdf::PdRole::KBD, pid);
}
uint64_t pd_pid_kbd() {
    return kernel::driver_pds::get_pd_pid(kernel::sdf::PdRole::KBD);
}
void pd_set_kbd(uint64_t pid) {
    kernel::driver_pds::set_pd_pid(kernel::sdf::PdRole::KBD, pid);
}
#endif
#if CONFIG_DRIVER_PD_RTC
void pd_restart_rtc(uint64_t pid) {
    pd_rebind_role(kernel::sdf::PdRole::RTC, pid);
}
uint64_t pd_pid_rtc() {
    return kernel::driver_pds::get_pd_pid(kernel::sdf::PdRole::RTC);
}
void pd_set_rtc(uint64_t pid) {
    kernel::driver_pds::set_pd_pid(kernel::sdf::PdRole::RTC, pid);
}
#endif
#if CONFIG_DRIVER_PD_FB
void pd_restart_fb(uint64_t pid) {
    pd_rebind_role(kernel::sdf::PdRole::FB, pid);
}
uint64_t pd_pid_fb() {
    return kernel::driver_pds::get_pd_pid(kernel::sdf::PdRole::FB);
}
void pd_set_fb(uint64_t pid) {
    kernel::driver_pds::set_pd_pid(kernel::sdf::PdRole::FB, pid);
}
#endif
#if CONFIG_DRIVER_PD_BLOCK
void pd_restart_block(uint64_t pid) {
    pd_rebind_role(kernel::sdf::PdRole::BLOCK, pid);
}
uint64_t pd_pid_block() {
    return kernel::driver_pds::get_pd_pid(kernel::sdf::PdRole::BLOCK);
}
void pd_set_block(uint64_t pid) {
    kernel::driver_pds::set_pd_pid(kernel::sdf::PdRole::BLOCK, pid);
}
#endif
#if CONFIG_DRIVER_PD_NET
void pd_restart_net(uint64_t pid) {
    pd_rebind_role(kernel::sdf::PdRole::NET, pid);
}
uint64_t pd_pid_net() {
    return kernel::driver_pds::get_pd_pid(kernel::sdf::PdRole::NET);
}
void pd_set_net(uint64_t pid) {
    kernel::driver_pds::set_pd_pid(kernel::sdf::PdRole::NET, pid);
}
#endif
#if CONFIG_VFS_SRV
void pd_restart_vfs_srv(uint64_t pid) {
    auto *t = kernel::Scheduler::find_task(pid);
    if (t == nullptr || t->magic != TaskControlBlock::TCB_MAGIC) {
        return;
    }
    if (!kernel::sdf::sdf_rebind(*t, kernel::sdf::PdRole::VFS_SRV)) {
        return;
    }
    kernel::vfs_srv::vfs_srv_on_restart(pid);
}
uint64_t pd_pid_vfs_srv() {
    return kernel::vfs_srv::get_vfs_srv_pid();
}
void pd_set_vfs_srv(uint64_t pid) {
    kernel::vfs_srv::set_vfs_srv_pid(pid);
}
#endif
#if CONFIG_BLOCK_SRV
void pd_restart_block_srv(uint64_t pid) {
    auto *t = kernel::Scheduler::find_task(pid);
    if (t == nullptr || t->magic != TaskControlBlock::TCB_MAGIC) {
        return;
    }
    if (!kernel::sdf::sdf_rebind(*t, kernel::sdf::PdRole::BLOCK_SRV)) {
        return;
    }
    kernel::block_srv::block_srv_on_restart(pid);
}
uint64_t pd_pid_block_srv() {
    return kernel::block_srv::get_block_srv_pid();
}
void pd_set_block_srv(uint64_t pid) {
    kernel::block_srv::set_block_srv_pid(pid);
}
#endif
#if CONFIG_TERM_SRV
void pd_restart_term_srv(uint64_t pid) {
    auto *t = kernel::Scheduler::find_task(pid);
    if (t == nullptr || t->magic != TaskControlBlock::TCB_MAGIC) {
        return;
    }
    if (!kernel::sdf::sdf_rebind(*t, kernel::sdf::PdRole::TERM_SRV)) {
        return;
    }
    kernel::term_srv::term_srv_on_restart(pid);
}
uint64_t pd_pid_term_srv() {
    return kernel::term_srv::get_term_srv_pid();
}
void pd_set_term_srv(uint64_t pid) {
    kernel::term_srv::set_term_srv_pid(pid);
}
#endif
#if CONFIG_LOG_SRV
void pd_restart_log_srv(uint64_t pid) {
    auto *t = kernel::Scheduler::find_task(pid);
    if (t == nullptr || t->magic != TaskControlBlock::TCB_MAGIC) {
        return;
    }
    if (!kernel::sdf::sdf_rebind(*t, kernel::sdf::PdRole::LOG_SRV)) {
        return;
    }
    kernel::log_srv::log_srv_on_restart(pid);
}
uint64_t pd_pid_log_srv() {
    return kernel::log_srv::get_log_srv_pid();
}
void pd_set_log_srv(uint64_t pid) {
    kernel::log_srv::set_log_srv_pid(pid);
}
#endif

// ── Task definition table ────────────────────────────────────────────────

constexpr TaskDef g_task_defs[] = {
    // init/reaper: PID 1 coordinator + zombie reaper.  Background priority
    // 0; raised to 10 (harness) only while the test runner is active,
    // dropped back to 0 in the reap loop.  Period 100 for RMS.  Named
    // for its steady-state job (the name is display-only): after boot
    // it spins the reap loop whenever nothing higher-priority runs,
    // which is why it accrues CPU time while showing READY.
    {"init/reaper", TaskType::KERNEL, true, init_task_main, nullptr, 0, 100,
     1, 0, 0, 0, nullptr, nullptr, nullptr, 1, 0, false},
    // vfsd: sporadic server, fast IPC response, 2% worst-case CPU
    {"vfsd", TaskType::SPORADIC_SERVER, true, nullptr, "bin/vfsd.c.elf", 20, 10, 0,
     2, 10, 0, "vfsd", vfsd::set_vfsd_pid, vfsd::get_vfsd_pid, 1, 0, false},
    // iocd: sporadic server for I/O, same prio as vfsd
    {"iocd", TaskType::SPORADIC_SERVER, true, nullptr, "bin/iocd.c.elf", 20, 10, 0,
     3, 10, 0, "iocd", iocd::set_iocd_pid, iocd::get_iocd_pid, 1, 0, false},
    // watchdogd: supervises vfsd/iocd watchdogs (issue #277). Sporadic
    // server, cheapest share SS(1,10,0): vfsd 2/10 + iocd 3/10 + 1/10 =
    // 0.6 < 0.78 (n=3 Liu-Leyland). Same prio band as its supervisees.
    {"watchdogd", TaskType::SPORADIC_SERVER, true, nullptr,
     "bin/watchdogd.c.elf", 20, 10, 0, 1, 10, 0, "watchdogd",
     watchdogd::set_watchdogd_pid, watchdogd::get_watchdogd_pid, 1, 0,
     false},
    // user-app: generic userspace ELF placeholder — aperiodic (period 0)
    {"user-app", TaskType::USER_ELF, true, nullptr, "bin/user-app.c.elf", 2, 0, 0,
     0, 0, 0, nullptr, nullptr, nullptr, 1, 0, false},
    // ramdiskd: block-storage server for the 16 MiB ramdisk (issue #275).
    // Sporadic server SS(2,10,0) like vfsd; PIDs 1-5 above are unchanged.
    {"ramdiskd", TaskType::SPORADIC_SERVER, true, nullptr, "bin/ramdiskd.c.elf",
     20, 10, 0, 2, 10, 0, "ramdiskd", ramdiskd::set_ramdiskd_pid,
     ramdiskd::get_ramdiskd_pid, 1, 0, false},
#if CONFIG_DRIVER_PD_SERIAL
    // drv-serial: RX-only console PD (issue #48, strangler step a).
    // Sporadic server SS(1,10,0); TX stays kernel (console + panic).
    {"drv-serial", TaskType::SPORADIC_SERVER, true, nullptr,
     "bin/serial-pd.c.elf", 20, 10, 0, 1, 10, 0, "drv-serial",
     pd_set_serial, pd_pid_serial, 1, 0, false,
     ServerMode::SPORADIC, pd_restart_serial, 0},
#endif
#if CONFIG_DRIVER_PD_KBD
    // drv-kbd: PS/2 keyboard PD (x86; arch stub elsewhere).
    {"drv-kbd", TaskType::SPORADIC_SERVER, true, nullptr,
     "bin/kbd-pd.c.elf", 20, 10, 0, 1, 10, 0, "drv-kbd",
     pd_set_kbd, pd_pid_kbd, 1, 0, false,
     ServerMode::SPORADIC, pd_restart_kbd, 1},
#endif
#if CONFIG_DRIVER_PD_RTC
    // drv-rtc: RTC PD (CMOS/PL031/Goldfish per arch).
    {"drv-rtc", TaskType::SPORADIC_SERVER, true, nullptr,
     "bin/rtc-pd.c.elf", 20, 10, 0, 1, 10, 0, "drv-rtc",
     pd_set_rtc, pd_pid_rtc, 1, 0, false,
     ServerMode::SPORADIC, pd_restart_rtc, 2},
#endif
#if CONFIG_DRIVER_PD_FB
    // drv-fb: framebuffer device PD (caps + PD isolation only;
    // emulation stays #51 term-srv; zero IRQ by design).
    {"drv-fb", TaskType::SPORADIC_SERVER, true, nullptr,
     "bin/fb-pd.c.elf", 20, 10, 0, 1, 10, 0, "drv-fb",
     pd_set_fb, pd_pid_fb, 1, 0, false,
     ServerMode::SPORADIC, pd_restart_fb, 3},
#endif
#if CONFIG_DRIVER_PD_BLOCK
    // drv-block: block-storage PD (AHCI/virtio-blk, DMA domains).
    {"drv-block", TaskType::SPORADIC_SERVER, true, nullptr,
     "bin/block-pd.c.elf", 20, 10, 0, 1, 10, 0, "drv-block",
     pd_set_block, pd_pid_block, 1, 0, false,
     ServerMode::SPORADIC, pd_restart_block, 4},
#endif
#if CONFIG_DRIVER_PD_NET
    // drv-net: virtio-net PD (RX rings + TX + DMA domain).
    {"drv-net", TaskType::SPORADIC_SERVER, true, nullptr,
     "bin/net-pd.c.elf", 20, 10, 0, 1, 10, 0, "drv-net",
     pd_set_net, pd_pid_net, 1, 0, false,
     ServerMode::SPORADIC, pd_restart_net, 5},
#endif
#if CONFIG_TIMER_SRV
    // timer-srv: Timer/Event externalization PD (issue #241, strangler a).
    // Sporadic server, cheapest share SS(1,10,0). Guarded row: absent at
    // OFF (shipped default), so the boot table is unchanged when 0.
    {"timer-srv", TaskType::SPORADIC_SERVER, true, nullptr,
     "bin/timer-srv.c.elf", 20, 10, 0, 1, 10, 0, "timer-srv",
     time::timer_srv::set_timer_srv_pid,
     time::timer_srv::get_timer_srv_pid, 1, 0, false},
#endif
#if CONFIG_VFS_SRV
    // vfs-srv: VFS backend server PD (issue #47, strangler step a).
    // Sporadic server SS(1,10,0); authorize stays kernel-side.
    {"vfs-srv", TaskType::SPORADIC_SERVER, true, nullptr,
     "bin/vfs-srv.c.elf", 20, 10, 0, 1, 10, 0, "vfs-srv",
     pd_set_vfs_srv, pd_pid_vfs_srv, 1, 0, false,
     ServerMode::SPORADIC, pd_restart_vfs_srv, 6},
#endif
#if CONFIG_BLOCK_SRV
    // block-srv: block-storage server PD (issue #47, strangler step a).
    // Sporadic server SS(1,10,0); kernel skips AHCI/ATA+virtio-blk probe.
    {"block-srv", TaskType::SPORADIC_SERVER, true, nullptr,
     "bin/block-srv.c.elf", 20, 10, 0, 1, 10, 0, "block-srv",
     pd_set_block_srv, pd_pid_block_srv, 1, 0, false,
     ServerMode::SPORADIC, pd_restart_block_srv, 7},
#endif
#if CONFIG_TERM_SRV
    // term-srv: terminal/emulation policy PD (issue #51, strangler step a).
    // Sporadic server SS(1,10,0); policy only, no device caps (SDF row is
    // the empty set by structural rule). Kernel keeps panic raw_write.
    {"term-srv", TaskType::SPORADIC_SERVER, true, nullptr,
     "bin/term-srv.c.elf", 20, 10, 0, 1, 10, 0, "term-srv",
     pd_set_term_srv, pd_pid_term_srv, 1, 0, false,
     ServerMode::SPORADIC, pd_restart_term_srv, 8},
#endif
#if CONFIG_LOG_SRV
    // log-srv: Klog/Dmesg sink PD (issue #51, strangler step a).
    // Sporadic server SS(1,10,0); sink + drop/meter only, no log-source
    // changes. Panic path never leaves the kernel.
    {"log-srv", TaskType::SPORADIC_SERVER, true, nullptr,
     "bin/log-srv.c.elf", 20, 10, 0, 1, 10, 0, "log-srv",
     pd_set_log_srv, pd_pid_log_srv, 1, 0, false,
     ServerMode::SPORADIC, pd_restart_log_srv, 9},
#endif
    // shell: interactive kernel debug shell.  Disabled in the table — the
    // real shell is created aperiodic by init_task_main at prio 2; values
    // kept here for documentation/validation parity.
    {"shell", TaskType::KERNEL, false, service::Shell::shell_task_main, nullptr,
     2, 0, 0, 0, 0, 0, nullptr, nullptr, nullptr, 1, 0, false},
    // debugd: GDB remote stub task (issue #232, §10 envelope). Disabled
    // in the table — started only on explicit request, never by the boot
    // gate (kernel.cpp DaemonWatch stays vfsd/iocd/watchdogd only);
    // values kept here for documentation/validation parity. Lowest
    // debuggable priority under a bounded sporadic-server budget.
    {"debugd", TaskType::SPORADIC_SERVER, false, nullptr, "bin/debugd.c.elf",
     2, 10, 0, 1, 10, 0, "debugd", set_debugd_pid_cell,
     get_debugd_pid_cell, 1, 0, false},
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

// Issue #24: single-entry server-params rule, shared by the compile-time
// table check below and the runtime verify class (one rule, no duplication).
constexpr bool server_params_valid(const TaskDef &d) {
    if (d.type != TaskType::SPORADIC_SERVER)
        return true;
    if (name_empty(d.elf_path))
        return false;
    if (d.ss_budget > d.ss_period)
        return false;
    // Issue #22: BACKGROUND never runs above bg — a non-idle bg
    // priority would silently promote best-effort work over RT tasks.
    if (d.ss_mode == ServerMode::BACKGROUND && d.ss_bg_prio > 1)
        return false;
    if (name_empty(d.daemon_name))
        return false;
    if (d.set_pid_fn == nullptr || d.get_pid_fn == nullptr)
        return false;
    // Issue #48/#51: an SDF row needs its supervisor hook (re-derive on
    // restart, never inherit) and a valid role index.
    if (d.sdf_role != 0xFF) {
        if (d.on_restart_fn == nullptr)
            return false;
        if (d.sdf_role >
            static_cast<uint64_t>(sdf::PdRole::LOG_SRV))
            return false;
    }
    return true;
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
            if (!server_params_valid(d))
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

// Issue #231: debugd PID cell accessors with external linkage (declared
// in taskdefs.hpp for the shell's on-demand startup). The cell itself
// stays TU-local above.
void set_debugd_pid_cell(uint64_t pid) noexcept {
    g_debugd_pid_cell = pid;
}
uint64_t get_debugd_pid_cell() noexcept {
    return g_debugd_pid_cell;
}

// ── Read-only table accessors (issue #24) ────────────────────────────────
// The verify class inspects table budgets without spawning anything.

size_t taskdefs_count() noexcept {
    return sizeof(g_task_defs) / sizeof(g_task_defs[0]);
}

const TaskDef *taskdefs_at(size_t i) noexcept {
    if (i >= taskdefs_count())
        return nullptr;
    return &g_task_defs[i];
}

bool taskdefs_server_params_valid(const TaskDef &d) noexcept {
    return server_params_valid(d);
}

bool taskdefs_valid() noexcept {
    return validate_all(g_task_defs);
}

// ── reboot_from_table ────────────────────────────────────────────────────

void reboot_from_table() {
#if defined(CONFIG_DEBUG)
    debug_write("[REBOOT] Rebuilding system from task-definition table\n");
#endif
#if CONFIG_BLOCK_SRV
    // Issue #47 S2: capture block topology before interrupts go off —
    // discovery must never run under IrqGuard (no IRQ-off allocation).
    kernel::sdf::capture_block_topology();
#endif
    arch::IrqGuard guard{};

    auto *idle = Scheduler::get_idle_task();

    // Suppress [DAEMON] died and task terminated noise during teardown —
    // old daemons and tasks are intentionally killed here, not failing.
    daemon::set_suppress_death_msg(true);
    Scheduler::set_suppress_terminated_log(true);
#if defined(CONFIG_DEBUG)
    debug_write("[INFO]  daemon: restarting vfsd, iocd\n");
#endif

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
                // Issue #234: broken task definition enters the ring.
                kernel::log::dmesg_push_sev(
                    kernel::log::ErrorSubsystem::INIT,
                    kernel::log::kDmesgBase_INIT + 13,
                    kernel::log::LogSeverity::ERROR, def.name, 0);
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
                                               def.ss_budget_granularity,
                                               def.ss_mode);
                    // Issue #19: daemon servers stay priority-dispatched
                    // (exempt from global EDF) — their IPC latency budget
                    // is governed by fixed priorities + admission (#20).
                    Scheduler::set_edf_exempt(*task, true);
                }
            }
            break;
        }
        }

        if (!task) {
            Logger::warn("reboot: failed to create task '%s', skipping",
                         def.name);
            // Issue #234: failed task creation enters the ring.
            kernel::log::dmesg_push_sev(kernel::log::ErrorSubsystem::INIT,
                                        kernel::log::kDmesgBase_INIT + 14,
                                        kernel::log::LogSeverity::ERROR,
                                        def.name, 0);
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
            if (def.on_restart_fn != nullptr) {
                daemon::register_daemon_ex(
                    def.daemon_name, def.elf_path, def.set_pid_fn,
                    def.get_pid_fn, def.ss_budget, def.on_restart_fn);
            } else {
                daemon::register_daemon(def.daemon_name, def.elf_path,
                                        def.set_pid_fn, def.get_pid_fn);
            }
#if CONFIG_DRIVER_PD_SERIAL || CONFIG_DRIVER_PD_KBD ||                        \
    CONFIG_DRIVER_PD_RTC || CONFIG_DRIVER_PD_FB ||                            \
    CONFIG_DRIVER_PD_BLOCK || CONFIG_DRIVER_PD_NET ||                         \
    CONFIG_VFS_SRV || CONFIG_BLOCK_SRV || CONFIG_TERM_SRV ||                  \
    CONFIG_LOG_SRV
            if (def.sdf_role != 0xFF) {
                // SDF compose (issues #47/#48/#51): mint the role caps
                // into the fresh task. Fail-closed: compose failure
                // destroys the task and skips the row (invalid-ELF
                // precedent below). Compile-guarded: boot_sdf.cpp is an
                // empty TU when all PD knobs are 0, so the call must not
                // be emitted at OFF.
                const auto role = static_cast<sdf::PdRole>(def.sdf_role);
                if (!sdf::sdf_compose(*task, role)) {
                    Logger::warn(
                        "reboot: SDF compose failed for task '%s', skipping",
                        def.name);
                    kernel::log::dmesg_push_sev(
                        kernel::log::ErrorSubsystem::INIT,
                        kernel::log::kDmesgBase_INIT + 13,
                        kernel::log::LogSeverity::ERROR, def.name, 0);
                    TaskControlBlock::destroy(task);
                    task = nullptr;
                }
            }
#endif // any PD knob (sdf_compose lives in boot_sdf.cpp)
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
            // Issue #19: PID 1 stays priority-dispatched (exempt from
            // global EDF) — preserves the BUGS#021 non-preemption guard
            // under the new dispatcher.  Migrates the live queue
            // membership (init was EDF-queued at add_task time).
            Scheduler::set_edf_exempt(*init_task, true);
        }
    }

#if defined(CONFIG_DEBUG)
    debug_write("[REBOOT] after add_tasks task_count()=0x");
    debug_write_hex(Scheduler::task_count());
    debug_write("\n");
#endif

#if defined(CONFIG_ARCH_X86_64)
    // Issue #25 C1: publish the AP scheduler start-gate AFTER spawning
    // (tables quiescent from here on) and before the idle loop.  APs spin
    // parked until this lands; post-gate they create their idles and tick.
    kernel::smp::publish_scheduler_ready();
#endif
#if defined(CONFIG_ARCH_AARCH64)
    // Issue #351 slice-2: same start-gate contract for aa64 APs (parked in
    // ap_main until this lands; post-gate they adopt idle tasks and tick).
    kernel::smp::publish_scheduler_ready();
#endif
#if defined(CONFIG_ARCH_RISCV64)
    // Issue #351 slice-3: same start-gate contract for rv64 APs (parked in
    // ap_main until this lands; post-gate they adopt idle tasks and tick).
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

    // Issue #244: reboot/idle diagnostics are debug-only (release boots
    // straight into a clean console for the shell).
#if defined(CONFIG_DEBUG)
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
#endif
#if defined(CONFIG_ARCH_AARCH64) || defined(CONFIG_ARCH_RISCV64)
    // Issue #351 slice-2/slice-3: one-shot cross-CPU IPI verification,
    // last reboot step before unmasking IRQs. Placement is deliberate: the
    // BSP needs no IRQs of its own (MMIO/SBI send + shared-memory ack
    // polling), while the APs are already IRQ-live in their idle loops
    // (start-gate published above). Running it here — instead of after
    // sti — keeps it deterministic: the first timer tick would otherwise
    // preempt the idle entry and strand this one-shot behind the
    // scheduler. Panic on failure (fail-closed).
    kernel::smp::verify_booted_aps();
#endif
    arch::sti();
    for (;;) {
        arch::hlt();
    }
    __builtin_unreachable();
}

} // namespace task
} // namespace kernel
