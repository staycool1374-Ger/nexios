#pragma once

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

/// @file fb_backing.hpp
/// @brief Boot-time framebuffer backing store for non-x86 virtio-gpu/ramfb
/// backends (issue #394).  Guest-RAM scanout: the device reads pixels from
/// this buffer, so no VMM Device/UC remap is needed.
/// @note PMM-backed, NOT static BSS: a 4 MiB static backing trips the riscv64
/// boot.S fail-stop (`kernel_phys_end` must stay below 0x80C00000 for the
/// fixed .boot_stack_rv + PMM metadata slack).  PMM pages allocated during
/// boot precede the test-isolation baseline, so the ResourceTracker delta
/// stays zero — same discipline as virtio-blk queue pages.

#include <types.hpp>

#if !defined(CONFIG_ARCH_X86_64)

namespace kernel::fb {

/// Backing geometry bound (QEMU std VGA 1280x800x32; device modes above are
/// rejected before any resource is created).
constexpr uint32_t FB_MAX_WIDTH = 1280;
constexpr uint32_t FB_MAX_HEIGHT = 800;
constexpr uint32_t FB_MAX_BPP = 32;
constexpr uint32_t FB_MAX_BYTES = FB_MAX_WIDTH * FB_MAX_HEIGHT * (FB_MAX_BPP / 8);
constexpr uint32_t FB_MAX_PAGES = FB_MAX_BYTES / 4096;
static_assert(FB_MAX_BYTES % 4096 == 0, "backing must be page-multiple");
static_assert(FB_MAX_BYTES == 4096000, "1280x800x32 backing size");

/// @brief Allocate the backing store for a validated mode (BOOT_ONLY).
/// PMM-contiguous, zeroed, readable via backing_ptr() (HHDM alias).
/// @param width,height Validated mode geometry.
/// @return true on success (backing_ptr()/backing_phys() live until reboot).
bool backing_init(uint32_t width, uint32_t height);

/// @brief Release the backing store (failure paths only; success pins it).
void backing_release();

/// @brief Backing virtual address (HHDM alias, zeroed) or nullptr.
/// @return VA of the backing store.
uint8_t *backing_ptr();

/// @brief Backing size in bytes (page-rounded mode span) or 0.
uint32_t backing_bytes();

/// @brief Physical address of backing page @p page_idx (0-based).
/// @param page_idx Page index (< page count).
/// @param out_phys Receives the page phys on success.
/// @return true on success (fails closed on bounds errors).
bool backing_phys(uint32_t page_idx, uint64_t &out_phys);

/// @brief Allocated page count (0 when uninitialised).
uint32_t backing_pages();

} // namespace kernel::fb

#endif // !CONFIG_ARCH_X86_64
