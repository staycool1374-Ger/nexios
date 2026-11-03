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

/// @file cap/nexios_cap.h
/// @brief Ring-3 capability lifecycle + mapping API (allow-list only).
/// @details Typed wrappers over the generated P2 stubs
///        (gen/nexios_syscalls.h). Opaque handles only: success values
///        are passed back to the kernel verbatim, never inspected or
///        forged. Every op fails closed (codes, never ENSURE):
///        handles, mapping addresses, IRQ vectors and OK(0) pass
///        through; GENERIC_FAIL maps to -1 with errno set (the spec
///        section 2 predicate, shared with the picolib_stubs map_err
///        precedent). Destination binding: dst must be a CNode handle
///        in the caller's own CSpace (GRANT right for grant, COPY for
///        copy/mint); anything else fails closed kernel-side at lookup
///        (spec section 12.5). Endpoint send/recv is NOT declared here:
///        it is the existing ipc.h API (SYS_SEND/RECEIVE address task
///        ids, not cap handles). Never exposed (no declarations, by
///        design): raw create-from-phys/vector, CNode::install/remove,
///        revoke cascades, IOPORT_GRANT, CAP_RETYPE (frames arrive via
///        creator seeding, cf. the P4 bootstrapping rule).

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/// @brief Opaque capability handle (LP64: 64-bit on every target).
///        Only -1 is special (failure, errno set); all other values
///        are kernel-issued and passed back verbatim.
typedef unsigned long nexios_cap_t;

/// @brief Rights bits (mirror kernel CapRights, ABI-pinned).
#define NEXIOS_CAP_READ (1UL << 0)
#define NEXIOS_CAP_WRITE (1UL << 1)
#define NEXIOS_CAP_GRANT (1UL << 2)
#define NEXIOS_CAP_COPY (1UL << 3)

/// @brief IRQ delivery modes (mirror kernel IrqDeliveryMode).
#define NEXIOS_IRQ_WAIT 0UL
#define NEXIOS_IRQ_NOTIFY 1UL

/// @brief Grant src into the CNode addressed by dst.
/// @return new opaque handle, or -1 with errno set.
long nexios_cap_grant(nexios_cap_t src, nexios_cap_t dst);

/// @brief Copy src into the CNode addressed by dst.
/// @return new opaque handle, or -1 with errno set.
long nexios_cap_copy(nexios_cap_t src, nexios_cap_t dst);

/// @brief Revoke handle (idempotent kernel-side).
/// @return 0, or -1 with errno set.
long nexios_cap_revoke(nexios_cap_t handle);

/// @brief Copy src into dst with reduced rights mask + badge.
/// @return new opaque handle, or -1 with errno set.
long nexios_cap_mint(nexios_cap_t src, nexios_cap_t dst,
                     unsigned long rights, unsigned long badge);

/// @brief Map the MMIO range behind cap into the caller window.
/// @return mapping address, or -1 with errno set.
long nexios_cap_mmio_map(nexios_cap_t cap);

/// @brief Unmap a previous MMIO mapping by address.
/// @return 0, or -1 with errno set.
long nexios_cap_mmio_unmap(unsigned long addr);

/// @brief Arm IRQ delivery on cap (WAIT or NOTIFY mode).
/// @return 0, or -1 with errno set.
long nexios_cap_irq_register(nexios_cap_t cap, unsigned long mode);

/// @brief Block until the IRQ behind cap fires (WAIT mode only).
/// @return firing vector, or -1 with errno set.
long nexios_cap_irq_wait(nexios_cap_t cap);

/// @brief Program one frame mapping into the DMA domain.
/// @return 0, or -1 with errno set.
long nexios_cap_iommu_map(nexios_cap_t dma, nexios_cap_t frame);

/// @brief Remove one frame mapping from the DMA domain.
/// @return 0, or -1 with errno set.
long nexios_cap_iommu_unmap(nexios_cap_t dma, nexios_cap_t frame);

/// @brief Allocate count contiguous user frames as a FrameCap.
/// @return new opaque handle, or -1 with errno set.
long nexios_cap_frame_create(unsigned long count);

/// @brief Map the frames behind cap into the caller window.
/// @return mapping address, or -1 with errno set.
long nexios_cap_frame_map(nexios_cap_t cap);

/// @brief Unmap a previous frame mapping by address.
/// @return 0, or -1 with errno set.
long nexios_cap_frame_unmap(unsigned long addr);

#ifdef __cplusplus
}
#endif
