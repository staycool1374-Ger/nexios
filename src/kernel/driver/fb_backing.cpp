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

/// @file fb_backing.cpp
/// @brief Boot-time PMM-backed framebuffer store (issue #394).

#include <kernel/driver/fb_backing.hpp>

#if !defined(CONFIG_ARCH_X86_64)

#include <kernel/arch/io.hpp>
#include <kernel/memory/pmm.hpp>
#include <constants.hpp>
#include <string.hpp>

namespace kernel::fb {

namespace {

uint64_t g_backing_phys = 0;
uint64_t g_backing_virt = 0;
uint32_t g_backing_pages = 0;

} // namespace

bool backing_init(uint32_t width, uint32_t height) {
    if (g_backing_phys != 0) {
        return g_backing_virt != 0;
    }
    if (width == 0 || height == 0 || width > FB_MAX_WIDTH ||
        height > FB_MAX_HEIGHT) {
        return false;
    }
    uint64_t span = static_cast<uint64_t>(width) * height * 4;
    if (span == 0 || span > FB_MAX_BYTES) {
        return false;
    }
    uint32_t pages = static_cast<uint32_t>((span + 4095) / 4096);
    if (pages == 0 || pages > FB_MAX_PAGES) {
        return false;
    }
    uint64_t phys = PMM::alloc_contiguous(pages);
    if (!phys) {
        return false;
    }
    g_backing_phys = phys;
    g_backing_virt = arch::HHDM_OFFSET + phys;
    g_backing_pages = pages;
    // NOLINTNEXTLINE(performance-no-int-to-ptr)
    memset(reinterpret_cast<void *>(g_backing_virt), 0,
           static_cast<size_t>(pages) * 4096);
    return true;
}

void backing_release() {
    if (g_backing_phys == 0)
        return;
    for (uint32_t i = 0; i < g_backing_pages; ++i) {
        PMM::free_page(g_backing_phys + static_cast<uint64_t>(i) * 4096);
    }
    g_backing_phys = 0;
    g_backing_virt = 0;
    g_backing_pages = 0;
}

uint8_t *backing_ptr() {
    // NOLINTNEXTLINE(performance-no-int-to-ptr)
    return reinterpret_cast<uint8_t *>(g_backing_virt);
}

uint32_t backing_bytes() {
    return g_backing_pages * 4096;
}

bool backing_phys(uint32_t page_idx, uint64_t &out_phys) {
    out_phys = 0;
    if (g_backing_phys == 0 || page_idx >= g_backing_pages)
        return false;
    out_phys = g_backing_phys + static_cast<uint64_t>(page_idx) * 4096;
    return true;
}

uint32_t backing_pages() {
    return g_backing_pages;
}

} // namespace kernel::fb

#endif // !CONFIG_ARCH_X86_64
