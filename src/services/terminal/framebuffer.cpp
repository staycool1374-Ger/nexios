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

#include <services/terminal/framebuffer.hpp>
#include <services/terminal/font.hpp>
#if defined(CONFIG_ARCH_X86_64)
#include <kernel/multiboot2.hpp>
#endif
#include <kernel/memory/vmm.hpp>
#include <assert.hpp>
#include <constants.hpp>

namespace service {

FramebufferInfo Framebuffer::info_ = {};
bool Framebuffer::initialized_ = false;

bool Framebuffer::init() {
#if defined(CONFIG_ARCH_X86_64)
    uint64_t tag_addr = mb2_find_tag(8);
    if (!tag_addr) return false;

    // NOLINTNEXTLINE(performance-no-int-to-ptr)
    auto* tag = reinterpret_cast<FramebufferTag*>(tag_addr);
    if (tag->type_specific != 1 && tag->type_specific != 2) return false;

    info_.addr   = tag->addr;
    info_.pitch  = tag->pitch;
    info_.width  = tag->width;
    info_.height = tag->height;
    info_.bpp    = tag->bpp;

    uint64_t fb_size = static_cast<uint64_t>(info_.pitch) * info_.height;
    uint64_t fb_start = info_.addr & ~0xFFFULL;
    uint64_t fb_end = (info_.addr + fb_size + 0xFFF) & ~0xFFFULL;
    for (uint64_t page = fb_start; page < fb_end; page += 0x1000) {
        kernel::VMM::map_page(page, page, false);
        kernel::VMM::map_page(arch::HHDM_OFFSET + page, page, false);
    }

    info_.addr = arch::HHDM_OFFSET + info_.addr;

    initialized_ = true;
    clear(0x000000);
    return true;
#else
    return false;
#endif
}

void Framebuffer::clear(uint32_t color) {
    if (!initialized_) return;

    // NOLINTNEXTLINE(performance-no-int-to-ptr)
    auto* fb = reinterpret_cast<uint8_t*>(static_cast<uint64_t>(info_.addr));
    uint32_t bpp_bytes = info_.bpp / 8;

    for (uint32_t y = 0; y < info_.height; ++y) {
        for (uint32_t x = 0; x < info_.width; ++x) {
            size_t offset = y * info_.pitch + x * bpp_bytes;
            fb[offset + 0] = (color >> 0) & 0xFF;
            fb[offset + 1] = (color >> 8) & 0xFF;
            fb[offset + 2] = (color >> 16) & 0xFF;
            if (bpp_bytes > 3) fb[offset + 3] = 0xFF;
        }
    }
}

void Framebuffer::draw_pixel(uint32_t x, uint32_t y, uint32_t color) {
    if (!initialized_ || x >= info_.width || y >= info_.height) return;
    put_pixel(x, y, color);
}

// NOLINTBEGIN(bugprone-easily-swappable-parameters)
void Framebuffer::fill_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t color)
// NOLINTEND(bugprone-easily-swappable-parameters)
{
    for (uint32_t dy = 0; dy < h; ++dy) {
        for (uint32_t dx = 0; dx < w; ++dx) {
            draw_pixel(x + dx, y + dy, color);
        }
    }
}

// NOLINTBEGIN(bugprone-easily-swappable-parameters)
void Framebuffer::draw_char(uint32_t x, uint32_t y, char c, uint32_t fg, uint32_t bg)
// NOLINTEND(bugprone-easily-swappable-parameters)
{
    if (!initialized_) return;
    if (c < 32 || c > 126) c = ' ';

    size_t char_index = static_cast<size_t>(c - 32);
    for (uint32_t row = 0; row < FONT_HEIGHT; ++row) {
        uint8_t bits = font8x16[char_index][row];
        for (uint32_t col = 0; col < FONT_WIDTH; ++col) {
            uint32_t color = (bits & (1 << (FONT_WIDTH - 1 - col))) ? fg : bg;
            put_pixel(x + col, y + row, color);
        }
    }
}

// NOLINTBEGIN(bugprone-easily-swappable-parameters)
void Framebuffer::put_pixel(uint32_t x, uint32_t y, uint32_t color)
// NOLINTEND(bugprone-easily-swappable-parameters)
{
    // NOLINTNEXTLINE(performance-no-int-to-ptr)
    auto* fb = reinterpret_cast<uint8_t*>(static_cast<uint64_t>(info_.addr));
    uint32_t bpp_bytes = info_.bpp / 8;
    size_t offset = y * info_.pitch + x * bpp_bytes;
    fb[offset + 0] = (color >> 0) & 0xFF;
    fb[offset + 1] = (color >> 8) & 0xFF;
    fb[offset + 2] = (color >> 16) & 0xFF;
    if (bpp_bytes > 3) fb[offset + 3] = 0xFF;
}

// NOLINTBEGIN(bugprone-easily-swappable-parameters)
uint32_t Framebuffer::get_pixel(uint32_t x, uint32_t y)
// NOLINTEND(bugprone-easily-swappable-parameters)
{
    if (!initialized_ || x >= info_.width || y >= info_.height)
        return 0x000000;
    // NOLINTNEXTLINE(performance-no-int-to-ptr)
    auto *fb = reinterpret_cast<const uint8_t *>(
        static_cast<uint64_t>(info_.addr));
    uint32_t bpp_bytes = info_.bpp / 8;
    if (bpp_bytes < 3)
        return 0x000000;
    size_t offset = static_cast<size_t>(y) * info_.pitch +
                    static_cast<size_t>(x) * bpp_bytes;
    uint32_t b = fb[offset + 0];
    uint32_t g = fb[offset + 1];
    uint32_t r = fb[offset + 2];
    return (r << 16) | (g << 8) | b;
}

} // namespace service
