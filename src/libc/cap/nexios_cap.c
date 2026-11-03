/*
 * NexIOS RTOS — Capability-API Ring-3 subset v0.1 (issue #171 P3).
 *
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

/// @file cap/nexios_cap.c
/// @brief Allow-list wrappers over the generated P2 stubs. Each wrapper
///        forwards its arguments verbatim and returns the stub result
///        unchanged: handles, mapping addresses, IRQ vectors and OK(0)
///        pass through; failures arrive as -1 with errno set (the stub
///        map_err already applies the section 2 predicate). No branches,
///        no ENSURE, no handle inspection: every policy decision
///        (rights, dst binding, table-full) fails closed kernel-side at
///        lookup/install time, so there is nothing to re-check here.

#include "nexios_cap.h"

#include <gen/nexios_syscalls.h>

long nexios_cap_grant(nexios_cap_t src, nexios_cap_t dst) {
    return nexios_sys_cap_grant((long)src, (long)dst, 0, 0);
}

long nexios_cap_copy(nexios_cap_t src, nexios_cap_t dst) {
    return nexios_sys_cap_copy((long)src, (long)dst, 0, 0);
}

long nexios_cap_revoke(nexios_cap_t handle) {
    return nexios_sys_cap_revoke((long)handle, 0, 0, 0);
}

long nexios_cap_mint(nexios_cap_t src, nexios_cap_t dst,
                     unsigned long rights, unsigned long badge) {
    return nexios_sys_cap_mint((long)src, (long)dst,
                               (long)rights, (long)badge);
}

long nexios_cap_mmio_map(nexios_cap_t cap) {
    return nexios_sys_mmio_map((long)cap, 0, 0, 0);
}

long nexios_cap_mmio_unmap(unsigned long addr) {
    return nexios_sys_mmio_unmap((long)addr, 0, 0, 0);
}

long nexios_cap_irq_register(nexios_cap_t cap, unsigned long mode) {
    return nexios_sys_irq_register((long)cap, (long)mode, 0, 0);
}

long nexios_cap_irq_wait(nexios_cap_t cap) {
    return nexios_sys_irq_wait((long)cap, 0, 0, 0);
}

long nexios_cap_iommu_map(nexios_cap_t dma, nexios_cap_t frame) {
    return nexios_sys_iommu_map((long)dma, (long)frame, 0, 0);
}

long nexios_cap_iommu_unmap(nexios_cap_t dma, nexios_cap_t frame) {
    return nexios_sys_iommu_unmap((long)dma, (long)frame, 0, 0);
}

long nexios_cap_frame_create(unsigned long count) {
    return nexios_sys_frame_create((long)count, 0, 0, 0);
}

long nexios_cap_frame_map(nexios_cap_t cap) {
    return nexios_sys_frame_map((long)cap, 0, 0, 0);
}

long nexios_cap_frame_unmap(unsigned long addr) {
    return nexios_sys_frame_unmap((long)addr, 0, 0, 0);
}
