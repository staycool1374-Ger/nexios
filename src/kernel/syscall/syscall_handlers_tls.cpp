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

/// @file syscall_handlers_tls.cpp
/// @brief TLS_SET(85) handler (issue #74): store the caller's
/// thread-local-storage base. Scalar only — no user-pointer dereference —
/// but stays out of k_syscall_fast[] (issue #92 discipline: the FAST list
/// changes only via audited pointer-free review + gate).

#include <kernel/syscall/syscall.hpp>
#include <kernel/syscall/syscall_errors.hpp>
#include <kernel/syscall/syscall_helpers.hpp>
#include <kernel/task/scheduler.hpp>
#include <kernel/task/task.hpp>
#include <kernel/memory/address.hpp>
#include <kernel/nexios_config.h>

namespace kernel {

/// @brief Set the calling task's TLS base (issue #74).
///        arg0 = user VA of the thread block, 0 = unset (clears).
///        Validation is per-arch (spec §7): x86_64 48-bit canonical +
///        47-bit user-half limit; aarch64 48-bit limit (subsumes
///        canonical — is_canonical() is x86-48-bit-specific and would
///        wrongly reject valid high user VAs); riscv64 Sv39 explicit
///        bits[63:39]==0 + 40-bit limit. Reachable failures return the
///        dedicated code (never ENSURE). Scalar store via
///        Scheduler::set_tls_base_err (IrqGuard + scheduler lock, live
///        register apply when self); never blocks, never reschedules.
/// @return 0, -SYS_ERR_TLS_INVALID_BASE, -SYS_ERR_SCHED_NO_CURRENT.
uint64_t Syscall::sys_tls_set(uint64_t arg0, uint64_t, uint64_t, uint64_t,
                              uint64_t *) {
    auto *t = syscall_task();
    if (t == nullptr)
        return static_cast<uint64_t>(-errors::SYS_ERR_SCHED_NO_CURRENT);
    if (arg0 != 0) {
#if defined(CONFIG_ARCH_X86_64)
        if (!VirtualAddress(arg0).is_canonical())
            return static_cast<uint64_t>(-errors::SYS_ERR_TLS_INVALID_BASE);
#elif defined(CONFIG_ARCH_RISCV64)
        // Sv39 user half: bits[63:39] must be zero (the 40-bit
        // CONFIG_USER_SPACE_LIMIT alone admits non-canonical values).
        if ((arg0 >> 39) != 0)
            return static_cast<uint64_t>(-errors::SYS_ERR_TLS_INVALID_BASE);
#endif
        // User-half gate on all archs (arch-appropriate LIMIT).
        if (arg0 >= CONFIG_USER_SPACE_LIMIT)
            return static_cast<uint64_t>(-errors::SYS_ERR_TLS_INVALID_BASE);
    }
    if (Scheduler::set_tls_base_err(*t, arg0) != errors::SCHED_ERR_OK)
        return static_cast<uint64_t>(-errors::SYS_ERR_SCHED_NO_CURRENT);
    return 0;
}

} // namespace kernel
