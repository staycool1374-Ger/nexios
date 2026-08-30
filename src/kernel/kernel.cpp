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

#include <kernel/kernel.hpp>
#include <kernel/core/global_state.hpp>
#include <kernel/arch/gdt.hpp>
#include <kernel/arch/idt.hpp>
#include <kernel/arch/timer.hpp>
#include <kernel/arch/apic.hpp>
#include <kernel/arch/irq_latency_histogram.hpp>
#include <kernel/irq_thread.hpp>
#include <kernel/irq_delivery.hpp>
#include <kernel/arch/interrupt_controller.hpp>
#include <kernel/arch/rtc.hpp>
#include <kernel/arch/io.hpp>
#include <kernel/arch/keyboard.hpp>
#include <kernel/arch/qemu_debugcon.hpp>
#include <kernel/arch/serial.hpp>
#include <kernel/memory/pmm.hpp>
#include <kernel/memory/vmm.hpp>
#include <kernel/memory/mempool.hpp>
#include <kernel/task/scheduler.hpp>
#include <kernel/task/task.hpp>
#include <kernel/ipc/ipc.hpp>
#include <kernel/ipc/buffer_pool.hpp>
#include <kernel/syscall/syscall.hpp>
#include <kernel/driver/driver.hpp>
#include <version.hpp>
#include <kernel/bootparams.hpp>
#include <kernel/sync/sync.hpp>
#include <kernel/multiboot2.hpp>
#include <kernel/elf/elf.hpp>
#include <kernel/elf/elf_loader.hpp>
#include <kernel/vfs/vfs.hpp>
#include <kernel/vfs/vfsd.hpp>
#include <kernel/log/dmesg.hpp>
#include <kernel/driver/iocd.hpp>
#include <kernel/driver/ata_pio.hpp>
#include <kernel/driver/ahci.hpp>
#include <kernel/driver/virtio_blk.hpp>
#include <kernel/driver/virtio_net.hpp>
#include <kernel/net/net.hpp>
#include <kernel/vfs/fat32_fs.hpp>
#include <kernel/ipc/ipc_boot.hpp>
#include <kernel/daemon/daemon_mgr.hpp>
#include <kernel/vfs/initrd_fs.hpp>
#include <kernel/random.hpp>
#include <kernel/vfs/devfs.hpp>
#include <kernel/vfs/procfs.hpp>
#include <kernel/vfs/tmpfs.hpp>
#include <initrd/initrd.hpp>
#include <services/terminal/framebuffer.hpp>
#include <services/terminal/terminal.hpp>
#include <services/program.hpp>
#include <services/shell.hpp>
#include <programs/demo/demo.hpp>
#include <logger.hpp>
#include <test.hpp>
#include <kernel/task/taskdefs.hpp>
#include <kernel/test/test_config.hpp>
#include <string.hpp>
#include <constants.hpp>
#include <signal.hpp>
#include <kernel/debug/dump.hpp>
#include <fdt/libfdt.h>
#include <fdt/libfdt_internal.h>
#include <string.hpp>

#ifdef CONFIG_PROFILING
extern "C" void gcov_flush_to_serial();
#endif

using namespace arch;

// Set by higherhalf_entry when tests are configured.  Read by init_task_main
// to run the test suite from a proper task context (IF=1) instead of from
// the boot context (IF=0, where reschedule()+hlt() would hang).
static bool g_run_tests = false;

// ── init_task_main ──────────────────────────────────────────────────────
// Init task entry point for PID 1.  Mounts fstab, runs /etc/rc, then
// blocks as reaper.  Referenced by both the manual pre-test create (below)
// and the post-test task-definition table (taskdefs.cpp).
extern "C" void debug_write(const char *msg);
void init_task_main() {
    debug_write("[DIAG] init_task_main entered\n");
    // Read /etc/fstab and mount entries
    {
        auto fstab = initrd::find("./etc/fstab");
        if (fstab.data) {
            const char *p = reinterpret_cast<const char *>(fstab.data);
            const char *end = p + fstab.size;
            while (p < end) {
                while (p < end && (*p == ' ' || *p == '\t'))
                    ++p;
                if (p >= end || *p == '#' || *p == '\n') {
                    while (p < end && *p != '\n')
                        ++p;
                    if (p < end)
                        ++p;
                    continue;
                }
                char fs_name[32];
                char mp[64];
                int n = 0;
                while (p < end && *p != ' ' && *p != '\t' && *p != '\n' &&
                       n < 31)
                    fs_name[n++] = *p++;
                fs_name[n] = '\0';
                while (p < end && (*p == ' ' || *p == '\t'))
                    ++p;
                n = 0;
                while (p < end && *p != ' ' && *p != '\t' && *p != '\n' &&
                       n < 63)
                    mp[n++] = *p++;
                mp[n] = '\0';
                while (p < end && *p != '\n')
                    ++p;
                if (p < end)
                    ++p;
                if (fs_name[0] && mp[0]) {
                    auto *fs = kernel::vfs::find_fs(fs_name);
                    if (fs && kernel::vfs::mount(*fs, mp) == 0) {
                        kernel::Logger::info("init: mounted %s at %s", fs_name,
                                             mp);
                    }
                }
            }
        }
    }
    // Read /etc/rc and execute each command
    {
        auto rc = initrd::find("./etc/rc");
        if (rc.data) {
            const char *p = reinterpret_cast<const char *>(rc.data);
            const char *end = p + rc.size;
            while (p < end) {
                while (p < end && (*p == ' ' || *p == '\t'))
                    ++p;
                if (p >= end || *p == '#' || *p == '\n') {
                    while (p < end && *p != '\n')
                        ++p;
                    if (p < end)
                        ++p;
                    continue;
                }
                char line[128];
                int n = 0;
                while (p < end && *p != '\n' && n < 127)
                    line[n++] = *p++;
                line[n] = '\0';
                if (p < end)
                    ++p;
                if (line[0] == '\0')
                    continue;
                char elf_path[160];
                n = 0;
                const char *src = line;
                while (*src && *src != ' ' && *src != '\t' && n < 127)
                    elf_path[n++] = *src++;
                elf_path[n] = '\0';
                char elf_path1[160], elf_path2[160];
                int m = 0;
                for (int i = 0; elf_path[i]; ++i)
                    elf_path1[m++] = elf_path[i];
                elf_path1[m++] = '.';
                elf_path1[m++] = 'e';
                elf_path1[m++] = 'l';
                elf_path1[m++] = 'f';
                elf_path1[m] = '\0';
                m = 0;
                for (int i = 0; elf_path[i]; ++i)
                    elf_path2[m++] = elf_path[i];
                elf_path2[m++] = '.';
                elf_path2[m++] = 'c';
                elf_path2[m++] = '.';
                elf_path2[m++] = 'e';
                elf_path2[m++] = 'l';
                elf_path2[m++] = 'f';
                elf_path2[m] = '\0';
                auto f = initrd::find(elf_path);
                if (!f.data)
                    f = initrd::find(elf_path1);
                if (!f.data)
                    f = initrd::find(elf_path2);
                if (f.data) {
                    auto *hdr =
                        reinterpret_cast<const kernel::elf::ELF64Header *>(
                            f.data);
                    if (kernel::elf::validate_header(hdr)) {
                        auto *task = kernel::elf::load(hdr, f.data, f.size);
                        if (task) {
                            task->priority = 2;
                            task->base_priority = 2;
                            task->period_ticks = 0;
                            kernel::Scheduler::add_task(*task);
                            kernel::Logger::info("init: started %s", elf_path);
                        }
                    }
                }
            }
        }
    }

    // ── Wait for daemon readiness ───────────────────────────────
    struct DaemonWatch {
        const char *name;
        uint64_t expected_pid;
        bool ready;
    } watch[] = {
        {"vfsd", kernel::vfsd::get_vfsd_pid(), false},
        {"iocd", kernel::iocd::get_iocd_pid(), false},
    };
    size_t watch_count = sizeof(watch) / sizeof(watch[0]);

    uint64_t deadline = arch::Timer::ticks() + 500;
    bool degraded = false;

    kernel::Logger::info(
        "[DIAG] init_task_main: starting daemon wait (g_run_tests=%u)",
        (unsigned)g_run_tests);

    while (!degraded) {
        bool all_ready = true;
        for (size_t wi = 0; wi < watch_count; ++wi) {
            if (!watch[wi].ready) {
                all_ready = false;
                break;
            }
        }
        if (all_ready)
            break;

        if (arch::Timer::ticks() >= deadline) {
            kernel::Logger::warn("init: timeout waiting for daemon(s), "
                                 "starting in degraded mode");
            degraded = true;
            break;
        }

        kernel::Message boot_msg{};
        while (kernel::IPC::recv(boot_msg)) {
            if (boot_msg.type == kernel::ipc::MSG_DAEMON_READY) {
                for (size_t wi = 0; wi < watch_count; ++wi) {
                    if (!watch[wi].ready &&
                        boot_msg.sender_id == watch[wi].expected_pid) {
                        watch[wi].ready = true;
                        kernel::Logger::info("init: daemon '%s' ready",
                                             watch[wi].name);
                        break;
                    }
                }
            } else if (boot_msg.type == kernel::ipc::MSG_DAEMON_FAILED) {
                for (size_t wi = 0; wi < watch_count; ++wi) {
                    if (boot_msg.sender_id == watch[wi].expected_pid) {
                        kernel::Logger::warn("init: daemon '%s' "
                                             "failed to init",
                                             watch[wi].name);
                        break;
                    }
                }
            }
        }

        // hlt without state change: stay READY so the scheduler can
        // pick us up when higher-priority tasks block or finish.
        // IPC::send only wakes BLOCKED tasks (ipc.cpp:183), so WAITING
        // would deadlock — we'd never be resumed after yielding.
        arch::hlt();
    }

    // ── Background ELF loader task (created before the test runner so it is
    //    part of the snapshot baseline and survives snapshot_restore). ──
    kernel::elf::ElfLoader::ensure_task();

    // ── Run tests from init-task context (IF=1) ──────────────────
    kernel::Logger::info(
        "[DIAG] init_task_main: reached test runner g_run_tests=%u",
        (unsigned)g_run_tests);
    if (g_run_tests) {
        // BUGS.md#021 harness exemption requires init (PID 1) at priority 10
        // (its boot/test-runner duty priority) during the suite; the base
        // priority in g_task_defs is 0 (background reaper).
        if (auto *self = kernel::Scheduler::current_task()) {
            kernel::Scheduler::set_priority(*self, 10);
        }
#ifdef CONFIG_DEBUG
        kernel::Logger::info("[TEST] Registry tests=%u classes=%u",
                             (unsigned)kernel::test::Registry::count(),
                             (unsigned)kernel::test::Registry::class_count());
        kernel::test::set_class_auto_shutdown(true);
        kernel::test::run_registered(0);
#else
        kernel::test::set_class_auto_shutdown(false);
        kernel::test::run_filtered(kernel::test::TF_RELEASE, false);
#endif
    }

    // ── Create shell task ───────────────────────────────────────
    {
        auto *shell = kernel::TaskControlBlock::create(
            service::Shell::shell_task_main, 2, 0);
        if (shell) {
            const char *src = "shell";
            size_t i = 0;
            while (src[i] && i < CONFIG_TASK_NAME_LEN - 1) {
                shell->name[i] = src[i];
                ++i;
            }
            shell->name[i] = '\0';
            kernel::Scheduler::add_task(*shell);
            kernel::Scheduler::set_shell_task(shell);
            kernel::Logger::info("init: shell task %u created", shell->id);
        } else {
            kernel::Logger::warn("init: failed to create shell task");
        }
    }

    // ── Register keyboard ISR ───────────────────────────────────
    // Must be done AFTER reboot_from_table() (which killed the boot-time
    // IrqThread task).  Only in interactive mode (no test suite): during the
    // test run keyboard input is unused and a persistent IrqThread task would
    // perturb snapshot-isolation task-count expectations.
    if (!g_run_tests) {
#if defined(CONFIG_ARCH_X86_64) && CONFIG_THREADED_IRQS
    kernel::IrqThread::create(33, 50,
                              [](uint64_t, uint64_t, uint64_t) {
                                  arch::Keyboard::handle_irq();
                              },
                              [](uint8_t vector) {
                                  // x86_64: send APIC EOI + PIC EOI
                                  if (arch::APIC::is_enabled())
                                      arch::APIC::eoi();
                                  arch::ArchInterruptController::eoi(vector);
                              });
#elif defined(CONFIG_ARCH_X86_64)
    arch::IDT::register_handler(arch::InterruptVector::KEYBOARD,
                                [](uint64_t, uint64_t, uint64_t) {
                                    arch::Keyboard::handle_irq();
                                    outb(arch::PIC1_CMD, 0x20);
                                });
#endif
    }

    // ── Reap loop — block until a child exits ───────────────────
    // Boot/test-runner duty is done; the reaper + daemon-restart logger only
    // needs background priority (0) so it never starves the shell (prio 2)
    // or the daemons (prio 20).  The shell's `selftest` command raises it
    // back to 10 for the test run.
    if (auto *init_self = kernel::Scheduler::current_task()) {
        kernel::Scheduler::set_priority(*init_self, 0);
        // The reaper is best-effort background work, not a hard real-time
        // periodic task.  g_task_defs gives init a nominal period/WCET
        // (period=100, wcet=1) so it participates in RMS while it is the
        // boot/test-runner harness.  Once dropped to prio 0 it can never
        // meet a 100-tick deadline (the shell/daemons always run first),
        // so scan_deadlines() and the WCET scan would flag it every period
        // with LOG_ONLY spam.  Clear the RT budget so the monitors skip it.
        init_self->period_ticks = 0;
        init_self->deadline_ticks = 0;
        init_self->wcet_ticks = 0;
    }
    for (;;) {
        arch::pause();
        kernel::Scheduler::drain_zombie_list();

        kernel::Message msg{};
        while (kernel::IPC::recv(msg)) {
            if (msg.type == kernel::ipc::MSG_DAEMON_READY) {
                kernel::Logger::info("init: daemon (PID %u) ready "
                                     "(restart)",
                                     msg.sender_id);
            } else if (msg.type == kernel::ipc::MSG_DAEMON_FAILED) {
                kernel::Logger::warn("init: daemon (PID %u) init "
                                     "failed (restart)",
                                     msg.sender_id);
            }
        }

        arch::hlt();
    }
}

static void debug_putchar(char c) {
#if defined(CONFIG_ARCH_X86_64)
    // Debugcon (port 0xE9): lock-free, single-digit ns per byte.  The UART
    // path below busy-waits on LSR.THRE for ~87us/byte at 115200 baud, which
    // measurably warps scheduler/IPC timing (H2 investigation) — but ONLY
    // while the test suite is actively running.  When the suite is done (the
    // intended post-selftest interactive shell) route to the UART: the QEMU
    // mux chardev gives input focus to the frontend that last wrote, and
    // constant debugcon writes steal the keystroke stream from the shell's
    // COM1 polling (run-release-mode input-dead regression, 2026-08-08).
#if CONFIG_DEADLINE_MONITOR_TASK
    if (kernel::Scheduler::is_test_active()) {
        arch::QemuDebugcon::putc(c);
        return;
    }
#endif
    arch::Serial::putchar(c);
#else
    arch::Serial::putchar(c);
#endif
}

extern "C" void debug_write(const char *s) {
    while (*s)
        debug_putchar(*s++);
}

extern "C" void debug_write_hex(uint64_t value) {
    char hb[17] = "0000000000000000";
    int pos = 16;
    do {
        hb[--pos] = "0123456789ABCDEF"[value & 0xF];
        value >>= 4;
    } while (value);
    debug_write(hb + pos);
}

extern "C" void debug_write_dec(uint64_t value) {
    char db[21] = "00000000000000000000";
    int pos = 20;
    if (value == 0) {
        debug_write("0");
        return;
    }
    do {
        db[--pos] = "0123456789"[value % 10];
        value /= 10;
    } while (value);
    debug_write(db + pos);
}

extern "C" void debug_task_switch(uint64_t old_id, uint64_t new_id,
                                  uint64_t cr3) {
    debug_write("[SWITCH] old=");
    debug_write_hex(old_id);
    debug_write(" new=");
    debug_write_hex(new_id);
    debug_write(" cr3=");
    debug_write_hex(cr3);
    debug_write("\n");
}

extern char kernel_virt_end[];
extern "C" uint8_t _binary_initrd_cpio_start[];
extern "C" uint8_t _binary_initrd_cpio_end[];

#if defined(CONFIG_ARCH_AARCH64)
extern "C" {
void *g_dtb_ptr = nullptr;
}
#endif

#if defined(CONFIG_ARCH_X86_64)
static void init_pic() {
    outb(arch::PIC1_CMD, 0x11);
    outb(arch::PIC2_CMD, 0x11);
    outb(arch::PIC1_DATA, 0x20);
    outb(arch::PIC2_DATA, 0x28);
    outb(arch::PIC1_DATA, 0x04);
    outb(arch::PIC2_DATA, 0x02);
    outb(arch::PIC1_DATA, 0x01);
    outb(arch::PIC2_DATA, 0x01);
    outb(arch::PIC1_DATA, 0x00);
    outb(arch::PIC2_DATA, 0x00);
}
#endif

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
extern "C" void higherhalf_entry(uint64_t magic, uint64_t mb_info) {
    kernel::gs::boot_info() = BootInfo();
    [[maybe_unused]] kernel::gs::WriteContext ctx{
        kernel::gs::StatePhase::BOOT, 0};

#if defined(CONFIG_ARCH_X86_64)
    kernel::gs::try_set_multiboot(magic, mb_info, ctx);
    extern const uint64_t kernel_stack_top;
    asm volatile("mov %0, %%rsp\n" : : "r"(kernel_stack_top));
    kernel::gs::boot_info().multiboot_magic = magic;
    kernel::gs::boot_info().multiboot_info = mb_info;
#elif defined(CONFIG_ARCH_AARCH64)
    g_dtb_ptr = reinterpret_cast<void *>(magic);
    kernel::gs::boot_info().dtb_ptr = magic;
    (void)mb_info;
    arch::fp_enable();
#elif defined(CONFIG_ARCH_RISCV64)
    kernel::gs::boot_info().hart_id = static_cast<int>(magic);
    kernel::gs::boot_info().dtb_ptr = mb_info;
    arch::fp_enable();

#endif


    kernel::Logger::init();
    kernel::test::set_kernel_entry_ns();
    kernel::test::Registry::init();
    debug_write("[BOOT] ");
    debug_write(kernel::Version::string());

#ifdef CONFIG_DEBUG
    debug_write(" [DEBUG]");
#else
    debug_write(" [RELEASE]");

#endif

    debug_write("\n");

    debug_write("[BOOT] Arch init...\n");
    arch::GDT::init();
    arch::GDT::load();
#if defined(CONFIG_ARCH_X86_64)
    extern const uint64_t kernel_stack_top;
    arch::GDT::set_tss_rsp0(kernel_stack_top);

    // Enable x87 FPU: clear CR0.EM (bit 2), set CR0.NE (bit 5), set CR0.MP (bit
    // 1)
    uint64_t cr0 = arch::read_cr0();
    cr0 &= ~(1ULL << 2);
    cr0 |= (1ULL << 1);
    cr0 |= (1ULL << 5);
    arch::write_cr0(cr0);

    // Enable FXSAVE/FXRSTOR and SSE: set CR4.OSFXSR (bit 9) and CR4.OSXMMEXCPT
    // (bit 10)
    uint64_t cr4 = arch::read_cr4();
    cr4 |= (1ULL << 9);
    cr4 |= (1ULL << 10);

    // v0.4.0 MP-4: SMEP (CR4 bit 20) + SMAP (CR4 bit 21) — supervisor-mode
    // execution/prevention.  When supported, ring-3 code can no longer execute
    // kernel text/data VAs (SMEP) and the kernel must stac/clac around any
    // user-memory access (SMAP; every such site is wrapped in checked_ptr /
    // the syscall handlers).
#if CONFIG_SMEP || CONFIG_SMAP
    {
        uint32_t max_leaf = 0, ebx = 0, ecx = 0, edx = 0;
        asm volatile("cpuid"
                     : "=a"(max_leaf), "=b"(ebx), "=c"(ecx), "=d"(edx)
                     : "a"(7), "c"(0));
        (void)max_leaf;
        (void)ecx;
        (void)edx;
#if CONFIG_SMEP
        if (ebx & (1u << 7)) {
            cr4 |= (1ULL << 20); // CR4.SMEP
        } else {
            debug_write("[BOOT] SMEP not supported by CPU — leaving off\n");
        }
#endif
#if CONFIG_SMAP
        if (ebx & (1u << 20)) {
            cr4 |= (1ULL << 21); // CR4.SMAP
        } else {
            debug_write("[BOOT] SMAP not supported by CPU — leaving off\n");
        }
#endif
    }
#endif
    arch::write_cr4(cr4);

#endif

    arch::IDT::init();
    arch::IDT::load();

#if defined(CONFIG_ARCH_AARCH64) && CONFIG_PAN
    // v0.4.2 MP-4.4: PAN (privileged access never) — SCTLR_EL1.PAN.
    // When FEAT_PAN is present, stac/clac become real PSTATE.PAN toggles
    // (mirror of x86_64 SMAP); every kernel->user deref stays wrapped.
    if (arch::pan_init()) {
        debug_write("[BOOT] PAN enabled (SCTLR_EL1.PAN)\n");
    } else {
        debug_write("[BOOT] PAN not supported by CPU — leaving off\n");
    }
#endif

#if defined(CONFIG_ARCH_AARCH64) || defined(CONFIG_ARCH_RISCV64)
    arch::ArchInterruptController::init();

#endif


#if defined(CONFIG_ARCH_X86_64)
    {
        uint64_t tag_addr = mb2_find_tag(6);
        if (tag_addr) {
            // NOLINTNEXTLINE(performance-no-int-to-ptr)
            auto *mem_tag = reinterpret_cast<MemoryMapTag *>(tag_addr);
            uint64_t entries =
                (mem_tag->size - sizeof(MemoryMapTag)) / mem_tag->entry_size;
            for (uint64_t i = 0; i < entries; ++i) {
                auto &entry = mem_tag->entries[i];
                kernel::gs::boot_info().add_region(entry.base_addr, entry.length,
                                       entry.type);
            }
        }
    }
#endif

#if defined(CONFIG_ARCH_AARCH64) || defined(CONFIG_ARCH_RISCV64)
    {
        void *dtb = reinterpret_cast<void *>(kernel::gs::boot_info().dtb_ptr);
        if (dtb && fdt_check_header(dtb) == 0) {
            int offset = fdt_node_offset_by_prop_value(dtb, -1, "device_type",
                                                       "memory", 7);
            if (offset < 0) {
                offset = fdt_subnode_offset_namelen(dtb, 0, "memory", 6);
            }
            if (offset >= 0) {
                int len{};
                const uint32_t *reg = static_cast<const uint32_t *>(
                    fdt_getprop_namelen(dtb, offset, "reg", &len));
                if (reg && len >= 16) {
                    uint64_t base =
                        (static_cast<uint64_t>(fdt32_to_cpu(reg[0])) << 32) |
                        fdt32_to_cpu(reg[1]);
                    uint64_t size =
                        (static_cast<uint64_t>(fdt32_to_cpu(reg[2])) << 32) |
                        fdt32_to_cpu(reg[3]);
                    kernel::gs::boot_info().add_region(base, size, 1);
                }
            }
            int chosen = fdt_subnode_offset_namelen(dtb, 0, "chosen", 6);
            if (chosen >= 0) {
                int len{};
                const char *bootargs = static_cast<const char *>(
                    fdt_getprop_namelen(dtb, chosen, "bootargs", &len));
                if (bootargs && len > 0 && len < 256) {
                    strlcpy(kernel::gs::boot_info().cmdline, bootargs, 256);
                }
            }
        }
    }
#endif

    uint64_t kend =
        reinterpret_cast<uint64_t>(kernel_virt_end) - arch::HHDM_OFFSET;
    uint64_t mem_size = 0;
    // Allocatable-window base: min usable RAM base from the firmware map.
    // arch::RAM_BASE_FALLBACK is 0 on x86_64 (window byte-identical to the
    // pre-window-base behavior) and the QEMU virt RAM base for
    // aarch64/riscv64.
    uint64_t ram_base = arch::RAM_BASE_FALLBACK;
    if (kernel::gs::boot_info().num_mem_regions > 0) {
        for (int i = 0; i < kernel::gs::boot_info().num_mem_regions; ++i) {
            const auto &region = kernel::gs::boot_info().mem_regions[i];
            uint64_t region_end = region.base + region.size;
            if (region_end > mem_size)
                mem_size = region_end;
            if (region.type == 1 && region.base < ram_base)
                ram_base = region.base;
        }
    } else {
#if defined(CONFIG_ARCH_X86_64)
        mem_size = 64_MiB;
#elif defined(CONFIG_ARCH_AARCH64)
        mem_size = arch::RAM_BASE_FALLBACK + 256_MiB;
#elif defined(CONFIG_ARCH_RISCV64)
        mem_size = arch::RAM_BASE_FALLBACK + 128_MiB;

#endif

    }
    kernel::PMM::init(mem_size, arch::PAGE_SIZE_2M, kend, ram_base);
    kernel::VMM::init();

    // v0.4.0 MP-1.5: the per-kernel-task private-data window must not overlap
    // anything the boot kernel PML4 maps — a task that maps a page there gets
    // TRUE cross-task isolation, not an aliased kernel mapping.
    {
        constexpr uint64_t priv_base = CONFIG_KERNEL_PRIV_DATA_BASE;
        size_t pml4_idx = (priv_base >> 39) & 0x1FF;
        auto *kpml4 = reinterpret_cast<const uint64_t *>(
            arch::HHDM_OFFSET + (kernel::VMM::get_kernel_pml4() & ~0xFFFULL));
        if (kpml4[pml4_idx] & 1ULL)
            panic("CONFIG_KERNEL_PRIV_DATA_BASE overlaps kernel PML4 entry");
    }

    // Map APIC MMIO pages and initialise the local/I/O APIC (x86_64 only;
    // AArch64/RISC-V use their interrupt controllers, initialised elsewhere).
#if defined(CONFIG_ARCH_X86_64)
    if (arch::APIC::is_apic_supported()) {
        arch::APIC::map_mmio();
        arch::APIC::init();
    }
#endif // CONFIG_ARCH_X86_64
    if (kernel::gs::boot_info().cmdline[0]) {
        kernel::BootParams::parse_cstr(kernel::gs::boot_info().cmdline);
    }
#if defined(CONFIG_ARCH_X86_64)
    kernel::BootParams::parse_multiboot_cmdline();

#endif

    {
        auto &bp = kernel::BootParams::instance();
        debug_write("[BOOT] timer_hz=");
        debug_write_hex(bp.timer_hz);
        debug_write(" preempt=");
        debug_write(bp.preempt_enabled ? "yes" : "no");
        debug_write(" max_tasks=");
        debug_write_hex(bp.max_tasks);
        debug_write("\n");
    }
    debug_write("[BOOT] Memory init done\n");

    debug_write("[BOOT] BEFORE STVEC\n");
    uint64_t val = 42;
    if (val == 42) {
        debug_write("[BOOT] val==42, all good\n");
    }
    debug_write("[BOOT] AFTER STVEC\n");

    debug_write("[BOOT] Kernel init...\n");
    kernel::MemPool::init();
    initrd::init(_binary_initrd_cpio_start, _binary_initrd_cpio_end);
    kernel::vfs::init();
    kernel::vfs::mount(kernel::vfs::initrd_fs, "/");
    kernel::IPC::init();
    kernel::Syscall::init();
    // PfA-A: scheduler boot configuration flows down from kernel_init instead
    // of being hardcoded defaults reachable as globals.
    kernel::Scheduler::init(kernel::SchedulerConfig{});

    // Init task (PID 1) — mounts fstab, runs /etc/rc, then blocks as reaper.
    // Priority 0 as background reaper; init_task_main raises itself to 10
    // (harness priority) when g_run_tests is active, and drops back to 0 in
    // the reap loop.  Spawned below by reboot_from_table() from g_task_defs.
#ifndef __clang__
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wanalyzer-null-argument"
#endif
    kernel::PMM::set_oom_handler([]() -> bool {
        kernel::TaskControlBlock *victim = nullptr;
        uint64_t victim_priority = ~0ULL;
        for (uint64_t i = 0; i < kernel::Scheduler::task_count(); ++i) {
            auto *t = kernel::Scheduler::task_at(i);
            if (!t || t == kernel::Scheduler::task_at(0))
                continue;
            if (t->state != kernel::TaskState::READY &&
                t->state != kernel::TaskState::RUNNING)
                continue;
            if (!t->is_user_)
                continue;
            if (t->priority < victim_priority) {
                victim = t;
                victim_priority = t->priority;
            }
        }
        if (!victim)
            return false;
        debug_write("[OOM] Killing task ");
        debug_write_hex(victim->id);
        debug_write(" priority=");
        debug_write_hex(victim->priority);
        debug_write("\n");
        kernel::Scheduler::terminate(*victim,
            static_cast<uint64_t>(-static_cast<int64_t>(9)));
        kernel::Scheduler::drain_zombie_list();
        return true;
    });
#ifndef __clang__
#pragma GCC diagnostic pop
#endif
    kernel::DriverRegistry::init();
    debug_write("[BOOT] Kernel init done\n");
    kernel::PMM::mark_init_done();

    service::Framebuffer::init();
    service::Terminal::init();
    if (service::Terminal::instance()) {
        service::Terminal::show_splash();
    }

#if defined(CONFIG_ARCH_X86_64)
    init_pic();
    arch::Keyboard::init();
#endif

    kernel::vfs::devfs_init();
    kernel::vfs::mount(kernel::vfs::dev_fs, "/dev");
    kernel::vfs::mount(kernel::vfs::proc_fs, "/proc");
    kernel::vfs::mount(kernel::vfs::tmpfs_fs, "/tmp");

    // Probe AHCI controller first, fall back to legacy ATA PIO
#if defined(CONFIG_ARCH_X86_64)
    {
        auto *ahci = kernel::block::AhciDriver::probe();
        if (ahci) {
            debug_write("[BOOT] AHCI drive found\n");
            auto *part_mem = kernel::MemPool::alloc(sizeof(kernel::fat32::Fat32Partition));
            if (!part_mem) {
                debug_write("[BOOT] FAT32 OOM\n");
            } else {
                auto *part = new (part_mem) kernel::fat32::Fat32Partition(*ahci);
                if (part->mount()) {
                    debug_write("[BOOT] FAT32 partition mounted via AHCI\n");
                    kernel::gs::try_set_fat32_partition(part);
                    if (kernel::vfs::mount_fat32("/mnt") == 0) {
                        debug_write("[BOOT] FAT32 filesystem mounted at /mnt\n");
                    } else {
                        debug_write("[BOOT] mount_fat32 failed\n");
                    }
                } else {
                    debug_write("[BOOT] FAT32 partition mount failed\n");
                    part->~Fat32Partition();
                    kernel::MemPool::free(part);
                }
            }
        } else {
            debug_write("[BOOT] No AHCI drive found, trying legacy ATA PIO\n");
            auto *ata = kernel::block::AtaPioDriver::probe_first_drive();
            if (ata) {
                debug_write("[BOOT] ATA drive found\n");
                auto *part_mem = kernel::MemPool::alloc(sizeof(kernel::fat32::Fat32Partition));
                if (!part_mem) {
                    debug_write("[BOOT] FAT32 OOM\n");
                } else {
                    auto *part = new (part_mem) kernel::fat32::Fat32Partition(*ata);
                    if (part->mount()) {
                        debug_write("[BOOT] FAT32 partition mounted via PIO\n");
                        kernel::gs::try_set_fat32_partition(part);
                        if (kernel::vfs::mount_fat32("/mnt") == 0) {
                            debug_write("[BOOT] FAT32 filesystem mounted at /mnt\n");
                        } else {
                            debug_write("[BOOT] mount_fat32 failed\n");
                        }
                    } else {
                        debug_write("[BOOT] FAT32 partition mount failed\n");
                        part->~Fat32Partition();
                        kernel::MemPool::free(part);
                    }
                }
            } else {
                debug_write("[BOOT] No ATA drive found\n");
            }
        }
    }

#endif

    // Probe virtio-blk device (arch-independent, uses unified PCI HAL)
    {
        auto *vblk = kernel::block::VirtioBlkDriver::probe();
        if (vblk) {
            debug_write("[BOOT] Virtio-blk drive found\n");
            auto *part_mem = kernel::MemPool::alloc(sizeof(kernel::fat32::Fat32Partition));
            if (!part_mem) {
                debug_write("[BOOT] FAT32 OOM\n");
            } else {
                auto *part = new (part_mem) kernel::fat32::Fat32Partition(*vblk);
                if (part->mount()) {
                    debug_write("[BOOT] FAT32 partition mounted via virtio-blk\n");
                    if (!kernel::gs::get_fat32_partition()) {
                        kernel::gs::try_set_fat32_partition(part);
                        if (kernel::vfs::mount_fat32("/mnt") == 0) {
                            debug_write(
                                "[BOOT] FAT32 filesystem mounted at /mnt\n");
                        } else {
                            debug_write("[BOOT] mount_fat32 failed\n");
                        }
                    }
                } else {
                    debug_write("[BOOT] FAT32 partition mount failed\n");
                    part->~Fat32Partition();
                    kernel::MemPool::free(part);
                }
            }
        } else {
            debug_write("[BOOT] No virtio-blk device found\n");
        }
    }

    // Probe virtio-net NIC and initialize network stack
#if defined(CONFIG_ARCH_X86_64)
    {
        static net::Nic g_boot_nic{};
        if (kernel::net::virtio_net_probe(g_boot_nic)) {
            net::net_init(g_boot_nic, g_boot_nic.mac,
                          net::Ipv4Addr{{10, 0, 2, 15}},
                          net::Ipv4Addr{{255, 255, 255, 0}},
                          net::Ipv4Addr{{10, 0, 2, 2}});
            debug_write("[BOOT] Virtio-net NIC probed, IP=10.0.2.15\n");
        } else {
            debug_write("[BOOT] No virtio-net NIC found\n");
        }
    }
#endif

    arch::Timer::init(kernel::BootParams::instance().timer_hz);

#if CONFIG_IRQ_LATENCY_HISTOGRAM
    kernel::IrqLatencyHistogram::init();
#endif

    arch::RTC::init();
    kernel::gs::WriteContext bctx{kernel::gs::StatePhase::BOOT, 0};
    kernel::gs::try_set_boot_epoch(arch::RTC::read_seconds(), bctx);
    kernel::random_init();
    debug_write("[BOOT] Hardware init done\n");

    kernel::DriverRegistry::register_driver("keyboard", "PS/2 Tastaturtreiber",
                                            nullptr, nullptr, 1);
    kernel::DriverRegistry::register_driver("timer", "PIT Timer (1000 Hz)",
                                            nullptr, nullptr, 0);
    kernel::DriverRegistry::register_driver(
        "framebuffer", "Framebuffer Grafiktreiber", nullptr, nullptr, 0);
    kernel::DriverRegistry::register_driver("pcspkr", "PC Speaker Soundtreiber",
                                            nullptr, nullptr, 0);

    service::ProgramRegistry::init();
    service::ProgramRegistry::register_program(
        "demo", "Mandelbrot set + spinning rectangles (framebuffer)",
        programs::demo_main);

    kernel::BufferPool::init();
    kernel::daemon::init();

    // vfsd/iocd are NOT loaded here — reboot_from_table() below rebuilds the
    // system from g_task_defs (taskdefs.cpp), which is the single source of
    // truth for daemon priorities/periods.  Pre-reboot daemon loads were dead
    // weight (killed unconditionally by reboot_from_table).

    if (service::Terminal::instance()) {
        service::Terminal::set_fb_enabled(false);
    }

    kernel::test::Registry::init();
    kernel::test::parse_test_config("./tests/test-config.txt");

    const char **classes = kernel::test::get_test_classes();
    size_t class_count = kernel::test::get_test_class_count();

    // "none" class means interactive mode — skip the test suite entirely
    bool skip_tests = (class_count == 1 && classes[0] != nullptr &&
                       strcmp(classes[0], "none") == 0);

    // "dump-counts" class — print per-class registration counts then exit
    bool dump_counts = (class_count == 1 && classes[0] != nullptr &&
                        strcmp(classes[0], "dump-counts") == 0);
    if (dump_counts) {
        kernel::test::dump_class_counts();
        arch::qemu_debug_exit(0);
    }

    if (!skip_tests && !dump_counts) {
        for (size_t i = 0; i < class_count; ++i) {
            kernel::test::register_class(classes[i]);
        }

        g_run_tests = true;
    }

    if (service::Terminal::instance()) {
        service::Terminal::set_fb_enabled(true);
    }

    // Register shell commands before the shell task starts
    service::Shell::init();

    // Kill all tasks and rebuild system from the task-definition table
    kernel::task::reboot_from_table();
}

extern "C" void panic(const char *msg) {
    cli();
    kernel::Logger::fatal("KERNEL PANIC: %s", msg);
    if (service::Terminal::instance()) {
        service::Terminal::set_fg(0xFF0000);
        service::Terminal::write("\nKERNEL PANIC: ");
        service::Terminal::write(msg);
        service::Terminal::set_fg(0xC0C0C0);
    }
    for (uint64_t _i = 0; _i < UINT64_MAX; ++_i) {
        arch::hlt();
    }
    __builtin_unreachable();
}

#if CONFIG_ARCH_X86_64
static void dump_regs(uint64_t *regs) {
    if (!regs)
        return;

    using L = kernel::Logger;

    L::raw_write("  RAX: ");
    L::print_hex(regs[0]);
    L::raw_write("  RBX: ");
    L::print_hex(regs[1]);
    L::raw_write("\n");
    L::raw_write("  RCX: ");
    L::print_hex(regs[2]);
    L::raw_write("  RDI: ");
    L::print_hex(regs[5]);
    L::raw_write("\n");
    L::raw_write("  RDX: ");
    L::print_hex(regs[3]);
    L::raw_write("  RSI: ");
    L::print_hex(regs[4]);
    L::raw_write("\n");
    L::raw_write("  RBP: ");
    L::print_hex(regs[6]);
    L::raw_write("  RSP: ");
    L::print_hex(reinterpret_cast<uint64_t>(&regs));
    L::raw_write("\n");
    L::raw_write("  R8:  ");
    L::print_hex(regs[7]);
    L::raw_write("  R9:  ");
    L::print_hex(regs[8]);
    L::raw_write("\n");
    L::raw_write("  R10: ");
    L::print_hex(regs[9]);
    L::raw_write("  R11: ");
    L::print_hex(regs[10]);
    L::raw_write("\n");
    L::raw_write("  R12: ");
    L::print_hex(regs[11]);
    L::raw_write("  R13: ");
    L::print_hex(regs[12]);
#if defined(CONFIG_ARCH_X86_64)
    L::raw_write("\n");
    L::raw_write("  R14: ");
    L::print_hex(regs[13]);
    L::raw_write("\n");
    L::raw_write("  R15: ");
    L::print_hex(regs[14]);
    L::raw_write("\n");
    L::raw_write("  RIP: ");
    L::print_hex(regs[17]);
    L::raw_write("  CS:  ");
    L::print_hex(regs[18]);
    L::raw_write("\n");
    L::raw_write("  RFL: ");
    L::print_hex(regs[19]);
    L::raw_write("\n");

    uint64_t cr0 = read_cr0();
    uint64_t cr2 = read_cr2();
    uint64_t cr3 = read_cr3();
    uint64_t cr4 = read_cr4();

    L::raw_write("  CR0: ");
    L::print_hex(cr0);
    L::raw_write("\n");
    L::raw_write("  CR2: ");
    L::print_hex(cr2);
    L::raw_write("  CR3: ");
    L::print_hex(cr3);
    L::raw_write("\n");
    L::raw_write("  CR4: ");
    L::print_hex(cr4);
    L::raw_write("\n");

    L::raw_write("  Stack trace:\n");
    // NOLINTNEXTLINE(performance-no-int-to-ptr)
    uint64_t *rbp = reinterpret_cast<uint64_t *>(regs[6]);
    // MP-4 (SMAP): only walk the frame-pointer chain when it is a KERNEL
    // frame.  A user task's RBP (regs[6]) points into user memory; dereferencing
    // it from kernel mode with AC=0 is a SMAP fault (recursive panic in the
    // #PF handler).  Kernel stacks live in the kslot window / HHDM (>= the
    // kstack window base); user RBP is far below it.
    if (rbp && reinterpret_cast<uint64_t>(rbp) >= 0xFFFF900000000000ULL) {
        for (int i = 0; i < 8 && rbp && rbp[1]; ++i) {
            L::raw_write("    [");
            L::print_dec(i);
            L::raw_write("] ");
            L::print_hex(rbp[1]);
            L::raw_write("\n");
            // NOLINTNEXTLINE(performance-no-int-to-ptr)
            rbp = reinterpret_cast<uint64_t *>(rbp[0]);
        }
    } else {
        L::raw_write("    (user-mode fault — kernel stack trace unavailable)\n");
    }
#elif defined(CONFIG_ARCH_AARCH64)
    L::raw_write("  ELR: ");
    L::print_hex(regs[31]);
    L::raw_write("  SPSR: ");
    L::print_hex(regs[32]);
    L::raw_write("\n  Stack trace:\n");
    uint64_t *fp = reinterpret_cast<uint64_t *>(regs[29]);
    // MP-4 (PAN): only walk a KERNEL frame chain; a user FP deref would be a
    // PAN fault on aarch64.  Kernel stacks live in the kslot/HHDM region.
    if (fp && reinterpret_cast<uint64_t>(fp) >= 0xFFFF000000000000ULL) {
        for (int i = 0; i < 8 && fp && fp[1]; ++i) {
            L::raw_write("    [");
            L::print_dec(i);
            L::raw_write("] ");
            L::print_hex(fp[1]);
            L::raw_write("\n");
            fp = reinterpret_cast<uint64_t *>(fp[0]);
        }
    } else {
        L::raw_write("    (user-mode fault — kernel stack trace unavailable)\n");
    }
#endif
}

#endif // CONFIG_ARCH_X86_64

#if CONFIG_STACK_OVERFLOW_HOOK
// v0.4.0 MP-6.2: weak default stack-overflow hook — production panics.  A
// strong override (test suites) may instead recover the faulting task; see
// the guard-page #PF handler in handle_interrupt_c.
__attribute__((weak)) void stack_overflow_hook(kernel::TaskControlBlock *task) {
    (void)task;
    panic("kernel stack overflow");
}
#endif // CONFIG_STACK_OVERFLOW_HOOK

static const char *exception_name(uint64_t vector) __attribute__((unused));
static const char *exception_name(uint64_t vector) {
#if defined(CONFIG_ARCH_X86_64)
    switch (vector) {
    case 0:
        return "Division by Zero";
    case 1:
        return "Debug";
    case 2:
        return "NMI";
    case 3:
        return "Breakpoint";
    case 4:
        return "Overflow";
    case 5:
        return "Bound Range";
    case 6:
        return "Invalid Opcode";
    case 7:
        return "Device Not Available";
    case 8:
        return "Double Fault";
    case 10:
        return "Invalid TSS";
    case 11:
        return "Segment Not Present";
    case 12:
        return "Stack Segment Fault";
    case 13:
        return "General Protection Fault";
    case 14:
        return "Page Fault";
    case 16:
        return "x87 FPU Error";
    case 17:
        return "Alignment Check";
    case 18:
        return "Machine Check";
    case 19:
        return "SIMD FP Exception";
    case 30:
        return "Security Exception";
    default:
        return "Reserved";
    }
#elif defined(CONFIG_ARCH_AARCH64)
    switch (vector) {
    case 0:
        return "Synchronous EL1t";
    case 1:
        return "IRQ EL1t";
    case 2:
        return "FIQ EL1t";
    case 3:
        return "SError EL1t";
    case 4:
        return "Synchronous EL1h";
    case 5:
        return "IRQ EL1h";
    case 6:
        return "FIQ EL1h";
    case 7:
        return "SError EL1h";
    case 8:
        return "Synchronous EL0 64";
    case 9:
        return "IRQ EL0 64";
    case 10:
        return "FIQ EL0 64";
    case 11:
        return "SError EL0 64";
    case 12:
        return "Synchronous EL0 32";
    case 13:
        return "IRQ EL0 32";
    case 14:
        return "FIQ EL0 32";
    case 15:
        return "SError EL0 32";
    default:
        return "Reserved";
    }
#else
    (void)vector;
    return "Unknown";
#endif
}

extern "C" uint64_t syscall_handler(uint64_t number, uint64_t arg0,
                                    uint64_t arg1, uint64_t arg2, uint64_t arg3,
                                    uint64_t *regs);

/// @brief Delivers a signal to a user task by setting up a signal frame on the
/// user stack.
///        Modifies regs to return to the user's registered signal handler (or
///        terminates the task if no handler is registered and the default
///        action is to terminate).
/// @return true if signal was delivered (handler will run), false if task was
/// terminated.
static bool deliver_signal_to_user(kernel::TaskControlBlock *task, uint64_t sig,
                                   uint64_t vector, uint64_t error_code,
                                   uint64_t rip, uint64_t *regs) {
    if (!task || !regs)
        return false;

    // SIGKILL is always fatal — cannot be caught or ignored
    if (kernel::signal_is_fatal(sig)) {
        kernel::Logger::error("Task %x: SIGKILL (fatal, no handler "
                              "allowed)",
                              task->id);
        // INV-5: terminate dequeues the task from the ready queue so it is
        // never left inrq=1 outside the physical queue (which corrupts the
        // ready-queue count_ and wedges the scheduler).
        kernel::Scheduler::terminate(*task,
                                     static_cast<uint64_t>(-static_cast<int64_t>(sig)));
        return false;
    }

#if defined(CONFIG_ARCH_X86_64)
    // If the task has a registered handler, invoke it
    if (task->has_signal_handler(sig)) {
        kernel::Logger::info(
            "Task %x: delivering signal %x to handler at %x", task->id, sig,
            reinterpret_cast<uint64_t>(task->get_signal_handler(sig)));

        uint64_t user_rsp = regs[20]; // current user RSP
        // Align stack: push SignalFrame
        user_rsp -= sizeof(kernel::SignalFrame);
        // Align to 16 bytes: (RSP + 8) % 16 == 0 at handler entry
        // After pushing SignalFrame, we adjust so that at handler entry:
        // RSP is 8 mod 16 (because call pushes return addr)
        user_rsp &= ~0xFULL;

        uint64_t frame_phys =
            kernel::VMM::virt_to_phys_in_pml4(user_rsp, task->page_table_);
        // NOLINTNEXTLINE(performance-no-int-to-ptr)
        auto *frame = reinterpret_cast<kernel::SignalFrame *>(
            arch::HHDM_OFFSET + frame_phys);
        if (!frame) {
            // If we cannot write the signal frame (bad stack),
            // terminate
            kernel::Logger::error("Task %x: cannot write signal frame, "
                                  "terminating",
                                  task->id);
            kernel::Scheduler::terminate(
                *task, static_cast<uint64_t>(-static_cast<int64_t>(sig)));
            return false;
        }

        *frame = kernel::SignalFrame{.sig = sig,
                                     .saved_rip = regs[17],
                                     .saved_rsp = regs[20],
                                     .saved_rflags = regs[19],
                                     .saved_cs = regs[18],
                                     .saved_ss = regs[21],
                                     .reserved = {0, 0}};

        // Modify return context to go to the signal handler
        regs[5] = sig; // RDI = signal number
        regs[17] = reinterpret_cast<uint64_t>(
            task->get_signal_handler(sig)); // RIP = handler
        regs[20] = user_rsp;                // RSP = adjusted user stack
        // Clear direction flag and RF
        regs[19] = arch::RFLAGS_DEFAULT; // RFLAGS with IF set

        return true;
    }

    // No handler registered — check default action
    auto action = kernel::default_signal_action(sig);
    if (action == kernel::SignalAction::IGNORE) {
        kernel::Logger::warn("Task %x: signal %x ignored (default)", task->id,
                             sig);
        return true; // ignored, resume execution
    }

    // Default is to terminate
    kernel::Logger::error("Task %x: unhandled signal %x "
                          "vector=%x rip=%x err=%x",
                          task->id, sig, vector, rip, error_code);
    if (vector == 14) {
        uint64_t cr2_val = read_cr2();
        kernel::Logger::error("  CR2=%x", cr2_val);
    }
    dump_regs(regs);
    // INV-5: terminate dequeues so the task is never left inrq=1 outside the
    // physical queue.
    kernel::Scheduler::terminate(
        *task, static_cast<uint64_t>(-static_cast<int64_t>(sig)));
    return false;
#elif defined(CONFIG_ARCH_AARCH64)
    (void)vector;
    (void)error_code;
    (void)rip;
    kernel::Logger::error("Task %x: signal %x (aarch64 stub, terminating)",
                          task->id, sig);
    task->state = kernel::TaskState::TERMINATED;
    task->exit_code = static_cast<uint64_t>(-static_cast<int64_t>(sig));
    return false;
#elif defined(CONFIG_ARCH_RISCV64)
    (void)vector;
    (void)error_code;
    (void)rip;
    kernel::Logger::error("Task %x: signal %x (riscv64 stub, terminating)",
                          task->id, sig);
    task->state = kernel::TaskState::TERMINATED;
    task->exit_code = static_cast<uint64_t>(-static_cast<int64_t>(sig));
    return false;
#endif
}

extern "C" void handle_interrupt_c(uint64_t vector, uint64_t error_code,
                                   uint64_t rip, uint64_t *regs,
                                   uint64_t entry_tsc) {
#if !CONFIG_IRQ_LATENCY_HISTOGRAM || !defined(CONFIG_ARCH_X86_64)
    (void)entry_tsc;
#endif
#if defined(CONFIG_ARCH_X86_64)
    // #NM (Device Not Available, vector 7) — lazy FPU/SSE context switch
    if (vector == 7) {
        auto *current = kernel::Scheduler::current_task();
        if (!current) {
            panic("#NM with no current task");
        }

        // Clear CR0.TS first so FNINIT/FXSAVE/FXRSTOR don't recursively #NM
        uint64_t cr0 = arch::read_cr0();
        cr0 &= ~(1ULL << 3);
        arch::write_cr0(cr0);

        // Save previous owner's FPU state
        auto *prev_fpu_owner =
            __atomic_load_n(&kernel::fpu_owner, __ATOMIC_ACQUIRE);
        if (prev_fpu_owner && prev_fpu_owner != current) {
            arch::fxsave(prev_fpu_owner->fpu_state);
        }

        // Restore or initialize for current task
        if (current->fpu_used) {
            arch::fxrstor(current->fpu_state);
        } else {
            arch::fninit();
            uint32_t default_mxcsr = 0x1F80;
            arch::ldmxcsr(default_mxcsr);
            current->fpu_used = true;
        }

        __atomic_store_n(&kernel::fpu_owner, current, __ATOMIC_RELEASE);
        return;
    }
#endif

#if defined(CONFIG_ARCH_X86_64)
    // Fault recovery for safe_copy_from_user / safe_copy_to_user.  While a copy
    // is in progress, ANY page fault — taken in user OR kernel mode — must be
    // redirected to the recovery label so the copy returns false instead of
    // panicking the kernel.  This is checked before the from_user signal path
    // because the copy itself runs in kernel mode (CS=0x8), so a fault there
    // would otherwise be misclassified as a fatal kernel exception.  BUGS.md#020
    // root cause #3: the user-app write path dereferences a user VA that is out
    // of range / unmapped; with recovery honored the syscall returns -EFAULT.
    if (kernel::g_user_access_recover_ip && regs) {
        regs[17] = kernel::g_user_access_recover_ip;
        kernel::g_user_access_recover_ip = 0;
        return;
    }

    if (vector < 32) {
        auto *t = kernel::Scheduler::current_task();
        uint64_t cs = regs ? regs[18] : 0;
        bool from_user =
            (cs == arch::SEG_USER_CODE || cs == arch::SEG_USER_DATA);
        if (from_user && t) {
            auto mapping = kernel::exception_to_signal(vector);
            uint64_t sig = static_cast<uint64_t>(mapping.signal);
            kernel::Logger::warn("Task %x: exception vector=%x (%s) "
                                 "→ signal %x",
                                 t->id, vector, mapping.name, sig);

            if (vector == 14) {
                kernel::Logger::error("  CR2=%x", read_cr2());
            }

            bool was_delivered =
                deliver_signal_to_user(t, sig, vector, error_code, rip, regs);
            if (!was_delivered && t->state == kernel::TaskState::TERMINATED) {
                // If task was terminated, reschedule on return
                kernel::Scheduler::reschedule();
            }
            return;
        }

        // ---- Guard-page check (kernel stack overflow) ----
        if (vector == 14 && t && t->kstack_slot_va_) {
            uint64_t cr2 = read_cr2();
            if (cr2 >= t->kstack_slot_va_ &&
                cr2 < t->kstack_slot_va_ + arch::PAGE_SIZE) {
                kernel::Logger::fatal(
                    "STACK OVERFLOW: task '%s' (ID=%u) overflowed "
                    "kernel stack (CR2=0x%lx slot_va=0x%lx top=0x%lx)",
                    t->name, t->id, cr2,
                    t->kstack_slot_va_,
                    t->kernel_stack_top);
                dump_regs(regs);
#if CONFIG_STACK_OVERFLOW_HOOK
                // v0.4.0 MP-6.2: invoke the overflow hook.  The weak default
                // panics internally — production behaviour is unchanged.  A
                // strong override (test suite) may recover the faulting task
                // (rewrite the iret frame + TERMINATE), in which case we
                // return instead of panicking (mirrors the
                // g_user_access_recover_ip recovery).  The weak symbol always
                // resolves (to the default or the override), so no null
                // check is needed.
                stack_overflow_hook(t);
                return;
#else
                panic("kernel stack overflow");
#endif
            }
        }

        kernel::Logger::fatal("CPU EXCEPTION: %s (vector=%x err=%x rip=%x)",
                              exception_name(vector), vector, error_code, rip);
        dump_regs(regs);

        // ---- Full diagnostic dump before panic ----
        kernel::debug::dump_all_tasks();
        kernel::debug::dump_scheduler_info();

        if (t) {
            kernel::Logger::raw_write("  Task: id=");
            kernel::Logger::print_hex(t->id);
            kernel::Logger::raw_write(" prio=");
            kernel::Logger::print_dec(t->priority);
            kernel::Logger::raw_write(" state=");
            kernel::Logger::print_hex(static_cast<uint64_t>(t->state));
            kernel::Logger::raw_write("\n");
        }

        panic("CPU EXCEPTION");
        return;
    }
#elif defined(CONFIG_ARCH_AARCH64)
    if (vector < 32) {
        auto *t = kernel::Scheduler::current_task();
        (void)t;
        if (t && t->is_user_) {
            auto mapping = kernel::exception_to_signal(vector);
            uint64_t sig = static_cast<uint64_t>(mapping.signal);
            kernel::Logger::warn("Task %x: exception vector=%x (%s)", t->id,
                                 vector, mapping.name);
            bool was_delivered =
                deliver_signal_to_user(t, sig, vector, error_code, rip, regs);
            if (!was_delivered && t->state == kernel::TaskState::TERMINATED) {
                kernel::Scheduler::reschedule();
            }
            return;
        }
        kernel::Logger::fatal("CPU EXCEPTION: %s (vector=%x err=%x elr=%x)",
                              exception_name(vector), vector, error_code, rip);
        kernel::debug::dump_all_tasks();
        kernel::debug::dump_scheduler_info();
        kernel::debug::dump_cpu_regs();
        panic("CPU EXCEPTION");
    }
#endif

    if (vector == 0x80) {
        regs[0] =
            syscall_handler(regs[0], regs[1], regs[2], regs[3], regs[4], regs);
        auto *task = kernel::Scheduler::current_task();

        // After syscall, check for pending signals on the current task
        if (task && task->pending_signals && task->is_user_) {
            // Find the highest-priority pending signal
            uint64_t sig = __builtin_ctzll(task->pending_signals);
            if (sig < 32) {
                kernel::Logger::debug("Task %x: pending signal %x "
                                      "after syscall",
                                      task->id, sig);
                deliver_signal_to_user(task, sig, 0, 0, regs[17], regs);
                task->pending_signals &= ~(1ULL << sig);
            }
        }

        if (task && (task->state == kernel::TaskState::TERMINATED ||
                     task->state == kernel::TaskState::BLOCKED)) {
            kernel::Scheduler::reschedule();
        }
        return;
    }

#if defined(CONFIG_ARCH_X86_64) && CONFIG_CAP_MAX_IRQ
    // User-space IRQ delivery (issue #2): an armed IrqCap slot consumes the
    // vector — EOI + pending-record + waiter wake — and returns early so the
    // tail EOI below does not double-fire.  Unarmed vectors fall through to
    // the generic handler.
    if (kernel::IrqDelivery::isr_entry(static_cast<uint8_t>(vector)))
        return;
#endif

#if CONFIG_THREADED_IRQS
    // Threaded IRQ dispatch: if this vector has an IrqThread, let the
    // handler task process it.  The ISR entry does ack + Notify and returns.
    {
        auto *irqt = kernel::IrqThread::for_vector(static_cast<uint8_t>(vector));
        if (irqt) {
            kernel::IrqThread::isr_entry(static_cast<uint8_t>(vector),
                                          error_code, rip);
            return;
        }
    }
#endif

    arch::IDT::handle_interrupt(vector, error_code, rip);

#if defined(CONFIG_ARCH_X86_64)
    // Send APIC EOI for every interrupt — the APIC requires EOI
    // for any vector it delivered, regardless of the source.
    if (arch::APIC::is_enabled()) {
        arch::APIC::eoi();
    }

    // Legacy PIC EOI for vectors routed through the PIC (IRQ0‑15 → 32‑47).
    // When the APIC is active, these interrupts still reach the PIC as well
    // (both controllers receive the same physical IRQ signal), so the PIC
    // must be acknowledged to keep its state machine consistent.
    if (vector >= 32 && vector < 48) {
        outb(arch::PIC1_CMD, 0x20);
        if (vector >= 40)
            outb(arch::PIC2_CMD, 0x20);
    }

    // Record IRQ latency histogram for hardware IRQs (vectors 32-47).
#if CONFIG_IRQ_LATENCY_HISTOGRAM
    if (vector >= 32 && vector < 48) {
        kernel::IrqLatencyHistogram::record(entry_tsc);
    }
#endif
#endif
}

extern "C" uint64_t syscall_handler(uint64_t number, uint64_t arg0,
                                    uint64_t arg1, uint64_t arg2, uint64_t arg3,
                                    uint64_t *regs) {
    return kernel::Syscall::handle(number, arg0, arg1, arg2, arg3, regs);
}

// ── Boot-time clock capture ──────────────────────────────────────────────────
// ── Epoch-to-date conversion (no libc) ──────────────────────────────────────
static const uint16_t s_days_in_mon[12] = {31, 28, 31, 30, 31, 30,
                                           31, 31, 30, 31, 30, 31};

static bool is_leap(uint16_t y) {
    return (y % 4 == 0) && (y % 100 != 0 || y % 400 == 0);
}

/// @brief Convert wall-clock nanoseconds since epoch to a human-readable
///        date-time string: "YYYY-MM-DD hh:mm:ss:mmm".
/// @param buf    Output buffer (must be >= 24 bytes).
/// @param size   Buffer size.
/// @param wall_ns  Nanoseconds since 1970-01-01 00:00:00 UTC.
void format_datetime(char *buf, size_t size, uint64_t wall_ns) {
    if (!buf || size < 24)
        return;
    uint64_t total_sec = wall_ns / 1000000000ULL;
    uint32_t ms = static_cast<uint32_t>((wall_ns % 1000000000ULL) / 1000000ULL);

    // Break total_sec into date/time components
    // Days since epoch
    uint64_t d = total_sec / 86400ULL;
    uint64_t remaining_sec = total_sec % 86400ULL;
    uint16_t hh = static_cast<uint16_t>(remaining_sec / 3600ULL);
    uint16_t mm = static_cast<uint16_t>((remaining_sec % 3600ULL) / 60ULL);
    uint16_t ss = static_cast<uint16_t>(remaining_sec % 60ULL);

    // Year
    uint16_t year = 1970;
    while (true) {
        uint16_t days_this = is_leap(year) ? 366 : 365;
        if (d < days_this)
            break;
        d -= days_this;
        ++year;
    }

    // Month
    uint8_t mon = 0;
    for (; mon < 12; ++mon) {
        uint16_t dim = s_days_in_mon[mon];
        if (mon == 1 && is_leap(year))
            dim = 29;
        if (d < dim)
            break;
        d -= dim;
    }
    uint16_t day = static_cast<uint16_t>(d + 1); // 1-based

    // Format
    size_t pos = 0;
    auto put = [&](char c) {
        if (pos < size - 1)
            buf[pos++] = c;
    };
    auto putn = [&](uint64_t val, uint8_t digits) {
        char tmp[20];
        int ti = 0;
        if (val == 0)
            tmp[ti++] = '0';
        else {
            while (val > 0 && ti < 19) {
                tmp[ti++] = static_cast<char>('0' + (val % 10));
                val /= 10;
            }
        }
        while (ti < digits)
            tmp[ti++] = '0';
        for (int j = ti - 1; j >= 0; --j)
            put(tmp[j]);
    };
    putn(year, 4);
    put('-');
    putn(mon + 1, 2);
    put('-');
    putn(day, 2);
    put(' ');
    putn(hh, 2);
    put(':');
    putn(mm, 2);
    put(':');
    putn(ss, 2);
    put(':');
    putn(ms, 3);
    buf[pos] = '\0';
}

uint8_t kernel_stack[16_KiB] __attribute__((section(".boot_stack")));
