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

/// @file errno.c
/// @brief Thread-local errno backend (issue #338).  The per-task block
/// (struct nexios_tcb) is allocated by crt0 and published via TLS_SET;
/// the kernel reloads TP on every dispatch, so this read is always the
/// current task's cell — including future same-address-space threads,
/// which will carry distinct TP values.

#include <errno.h>
#include <sys/types.h>

/// @brief Read the thread-pointer register (userspace-readable on all
/// arches: RDFSBASE needs CR4.FSGSBASE, enabled at boot on x86_64).
static uintptr_t read_tp(void) {
#if defined(__x86_64__)
    unsigned long v = 0;
    __asm__ volatile("rdfsbase %0" : "=r"(v));
    return (uintptr_t)v;
#elif defined(__aarch64__)
    unsigned long v = 0;
    __asm__ volatile("mrs %0, tpidr_el0" : "=r"(v));
    return (uintptr_t)v;
#elif defined(__riscv)
    uintptr_t v = 0;
    __asm__ volatile("mv %0, tp" : "=r"(v));
    return v;
#else
    return 0;
#endif
}

int *__errno_location(void) {
    return &((struct nexios_tcb *)read_tp())->errno_value;
}
