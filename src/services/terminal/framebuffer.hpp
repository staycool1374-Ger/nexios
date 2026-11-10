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
    ///        With CONFIG_DRIVER_PD_FB the kernel still probes the device
    ///        (geometry + scanout phys for the SDF device cap) but does not
    ///        own it — see device_owned().
    /// @return True if available.
    static bool available() { return initialized_; }
    /// @brief True when the framebuffer device is owned by a userspace PD
    ///        (issue #51): the kernel keeps the probe for cap minting and
    ///        the read-only pixel oracle, but every render path is a no-op
    ///        — writes belong to the PD alone.
    /// @return True when the PD owns presentation.
    static bool device_owned() {
#if CONFIG_DRIVER_PD_FB
        return true;
#else
        return false;
#endif
    }
    /// @brief Backend name (issue #394: runtime-selected, was
    /// compile-time-only in #357). "multiboot2" on x86_64; on other arches
    /// the init chain selects "virtio-gpu" then "ramfb", else "none".
    /// @return Backend name string (stable pointer, never null).
    static const char *backend_name();
    /// @brief Flushes dirty rows to the host scanout (issue #394).
    /// No-op for backends without host presentation (multiboot2 writes
    /// real RAM directly; ramfb points the host at guest RAM directly).
    /// Safe to call when unavailable (returns immediately).
    static void present();
    /// @brief Marks the whole framebuffer dirty (issue #394).
    /// Call after direct framebuffer writes that bypass put_pixel
    /// (Terminal clear/scroll fast paths) and before the next present().
    static void invalidate();
    /// @brief Reads one pixel back (issue #269 test oracle).
    /// @return 0x000000 when unavailable or out of bounds.
    static uint32_t get_pixel(uint32_t x, uint32_t y);

    /// @brief Physical base of the scanout, for SDF device-cap minting
    /// (issue #51 console PD: #48 owns the device cap, #51 the emulation).
    /// x86: VGA VRAM (true device MMIO → MmioCap). virtio-gpu/ramfb:
    /// PMM-backed guest RAM (→ FRAME pins over user frames).
    /// x86_64 is the multiboot2 tag address; aarch64/riscv64 is the
    /// PMM-backed guest-RAM backing page 0. Zero when unavailable.
    /// @return Scanout physical base address, or 0.
    static uint64_t scanout_phys() { return scanout_phys_; }
    /// @brief Scanout span in bytes (pitch * height), page-rounded up by
    /// the caller when minting the cap. Zero when unavailable.
    /// @return Scanout byte span, or 0.
    static uint64_t scanout_bytes() { return scanout_bytes_; }
    /// @brief True when the scanout is PMM-backed guest RAM (issue #51:
    ///        virtio-gpu/ramfb) rather than true device MMIO (x86 VGA).
    ///        The SDF row mints FRAME pins for RAM, MmioCaps for MMIO —
    ///        SYS_FRAME_MAP refuses kernel pages and SYS_MMIO_MAP
    ///        refuses RAM, so the distinction is load-bearing.
    /// @return True for RAM-backed scanouts.
    static bool scanout_ram_backed() { return scanout_ram_backed_; }

private:
    // NOLINTBEGIN(bugprone-dynamic-static-initializers)
    static FramebufferInfo info_;
    static bool initialized_;
    static uint64_t scanout_phys_;
    static uint64_t scanout_bytes_;
    static bool scanout_ram_backed_;
    // NOLINTEND(bugprone-dynamic-static-initializers)

    /// @brief Writes a pixel value directly to the framebuffer memory.
    /// @param x     X coordinate.
    /// @param y     Y coordinate.
    /// @param color Pixel color value (format depends on bpp).
    static void put_pixel(uint32_t x, uint32_t y, uint32_t color);
};

} // namespace service
