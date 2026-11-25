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

/// @file proc_errors.hpp
/// @brief Process-manager (F11, #564) error codes (X-macro table).
///        Every refusal at the proc-mgr boundary (hold overflow, stale
///        generation, server down, RPC bound, malformed verdict, refused
///        stop) carries one of these codes — never a generic -1. Stage A
///        producers: hold/lookup/release/release_on_death
///        (proc_tcb_caps.cpp), the verdict validator (proc_svc.cpp
///        handle_decision) and the bounded proxy Call (proc_mgr_proxy.cpp,
///        `#if CONFIG_PROC_MGR` only). Handlers translate to
///        SYS_ERR_GENERIC_FAIL at the syscall boundary.

#pragma once

#include <types.hpp>
#include <assert.hpp>

namespace kernel::errors {

#define PROC_ERROR_CODES                                                       \
    X(OK, 0, "OK")                                                             \
    X(NO_SERVER, 1, "Proc-mgr server unavailable")                              \
    X(RPC_BOUND, 2, "Proc-mgr RPC bound expired")                              \
    X(STALE_PID, 3, "Proc-mgr hold target stale or recycled")                   \
    X(CAP_TABLE_FULL, 4, "Proc-mgr hold table full (child refused)")            \
    X(HOLD_INSTALL_FAILED, 5, "Proc-mgr hold install rolled back")              \
    X(BAD_REQUEST, 6, "Proc-mgr malformed verdict or request")                  \
    X(STOP_REFUSED, 7, "Proc-mgr stop refused (no PD policy yet)")                             \
    X(SELF_REFUSED, 8, "Proc-mgr self-call refused (no self-RPC)")

/// @brief Proc-mgr boundary errors.
// NOLINTNEXTLINE(performance-enum-size)
enum ProcError : uint64_t {
#define X(name, num, msg) PROC_ERR_##name = (num),
    PROC_ERROR_CODES
#undef X
};

/// @brief OK anchor: success is zero by convention.
static_assert(PROC_ERR_OK == 0, "proc error OK must be 0");

/// @brief Return a human-readable string for a proc-mgr error code.
template <> inline const char *error_string(ProcError err) {
    switch (err) {
#define X(name, num, msg)                                                      \
    case PROC_ERR_##name:                                                      \
        return msg;
        PROC_ERROR_CODES
#undef X
    }
    return "Unknown proc-mgr error";
}

} // namespace kernel::errors
