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

/// @file shell.hpp
/// @brief Interactive command-line shell with built-in and user commands.

#pragma once

#include <types.hpp>

namespace service {

/// @brief Interactive shell with command registration and dispatch.
class Shell {
public:
    /// @brief Initialises the shell and registers built-in commands.
    static void init();
    /// @brief Parses and executes a command string.
    /// @param cmd The command line to execute.
    static void execute(const char* cmd);

    /// @brief Type for shell command handler functions.
    using CommandFunc = void (*)(int argc, const char** argv);
    /// @brief Describes a registered shell command.
    struct Command {
        const char* name;
        const char* help;
        CommandFunc func;
    };

    /// @brief Registers a new shell command.
    /// @param name Command name (used for matching).
    /// @param help Help text shown in "help" command.
    /// @param func Handler function.
    static void register_command(const char* name, const char* help, CommandFunc func);
    /// @brief Main shell loop (reads input, dispatches commands).
    static void shell_task_main();

private:
    static constexpr size_t MAX_COMMANDS = 64;
    static constexpr size_t MAX_ARGS = 16;
    static constexpr size_t BUF_SIZE = 256;
    static constexpr size_t MAX_ENV = 32;

    // NOLINTBEGIN(bugprone-dynamic-static-initializers)
    static Command commands_[MAX_COMMANDS];
    static size_t num_commands_;
    static bool initialized_;
    static char work_dir_[BUF_SIZE];
    static char env_[MAX_ENV][BUF_SIZE];
    static size_t env_count_;
    static int last_exit_code_;

    static constexpr size_t MAX_ALIASES = 32;
    static constexpr size_t MAX_HISTORY = 100;
    static constexpr size_t MAX_DIR_STACK = 16;

    struct AliasEntry {
        char name[64];
        char value[256];
        bool used = false;
    };
    struct HistoryEntry {
        char cmd[BUF_SIZE];
        bool used = false;
    };
    struct TrapEntry {
        int signo;
        char handler[256];
        bool used = false;
    };

    static AliasEntry aliases_[MAX_ALIASES];
    static size_t alias_count_;
    static HistoryEntry history_[MAX_HISTORY];
    static size_t history_count_;
    static size_t history_head_;
    static char dir_stack_[MAX_DIR_STACK][BUF_SIZE];
    static size_t dir_stack_count_;
    static int shell_options_;
    static int positional_argc_;
    static char* positional_argv_[32];
    static int umask_;
    static TrapEntry traps_[32];
    // NOLINTEND(bugprone-dynamic-static-initializers)

    /// @brief Parses a command line and dispatches to the matching command.
    /// @param line Raw input line to parse.
    /// @return 0 on success (command found), 1 on failure (unknown command).
    static int parse_and_exec(const char* line);

    /// @brief Built-in: lists available commands.
    static void cmd_help(int argc, const char** argv);
    /// @brief Built-in: clears the terminal.
    static void cmd_clear(int argc, const char** argv);
    /// @brief Built-in: prints its arguments.
    static void cmd_echo(int argc, const char** argv);
    /// @brief Built-in: displays system uptime.
    static void cmd_uptime(int argc, const char** argv);
    /// @brief Built-in: lists running tasks.
    static void cmd_tasks(int argc, const char** argv);
    /// @brief Built-in: shows memory usage info.
    static void cmd_meminfo(int argc, const char** argv);
    /// @brief Built-in: reboots the system.
    static void cmd_reboot(int argc, const char** argv);
    /// @brief Built-in: runs a program by name.
    static void cmd_run(int argc, const char** argv);
    /// @brief Built-in: shows kernel version.
    static void cmd_version(int argc, const char** argv);
    /// @brief Built-in: lists background jobs.
    static void cmd_jobs(int argc, const char** argv);
    /// @brief Built-in: loads a kernel driver.
    static void cmd_modprobe(int argc, const char** argv);
    /// @brief Built-in: lists available drivers.
    static void cmd_modlist(int argc, const char** argv);
    /// @brief Built-in: lists installable programs.
    static void cmd_listprog(int argc, const char** argv);
    /// @brief Built-in: change working directory.
    static void cmd_cd(int argc, const char** argv);
    /// @brief Built-in: set environment variable.
    static void cmd_export(int argc, const char** argv);
    /// @brief Built-in: run a userspace ELF from initrd as a new task.
    static void cmd_runelf(int argc, const char** argv);
    /// @brief Built-in: start a background ELF load (returns immediately).
    static void cmd_load(int argc, const char** argv);
    /// @brief Built-in: cancel the in-flight background ELF load.
    static void cmd_cancel_load(int argc, const char** argv);
    /// @brief Built-in: shut down the system.
    static void cmd_exit(int argc, const char** argv);
    /// @brief Built-in: run kernel self-tests.
    static void cmd_selftest(int argc, const char** argv);
    /// @brief Built-in: print working directory.
    static void cmd_pwd(int argc, const char** argv);
    /// @brief Built-in: locate a command.
    static void cmd_which(int argc, const char** argv);
    /// @brief Built-in: print environment variables.
    static void cmd_env(int argc, const char** argv);
    /// @brief Built-in: sleep for N seconds.
    static void cmd_sleep(int argc, const char** argv);
    /// @brief Built-in: create a directory.
    static void cmd_mkdir(int argc, const char** argv);
    /// @brief Built-in: remove a file.
    static void cmd_rm(int argc, const char** argv);
    /// @brief Built-in: remove an empty directory.
    static void cmd_rmdir(int argc, const char** argv);
    /// @brief Built-in: define or list aliases.
    static void cmd_alias(int argc, const char** argv);
    /// @brief Built-in: remove an alias.
    static void cmd_unalias(int argc, const char** argv);
    /// @brief Built-in: show command history.
    static void cmd_history(int argc, const char** argv);
    /// @brief Built-in: show command type.
    static void cmd_type(int argc, const char** argv);
    /// @brief Built-in: execute a script file.
    static void cmd_source(int argc, const char** argv);
    /// @brief Built-in: set/unset shell options.
    static void cmd_set(int argc, const char** argv);
    /// @brief Built-in: read a line into variable(s).
    static void cmd_read(int argc, const char** argv);
    /// @brief Built-in: formatted output.
    static void cmd_printf(int argc, const char** argv);
    /// @brief Built-in: evaluate conditional expression.
    static void cmd_test(int argc, const char** argv);
    /// @brief Built-in: shift positional parameters.
    static void cmd_shift(int argc, const char** argv);
    /// @brief Built-in: trap signals.
    static void cmd_trap(int argc, const char** argv);
    /// @brief Built-in: wait for background jobs.
    static void cmd_wait(int argc, const char** argv);
    /// @brief Built-in: bring job to foreground.
    static void cmd_fg(int argc, const char** argv);
    /// @brief Built-in: send job to background.
    static void cmd_bg(int argc, const char** argv);
    /// @brief Built-in: remove job from table.
    static void cmd_disown(int argc, const char** argv);
    /// @brief Built-in: resource limits.
    static void cmd_ulimit(int argc, const char** argv);
    /// @brief Built-in: file creation mask.
    static void cmd_umask(int argc, const char** argv);
    /// @brief Built-in: process times.
    static void cmd_times(int argc, const char** argv);
    /// @brief Built-in: exit login shell.
    static void cmd_logout(int argc, const char** argv);
    /// @brief Built-in: directory stack.
    static void cmd_dirs(int argc, const char** argv);
    /// @brief Built-in: push directory to stack.
    static void cmd_pushd(int argc, const char** argv);
    /// @brief Built-in: pop directory from stack.
    static void cmd_popd(int argc, const char** argv);
    /// @brief Built-in: list directory contents.
    static void cmd_ls(int argc, const char** argv);
    /// @brief Built-in: show/configure network interface.
    static void cmd_ifconfig(int argc, const char** argv);
    /// @brief Built-in: ping a host.
    static void cmd_ping(int argc, const char** argv);
    /// @brief Built-in: page through a file.
    static void cmd_less(int argc, const char** argv);
    /// @brief Built-in: print a file to stdout.
    static void cmd_cat(int argc, const char** argv);
    /// @brief Built-in: create an empty file.
    static void cmd_touch(int argc, const char** argv);
    /// @brief Built-in: print kernel log buffer.
    static void cmd_dmesg(int argc, const char** argv);
    /// @brief Built-in: list PCI devices.
    static void cmd_lspci(int argc, const char** argv);
};

} // namespace service
