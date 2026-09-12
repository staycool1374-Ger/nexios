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

/// @file signal.hpp
/// @brief Signal definitions used by the kernel for user exception delivery.

#pragma once

#include <types.hpp>

// NOLINTBEGIN(performance-no-int-to-ptr,bugprone-branch-clone)
namespace kernel {

/// @brief Standard POSIX-like signal numbers.
enum class Signal : uint8_t {
    SIG_NONE    = 0,
    SIGHUP      = 1,
    SIGINT      = 2,
    SIGQUIT     = 3,
    SIGILL      = 4,
    SIGTRAP     = 5,
    SIGABRT     = 6,
    SIGBUS      = 7,
    SIGFPE      = 8,
    SIGKILL     = 9,
    SIGUSR1     = 10,
    SIGSEGV     = 11,
    SIGUSR2     = 12,
    SIGPIPE     = 13,
    SIGALRM     = 14,
    SIGTERM     = 15,
    SIGSTKFLT   = 16,
    SIGCHLD     = 17,
    SIGCONT     = 18,
    SIGSTOP     = 19,
    SIGTSTP     = 20,
    SIGSYS      = 31,
};

/// @brief Signal handler type (user-space function pointer).
using sighandler_t = void (*)(int signal);

/// @brief Default signal actions taken when no user handler is installed.
enum class SignalAction : uint8_t {
    TERMINATE,
    IGNORE,
    STOP,
    CONT,
};

/// @brief Maps a CPU exception vector to a signal number and action.
struct ExceptionSignalMap {
    Signal signal;
    SignalAction action;
    const char* name;
};

/// @brief Returns the signal mapping for a given CPU exception vector.
static inline ExceptionSignalMap exception_to_signal(uint64_t vector) {
    switch (vector) {
        case 0:  return {Signal::SIGFPE,  SignalAction::TERMINATE, "SIGFPE"};   // #DE
        case 4:  return {Signal::SIGILL,  SignalAction::TERMINATE, "SIGILL"};   // #OF
        case 5:  return {Signal::SIGSEGV, SignalAction::TERMINATE, "SIGSEGV"};  // #BR
        case 6:  return {Signal::SIGILL,  SignalAction::TERMINATE, "SIGILL"};   // #UD
        case 7: case 8: case 10: case 11: case 12: case 13: case 14: case 17:
            return {Signal::SIGSEGV, SignalAction::TERMINATE, "SIGSEGV"};       // #NM..#AC
        case 16: case 19:
            return {Signal::SIGFPE,  SignalAction::TERMINATE, "SIGFPE"};         // #MF, #XF
        default: return {Signal::SIGSYS,  SignalAction::TERMINATE, "SIGSYS"};
    }
}

/// @brief Maximum number of signal handlers per task (one per signal number).
static constexpr size_t MAX_SIGNAL_HANDLERS = CONFIG_MAX_SIGNAL_HANDLERS;

/// @brief Global recovery IP for safe user memory access.
///        Non-zero means we are in a user memory access recovery zone.
///        If a fault occurs and this is set, execution is redirected here.
extern "C" constinit uint64_t g_user_access_recover_ip;

/// @brief Signal frame placed on the user stack when delivering a signal.
///        The handler returns via SYS_SIGRETURN which restores this frame.
struct SignalFrame {
    uint64_t sig;          ///< Signal number delivered
    uint64_t saved_rip;    ///< Original RIP to restore on sigreturn
    uint64_t saved_rsp;    ///< Original RSP to restore on sigreturn
    uint64_t saved_rflags; ///< Original RFLAGS to restore on sigreturn
    uint64_t saved_cs;     ///< Original CS
    uint64_t saved_ss;     ///< Original SS
    uint64_t reserved[2];  ///< For future expansion (16-byte alignment padding)
};

/// @brief Maximum signal frame size (pushed on user stack).
static constexpr size_t SIGNAL_FRAME_SIZE = sizeof(SignalFrame);

/// @brief Returns true if a signal code indicates immediate termination
///        (SIGKILL cannot be caught or ignored).
static inline bool signal_is_fatal(uint64_t sig) {
    return sig == static_cast<uint64_t>(Signal::SIGKILL);
}

/// @brief Default signal action dispatcher.
static inline SignalAction default_signal_action(uint64_t sig) {
    switch (static_cast<Signal>(sig)) {
    case Signal::SIGCHLD:
    case Signal::SIGCONT:
        return SignalAction::IGNORE;
    case Signal::SIGSTOP:
    case Signal::SIGTSTP:
        return SignalAction::STOP;
    default:
        return SignalAction::TERMINATE;
    }
}

} // namespace kernel
// NOLINTEND(performance-no-int-to-ptr,bugprone-branch-clone)
