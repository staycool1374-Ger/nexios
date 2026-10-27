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

/// @file kslot_backend.hpp
/// @brief #434 P1: arch-neutral kslot window backend interface (SPEC §3.1).
///
/// Shared mechanism (slot pool, first-fit+bump, guard+stack layout, snapshot
/// framing, teardown order) lives once in task.cpp and calls ONLY these
/// hooks. Per-arch mechanics (window table wiring, leaf flags, TLB
/// maintenance) live in build-selected TUs:
///   src/kernel/arch/x86_64/kslot_backend.cpp
///   src/kernel/arch/aarch64/kslot_backend.cpp
///   src/kernel/arch/riscv64/kslot_backend.cpp
/// C3 binding: zero CONFIG_ARCH/#if ARCH in task.cpp alloc/free/map/call
/// paths outside this backend. Per-arch nexios_config.h window defaults are
/// permitted config (not a memory-path carve-out).

#pragma once

#include <types.hpp>

namespace kslot_backend {

// Wire the window tables into the boot kernel PML4/TTBR1 (lazy, once).
// Returns true when the window is usable; false means unusable on this
// arch/build (caller takes the exhaustion-0 path — no arch guard outside).
// P1-C1: all-or-nothing re-entry — partial wiring completes on retry,
// usable is reported only when fully wired; failure is loud-once
// (one-time serial/boot diagnostic), never silent.
// Must be idempotent: a second call with tables present re-adopts them.
bool wire_window_tables();

// Map/unmap one 4 KiB kernel-stack page at @p virt.
// Out-of-window VAs are a silent no-op (matches x86 pt_idx>=8 rule).
void map_window_page(uint64_t virt, uint64_t phys);
void unmap_window_page(uint64_t virt);

// TLB maintenance for one window VA (invlpg / tlbi vae1 / sfence.vma).
void flush_window_va(uint64_t virt);

// Snapshot support: byte size, capture, restore (restore flushes each VA
// via flush_window_va). Layout is backend-private; task.cpp treats it as
// opaque bytes appended after its own pool/bump/lists framing.
size_t snapshot_bytes();
void snapshot_capture(uint8_t *dst);
void snapshot_restore(const uint8_t *src);

} // namespace kslot_backend
