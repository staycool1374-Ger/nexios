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

/// @file terminal.hpp
/// @brief Text-mode terminal on top of the framebuffer.

#pragma once

#include <types.hpp>

namespace service {

/// @brief Text terminal with cursor, scrolling, and line editing.
/// @note Renders to the framebuffer using the built-in bitmap font.
class Terminal {
public:
    /// @brief Initialises the terminal and clears the screen.
    static void init();
    /// @brief Writes a single character to the terminal.
    /// @param c Character to display.
    static void putchar(char c);
    /// @brief Writes a null-terminated string to the terminal.
    /// @param str String to display.
    static void write(const char* str);
    /// @brief Writes a buffer of given length to the terminal.
    /// @param data Pointer to the data buffer.
    /// @param len  Number of bytes to write.
    static void write(const char* data, size_t len);

    /// @brief Reads a line of input from the keyboard into a buffer.
    /// @param buf     Output buffer for the line.
    /// @param max_len Maximum buffer size (including null terminator).
    /// @return True if a line was read.
    static bool readline(char* buf, size_t max_len);
    /// @brief Writes a string followed by a newline.
    /// @param str String to display.
    static void puts(const char* str);

    /// @brief Renders a string on the framebuffer only (no serial output,
    ///        no capture-buffer feed).  For background producers (e.g. the
    ///        ELF loader event log) whose serial channel is already served
    ///        elsewhere (Logger/dmesg): without this, console users would
    ///        never see their progress.  Mirrors putchar's framebuffer
    ///        half exactly (cursor, newline, fb_enabled_ gate).
    /// @param str String to display (NUL-terminated).
    static void write_fb(const char* str);

    /// @brief Sets the foreground (text) color.
    /// @param color 24-bit RGB color.
    static void set_fg(uint32_t color);
    /// @brief Sets the background color.
    /// @param color 24-bit RGB color.
    static void set_bg(uint32_t color);

    /// @brief Clears the entire terminal screen.
    static void clear();
    /// @brief Scrolls the terminal content up by one line.
    static void scroll();

    /// @brief Enables or disables framebuffer rendering.
    /// @param v True to enable, false to disable framebuffer output.
    static void set_fb_enabled(bool v) { if (instance_) instance_->fb_enabled_ = v; }

    /// @brief Returns the singleton terminal instance.
    /// @return Pointer to the Terminal instance.
    static Terminal* instance() { return instance_; }

    /// @brief Number of pixel rows reserved for the persistent status bar at the bottom.
    static constexpr uint32_t STATUS_BAR_ROWS = 2;
    /// @brief Draws or updates the persistent status bar at the bottom of the screen.
    /// @param left  Left-aligned text (e.g. version + build type).
    /// @param right Right-aligned text (e.g. date, time, uptime).
    static void draw_status_bar(const char* left, const char* right);
    /// @brief Shows the boot splash screen (version, build type) centered on the framebuffer.
    static void show_splash();
    /// @brief Shows or hides a block cursor at the current text cursor position (framebuffer only).
    /// @param visible True to draw cursor, false to erase it.
    static void set_cursor_visible(bool visible);

    /// @brief Begin capturing serial output into a buffer (for shell redirection).
    /// @param buf Buffer to write captured output into.
    /// @param size Maximum bytes to capture (capture stops when full).
    static void capture_begin(char* buf, size_t size);
    /// @brief Stop capturing serial output.
    static void capture_end();

    /// @brief Maximum length of the line-input buffer.
    static constexpr size_t LINE_BUF_SIZE = 256;

private:
    // NOLINTNEXTLINE(bugprone-dynamic-static-initializers)
    static Terminal* instance_;

    static constexpr uint32_t DEFAULT_FG = 0xC0C0C0;
    static constexpr uint32_t DEFAULT_BG = 0x000000;

    bool fb_enabled_ = true;

    uint32_t cursor_x_ = 0;
    uint32_t cursor_y_ = 0;
    uint32_t fg_ = DEFAULT_FG;
    uint32_t bg_ = DEFAULT_BG;
    uint32_t cols_ = 0;
    uint32_t rows_ = 0;

    size_t line_pos_ = 0;

    Terminal() = default;

    /// @brief Moves cursor to the beginning of the next line (scrolls if needed).
    void newline();
    /// @brief Advances the cursor by one column (wraps to next line if needed).
    void advance_cursor();
    /// @brief Moves the cursor back one position (for backspace handling).
    void backspace();

    char* capture_buf_ = nullptr;
    size_t capture_size_ = 0;
    size_t capture_pos_ = 0;
};

} // namespace service
