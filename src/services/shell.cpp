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

#include <services/shell.hpp>
#include <services/terminal/terminal.hpp>
#include <services/terminal/framebuffer.hpp>
#include <services/program.hpp>
#include <kernel/task/scheduler.hpp>
#include <kernel/task/task.hpp>
#include <kernel/core/global_state.hpp>
#include <kernel/elf/elf_loader.hpp>
#include <kernel/memory/pmm.hpp>
#include <kernel/memory/vmm.hpp>
#include <kernel/arch/timer.hpp>
#include <kernel/arch/rtc.hpp>
#include <kernel/kernel.hpp>
#include <kernel/arch/io.hpp>
#include <kernel/arch/irq_guard.hpp>
#include <kernel/sync/spinlock.hpp>
#include <kernel/sync/spinlock_guard.hpp>
#include <kernel/elf/elf.hpp>
#include <kernel/vfs/vfs.hpp>
#include <initrd/initrd.hpp>
#include <version.hpp>
#include <kernel/driver/driver.hpp>
#include <kernel/arch/keyboard.hpp>
#include <kernel/syscall/syscall_helpers.hpp>
#include <kernel/log/ring_buffer.hpp>
#include <kernel/log/dmesg.hpp>
#include <kernel/net/net.hpp>
#include <kernel/driver/virtio_net.hpp>
#include <kernel/arch/pci.hpp>
#include <test.hpp>
#include <string.hpp>
#include <constants.hpp>

namespace {

static constexpr uint32_t COLOR_DEFAULT  = 0xC0C0C0;
static constexpr uint32_t COLOR_ERROR    = 0xFF4444;
static constexpr uint32_t COLOR_WARN     = 0xCC3333;
static constexpr uint32_t COLOR_SUCCESS  = 0x00AA00;
static constexpr uint32_t COLOR_DIR      = 0x00AAFF;
static constexpr uint32_t COLOR_GREEN    = 0x00FF00;
static constexpr uint32_t COLOR_RED      = 0xFF0000;
static constexpr uint32_t COLOR_YELLOW   = 0xFFFF00;

}

namespace service {

static void shell_error(const char* cmd, const char* msg) {
    Terminal::set_fg(COLOR_ERROR);
    Terminal::write(cmd); Terminal::write(": "); Terminal::write(msg);
    Terminal::putchar('\n');
    Terminal::set_fg(COLOR_DEFAULT);
}

static void shell_error_path(const char* cmd, const char* path, const char* msg) {
    Terminal::set_fg(COLOR_ERROR);
    Terminal::write(cmd); Terminal::write(": "); Terminal::write(msg);
    Terminal::write(": "); Terminal::write(path);
    Terminal::putchar('\n');
    Terminal::set_fg(COLOR_DEFAULT);
}

static void shell_vfs_error(const char* cmd, kernel::errors::VfsError err) {
    shell_error(cmd, kernel::errors::error_string(err));
}

static void background_task_wrapper() {
    auto* task = kernel::Scheduler::current_task();
    auto* entry = task ? reinterpret_cast<void (*)()>(task->user_data) : nullptr;
    if (entry) entry();
    if (task) task->state = kernel::TaskState::TERMINATED;
    while (true) arch::hlt();
}

Shell::Command Shell::commands_[MAX_COMMANDS] = {};
size_t Shell::num_commands_ = 0;
bool Shell::initialized_ = false;
char Shell::work_dir_[BUF_SIZE] = {};
char Shell::env_[Shell::MAX_ENV][Shell::BUF_SIZE] = {};
size_t Shell::env_count_ = 0;
int Shell::last_exit_code_ = 0;

Shell::AliasEntry Shell::aliases_[Shell::MAX_ALIASES] = {};
size_t Shell::alias_count_ = 0;

Shell::HistoryEntry Shell::history_[Shell::MAX_HISTORY] = {};
size_t Shell::history_count_ = 0;
size_t Shell::history_head_ = 0;

char Shell::dir_stack_[Shell::MAX_DIR_STACK][Shell::BUF_SIZE] = {};
size_t Shell::dir_stack_count_ = 0;

int Shell::shell_options_ = 0;
int Shell::positional_argc_ = 0;
char* Shell::positional_argv_[32] = {};

int Shell::umask_ = 0022;

Shell::TrapEntry Shell::traps_[32] = {};

void Shell::init() {
    if (initialized_) return;

    register_command("help",    "Show available commands",          cmd_help);
    register_command("clear",   "Clear terminal screen",            cmd_clear);
    register_command("echo",    "Echo text to terminal",            cmd_echo);
    register_command("uptime",  "Show system uptime",               cmd_uptime);
    register_command("tasks",   "List running tasks",               cmd_tasks);
    register_command("meminfo", "Show memory usage",                cmd_meminfo);
    register_command("reboot",  "Reboot the system",                cmd_reboot);
    register_command("run",     "Run a registered program",         cmd_run);
    register_command("version", "Show kernel version info",         cmd_version);
    register_command("jobs",    "List background tasks",            cmd_jobs);
    register_command("modprobe","Load/init a kernel driver",        cmd_modprobe);
    register_command("modlist", "List available kernel drivers",    cmd_modlist);
    register_command("listprog","List registered programs",         cmd_listprog);
    register_command("cd",      "Change working directory",          cmd_cd);
    register_command("export",  "Set environment variable",          cmd_export);
    register_command("runelf",  "Run userspace ELF from initrd",     cmd_runelf);
    register_command("load",    "Background-load an ELF file (returns immediately)", cmd_load);
    register_command("cancel-load", "Cancel the background ELF load", cmd_cancel_load);
    register_command("exit",    "Shut down the system",              cmd_exit);
    register_command("shutdown","Shut down the system",              cmd_exit);
    register_command("selftest","Run kernel self-tests",             cmd_selftest);
    register_command("pwd",     "Print working directory",           cmd_pwd);
    register_command("env",     "Print environment variables",       cmd_env);
    register_command("sleep",   "Sleep for N seconds",               cmd_sleep);
    register_command("mkdir",   "Create a directory",                cmd_mkdir);
    register_command("rm",      "Remove a file",                     cmd_rm);
    register_command("rmdir",   "Remove an empty directory",         cmd_rmdir);
    register_command("which",   "Locate a command",                  cmd_which);
    register_command("locate",  "Locate a command",                  cmd_which);
    register_command("alias",   "Define or list aliases",            cmd_alias);
    register_command("unalias", "Remove an alias",                   cmd_unalias);
    register_command("history", "Show command history",              cmd_history);
    register_command("type",    "Show command type",                 cmd_type);
    register_command("source",  "Execute a script file",             cmd_source);
    register_command(".",       "Execute a script file (POSIX)",     cmd_source);
    register_command("set",     "Set/unset shell options",           cmd_set);
    register_command("read",    "Read a line into variable(s)",      cmd_read);
    register_command("printf",  "Formatted output",                  cmd_printf);
    register_command("test",    "Evaluate conditional expression",   cmd_test);
    register_command("[",       "Evaluate conditional expression",   cmd_test);
    register_command("shift",   "Shift positional parameters",       cmd_shift);
    register_command("trap",    "Trap signals",                      cmd_trap);
    register_command("wait",    "Wait for background jobs",          cmd_wait);
    register_command("fg",      "Bring job to foreground",           cmd_fg);
    register_command("bg",      "Send job to background",            cmd_bg);
    register_command("disown",  "Remove job from table",             cmd_disown);
    register_command("ulimit",  "Resource limits",                   cmd_ulimit);
    register_command("umask",   "File creation mask",                cmd_umask);
    register_command("times",   "Process times",                     cmd_times);
    register_command("logout",  "Exit login shell",                  cmd_logout);
    register_command("dirs",    "Directory stack",                   cmd_dirs);
    register_command("pushd",   "Push directory to stack",           cmd_pushd);
    register_command("popd",    "Pop directory from stack",          cmd_popd);
    register_command("ls",      "List directory contents",           cmd_ls);
    register_command("ifconfig","Show/configure network interface",  cmd_ifconfig);
    register_command("ping",    "Send ICMP echo requests",           cmd_ping);
    register_command("less",    "Page through a text file",          cmd_less);
    register_command("cat",     "Print file contents to terminal",   cmd_cat);
    register_command("touch",   "Create an empty file",              cmd_touch);
    register_command("dmesg",   "Print kernel log buffer",           cmd_dmesg);
    register_command("lspci",   "List PCI devices",                  cmd_lspci);

    work_dir_[0] = '/';
    work_dir_[1] = '\0';
    env_count_ = 0;
    last_exit_code_ = 0;

    initialized_ = true;
}

// NOLINTBEGIN(bugprone-easily-swappable-parameters)
void Shell::register_command(const char* name, const char* help, CommandFunc func) {
    if (num_commands_ >= MAX_COMMANDS) return;
    commands_[num_commands_].name = name;
    commands_[num_commands_].help = help;
    commands_[num_commands_].func = func;
    ++num_commands_;
}
// NOLINTEND(bugprone-easily-swappable-parameters)

static int str_cmp(const char* a, const char* b) {
    while (*a && *b && *a == *b) { ++a; ++b; }
    return static_cast<unsigned char>(*a) - static_cast<unsigned char>(*b);
}

#ifndef __clang__
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wanalyzer-possible-null-dereference"
#pragma GCC diagnostic ignored "-Wanalyzer-use-of-uninitialized-value"
#endif
int Shell::parse_and_exec(const char* line) {
    while (*line == ' ') ++line;
    if (!*line) return 0;

    size_t len = 0;
    const char* p = line;
    while (*p) { ++len; ++p; }

    char* buf = new char[len + 1];
    if (!buf) return 1;
    memcpy(buf, line, len + 1);

    const char* argv[MAX_ARGS];
    int argc = 0;
    char* ptr = buf;

    while (*ptr && static_cast<size_t>(argc) < MAX_ARGS) {
        while (*ptr == ' ') ++ptr;
        if (!*ptr) break;
        argv[argc++] = ptr;
        while (*ptr && *ptr != ' ') ++ptr;
        if (*ptr) { *ptr++ = '\0'; }
    }

    if (argc == 0) {
        delete[] buf;
        return 0;
    }

    // Scan for redirect tokens
    const char* redirect_file = nullptr;
    int redirect_pos = -1;
    bool redirect_stdout = false;

    for (int i = 0; i < argc; ++i) {
        if (str_cmp(argv[i], ">") == 0 && i + 1 < argc) {
            redirect_file = argv[i + 1];
            redirect_pos = i;
            redirect_stdout = true;
            break;
        }
    }

    // Execute with optional output capture

    // Determine command name and effective argc (excluding redirect tokens)
    const char* cmd_name = argv[0];
    int effective_argc = (redirect_pos >= 0) ? redirect_pos : argc;

    // Alias expansion: if argv[0] is an alias, expand and re-parse
    for (size_t ai = 0; ai < alias_count_; ++ai) {
        if (str_cmp(cmd_name, aliases_[ai].name) == 0) {
            // Reconstruct command: alias value + remaining args
            char expanded[BUF_SIZE * 2];
            size_t pos = 0;
            for (const char* v = aliases_[ai].value; *v && pos < sizeof(expanded) - 2; ++v)
                expanded[pos++] = *v;
            for (int ai_arg = 1; ai_arg < effective_argc; ++ai_arg) {
                if (pos >= sizeof(expanded) - 2) break;
                expanded[pos++] = ' ';
                for (const char* a = argv[ai_arg]; *a && pos < sizeof(expanded) - 2; ++a)
                    expanded[pos++] = *a;
            }
            expanded[pos] = '\0';
            delete[] buf;
            // Ensure history is added for the expanded command? Just recurse
            return parse_and_exec(expanded);
        }
    }

    // Record history
    if (history_count_ < Shell::MAX_HISTORY) {
        size_t hi = history_count_;
        size_t hp = 0;
        for (const char* hl = line; *hl && hp < BUF_SIZE - 1; ++hl) history_[hi].cmd[hp++] = *hl;
        history_[hi].cmd[hp] = '\0';
        history_[hi].used = true;
        ++history_count_;
    } else {
        size_t hi = history_head_;
        size_t hp = 0;
        for (const char* hl = line; *hl && hp < BUF_SIZE - 1; ++hl) history_[hi].cmd[hp++] = *hl;
        history_[hi].cmd[hp] = '\0';
        history_head_ = (history_head_ + 1) % Shell::MAX_HISTORY;
    }

    for (size_t i = 0; i < num_commands_; ++i) {
        if (str_cmp(cmd_name, commands_[i].name) == 0) {

            if (redirect_stdout && redirect_file) {
                char capture_buf[4096];
                Terminal::capture_begin(capture_buf, sizeof(capture_buf));

                commands_[i].func(effective_argc, argv);

                Terminal::capture_end();

                // Write captured output to the file
                if (capture_buf[0]) {
                    auto* task = kernel::Scheduler::current_task();
                    if (task) {
                        int fd = kernel::syscall_path_open(
                            redirect_file,
                            kernel::vfs::O_WRONLY | kernel::vfs::O_CREAT);
                        if (fd >= 0) {
                            size_t slen = 0;
                            while (capture_buf[slen]) ++slen;
                            auto* f = task->fd_table.get(fd);
                            if (f && f->vnode && f->vnode->ops->write) {
                                f->vnode->ops->write(*f->vnode,
                                    reinterpret_cast<const uint8_t*>(capture_buf),
                                    slen, 0);
                            }
                            task->fd_table.free(fd);
                        }
                    }
                }
            } else {
                commands_[i].func(effective_argc, argv);
            }

            delete[] buf;
            return 0;
        }
    }

    Terminal::set_fg(COLOR_ERROR);
    Terminal::write("Unbekannter Befehl: ");
    Terminal::write(argv[0]);
    Terminal::write("\n");
    Terminal::set_fg(COLOR_DEFAULT);
    Terminal::write("Verfuegbar: help\n");

    delete[] buf;
    return 1;
}
#ifndef __clang__
#pragma GCC diagnostic pop
#endif

void Shell::execute(const char* cmd) {
    if (!initialized_) init();
    parse_and_exec(cmd);
}

static void update_status_bar();

static bool readline(char* buf, size_t max_len) {
    size_t pos = 0;
    static uint64_t last_blink = 0;
    static bool cursor_on = false;

    auto redraw_cursor = [&](bool on) {
        cursor_on = on;
        Terminal::set_cursor_visible(on);
    };

    for (;;) {
        uint64_t now = arch::Timer::ticks();
        if (now - last_blink >= 500) {
            last_blink = now;
            redraw_cursor(!cursor_on);
            update_status_bar();
        }

        char c = 0;
        bool got_char = false;

#if defined(CONFIG_ARCH_X86_64)
        if (arch::inb(arch::COM1_LSR) & 1) {
            c = static_cast<char>(arch::inb(arch::COM1));
            got_char = true;
        }
#endif

        if (!got_char) {
            got_char = arch::Keyboard::getchar(c);
        }

        if (!got_char) {
            arch::pause();
            continue;
        }

        redraw_cursor(true);

        if (c == '\r') c = '\n';

        if (c == '\n') {
            redraw_cursor(false);
            Terminal::putchar('\n');
            buf[pos] = '\0';
            return true;
        }
        if (c == '\b' || c == 0x7F) {
            if (pos > 0) {
                --pos;
                Terminal::putchar('\b');
            }
            continue;
        }
        if (pos < max_len - 1) {
            buf[pos++] = c;
            Terminal::putchar(c);
        }
    }
}

static void append_two_digit(char*& p, uint32_t n) {
    *p++ = static_cast<char>('0' + (n / 10) % 10);
    *p++ = static_cast<char>('0' + n % 10);
}

static void append_four_digit(char*& p, uint32_t n) {
    *p++ = static_cast<char>('0' + (n / 1000) % 10);
    *p++ = static_cast<char>('0' + (n / 100) % 10);
    append_two_digit(p, n % 100);
}

static void append_size(char*& p, uint64_t bytes) {
    uint64_t mb = bytes / (static_cast<uint64_t>(1024) * 1024);
    if (mb >= 100) *p++ = static_cast<char>('0' + (mb / 100) % 10);
    if (mb >= 10) *p++ = static_cast<char>('0' + (mb / 10) % 10);
    *p++ = static_cast<char>('0' + mb % 10);
}

static void update_status_bar() {
    if (!Terminal::instance()) return;
    if (!Framebuffer::available()) return;

    char left[64];
    char* lp = left;
    for (const char* s = "NexIOS RTOS "; *s; ++s) *lp++ = *s;
    for (const char* s = kernel::Version::string(); *s; ++s) *lp++ = *s;
#ifdef CONFIG_DEBUG
    for (const char* s = " [D]"; *s; ++s) *lp++ = *s;
#else
    for (const char* s = " [R]"; *s; ++s) *lp++ = *s;
#endif
    *lp = '\0';

    char right[96];
    char* rp = right;

    arch::tm tm;
    arch::RTC::read_time(&tm);

    uint16_t year = static_cast<uint16_t>(tm.tm_year + 1900);
    append_four_digit(rp, year);
    *rp++ = '-';
    append_two_digit(rp, static_cast<uint32_t>(tm.tm_mon + 1));
    *rp++ = '-';
    append_two_digit(rp, static_cast<uint32_t>(tm.tm_mday));
    *rp++ = ' ';
    append_two_digit(rp, static_cast<uint32_t>(tm.tm_hour));
    *rp++ = ':';
    append_two_digit(rp, static_cast<uint32_t>(tm.tm_min));
    *rp++ = ':';
    append_two_digit(rp, static_cast<uint32_t>(tm.tm_sec));

    for (const char* s = " | Up "; *s; ++s) *rp++ = *s;

    uint64_t total_sec = arch::Timer::ticks() / 1000;
    uint32_t uh = static_cast<uint32_t>(total_sec / 3600);
    uint32_t um = static_cast<uint32_t>((total_sec % 3600) / 60);
    uint32_t us = static_cast<uint32_t>(total_sec % 60);
    append_two_digit(rp, uh);
    *rp++ = ':';
    append_two_digit(rp, um);
    *rp++ = ':';
    append_two_digit(rp, us);

    uint64_t total_mem = kernel::PMM::total_memory();
    uint64_t used_mem = total_mem - kernel::PMM::free_memory();
    for (const char* s = " | Mem: "; *s; ++s) *rp++ = *s;
    append_size(rp, used_mem);
    *rp++ = '/';
    append_size(rp, total_mem);
    for (const char* s = "M"; *s; ++s) *rp++ = *s;

    for (const char* s = " | T:"; *s; ++s) *rp++ = *s;

    uint64_t tc = kernel::Scheduler::task_count();
    if (tc >= 100) *rp++ = static_cast<char>('0' + (tc / 100) % 10);
    if (tc >= 10) *rp++ = static_cast<char>('0' + (tc / 10) % 10);
    *rp++ = static_cast<char>('0' + tc % 10);
    *rp = '\0';

    Terminal::draw_status_bar(left, right);
}

void Shell::shell_task_main() {
    if (!initialized_) init();

    // Enable dmesg recording now that the shell is interactive
    // (release mode suppresses boot/test entries for a clean console).
    kernel::log::DmesgService::instance().set_suppressed(false);

    Terminal::clear();
    Terminal::write("\n");
    Terminal::set_fg(COLOR_GREEN);
    Terminal::write("NexIOS RTOS ");
    Terminal::write(kernel::Version::string());
    Terminal::write("\n");
    Terminal::set_fg(COLOR_DEFAULT);
    Terminal::write("Type 'help' for available commands\n\n");

    char line[BUF_SIZE];

    while (true) {
        update_status_bar();

        auto* task = kernel::Scheduler::current_task();
        const char* cwd = task ? task->cwd : "/";

        // Exit status indicator: green checkmark for 0, red X for non-zero
        if (last_exit_code_ == 0) {
            Terminal::set_fg(COLOR_SUCCESS);
            Terminal::write("✓ ");
        } else {
            Terminal::set_fg(COLOR_WARN);
            Terminal::write("✗ ");
        }

        // Current directory in blue
        Terminal::set_fg(COLOR_DIR);
        Terminal::write(cwd);
        Terminal::write(" ");
        Terminal::set_fg(COLOR_DEFAULT);
        Terminal::write("$ ");

        arch::Keyboard::flush();
        if (readline(line, BUF_SIZE)) {
            last_exit_code_ = parse_and_exec(line);
            update_status_bar();
        }
    }
}

void Shell::cmd_help(int, const char**) {
    Terminal::write("\nVerfuegbare Befehle:\n");
    for (size_t i = 0; i < num_commands_; ++i) {
        Terminal::write("  ");
        Terminal::set_fg(COLOR_DIR);
        Terminal::write(commands_[i].name);
        Terminal::set_fg(COLOR_DEFAULT);

        size_t name_len = 0;
        const char* n = commands_[i].name;
        while (*n++) ++name_len;
        for (size_t p = name_len; p < 14; ++p) Terminal::putchar(' ');
        Terminal::write(commands_[i].help);
        Terminal::putchar('\n');
    }
    Terminal::putchar('\n');
}

void Shell::cmd_clear(int, const char**) {
    Terminal::clear();
}

void Shell::cmd_echo(int argc, const char** argv) {
    for (int i = 1; i < argc; ++i) {
        if (i > 1) Terminal::putchar(' ');
        Terminal::write(argv[i]);
    }
    Terminal::putchar('\n');
}

void Shell::cmd_uptime(int, const char**) {
    uint64_t ticks = arch::Timer::ticks();
    uint64_t secs = ticks / 1000;
    uint64_t mins = secs / 60;
    uint64_t hours = mins / 60;
    mins %= 60;
    secs %= 60;

    auto print2 = [](uint64_t n) {
        Terminal::putchar(static_cast<char>('0' + n / 10));
        Terminal::putchar(static_cast<char>('0' + n % 10));
    };

    Terminal::write("Uptime: ");
    print2(hours);
    Terminal::putchar(':');
    print2(mins);
    Terminal::putchar(':');
    print2(secs);
    Terminal::putchar('\n');
}

static void print_uint(uint64_t n) {
    if (n == 0) { Terminal::putchar('0'); return; }
    char buf[21];
    int pos = 0;
    while (n > 0 && pos < 20) {
        buf[pos++] = static_cast<char>('0' + (n % 10));
        n /= 10;
    }
    while (pos > 0) Terminal::putchar(buf[--pos]);
}

/// @brief Compute a right-alignment padding count that never underflows.
/// @param width Total field width.
/// @param value  Value to be printed in that field.
/// @return Number of leading spaces (clamped to [1, width]).
/// @note A naive `while (v >= 10) { --pad; v /= 10; }` underflows when a
///       field holds a value with 10+ digits (e.g. the idle task's priority
///       0xFFFFFFFF), making the following `for` loop print 2^64 spaces.
static uint64_t right_pad(uint64_t width, uint64_t value) {
    uint64_t digits = 1;
    uint64_t v = value;
    while (v >= 10 && digits < width) {
        v /= 10;
        ++digits;
    }
    if (digits > width)
        return 0;
    return width - digits;
}

static const char *state_name(kernel::TaskState s) {
    using namespace kernel;
    switch (s) {
        case TaskState::READY:     return "READY    ";
        case TaskState::RUNNING:   return "RUNNING  ";
        case TaskState::BLOCKED:   return "BLOCKED  ";
        case TaskState::WAITING:   return "WAITING  ";
        case TaskState::TERMINATED:return "TERM     ";
        case TaskState::REAPED:    return "REAPED   ";
        default:                   return "?        ";
    }
}

void Shell::cmd_tasks(int, const char**) {
    Terminal::write("ID  NAME             STATE      PRIO  PERIOD   MEM_PG STACK_KiB CPU_TIME  CURRENT\n");
    Terminal::write("--- ---------------- ---------- ----- ------- ------- --------- --------- -------\n");

    auto *cur = kernel::Scheduler::current_task();
    auto count = kernel::Scheduler::task_count();
    // Collect all valid tasks, then print sorted by task ID (task_at() returns
    // priority-then-insertion order, which is not stable/intuitive).
    kernel::TaskControlBlock *sorted[CONFIG_MAX_TASKS];
    size_t n = 0;
    for (uint64_t i = 0; i < count && n < CONFIG_MAX_TASKS; ++i) {
        auto *t = kernel::Scheduler::task_at(i);
        if (!t || t->magic != kernel::TaskControlBlock::TCB_MAGIC)
            continue;
        sorted[n++] = t;
    }
    // Insertion sort by ID (small N; stable).
    for (size_t i = 1; i < n; ++i) {
        kernel::TaskControlBlock *key = sorted[i];
        size_t j = i;
        while (j > 0 && sorted[j - 1]->id > key->id) {
            sorted[j] = sorted[j - 1];
            --j;
        }
        sorted[j] = key;
    }
    for (size_t i = 0; i < n; ++i) {
        auto *t = sorted[i];

        // ID (right-aligned, 3 chars)
        if (t->id < 100) Terminal::putchar(' ');
        if (t->id < 10)  Terminal::putchar(' ');
        print_uint(t->id);
        Terminal::putchar(' ');

        // Name (left-aligned, 16 chars)
        for (int c = 0; c < 15; ++c) {
            if (t->name[c])
                Terminal::putchar(t->name[c]);
            else
                Terminal::putchar(' ');
        }
        Terminal::putchar(' ');

        // State (9 chars)
        Terminal::write(state_name(t->state));
        Terminal::putchar(' ');

        // Priority (right-aligned, 5 chars)
        for (uint64_t p = 1; p <= right_pad(5, t->priority); ++p)
            Terminal::putchar(' ');
        print_uint(t->priority);
        Terminal::putchar(' ');

        // Period (right-aligned, 7 chars) — aperiodic tasks (NO_PERIOD)
        // have no period, display '-'.
        if (t->period_ticks == kernel::TaskControlBlock::NO_PERIOD) {
            for (int p = 0; p < 6; ++p) Terminal::putchar(' ');
            Terminal::putchar('-');
        } else {
            for (uint64_t p = 1; p <= right_pad(7, t->period_ticks); ++p)
                Terminal::putchar(' ');
            print_uint(t->period_ticks);
        }
        Terminal::putchar(' ');

        // Memory used pages (right-aligned, 7 chars)
        for (uint64_t p = 1; p <= right_pad(7, t->memory_used_pages_); ++p)
            Terminal::putchar(' ');
        print_uint(t->memory_used_pages_);
        Terminal::putchar(' ');

        // Stack size in KiB (right-aligned, 9 chars)
        uint64_t stack_kib = kernel::TaskControlBlock::STACK_SIZE / 1024;
        for (uint64_t p = 1; p <= right_pad(9, stack_kib); ++p)
            Terminal::putchar(' ');
        print_uint(stack_kib);
        Terminal::putchar(' ');

        // CPU time as hh:mm:ss (ticks at 1000 Hz = 1 ms per tick)
        {
            uint64_t total_ms = t->executed_ticks;
            uint64_t hh = total_ms / 3600000;
            total_ms %= 3600000;
            uint64_t mm = total_ms / 60000;
            total_ms %= 60000;
            uint64_t ss = total_ms / 1000;

            if (hh < 100) Terminal::putchar(' ');
            if (hh < 10)  Terminal::putchar(' ');
            print_uint(hh);
            Terminal::putchar(':');
            if (mm < 10) Terminal::putchar('0');
            print_uint(mm);
            Terminal::putchar(':');
            if (ss < 10) Terminal::putchar('0');
            print_uint(ss);
        }
        Terminal::putchar(' ');

        // Current marker
        if (t == cur)
            Terminal::write("<-");
        Terminal::write("\n");
    }

    // Zombie list summary
    uint64_t zcount = kernel::Scheduler::zombie_count();
    Terminal::write("\nZombies: ");
    print_uint(zcount);
    Terminal::write("\n");
}

void Shell::cmd_meminfo(int, const char**) {
    uint64_t total = kernel::PMM::total_memory();
    uint64_t free = kernel::PMM::free_memory();
    uint64_t used = total - free;

    auto print_size = [](uint64_t bytes) {
        uint64_t mb = bytes / (static_cast<uint64_t>(1024) * 1024);
        uint64_t remainder = bytes % (static_cast<uint64_t>(1024) * 1024);
        uint64_t kb = remainder / 1024;

        char buf[16];
        int pos = 0;
        if (mb == 0) { Terminal::putchar('0'); }
        else {
            uint64_t m = mb;
            while (m > 0) { buf[pos++] = static_cast<char>('0' + (m % 10)); m /= 10; }
            while (pos > 0) Terminal::putchar(buf[--pos]);
        }
        Terminal::write(" MiB");
        if (kb > 0) {
            Terminal::write(" ");
            pos = 0;
            while (kb > 0) { buf[pos++] = static_cast<char>('0' + (kb % 10)); kb /= 10; }
            while (pos > 0) Terminal::putchar(buf[--pos]);
            Terminal::write(" KiB");
        }
    };

    Terminal::write("Gesamt: "); print_size(total); Terminal::putchar('\n');
    Terminal::write("Belegt: "); print_size(used); Terminal::putchar('\n');
    Terminal::write("Frei:   "); print_size(free); Terminal::putchar('\n');

    // Page-table pool
    uint64_t pt_start = kernel::PMM::pool_start();
    uint64_t pt_end   = kernel::PMM::pool_end();
    if (pt_start) {
        uint64_t pt_total = kernel::PMM::pool_total_pages();
        uint64_t pt_used  = kernel::PMM::pool_used_pages();
        uint64_t pt_free  = pt_total - pt_used;
        Terminal::write("PT-Pool:\n");
        Terminal::write("  Pages:  total="); print_uint(pt_total);
        Terminal::write(" used="); print_uint(pt_used);
        Terminal::write(" free="); print_uint(pt_free);
        Terminal::write("\n  Size:   "); print_size(pt_end - pt_start);
        Terminal::write("\n  Gen:    "); print_uint(kernel::PMM::pool_generation());
        Terminal::write("\n  Ref:    "); print_uint(kernel::PMM::pool_refcount());
        Terminal::write("\n  Flags:  ");
        Terminal::write(kernel::PMM::pool_is_mapped() ? "mapped" : "unmapped");
        Terminal::write(kernel::PMM::pool_is_tainted() ? " tainted" : " clean");
        Terminal::write(kernel::PMM::pool_is_poisoned() ? " poisoned" : "");
        Terminal::putchar('\n');
    }

    // Kernel config info
    Terminal::write("Config:  Stack="); print_uint(CONFIG_STACK_SIZE / 1024);
    Terminal::write(" KiB  MaxTasks="); print_uint(CONFIG_MAX_TASKS);
    Terminal::putchar('\n');
}

void Shell::cmd_version(int, const char**) {
    Terminal::set_fg(COLOR_GREEN);
    Terminal::write(kernel::Version::full_string());
    Terminal::write("\n");
    Terminal::set_fg(COLOR_DEFAULT);
    Terminal::write("Kernel: ");
    Terminal::write(kernel::Version::string());
    Terminal::write("\nBuild:  ");
    Terminal::write(kernel::Version::build_date());
    Terminal::write(" ");
    Terminal::write(kernel::Version::build_time());
    Terminal::write("\n");
}

void Shell::cmd_jobs(int, const char**) {
    Terminal::write("Background tasks:\n");
    uint64_t count = kernel::Scheduler::task_count();
    uint64_t shown = 0;
    for (uint64_t i = 0; i < count; ++i) {
        auto* task = kernel::Scheduler::task_at(i);
        if (task && task->state == kernel::TaskState::TERMINATED) continue;
        if (task && task->priority > 0 && task != kernel::Scheduler::current_task()) {
            char buf[16];
            int pos = 0;
            uint64_t id = task->id;
            while (id > 0) { buf[pos++] = static_cast<char>('0' + (id % 10)); id /= 10; }
            while (pos > 0) Terminal::putchar(buf[--pos]);
            Terminal::write(": priority=");
            pos = 0; uint64_t p = task->priority;
            while (p > 0) { buf[pos++] = static_cast<char>('0' + (p % 10)); p /= 10; }
            while (pos > 0) Terminal::putchar(buf[--pos]);
            Terminal::putchar('\n');
            ++shown;
        }
    }
    if (shown == 0) Terminal::write("  (none)\n");
}

void Shell::cmd_modprobe(int argc, const char** argv) {
    if (argc < 2) {
        Terminal::write("Usage: modprobe <driver>\n");
        return;
    }
    if (kernel::DriverRegistry::load(argv[1])) {
        auto* drv = kernel::DriverRegistry::find(argv[1]);
        Terminal::write("Driver geladen: ");
        Terminal::set_fg(COLOR_GREEN);
        Terminal::write(drv ? drv->name : argv[1]);
        Terminal::set_fg(COLOR_DEFAULT);
        Terminal::putchar('\n');
    } else {
        shell_error_path("Fehler", argv[1], "Treiber nicht gefunden oder Init fehlgeschlagen");
    }
}

void Shell::cmd_modlist(int, const char**) {
    size_t count = kernel::DriverRegistry::count();
    Terminal::write("Verfuegbare Treiber:\n");
    if (count == 0) { Terminal::write("  (keine)\n"); return; }
    for (size_t i = 0; i < count; ++i) {
        auto* drv = kernel::DriverRegistry::get(i);
        if (!drv) continue;
        Terminal::write("  ");
        Terminal::set_fg(COLOR_DIR);
        Terminal::write(drv->name);
        Terminal::set_fg(COLOR_DEFAULT);
        size_t nlen = 0;
        while (drv->name[nlen]) ++nlen;
        for (size_t p = nlen; p < 16; ++p) Terminal::putchar(' ');
        Terminal::write(drv->description);
        Terminal::write(" [");
        switch (drv->state) {
        case kernel::DriverState::LOADED:   Terminal::set_fg(COLOR_GREEN); Terminal::write("geladen"); break;
        case kernel::DriverState::UNLOADED: Terminal::set_fg(COLOR_YELLOW); Terminal::write("verfuegbar"); break;
        case kernel::DriverState::ERROR:    Terminal::set_fg(COLOR_RED); Terminal::write("fehler"); break;
        }
        Terminal::set_fg(COLOR_DEFAULT);
        Terminal::write("]\n");
    }
}

void Shell::cmd_reboot(int, const char**) {
    Terminal::write("Reboot...\n");
    uint8_t good;
    do {
        good = arch::inb(0x64);
    } while (good & 0x02);
    arch::outb(0x64, 0xFE);
    arch::hlt();
}

void Shell::cmd_run(int argc, const char** argv) {
    if (argc < 2) {
        Terminal::write("Usage: run <program> [&]\n");
        Terminal::write("Verfuegbare Programme:\n");
        for (size_t i = 0; i < ProgramRegistry::count(); ++i) {
            auto* prog = ProgramRegistry::get(i);
            if (prog) {
                Terminal::write("  ");
                Terminal::write(prog->name);
                Terminal::write(" - ");
                Terminal::write(prog->description);
                Terminal::putchar('\n');
            }
        }
        return;
    }

    bool background = false;
    if (argc > 2 && str_cmp(argv[argc - 1], "&") == 0) {
        background = true;
        --argc;
    }

    auto* prog = ProgramRegistry::find(argv[1]);
    if (!prog) {
        Terminal::write("Programm nicht gefunden: ");
        Terminal::write(argv[1]);
        Terminal::putchar('\n');
        return;
    }

    if (background) {
        auto* task = kernel::TaskControlBlock::create(
            background_task_wrapper, 1, 100);
        if (task) {
            task->user_data = reinterpret_cast<void*>(prog->entry);
            auto* cur = kernel::Scheduler::current_task();
            if (cur) task->parent_id = cur->id;
            {
                size_t i = 0;
                if (prog->name) {
                    task->name[i++] = 'b';
                    task->name[i++] = 'g';
                    task->name[i++] = '_';
                    size_t j = 0;
                    while (prog->name[j] && i < CONFIG_TASK_NAME_LEN - 1)
                        task->name[i++] = prog->name[j++];
                }
                task->name[i] = '\0';
            }
        }
        if (task) {
            kernel::Scheduler::add_task(*task);
            Terminal::write("Task #");
            char buf[16];
            int pos = 0;
            uint64_t id = task->id;
            while (id > 0) { buf[pos++] = static_cast<char>('0' + (id % 10)); id /= 10; }
            while (pos > 0) Terminal::putchar(buf[--pos]);
            Terminal::write(": ");
            Terminal::write(prog->name);
            Terminal::write(" (background)\n");
        }
        return;
    }

    Terminal::write("Starte: ");
    Terminal::write(prog->name);
    Terminal::putchar('\n');

    prog->entry();
}

void Shell::cmd_listprog(int, const char**) {
    Terminal::write("Registrierte Programme:\n");
    for (size_t i = 0; i < ProgramRegistry::count(); ++i) {
        auto* prog = ProgramRegistry::get(i);
        if (prog) {
            Terminal::write("  ");
            Terminal::set_fg(COLOR_DIR);
            Terminal::write(prog->name);
            Terminal::set_fg(COLOR_DEFAULT);
            Terminal::write(" - ");
            Terminal::write(prog->description);
            Terminal::putchar('\n');
        }
    }
    if (ProgramRegistry::count() == 0) {
        Terminal::write("  (keine)\n");
    }
}

/// @brief Build the canonical absolute path from a target and current cwd.
static void build_canonical_path(const char* cwd, const char* target,
                                 char* out, size_t out_size) {
    char buf[256];
    size_t pos = 0;
    if (target[0] == '/') {
        while (target[pos] && pos < out_size - 1) { buf[pos] = target[pos]; ++pos; }
        buf[pos] = '\0';
    } else {
        size_t i = 0;
        while (cwd[i] && i < out_size - 1) { buf[i] = cwd[i]; ++i; }
        if (i > 1) buf[i++] = '/';
        size_t j = 0;
        while (target[j] && i < out_size - 1) { buf[i++] = target[j++]; }
        buf[i] = '\0';
    }
    // Normalize in-place: collapse //, resolve . and ..
    char stack[256];
    size_t sp = 0;
    const char* p = buf;
    while (*p) {
        while (*p == '/') ++p;
        if (!*p) break;
        const char* start = p;
        while (*p && *p != '/') ++p;
        size_t seg_len = static_cast<size_t>(p - start);
        if (seg_len == 1 && start[0] == '.') continue;
        if (seg_len == 2 && start[0] == '.' && start[1] == '.') {
            if (sp > 1) {
                --sp;
                while (sp > 0 && stack[sp - 1] != '/') --sp;
            }
            continue;
        }
        if (sp > 0 && stack[sp - 1] != '/') stack[sp++] = '/';
        for (size_t i = 0; i < seg_len; ++i) stack[sp++] = start[i];
    }
    if (sp == 0) { stack[sp++] = '/'; }
    stack[sp] = '\0';
    size_t i = 0;
    while (stack[i] && i < out_size - 1) { out[i] = stack[i]; ++i; }
    out[i] = '\0';
}

void Shell::cmd_cd(int argc, const char** argv) {
    const char* target = (argc < 2) ? "/" : argv[1];
    auto* vn = kernel::vfs::resolve(target);
    if (!vn) {
        shell_error_path("cd", target, "no such directory");
        return;
    }
    if (!(vn->mode & kernel::vfs::S_IFDIR)) {
        shell_error_path("cd", target, "not a directory");
        return;
    }
    auto* task = kernel::Scheduler::current_task();
    if (!task) return;
    task->cwd_lock_.lock();
    if (task->cwd_vnode)
        kernel::vfs::vnode_ref_dec(task->cwd_vnode);
    task->cwd_vnode = vn;
    kernel::vfs::vnode_ref_inc(vn);
    task->cwd_lock_.unlock();
    char canonical[256];
    build_canonical_path(task->cwd, target, canonical, sizeof(canonical));
    size_t i = 0;
    while (canonical[i] && i < 255) { task->cwd[i] = canonical[i]; ++i; }
    task->cwd[i] = '\0';
}

void Shell::cmd_export(int argc, const char** argv) {
    if (argc < 2) {
        Terminal::write("Usage: export VAR=value\n");
        return;
    }

    const char* arg = argv[1];
    const char* eq = nullptr;
    for (const char* p = arg; *p; ++p) {
        if (*p == '=') { eq = p; break; }
    }
    if (!eq || eq == arg) {
        shell_error("export", "malformed, use VAR=value");
        return;
    }

    size_t name_len = static_cast<size_t>(eq - arg);
    const char* val = eq + 1;

    // Check if variable already exists, update it
    for (size_t i = 0; i < env_count_; ++i) {
        bool match = true;
        for (size_t j = 0; j < name_len; ++j) {
            if (env_[i][j] != arg[j]) { match = false; break; }
        }
        if (match && env_[i][name_len] == '=') {
            // Update existing entry
            size_t pos = 0;
            for (size_t j = 0; j < name_len; ++j) env_[i][pos++] = arg[j];
            env_[i][pos++] = '=';
            for (const char* v = val; *v && pos < BUF_SIZE - 1; ++v) env_[i][pos++] = *v;
            env_[i][pos] = '\0';
            return;
        }
    }

    // Add new entry
    if (env_count_ >= MAX_ENV) {
        shell_error("export", "environment full");
        return;
    }

    size_t pos = 0;
    for (size_t j = 0; j < name_len; ++j) env_[env_count_][pos++] = arg[j];
    env_[env_count_][pos++] = '=';
    for (const char* v = val; *v && pos < BUF_SIZE - 1; ++v) env_[env_count_][pos++] = *v;
    env_[env_count_][pos] = '\0';
    ++env_count_;
}

void Shell::cmd_runelf(int argc, const char** argv) {
    if (argc < 2) {
        Terminal::write("Usage: runelf <path.elf> [args...]\n");
        return;
    }

    const char* path = argv[1];

    initrd::InitrdFile f = initrd::find(path);
    if (!f.data) {
        shell_error_path("runelf", path, "file not found in initrd");
        return;
    }

    auto* hdr = reinterpret_cast<const kernel::elf::ELF64Header*>(f.data);
    if (!kernel::elf::validate_header(hdr)) {
        shell_error_path("runelf", path, "invalid ELF");
        return;
    }

    auto* task = kernel::elf::load(hdr, f.data, f.size);
    if (!task) {
        shell_error_path("runelf", path, "failed to load");
        return;
    }

    kernel::Scheduler::add_task(*task);

    Terminal::set_fg(COLOR_GREEN);
    Terminal::write("Started task #");
    char buf[16];
    int pos = 0;
    uint64_t id = task->id;
    while (id > 0) { buf[pos++] = static_cast<char>('0' + (id % 10)); id /= 10; }
    while (pos > 0) Terminal::putchar(buf[--pos]);
    Terminal::write(": ");
    Terminal::write(path);
    Terminal::putchar('\n');
    Terminal::set_fg(COLOR_DEFAULT);
}

/// @brief Built-in: start a background ELF load (returns immediately).
void Shell::cmd_load(int argc, const char** argv) {
    if (argc < 2) {
        Terminal::write("Usage: load <path.elf>\n");
        return;
    }
    const char* path = argv[1];
    auto result = kernel::elf::ElfLoader::request_load(path);
    switch (result) {
    case kernel::elf::LoadResult::OK:
        // request_load already posted the "started" event (loader prints the
        // start line + dmesg).  Return immediately.
        Terminal::write("loading ");
        Terminal::write(path);
        Terminal::write(" started\n");
        return;
    case kernel::elf::LoadResult::ALREADY_LOADING:
        shell_error_path("load", path, "already loading");
        return;
    case kernel::elf::LoadResult::FILE_NOT_FOUND:
        shell_error_path("load", path, "file not found");
        return;
    default:
        return;
    }
}

/// @brief Built-in: cancel the in-flight background ELF load.
void Shell::cmd_cancel_load(int, const char**) {
    auto result = kernel::elf::ElfLoader::request_cancel();
    switch (result) {
    case kernel::elf::LoadResult::OK:
        Terminal::write("loading ");
        Terminal::write(kernel::elf::ElfLoader::current_path());
        Terminal::write(" canceled\n");
        return;
    case kernel::elf::LoadResult::NOT_LOADING:
        shell_error("cancel-load", "not loading");
        return;
    default:
        return;
    }
}

void Shell::cmd_selftest(int argc, const char** argv) {
    Terminal::write("Running kernel self-tests...\n");
    // Disable framebuffer: test activity may corrupt kernel page-table entries
    // (tests allocate/free pages, map/unmap memory).  Any subsequent
    // framebuffer access could fault.  Serial (COM1) is safe.
    Terminal::set_fb_enabled(false);

    // Boost the runner's priority above the realtime daemons (init = 10) so
    // the suite is not starved after the long IRQs-disabled window inside
    // test-isolation snapshot_create builds a scheduling backlog.  The
    // deadline monitor (127) still preempts briefly.  This mirrors the boot
    // path, where selftest runs on init (priority 10) and is therefore not
    // starved by the daemons.
    auto *self_task = kernel::Scheduler::current_task();
    uint64_t saved_prio = 0, saved_base = 0;
    if (self_task) {
        saved_prio = self_task->priority;
        saved_base = self_task->base_priority;
        self_task->priority = 126;
        self_task->base_priority = 126;
    }
    // The init task normally idles at priority 1 (reaper/logger duty).  For
    // the test run it must be back at its boot-time priority 10 so the suite
    // is not starved and the harness exemption (BUGS.md#021) applies.
    auto *init_task = kernel::Scheduler::get_harness_task();
    if (init_task && init_task != self_task) {
        kernel::Scheduler::set_priority(*init_task, 10);
    }

    {
        // Protect test execution from concurrent timer-ISR dispatch of
        // test-created tasks with fine-grained SpinLock.
        static kernel::sync::SpinLock test_lock;
        SpinLockGuard<kernel::sync::SpinLock> guard(test_lock);

        kernel::test::Registry::init();

        if (argc <= 1) {
            // No class arguments → run the "safe" class with TF_RELEASE filter
            kernel::test::register_class("safe");
            kernel::test::run_registered(kernel::test::TF_RELEASE);
        } else {
            // Register each named class, then run all registered tests
            for (int i = 1; i < argc; ++i) {
                if (!kernel::test::register_class(argv[i])) {
                    Terminal::write("Unknown test class: ");
                    Terminal::write(argv[i]);
                    Terminal::write("\n");
                }
            }
            // Enable auto-shutdown for explicit test class runs (e.g. "all")
            kernel::test::set_class_auto_shutdown(true);
            kernel::test::run_registered(0);
        }

        // Disable interrupts during cleanup so a timer IRQ cannot race
        // with task-array modifications or read stale scheduler globals.
        {
            arch::IrqGuard irq_guard;

        // Reap any terminated test tasks that accumulated while interrupts
        // were disabled (on_tick → reap_orphans never ran).
        kernel::Scheduler::reap_orphans();
        // Drain any tasks in the zombie list (terminated via release_zombie
        // but not yet cleaned up by the idle task).
        kernel::Scheduler::drain_zombie_list();

        // Clear stale scheduler context-switch globals.  Test code may have
        // called reschedule()/switch_to_task() while interrupts were disabled
        // (setting scheduler_save_rsp_to / scheduler_load_rsp_from), but the
        // ISR assembly never consumed them.  If left dangling, the next timer
        // IRQ would attempt to context-switch via stalled pointers to freed
        // task memory → GPF at iretq.
        __atomic_store_n(&kernel::scheduler_load_rsp_from, (uint64_t)0, __ATOMIC_RELEASE);
        __atomic_store_n(&kernel::scheduler_load_cr3_from, (uint64_t)0, __ATOMIC_RELEASE);
        __atomic_store_n(&kernel::scheduler_next_task_id, (uint64_t)0, __ATOMIC_RELEASE);
        __atomic_store_n(&kernel::scheduler_save_rsp_to, (uint64_t*)nullptr, __ATOMIC_RELEASE);
        __atomic_store_n(&kernel::isr_nesting_depth, (uint64_t)0, __ATOMIC_RELEASE);
        }
    }
    if (self_task) {
        self_task->priority = saved_prio;
        self_task->base_priority = saved_base;
    }
    // Restore init to its background reaper priority (0) now that the suite
    // is done.
    if (init_task && init_task != self_task) {
        kernel::Scheduler::set_priority(*init_task, 0);
    }
    Terminal::set_fb_enabled(true);
    Terminal::write("Self-tests complete.\n");
}

void Shell::cmd_exit(int, const char**) {
    Terminal::set_fg(COLOR_ERROR);
    Terminal::write("\nShutting down...\n");
    Terminal::set_fg(COLOR_DEFAULT);

    arch::outw(arch::QEMU_ACPI_PORT, 0x2000);
    arch::outw(arch::QEMU_SHUTDOWN_PORT, 0x2000);
    arch::qemu_debug_exit(0);

    for (int i = 0; i < 100000000; ++i) arch::pause();
    Terminal::write("Shutdown failed. Halting.\n");
    arch::cli(); arch::hlt();
}

void Shell::cmd_pwd(int, const char**) {
    auto* task = kernel::Scheduler::current_task();
    if (!task) return;
    Terminal::write(task->cwd);
    Terminal::putchar('\n');
}

void Shell::cmd_which(int argc, const char** argv) {
    if (argc < 2) {
        Terminal::write("Usage: which <command...>\n");
        return;
    }
    for (int i = 1; i < argc; ++i) {
        bool found = false;
        for (size_t j = 0; j < num_commands_; ++j) {
            if (strcmp(argv[i], commands_[j].name) == 0) {
                Terminal::write(commands_[j].name);
                Terminal::write(" (shell built-in)\n");
                found = true;
                break;
            }
        }
        if (!found) {
            Terminal::write(argv[i]);
            Terminal::write(": not found\n");
        }
    }
}

void Shell::cmd_env(int, const char**) {
    if (env_count_ == 0) {
        Terminal::write("(no environment variables set)\n");
        return;
    }
    for (size_t i = 0; i < env_count_; ++i) {
        Terminal::write(env_[i]);
        Terminal::putchar('\n');
    }
}

void Shell::cmd_sleep(int argc, const char** argv) {
    if (argc < 2) {
        Terminal::write("Usage: sleep <seconds>\n");
        return;
    }
    uint64_t secs = 0;
    const char* p = argv[1];
    while (*p) {
        secs = secs * 10 + static_cast<uint64_t>(*p++ - '0');
    }
    uint64_t target = arch::Timer::ticks() + secs * 1000;
    while (arch::Timer::ticks() < target) {
        arch::pause();
    }
}

void Shell::cmd_mkdir(int argc, const char** argv) {
    if (argc < 2) {
        Terminal::write("Usage: mkdir <path>\n");
        return;
    }
    auto r = kernel::vfs::mkdir_err(argv[1], 0);
    if (r != kernel::errors::VFS_ERR_OK) {
        shell_vfs_error("mkdir", r);
    }
}

void Shell::cmd_rm(int argc, const char** argv) {
    if (argc < 2) {
        Terminal::write("Usage: rm <path>\n");
        return;
    }
    auto r = kernel::vfs::unlink_err(argv[1]);
    if (r != kernel::errors::VFS_ERR_OK) {
        shell_vfs_error("rm", r);
    }
}

void Shell::cmd_rmdir(int argc, const char** argv) {
    if (argc < 2) {
        Terminal::write("Usage: rmdir <path>\n");
        return;
    }
    auto r = kernel::vfs::unlink_err(argv[1]);
    if (r != kernel::errors::VFS_ERR_OK) {
        shell_vfs_error("rmdir", r);
    }
}

void Shell::cmd_alias(int argc, const char** argv) {
    if (argc < 2) {
        Terminal::write("Aliases:\n");
        for (size_t i = 0; i < alias_count_; ++i) {
            Terminal::write("  ");
            Terminal::write(aliases_[i].name);
            Terminal::write("='");
            Terminal::write(aliases_[i].value);
            Terminal::write("'\n");
        }
        return;
    }
    const char* eq = nullptr;
    for (const char* p = argv[1]; *p; ++p) {
        if (*p == '=') { eq = p; break; }
    }
    if (!eq || eq == argv[1]) {
        // If alias exists, show its value
        for (size_t i = 0; i < alias_count_; ++i) {
            if (str_cmp(argv[1], aliases_[i].name) == 0) {
                Terminal::write(aliases_[i].name);
                Terminal::write("='");
                Terminal::write(aliases_[i].value);
                Terminal::write("'\n");
                return;
            }
        }
        shell_error_path("alias", argv[1], "not found");
        return;
    }
    size_t name_len = static_cast<size_t>(eq - argv[1]);
    const char* val = eq + 1;
    for (size_t i = 0; i < alias_count_; ++i) {
        bool match = true;
        for (size_t j = 0; j < name_len; ++j) {
            if (aliases_[i].name[j] != argv[1][j]) { match = false; break; }
        }
        if (match && aliases_[i].name[name_len] == '\0') {
            size_t pos = 0;
            for (size_t j = 0; j < name_len; ++j) aliases_[i].name[pos++] = argv[1][j];
            aliases_[i].name[pos] = '\0';
            pos = 0;
            for (const char* v = val; *v && pos < 255; ++v) aliases_[i].value[pos++] = *v;
            aliases_[i].value[pos] = '\0';
            return;
        }
    }
    if (alias_count_ >= Shell::MAX_ALIASES) {
        shell_error("alias", "limit reached");
        return;
    }
    size_t pos = 0;
    for (size_t j = 0; j < name_len; ++j) aliases_[alias_count_].name[pos++] = argv[1][j];
    aliases_[alias_count_].name[pos] = '\0';
    pos = 0;
    for (const char* v = val; *v && pos < 255; ++v) aliases_[alias_count_].value[pos++] = *v;
    aliases_[alias_count_].value[pos] = '\0';
    ++alias_count_;
}

void Shell::cmd_unalias(int argc, const char** argv) {
    if (argc < 2) {
        Terminal::write("Usage: unalias <name>\n");
        return;
    }
    for (size_t i = 0; i < alias_count_; ++i) {
        if (str_cmp(argv[1], aliases_[i].name) == 0) {
            for (size_t j = i; j < alias_count_ - 1; ++j) aliases_[j] = aliases_[j + 1];
            --alias_count_;
            return;
        }
    }
    shell_error_path("unalias", argv[1], "not found");
}

void Shell::cmd_history(int, const char**) {
    size_t start = history_count_ > 20 ? history_count_ - 20 : 0;
    for (size_t i = start; i < history_count_; ++i) {
        char num[16];
        int pos = 0;
        size_t n = i + 1;
        while (n > 0) { num[pos++] = static_cast<char>('0' + (n % 10)); n /= 10; }
        while (pos > 0) Terminal::putchar(num[--pos]);
        Terminal::write("  ");
        Terminal::write(history_[i].cmd);
        Terminal::putchar('\n');
    }
}

void Shell::cmd_type(int argc, const char** argv) {
    if (argc < 2) {
        Terminal::write("Usage: type <name...>\n");
        return;
    }
    for (int i = 1; i < argc; ++i) {
        // Check alias
        bool found = false;
        for (size_t j = 0; j < alias_count_; ++j) {
            if (str_cmp(argv[i], aliases_[j].name) == 0) {
                Terminal::write(argv[i]);
                Terminal::write(" is an alias for '");
                Terminal::write(aliases_[j].value);
                Terminal::write("'\n");
                found = true;
                break;
            }
        }
        if (found) continue;
        // Check built-in
        for (size_t j = 0; j < num_commands_; ++j) {
            if (str_cmp(argv[i], commands_[j].name) == 0) {
                Terminal::write(argv[i]);
                Terminal::write(" is a shell built-in\n");
                found = true;
                break;
            }
        }
        if (found) continue;
        Terminal::write(argv[i]);
        Terminal::write(": not found\n");
    }
}

void Shell::cmd_source(int argc, const char** argv) {
    if (argc < 2) {
        Terminal::write("Usage: source <path>\n");
        return;
    }
    auto* vn = kernel::vfs::resolve(argv[1]);
    if (!vn || !(vn->mode & kernel::vfs::S_IFREG)) {
        shell_error_path("source", argv[1], "cannot read");
        return;
    }
    if (!vn->ops->read) return;
    uint8_t buf[4096];
    int64_t nread = vn->ops->read(*vn, buf, sizeof(buf) - 1, 0);
    if (nread <= 0) return;
    buf[nread] = '\0';
    // Execute each line
    char* line = reinterpret_cast<char*>(buf);
    char* start = line;
    while (*line) {
        if (*line == '\n') {
            *line = '\0';
            if (*start) parse_and_exec(start);
            start = line + 1;
        }
        ++line;
    }
    if (*start) parse_and_exec(start);
}

void Shell::cmd_set(int argc, const char** argv) {
    if (argc < 2) {
        // Show all shell variables
        Terminal::write("Shell options:\n");
        Terminal::write("  positional args: ");
        char buf[16];
        int pos = 0;
        int n = positional_argc_;
        if (n == 0) { buf[pos++] = '0'; }
        else { while (n > 0) { buf[pos++] = static_cast<char>('0' + (n % 10)); n /= 10; } }
        while (pos > 0) Terminal::putchar(buf[--pos]);
        Terminal::putchar('\n');
        // Show environment
        for (size_t i = 0; i < env_count_; ++i) {
            Terminal::write("  ");
            Terminal::write(env_[i]);
            Terminal::putchar('\n');
        }
        return;
    }
    // Parse options
    if (argv[1][0] == '-') {
        for (const char* p = argv[1] + 1; *p; ++p) {
            switch (*p) {
            case 'x': shell_options_ |= 1; break;
            case 'e': shell_options_ |= 2; break;
            case 'u': shell_options_ |= 4; break;
            default:
                Terminal::set_fg(COLOR_ERROR);
                Terminal::write("set: unknown option: -");
                Terminal::putchar(*p);
                Terminal::putchar('\n');
                Terminal::set_fg(COLOR_DEFAULT);
                return;
            }
        }
    } else if (argv[1][0] == '+' && argv[1][1]) {
        for (const char* p = argv[1] + 1; *p; ++p) {
            switch (*p) {
            case 'x': shell_options_ &= ~1; break;
            case 'e': shell_options_ &= ~2; break;
            case 'u': shell_options_ &= ~4; break;
            default:
                Terminal::set_fg(COLOR_ERROR);
                Terminal::write("set: unknown option: +");
                Terminal::putchar(*p);
                Terminal::putchar('\n');
                Terminal::set_fg(COLOR_DEFAULT);
                return;
            }
        }
    } else {
        // Set positional parameters
        positional_argc_ = argc - 1;
        for (int i = 0; i < positional_argc_ && i < 32; ++i) {
            size_t slen = 0;
            while (argv[i + 1][slen]) ++slen;
            auto* old = positional_argv_[i];
#ifndef __clang__
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wanalyzer-possible-null-dereference"
#endif
            positional_argv_[i] = new char[slen + 1];
            if (!positional_argv_[i]) return;
            for (size_t j = 0; j <= slen; ++j) positional_argv_[i][j] = argv[i + 1][j];
#ifndef __clang__
#pragma GCC diagnostic pop
#endif
            delete[] old;
        }
    }
}

void Shell::cmd_read(int argc, const char** argv) {
    char line[BUF_SIZE];
    size_t pos = 0;
    for (;;) {
        char c = 0;
        bool got = false;
#if defined(CONFIG_ARCH_X86_64)
        if (arch::inb(arch::COM1_LSR) & 1) { c = static_cast<char>(arch::inb(arch::COM1)); got = true; }
#endif
        if (!got) got = arch::Keyboard::getchar(c);
        if (!got) { arch::pause(); continue; }
        if (c == '\r') c = '\n';
        if (c == '\n') { line[pos] = '\0'; break; }
        if ((c == '\b' || c == 0x7F) && pos > 0) { --pos; Terminal::putchar('\b'); continue; }
        if (pos < BUF_SIZE - 1) { line[pos++] = c; Terminal::putchar(c); }
    }
    Terminal::putchar('\n');
    if (argc < 2) return;
    // Store into variable
    for (size_t i = 0; i < env_count_; ++i) {
        bool match = true;
        for (size_t j = 0; argv[1][j]; ++j) {
            if (env_[i][j] != argv[1][j] || env_[i][j] == '\0') { match = false; break; }
        }
        if (match && env_[i][argv[1][0] ? 0 : 1] != 0) {
            // Wait, need proper match: name must be followed by '='
            // This is getting complex - just handle it
        }
    }
    // Simple: env variable VAR gets the line
    if (env_count_ < MAX_ENV) {
        size_t p = 0;
        for (const char* s = argv[1]; *s; ++s) env_[env_count_][p++] = *s;
        env_[env_count_][p++] = '=';
        for (size_t i = 0; i < pos && i < BUF_SIZE - p - 1; ++i) env_[env_count_][p++] = line[i];
        env_[env_count_][p] = '\0';
        ++env_count_;
    }
}

void Shell::cmd_printf(int argc, const char** argv) {
    if (argc < 2) {
        Terminal::write("Usage: printf <format> [args...]\n");
        return;
    }
    const char* fmt = argv[1];
    int arg_idx = 2;
    while (*fmt) {
        if (*fmt == '%' && *(fmt + 1)) {
            ++fmt;
            if (*fmt == 's') {
                if (arg_idx < argc) { Terminal::write(argv[arg_idx++]); }
                ++fmt;
            } else if (*fmt == 'd' || *fmt == 'i') {
                int val = 0;
                if (arg_idx < argc) {
                    const char* p = argv[arg_idx++];
                    bool neg = (*p == '-');
                    if (neg) ++p;
                    while (*p >= '0' && *p <= '9') val = val * 10 + (*p++ - '0');
                    if (neg) val = -val;
                }
                char num[16];
                int npos = 0;
                if (val < 0) { Terminal::putchar('-'); val = -val; }
                if (val == 0) { Terminal::putchar('0'); }
                else {
                    while (val > 0) { num[npos++] = static_cast<char>('0' + (val % 10)); val /= 10; }
                    while (npos > 0) Terminal::putchar(num[--npos]);
                }
                ++fmt;
            } else if (*fmt == 'u') {
                unsigned int val = 0;
                if (arg_idx < argc) {
                    const char* p = argv[arg_idx++];
                    while (*p >= '0' && *p <= '9') val = val * 10 + (*p++ - '0');
                }
                char num[16];
                int npos = 0;
                if (val == 0) { Terminal::putchar('0'); }
                else {
                    while (val > 0) { num[npos++] = static_cast<char>('0' + (val % 10)); val /= 10; }
                    while (npos > 0) Terminal::putchar(num[--npos]);
                }
                ++fmt;
            } else if (*fmt == 'c') {
                if (arg_idx < argc) Terminal::putchar(argv[arg_idx++][0]);
                ++fmt;
            } else if (*fmt == '%') {
                Terminal::putchar('%');
                ++fmt;
            } else if (*fmt == 'x' || *fmt == 'X') {
                unsigned int val = 0;
                if (arg_idx < argc) {
                    const char* p = argv[arg_idx++];
                    while (*p >= '0' && *p <= '9') val = val * 16 + (*p++ - '0');
                    // Also handle a-f
                }
                char hex[16];
                int hpos = 0;
                if (val == 0) { Terminal::putchar('0'); }
                else {
                    while (val > 0) {
                        unsigned int d = val & 0xF;
                        hex[hpos++] = static_cast<char>(d < 10 ? '0' + d : (*fmt == 'X' ? 'A' : 'a') + (d - 10));
                        val >>= 4;
                    }
                    while (hpos > 0) Terminal::putchar(hex[--hpos]);
                }
                ++fmt;
            } else {
                Terminal::putchar('%');
                Terminal::putchar(*fmt);
                ++fmt;
            }
        } else if (*fmt == '\\' && *(fmt + 1)) {
            ++fmt;
            switch (*fmt) {
            case 'n': Terminal::putchar('\n'); break;
            case 't': Terminal::putchar('\t'); break;
            case 'r': Terminal::putchar('\r'); break;
            case '\\': Terminal::putchar('\\'); break;
            default: Terminal::putchar('\\'); Terminal::putchar(*fmt); break;
            }
            ++fmt;
        } else {
            Terminal::putchar(*fmt);
            ++fmt;
        }
    }
}

void Shell::cmd_test(int argc, const char** argv) {
    // Parse [ expr ]
    int start = 1;
    bool negate = false;
    if (argc > 1 && str_cmp(argv[1], "!") == 0) { negate = true; start = 2; }

    // Remove trailing ] for [ command
    int end = argc;
    if (argc > 1 && str_cmp(argv[0], "[") == 0 && str_cmp(argv[argc - 1], "]") == 0) {
        end = argc - 1;
    }

    bool result = false;

    // NOLINTNEXTLINE(bugprone-branch-clone)
    if (end <= start) {
        result = false;
    } else if (end - start == 1) {
        // test <string> — true if non-empty
        result = argv[start][0] != '\0';
    } else if (end - start == 3) {
        const char* a = argv[start];
        const char* op = argv[start + 1];
        const char* b = argv[start + 2];
        if (str_cmp(op, "=") == 0 || str_cmp(op, "==") == 0) {
            result = str_cmp(a, b) == 0;
        } else if (str_cmp(op, "!=") == 0) {
            result = str_cmp(a, b) != 0;
        } else if (str_cmp(op, "-eq") == 0) {
            int va = 0, vb = 0;
            for (const char* p = a; *p; ++p) va = va * 10 + (*p - '0');
            for (const char* p = b; *p; ++p) vb = vb * 10 + (*p - '0');
            result = va == vb;
        } else if (str_cmp(op, "-ne") == 0) {
            int va = 0, vb = 0;
            for (const char* p = a; *p; ++p) va = va * 10 + (*p - '0');
            for (const char* p = b; *p; ++p) vb = vb * 10 + (*p - '0');
            result = va != vb;
        } else if (str_cmp(op, "-lt") == 0) {
            int va = 0, vb = 0;
            for (const char* p = a; *p; ++p) va = va * 10 + (*p - '0');
            for (const char* p = b; *p; ++p) vb = vb * 10 + (*p - '0');
            result = va < vb;
        } else if (str_cmp(op, "-le") == 0) {
            int va = 0, vb = 0;
            for (const char* p = a; *p; ++p) va = va * 10 + (*p - '0');
            for (const char* p = b; *p; ++p) vb = vb * 10 + (*p - '0');
            result = va <= vb;
        } else if (str_cmp(op, "-gt") == 0) {
            int va = 0, vb = 0;
            for (const char* p = a; *p; ++p) va = va * 10 + (*p - '0');
            for (const char* p = b; *p; ++p) vb = vb * 10 + (*p - '0');
            result = va > vb;
        } else if (str_cmp(op, "-ge") == 0) {
            int va = 0, vb = 0;
            for (const char* p = a; *p; ++p) va = va * 10 + (*p - '0');
            for (const char* p = b; *p; ++p) vb = vb * 10 + (*p - '0');
            result = va >= vb;
        } else if (str_cmp(op, "-z") == 0) {
            result = a[0] == '\0';
        } else if (str_cmp(op, "-n") == 0) {
            result = a[0] != '\0';
        } else if (op[0] == '-' && op[1] != '\0' && op[2] == '\0') {
            // File tests
            auto* vn = kernel::vfs::resolve(a);
            switch (op[1]) {
            case 'e':
            case 'r':
            case 'w':
            case 'x': result = vn != nullptr; break;
            case 'f': result = vn && (vn->mode & kernel::vfs::S_IFREG); break;
            case 'd': result = vn && (vn->mode & kernel::vfs::S_IFDIR); break;
            case 's': result = vn && vn->size > 0; break;
            default: result = false; break;
            }
        } else {
            result = false;
        }
    } else {
        result = false;
    }

    last_exit_code_ = (result != negate) ? 0 : 1;
}

void Shell::cmd_shift(int argc, const char** argv) {
    int n = 1;
    if (argc > 1) {
        n = 0;
        for (const char* p = argv[1]; *p; ++p) n = n * 10 + (*p - '0');
    }
    if (n <= 0) n = 1;
    if (n > positional_argc_) n = positional_argc_;
    for (int i = 0; i < positional_argc_ - n; ++i) {
        positional_argv_[i] = positional_argv_[i + n];
    }
    positional_argc_ -= n;
}

void Shell::cmd_trap(int argc, const char** argv) {
    if (argc < 2) {
        // List traps
        for (size_t i = 0; i < 32; ++i) {
            if (traps_[i].used) {
                char num[8];
                int pos = 0;
                int n = traps_[i].signo;
                if (n == 0) { Terminal::write("  EXIT"); }
                else {
                    Terminal::write("  ");
                    while (n > 0) { num[pos++] = static_cast<char>('0' + (n % 10)); n /= 10; }
                    while (pos > 0) Terminal::putchar(num[--pos]);
                }
                Terminal::write(": ");
                Terminal::write(traps_[i].handler);
                Terminal::putchar('\n');
            }
        }
        return;
    }
    if (argc == 2) {
        // Remove trap for signal
        int sig = 0;
        for (const char* p = argv[1]; *p; ++p) sig = sig * 10 + (*p - '0');
        for (size_t i = 0; i < 32; ++i) {
            if (traps_[i].used && traps_[i].signo == sig) {
                traps_[i].used = false;
                return;
            }
        }
        return;
    }
    // trap <action> <signal...>
    int sig = 0;
    for (const char* p = argv[argc - 1]; *p; ++p) sig = sig * 10 + (*p - '0');
    const char* action = argv[1];
    for (size_t i = 0; i < 32; ++i) {
        if (!traps_[i].used) {
            traps_[i].signo = sig;
            size_t pos = 0;
            for (const char* a = action; *a && pos < 255; ++a) traps_[i].handler[pos++] = *a;
            traps_[i].handler[pos] = '\0';
            traps_[i].used = true;
            return;
        }
    }
}

void Shell::cmd_wait(int, const char**) {
    // Wait for all child (background) tasks to finish
    auto* current = kernel::Scheduler::current_task();
    uint64_t current_id = current ? current->id : 0;
    while (true) {
        bool any_alive = false;
        uint64_t count = kernel::Scheduler::task_count();
        for (uint64_t i = 0; i < count; ++i) {
            auto* task = kernel::Scheduler::task_at(i);
            if (task && task->parent_id == current_id &&
                task->state != kernel::TaskState::TERMINATED) {
                any_alive = true;
                break;
            }
        }
        if (!any_alive) break;
        arch::pause();
    }
}

void Shell::cmd_fg(int, const char**) {
    Terminal::write("fg: job control not fully implemented\n");
}

void Shell::cmd_bg(int, const char**) {
    Terminal::write("bg: job control not fully implemented\n");
}

void Shell::cmd_disown(int argc, const char** argv) {
    if (argc < 2) {
        Terminal::write("Usage: disown <task-id>\n");
        return;
    }
    uint64_t id = 0;
    for (const char* p = argv[1]; *p; ++p) id = id * 10 + (*p - '0');
    // Just mark as disowned — we don't track them separately
    Terminal::write("disowned task ");
    char buf[16];
    int pos = 0;
    uint64_t n = id;
    while (n > 0) { buf[pos++] = static_cast<char>('0' + (n % 10)); n /= 10; }
    while (pos > 0) Terminal::putchar(buf[--pos]);
    Terminal::putchar('\n');
}

void Shell::cmd_ulimit(int, const char**) {
    Terminal::write("ulimit: not implemented (embedded system)\n");
}

void Shell::cmd_umask(int argc, const char** argv) {
    if (argc < 2) {
        char buf[8];
        int pos = 0;
        int m = umask_;
        for (int i = 0; i < 3; ++i) {
            int digit = (m >> (6 - i * 3)) & 7;
            buf[pos++] = static_cast<char>('0' + digit);
        }
        buf[pos] = '\0';
        Terminal::write(buf);
        Terminal::putchar('\n');
        return;
    }
    int new_mask = 0;
    for (const char* p = argv[1]; *p; ++p) {
        if (*p >= '0' && *p <= '7') new_mask = new_mask * 8 + (*p - '0');
    }
    umask_ = new_mask & 0777;
}

void Shell::cmd_times(int, const char**) {
    uint64_t ticks = arch::Timer::ticks();
    uint64_t secs = ticks / 1000;
    uint64_t mins = secs / 60;
    uint64_t hours = mins / 60;
    secs %= 60;
    mins %= 60;
    Terminal::write("shell running time: ");
    char buf[16];
    int pos = 0;
    uint64_t n = hours;
    while (n > 0) { buf[pos++] = static_cast<char>('0' + (n % 10)); n /= 10; }
    while (pos > 0) Terminal::putchar(buf[--pos]);
    Terminal::write("h");
    pos = 0; n = mins;
    while (n > 0) { buf[pos++] = static_cast<char>('0' + (n % 10)); n /= 10; }
    while (pos > 0) Terminal::putchar(buf[--pos]);
    Terminal::write("m");
    pos = 0; n = secs;
    while (n > 0) { buf[pos++] = static_cast<char>('0' + (n % 10)); n /= 10; }
    while (pos > 0) Terminal::putchar(buf[--pos]);
    Terminal::write("s\n");
}

void Shell::cmd_logout(int argc, const char** argv) {
    (void)argc;
    (void)argv;
    const char* exit_argv[] = {"exit"};
    cmd_exit(1, exit_argv);
}

void Shell::cmd_dirs(int, const char**) {
    Terminal::write("Directory stack:\n");
    for (size_t i = 0; i < dir_stack_count_; ++i) {
        Terminal::write("  ");
        Terminal::write(dir_stack_[i]);
        Terminal::putchar('\n');
    }
    if (dir_stack_count_ == 0) Terminal::write("  (empty)\n");
}

void Shell::cmd_pushd(int argc, const char** argv) {
    if (argc < 2) {
        Terminal::write("Usage: pushd <directory>\n");
        return;
    }
    if (dir_stack_count_ >= Shell::MAX_DIR_STACK) {
        shell_error("pushd", "directory stack full");
        return;
    }
    auto* vn = kernel::vfs::resolve(argv[1]);
    if (!vn || !(vn->mode & kernel::vfs::S_IFDIR)) {
        shell_error_path("pushd", argv[1], "not a directory");
        return;
    }
    size_t pos = 0;
    for (const char* p = argv[1]; *p && pos < BUF_SIZE - 1; ++p) dir_stack_[dir_stack_count_][pos++] = *p;
    dir_stack_[dir_stack_count_][pos] = '\0';
    ++dir_stack_count_;
    cmd_cd(2, argv);
}

void Shell::cmd_popd(int, const char**) {
    if (dir_stack_count_ == 0) {
        shell_error("popd", "directory stack empty");
        return;
    }
    --dir_stack_count_;
    // Change to popped directory
    const char* cd_argv[] = {"cd", dir_stack_[dir_stack_count_]};
    cmd_cd(2, cd_argv);
}

void Shell::cmd_ls(int argc, const char** argv) {
    const char* path = (argc < 2) ? "." : argv[1];
    auto* vn = kernel::vfs::resolve(path);
    if (!vn) {
        shell_error_path("ls", path, "cannot access");
        return;
    }
    if (!(vn->mode & kernel::vfs::S_IFDIR)) {
        Terminal::write(path);
        Terminal::putchar('\n');
        return;
    }

    uint64_t pos = 0;
    kernel::vfs::Dirent dent;
    while (vn->ops->readdir(*vn, pos, dent) == 0) {
        if (dent.d_name[0] == '\0') continue;
        if (str_cmp(dent.d_name, ".") == 0 || str_cmp(dent.d_name, "..") == 0) continue;

        auto* child = vn->ops->lookup(*vn, dent.d_name);
        if (child) {
            if (child->mode & kernel::vfs::S_IFDIR) {
                Terminal::set_fg(COLOR_DIR);
            } else {
                Terminal::set_fg(COLOR_DEFAULT);
            }
        } else {
            Terminal::set_fg(COLOR_DEFAULT);
        }
        Terminal::write(dent.d_name);
        Terminal::set_fg(COLOR_DEFAULT);
        Terminal::putchar(' ');
    }
    Terminal::putchar('\n');
}

static void print_ip(net::Ipv4Addr ip) {
    char buf[16];
    int pos = 0;
    for (int i = 0; i < 4; ++i) {
        if (i > 0) buf[pos++] = '.';
        uint8_t n = ip.addr[i];
        if (n >= 100) buf[pos++] = static_cast<char>('0' + (n / 100));
        if (n >= 10) buf[pos++] = static_cast<char>('0' + ((n / 10) % 10));
        buf[pos++] = static_cast<char>('0' + (n % 10));
    }
    buf[pos] = '\0';
    Terminal::write(buf);
}

static void print_mac(net::MacAddr mac) {
    char buf[18];
    int pos = 0;
    for (int i = 0; i < 6; ++i) {
        if (i > 0) buf[pos++] = ':';
        const char* hex = "0123456789abcdef";
        buf[pos++] = hex[(mac.addr[i] >> 4) & 0xF];
        buf[pos++] = hex[mac.addr[i] & 0xF];
    }
    buf[pos] = '\0';
    Terminal::write(buf);
}

void Shell::cmd_ifconfig(int argc, const char** argv) {
    if (!kernel::gs::get_nic()) {
        shell_error("ifconfig", "no network interface");
        return;
    }

    auto& nic = *kernel::gs::get_nic();

    if (argc >= 2) {
        // Parse IP
        uint8_t a[4] = {0, 0, 0, 0};
        int octet = 0;
        int val = 0;
        const char* p = argv[1];
        while (*p && octet < 4) {
            if (*p == '.') {
                a[octet++] = static_cast<uint8_t>(val);
                val = 0;
            } else if (*p >= '0' && *p <= '9') {
                val = val * 10 + (*p - '0');
            }
            ++p;
        }
        if (octet < 4) a[octet] = static_cast<uint8_t>(val);
        nic.ip = net::Ipv4Addr{{a[0], a[1], a[2], a[3]}};

        if (argc >= 3) {
            octet = 0; val = 0; p = argv[2];
            while (*p && octet < 4) {
                if (*p == '.') { a[octet++] = static_cast<uint8_t>(val); val = 0; }
                else if (*p >= '0' && *p <= '9') val = val * 10 + (*p - '0');
                ++p;
            }
            if (octet < 4) a[octet] = static_cast<uint8_t>(val);
            nic.subnet = net::Ipv4Addr{{a[0], a[1], a[2], a[3]}};
        }

        if (argc >= 4) {
            octet = 0; val = 0; p = argv[3];
            while (*p && octet < 4) {
                if (*p == '.') { a[octet++] = static_cast<uint8_t>(val); val = 0; }
                else if (*p >= '0' && *p <= '9') val = val * 10 + (*p - '0');
                ++p;
            }
            if (octet < 4) a[octet] = static_cast<uint8_t>(val);
            nic.gateway = net::Ipv4Addr{{a[0], a[1], a[2], a[3]}};
        }

        Terminal::set_fg(COLOR_GREEN);
        Terminal::write("ifconfig: interface configured\n");
        Terminal::set_fg(COLOR_DEFAULT);
        return;
    }

    // Show status
    Terminal::write(nic.name ? nic.name : "eth0");
    Terminal::write(": ");
    print_mac(nic.mac);
    Terminal::write("\n  inet ");
    print_ip(nic.ip);
    Terminal::write("  netmask ");
    print_ip(nic.subnet);
    Terminal::write("  gateway ");
    print_ip(nic.gateway);
    Terminal::write("\n");
}

static int parse_ip(const char* str, net::Ipv4Addr& out) {
    uint8_t a[4] = {0, 0, 0, 0};
    int octet = 0;
    int val = 0;
    while (*str && octet < 4) {
        if (*str == '.') {
            if (val > 255) return -1;
            a[octet++] = static_cast<uint8_t>(val);
            val = 0;
        } else if (*str >= '0' && *str <= '9') {
            val = val * 10 + (*str - '0');
        } else {
            return -1;
        }
        ++str;
    }
    if (octet < 4) {
        if (val > 255) return -1;
        a[octet] = static_cast<uint8_t>(val);
    }
    out = net::Ipv4Addr{{a[0], a[1], a[2], a[3]}};
    return 0;
}

void Shell::cmd_ping(int argc, const char** argv) {
    if (argc < 2) {
        Terminal::write("Usage: ping <ip> [count]\n");
        return;
    }

    net::Ipv4Addr dst;
    if (parse_ip(argv[1], dst) != 0) {
        shell_error_path("ping", argv[1], "invalid IP address");
        return;
    }

    bool is_loopback = (dst.as_u32() == 0x7F000001);

    if (!kernel::gs::get_nic() && !is_loopback) {
        shell_error("ping", "no network interface");
        return;
    }

    int count = 4;
    if (argc >= 3) {
        count = 0;
        for (const char* p = argv[2]; *p; ++p) count = count * 10 + (*p - '0');
    }

    Terminal::write("PING ");
    print_ip(dst);
    Terminal::write(" (");
    print_ip(dst);
    Terminal::write(") 56(84) bytes of data.\n");

    uint64_t pkt_tx = 0, pkt_rx = 0;
    uint64_t start_tick = arch::Timer::ticks();

    for (int i = 0; i < count; ++i) {
        net::net_icmp_clear_reply();
        uint16_t seq = static_cast<uint16_t>(i);
        uint16_t id = 0x1337;

        uint8_t payload[56];
        uint64_t sent_tick = arch::Timer::ticks();
        for (size_t p = 0; p < sizeof(payload); ++p) payload[p] = static_cast<uint8_t>(p + i);

        bool sent;
        if (is_loopback) {
            net::net_icmp_set_reply(id, seq, dst);
            sent = true;
        } else {
            sent = net::net_send_icmp_echo(*kernel::gs::get_nic(), dst, id, seq, payload, sizeof(payload));
        }

        ++pkt_tx;
        if (sent) {
            uint64_t deadline = sent_tick + 1000;
            while (arch::Timer::ticks() < deadline) {
                for (int p = 0; p < 10; ++p)
                    if (net::net_poll(*kernel::gs::get_nic())) break;
                auto* r = net::net_icmp_last_reply();
                if (r && r->ident == id && r->seq == seq) break;
                arch::pause();
            }
        }

        auto* reply = net::net_icmp_last_reply();
        if (reply && reply->ident == id && reply->seq == seq) {
            ++pkt_rx;
            uint64_t rtt = reply->rx_tick - sent_tick;
            Terminal::write("64 bytes from ");
            print_ip(reply->src);
            Terminal::write(": icmp_seq=");
            char nbuf[16];
            int np = 0;
            uint16_t ns = seq;
            if (ns == 0) { nbuf[np++] = '0'; }
            while (ns > 0) { nbuf[np++] = static_cast<char>('0' + (ns % 10)); ns /= 10; }
            while (np > 0) Terminal::putchar(nbuf[--np]);
            Terminal::write(" ttl=64 time=");
            np = 0;
            uint64_t r = rtt;
            while (r > 0) { nbuf[np++] = static_cast<char>('0' + (r % 10)); r /= 10; }
            while (np > 0) Terminal::putchar(nbuf[--np]);
            Terminal::write(" ms\n");
        }

        uint64_t wait_end = arch::Timer::ticks() + 100;
        while (arch::Timer::ticks() < wait_end) arch::pause();
    }

    uint64_t elapsed = arch::Timer::ticks() - start_tick;
    Terminal::write("--- ");
    print_ip(dst);
    Terminal::write(" ping statistics ---\n");
    char nbuf[16];
    auto write_dec = [&](uint64_t v) {
        int np = 0;
        if (v == 0) { Terminal::putchar('0'); return; }
        while (v > 0) { nbuf[np++] = static_cast<char>('0' + (v % 10)); v /= 10; }
        while (np > 0) Terminal::putchar(nbuf[--np]);
    };
    write_dec(pkt_tx);
    Terminal::write(" packets transmitted, ");
    write_dec(pkt_rx);
    Terminal::write(" received, ");
    uint64_t loss = (pkt_tx > 0) ? ((pkt_tx - pkt_rx) * 100 / pkt_tx) : 0;
    write_dec(loss);
    Terminal::write("% packet loss, time ");
    write_dec(elapsed);
    Terminal::write(" ms\n");
}

void Shell::cmd_less(int argc, const char** argv) {
    if (argc < 2) {
        Terminal::write("Usage: less <path>\n");
        return;
    }

    auto* vn = kernel::vfs::resolve(argv[1]);
    if (!vn || !(vn->mode & kernel::vfs::S_IFREG)) {
        shell_error_path("less", argv[1], "No such file");
        return;
    }
    if (!vn->ops->read) return;

    static constexpr int LINES_PER_PAGE = 24;
    static constexpr int BUF_SIZE = 512;

    uint8_t buf[BUF_SIZE];
    uint64_t offset = 0;
    int line_count = 0;

    for (;;) {
        int64_t nread = vn->ops->read(*vn, buf, BUF_SIZE, offset);
        if (nread <= 0) break;
        offset += static_cast<uint64_t>(nread);

        for (int64_t i = 0; i < nread; ++i) {
            Terminal::putchar(static_cast<char>(buf[i]));

            if (buf[i] == '\n') {
                ++line_count;
                if (line_count >= LINES_PER_PAGE) {
                    Terminal::write("--More--");
                    char c = 0;
                    for (;;) {
                        bool got = arch::Keyboard::getchar(c);
                        if (got && (c == 'q' || c == 'Q')) {
                            Terminal::putchar('\n');
                            return;
                        }
                        if (got) break;
                        arch::pause();
                    }
                    Terminal::putchar('\n');
                    line_count = 0;
                }
            }
        }
    }

    char c = 0;
    for (;;) {
        bool got = arch::Keyboard::getchar(c);
        if (got && (c == 'q' || c == 'Q')) {
            Terminal::putchar('\n');
            return;
        }
        if (got) break;
        arch::pause();
    }
}

void Shell::cmd_cat(int argc, const char** argv) {
    if (argc < 2) {
        Terminal::write("Usage: cat <path>\n");
        return;
    }

    auto* vn = kernel::vfs::resolve(argv[1]);
    if (!vn || !(vn->mode & kernel::vfs::S_IFREG)) {
        shell_error_path("cat", argv[1], "No such file");
        return;
    }
    if (!vn->ops->read) {
        shell_error_path("cat", argv[1], "not readable");
        return;
    }

    static constexpr int BUF_SIZE = 512;
    uint8_t buf[BUF_SIZE];
    uint64_t offset = 0;

    for (;;) {
        int64_t nread = vn->ops->read(*vn, buf, BUF_SIZE, offset);
        if (nread <= 0)
            break;
        offset += static_cast<uint64_t>(nread);
        for (int64_t i = 0; i < nread; ++i)
            Terminal::putchar(static_cast<char>(buf[i]));
    }
}

void Shell::cmd_touch(int argc, const char** argv) {
    if (argc < 2) {
        Terminal::write("Usage: touch <path>\n");
        return;
    }

    auto r = kernel::vfs::create_err(argv[1], kernel::vfs::S_IFREG);
    if (r != kernel::errors::VFS_ERR_OK && r != kernel::errors::VFS_ERR_EXISTS) {
        shell_vfs_error("touch", r);
    }
}

void Shell::cmd_dmesg(int argc, const char** argv) {
    bool human = false;
    if (argc >= 2) {
        if (strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "--human") == 0) {
            human = true;
        } else if (strcmp(argv[1], "-i") == 0 || strcmp(argv[1], "-I") == 0 || strcmp(argv[1], "--inject") == 0) {
            if (argc < 4) {
                Terminal::write("usage: dmesg -i|--inject <subsys> <err> [msg]\n");
                return;
            }
            unsigned subsys = 0; {
                const char* s = argv[2]; while (*s) { subsys = subsys * 10 + (*s - '0'); ++s; }
            }
            unsigned code = 0; {
                const char* s = argv[3]; while (*s) { code = code * 10 + (*s - '0'); ++s; }
            }
            const char* msg = (argc >= 5) ? argv[4] : "(no message)";
            kernel::log::DmesgService::instance().push(
                static_cast<kernel::log::ErrorSubsystem>(subsys), code, msg, 0);
            Terminal::write("ok\n");
            return;
        }
    }
    size_t total = 0;
    kernel::log::DmesgService::instance().for_each(
        [&](const kernel::log::LogEntry& e) {
        char buf[256];
        char* p = buf;
        char* end = buf + sizeof(buf) - 1;

        if (human) {
            // Human-readable: "YYYY-MM-DD hh:mm:ss:mmm task  ERR_NAME  msg"
            // Convert ticks to wall-clock nanoseconds (assumes 1 tick = 1 ms = 1e6 ns)
            uint64_t wall_ns = kernel::gs::get_boot_epoch() * 1000000000ULL +
                               e.timestamp * 1000000ULL;
            char dt[24];
            format_datetime(dt, sizeof(dt), wall_ns);
            const char* dp = dt;
            while (*dp && p < end) *p++ = *dp++;
            *p++ = ' ';

            // Task name start position for padding
            char* name_start = p;
            auto* tcb = kernel::Scheduler::find_task(e.task_id);
            if (tcb && tcb->name[0]) {
                const char* nn = tcb->name;
                while (*nn && p < end) *p++ = *nn++;
            } else {
                *p++ = 'P'; *p++ = ':';
                uint64_t tid = e.task_id;
                char tbuf[24]; int tl = 0;
                if (tid == 0) tbuf[tl++] = '0';
                else { while (tid > 0 && tl < 23) { tbuf[tl++] = static_cast<char>('0' + (tid % 10)); tid /= 10; } }
                for (int i = 0; i < tl/2; ++i) { char c = tbuf[i]; tbuf[i] = tbuf[tl-1-i]; tbuf[tl-1-i] = c; }
                for (int i = 0; i < tl && p < end; ++i) *p++ = tbuf[i];
            }
            // Left-pad name column to 14 chars minimum
            while (p - name_start < 14 && p < end) *p++ = ' ';

            // Error name
            const char* sub = kernel::log::subsystem_name(e.subsystem);
            while (*sub && p < end) *p++ = *sub++;
            *p++ = ':';
            const char* err_name = kernel::log::error_string(e.subsystem, e.error_code);
            while (*err_name && p < end) *p++ = *err_name++;
            *p++ = ' ';

            // Message (inline context)
            const char *msg = e.message; // owned char array, never null
            while (*msg && p < end) *p++ = *msg++;
            *p++ = '\n';
            *p = '\0';

            Terminal::write(buf);
            total += p - buf;
            return;
        }

        // Technical format (default)
        *p++ = '['; *p++ = 'T'; *p++ = 'S'; *p++ = '=';
        uint64_t ts = e.timestamp;
        char tsbuf[24]; int tlen = 0;
        if (ts == 0) tsbuf[tlen++] = '0';
        else { while (ts > 0 && tlen < 23) { tsbuf[tlen++] = static_cast<char>('0' + (ts % 10)); ts /= 10; } }
        for (int i = 0; i < tlen/2; ++i) { char c = tsbuf[i]; tsbuf[i] = tsbuf[tlen-1-i]; tsbuf[tlen-1-i] = c; }
        for (int i = 0; i < tlen && p < end; ++i) *p++ = tsbuf[i];
        *p++ = ']'; *p++ = ' ';

        const char* task_s = "TASK=";
        while (*task_s && p < end) *p++ = *task_s++;
        uint64_t tid = e.task_id;
        char tidbuf[24]; int tidlen = 0;
        if (tid == 0) tidbuf[tidlen++] = '0';
        else { while (tid > 0 && tidlen < 23) { tidbuf[tidlen++] = static_cast<char>('0' + (tid % 10)); tid /= 10; } }
        for (int i = 0; i < tidlen/2; ++i) { char c = tidbuf[i]; tidbuf[i] = tidbuf[tidlen-1-i]; tidbuf[tidlen-1-i] = c; }
        for (int i = 0; i < tidlen && p < end; ++i) *p++ = tidbuf[i];
        *p++ = ' ';

        const char* err_s = "ERR=";
        while (*err_s && p < end) *p++ = *err_s++;
        const char* sub = kernel::log::subsystem_name(e.subsystem);
        while (*sub && p < end) *p++ = *sub++;
        *p++ = ':';
        const char* err_name = kernel::log::error_string(e.subsystem, e.error_code);
        while (*err_name && p < end) *p++ = *err_name++;
        *p++ = ' ';

        const char* ctx_s = "CTX=";
        while (*ctx_s && p < end) *p++ = *ctx_s++;
        uintptr_t ctx = e.context;
        *p++ = '0'; *p++ = 'x';
        for (int i = (sizeof(uintptr_t)*2)-1; i >= 0 && p < end; --i) {
            uint8_t nib = (ctx >> (i*4)) & 0xF;
            *p++ = static_cast<char>(nib < 10 ? '0' + nib : 'a' + (nib - 10));
        }
        *p++ = ':'; *p++ = ' ';

        const char *msg = e.message; // owned char array, never null
        while (*msg && p < end) *p++ = *msg++;
        *p++ = '\n';
        *p = '\0';

        Terminal::write(buf);
        total += p - buf;
    });
    if (total == 0) {
        Terminal::write("Kernel log buffer is empty.\n");
    }
}

void Shell::cmd_lspci(int /*argc*/, const char** /*argv*/) {
#if defined(CONFIG_ARCH_X86_64)
    Terminal::write("PCI Device Tree:\n");
    size_t n = arch::pci_device_count();
    if (n == 0) {
        Terminal::write("  No PCI devices found.\n");
        return;
    }
    const auto* devs = arch::pci_devices();
    for (size_t i = 0; i < n; ++i) {
        const auto& d = devs[i];
        char line[128];
        int pos = 0;
        auto add = [&](const char* s) { while (*s && pos < 120) line[pos++] = *s++; };
        auto hex = [&](uint64_t v) {
            char h[20]; int hp = 19; h[19] = 0;
            if (v == 0) { line[pos++] = '0'; return; }
            while (v > 0 && hp > 0) { h[--hp] = "0123456789ABCDEF"[v & 0xF]; v >>= 4; }
            while (h[hp]) line[pos++] = h[hp++];
        };
        add("  ");
        hex(d.bdf.bus); add(":"); hex(d.bdf.device); add("."); hex(d.bdf.function);
        add("  [");
        hex(d.vendor_id); add(":"); hex(d.device_id);
        add("]  ");
        Terminal::write(line);
        Terminal::write("\n");
    }
#else
    Terminal::write("PCI not supported on this architecture.\n");
#endif
}

} // namespace service
