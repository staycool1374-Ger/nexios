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

/// @file kslot_backend.cpp (riscv64)
/// @brief #434 P1: rv64 kslot window backend — STUB (non-goal this issue).
/// wire_window_tables() returns false so alloc_kslot() takes the documented
/// exhaustion-0 path (general mechanism, no arch guard in callers). The
/// interface admits a future rv64 instantiation without caller changes.
/// Build-selected via arch/<arch>/ discovery; no CONFIG_ARCH guard here.

#include <kernel/task/kslot_backend.hpp>
#include <types.hpp>

namespace kslot_backend {

bool wire_window_tables() { return false; }
void map_window_page(uint64_t virt, uint64_t phys) {
    (void)virt;
    (void)phys;
}
void unmap_window_page(uint64_t virt) { (void)virt; }
void flush_window_va(uint64_t virt) { (void)virt; }
size_t snapshot_bytes() { return 0; }
void snapshot_capture(uint8_t *dst) { (void)dst; }
void snapshot_restore(const uint8_t *src) { (void)src; }

} // namespace kslot_backend
