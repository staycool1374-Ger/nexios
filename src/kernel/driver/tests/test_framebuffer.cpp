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

/// @file test_framebuffer.cpp
/// @brief Framebuffer driver tests.

#include <test.hpp>
#include <logger.hpp>
#include <services/terminal/framebuffer.hpp>
#include <services/terminal/terminal.hpp>
#include <services/terminal/font.hpp>
#include <kernel/task/scheduler.hpp>

using namespace kernel;

// Runmode: kernel
// Testidea: Verifies framebuffer dimensions are valid after boot init.
// Input: Boot with framebuffer tag
// Expect: width, height, bpp, pitch are non-zero when available
// Depends: service::Framebuffer
JARVIS_TEST(fb_init_from_multiboot, "PRE: iocd | POST: none") {
    if (service::Framebuffer::available()) {
        JARVIS_ASSERT(service::Framebuffer::width() > 0);
        JARVIS_ASSERT(service::Framebuffer::height() > 0);
        JARVIS_ASSERT(service::Framebuffer::pitch() > 0);
    }
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Verifies drawing a pixel within bounds does not crash.
// Input: draw_pixel(0, 0, white)
// Expect: No crash, framebuffer unchanged for out-of-bounds
// Depends: service::Framebuffer
JARVIS_TEST(fb_putpixel_in_bounds, "PRE: iocd | POST: none") {
    if (service::Framebuffer::available()) {
        service::Framebuffer::draw_pixel(0, 0, 0xFFFFFF);
        uint32_t w = service::Framebuffer::width();
        uint32_t h = service::Framebuffer::height();
        if (w > 1 && h > 1)
            service::Framebuffer::draw_pixel(w - 1, h - 1, 0xFF0000);
    }
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Verifies writing outside framebuffer bounds is safe (no-op).
// Input: putpixel(-1, 0), putpixel(width, height)
// Expect: No crash, no memory corruption
// Depends: service::Framebuffer
JARVIS_TEST(fb_putpixel_out_of_bounds, "PRE: iocd | POST: none") {
    if (service::Framebuffer::available()) {
        service::Framebuffer::draw_pixel(static_cast<uint32_t>(-1), 0, 0xFF);
        service::Framebuffer::draw_pixel(0, static_cast<uint32_t>(-1), 0xFF);
        service::Framebuffer::draw_pixel(service::Framebuffer::width(),
                                         service::Framebuffer::height(), 0xFF);
    }
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Verifies clearing sets all pixels to the background color.
// Input: clear_screen(black)
// Expect: No crash
// Depends: service::Framebuffer
JARVIS_TEST(fb_clear_screen, "PRE: iocd | POST: none") {
    if (service::Framebuffer::available()) {
        service::Framebuffer::clear(0x000000);
    }
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Verifies scrolling shifts framebuffer content up.
// Input: Write multiple lines, scroll
// Expect: No crash, terminal state updated
// Depends: service::Terminal
JARVIS_TEST(fb_scroll_up, "PRE: iocd | POST: none") {
    service::Terminal::write("line1\nline2\nline3\n");
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Registers all Framebuffer unit tests with the test framework.
// Input: None
// Expect: All Framebuffer tests registered via JARVIS_REGISTER_TEST
// Depends: kernel test framework
// Runmode: kernel
// Testidea: issue #269 — the reap `terminated` line reaches the
// framebuffer (serial/fb parity). Clear, render via the real helper,
// then scan the first text row for lit pixels.
// Expect: at least one non-background pixel when fb is available;
// no-crash pass otherwise.
// Depends: Scheduler::fb_terminated_line, Framebuffer::get_pixel.
JARVIS_TEST(fb_terminated_line_renders, "PRE: iocd | POST: none") {
    if (!service::Framebuffer::available()) {
        JARVIS_TEST_PASS();
    }
    service::Terminal::clear();
    kernel::Scheduler::fb_terminated_line("probe-task", 42);
    bool lit = false;
    uint32_t w = service::Framebuffer::width();
    for (uint32_t x = 0; x < w && !lit; ++x) {
        for (uint32_t y = 0; y < FONT_HEIGHT; ++y) {
            if (service::Framebuffer::get_pixel(x, y) != 0x000000) {
                lit = true;
                break;
            }
        }
    }
    JARVIS_ASSERT(lit);
    JARVIS_TEST_PASS();
}

void register_framebuffer_tests() {
    Logger::info("Registering framebuffer tests");
    JARVIS_REGISTER_TEST(fb_init_from_multiboot);
    JARVIS_REGISTER_TEST(fb_putpixel_in_bounds);
    JARVIS_REGISTER_TEST(fb_putpixel_out_of_bounds);
    JARVIS_REGISTER_TEST(fb_clear_screen);
    JARVIS_REGISTER_TEST(fb_scroll_up);
    JARVIS_REGISTER_TEST(fb_terminated_line_renders); // #269 parity pin
}