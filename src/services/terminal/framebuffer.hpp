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

/// @file framebuffer.hpp
/// @brief Framebuffer driver for linear framebuffer output.

#pragma once

#include <types.hpp>

namespace service {

/// @brief Describes the framebuffer dimensions and layout.
struct FramebufferInfo {
    uint64_t addr;
    uint32_t pitch;
    uint32_t width;
    uint32_t height;
    uint8_t  bpp;
};

/// @brief Provides pixel-level drawing on a linear framebuffer.
class Framebuffer {
public:
    /// @brief Initialises the framebuffer from Multiboot2 info.
    /// @return True if the framebuffer was successfully configured.
    static bool init();
    /// @brief Clears the entire framebuffer to a solid color.
    /// @param color 24-bit RGB color (default black).
    static void clear(uint32_t color = 0x000000);

    /// @brief Draws a single pixel at the given coordinates.
    /// @param x     X coordinate.
    /// @param y     Y coordinate.
    /// @param color 24-bit RGB color value.
    static void draw_pixel(uint32_t x, uint32_t y, uint32_t color);
    /// @brief Fills a rectangle with a solid color.
    /// @param x     Top-left X coordinate.
    /// @param y     Top-left Y coordinate.
    /// @param w     Width in pixels.
    /// @param h     Height in pixels.
    /// @param color 24-bit RGB fill color.
    static void fill_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t color);
    /// @brief Renders a character using the bitmap font.
    /// @param x  Top-left X coordinate.
    /// @param y  Top-left Y coordinate.
    /// @param c  ASCII character to render.
    /// @param fg Foreground (text) color.
    /// @param bg Background color.
    static void draw_char(uint32_t x, uint32_t y, char c, uint32_t fg, uint32_t bg);

    /// @brief Returns the framebuffer width in pixels.
    /// @return Width in pixels.
    static uint32_t width() { return info_.width; }
    /// @brief Returns the framebuffer height in pixels.
    /// @return Height in pixels.
    static uint32_t height() { return info_.height; }
    /// @brief Returns the framebuffer pitch (bytes per row).
    /// @return Pitch in bytes.
    static uint32_t pitch() { return info_.pitch; }
    /// @brief Returns the full framebuffer info structure.
    /// @return Const reference to FramebufferInfo.
    static const FramebufferInfo& info() { return info_; }

    /// @brief Returns whether the framebuffer was successfully initialised.
    /// @return True if available.
    static bool available() { return initialized_; }
    /// @brief Reads one pixel back (issue #269 test oracle).
    /// @return 0x000000 when unavailable or out of bounds.
    static uint32_t get_pixel(uint32_t x, uint32_t y);

private:
    // NOLINTBEGIN(bugprone-dynamic-static-initializers)
    static FramebufferInfo info_;
    static bool initialized_;
    // NOLINTEND(bugprone-dynamic-static-initializers)

    /// @brief Writes a pixel value directly to the framebuffer memory.
    /// @param x     X coordinate.
    /// @param y     Y coordinate.
    /// @param color Pixel color value (format depends on bpp).
    static void put_pixel(uint32_t x, uint32_t y, uint32_t color);
};

} // namespace service
