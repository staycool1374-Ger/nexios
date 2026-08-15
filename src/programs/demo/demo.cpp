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

#include <services/terminal/terminal.hpp>
#include <services/terminal/framebuffer.hpp>
#include <kernel/arch/timer.hpp>
#include <kernel/arch/keyboard.hpp>
#include <kernel/arch/io.hpp>

namespace programs {

static constexpr uint32_t W = 1024;
static constexpr uint32_t H = 768;

static const uint32_t palette[16] = {
    0x000000, 0xAA0000, 0x00AA00, 0xAAAA00,
    0x0000AA, 0xAA00AA, 0x00AAAA, 0xAAAAAA,
    0x555555, 0xFF5555, 0x55FF55, 0xFFFF55,
    0x5555FF, 0xFF55FF, 0x55FFFF, 0xFFFFFF,
};

static int mandelbrot_iter(int px, int py) {
    int x0 = static_cast<int>((px * 35 - 25 * W) / 10);
    int y0 = static_cast<int>((py * 20 - 10 * H) / 10);

    int x = 0, y = 0;
    int iter = 0;
    int max_iter = 64;

    while (x * x + y * y <= 40000 && iter < max_iter) {
        int xt = (x * x - y * y) / 100 + x0;
        y = (2 * x * y) / 100 + y0;
        x = xt;
        ++iter;
    }

    return iter;
}

void draw_mandelbrot() {
    service::Framebuffer::clear(0x000000);

    for (uint32_t py = 0; py < H; py += 2) {
        for (uint32_t px = 0; px < W; px += 2) {
            int iter = mandelbrot_iter(static_cast<int>(px), static_cast<int>(py));
            uint32_t color = palette[iter % 16];
            service::Framebuffer::draw_pixel(px, py, color);
            service::Framebuffer::draw_pixel(px + 1, py, color);
            service::Framebuffer::draw_pixel(px, py + 1, color);
            service::Framebuffer::draw_pixel(px + 1, py + 1, color);
        }
    }

    service::Terminal::set_fg(0xFFFFFF);
    service::Terminal::write("\nMandelbrot gerendert. Taste druecken...\n");
}

void draw_spinning_rect() {
    service::Framebuffer::clear(0x000000);

    int cx = W / 2, cy = H / 2;
    int size = 100;
    int step = 0;

    uint64_t start = arch::Timer::ticks();
    while (arch::Timer::ticks() - start < 3000) {
        service::Framebuffer::clear(0x000000);

        int angle_idx = (step / 5) % 8;
        int cos_val = 0, sin_val = 0;

        switch (angle_idx) {
            case 0: cos_val = 100; sin_val = 0; break;
            case 1: cos_val = 71;  sin_val = 71; break;
            case 2: cos_val = 0;   sin_val = 100; break;
            default: break;
            case 3: cos_val = -71; sin_val = 71; break;
            case 4: cos_val = -100; sin_val = 0; break;
            case 5: cos_val = -71; sin_val = -71; break;
            case 6: cos_val = 0;   sin_val = -100; break;
            case 7: cos_val = 71;  sin_val = -71; break;
        }
        ++step;

        int x1 = cx + (size * cos_val - size * sin_val) / 100;
        int y1 = cy + (size * sin_val + size * cos_val) / 100;
        int x2 = cx + (size * cos_val + size * sin_val) / 100;
        int y2 = cy + (size * sin_val - size * cos_val) / 100;
        int x3 = cx + (-size * cos_val + size * sin_val) / 100;
        int y3 = cy + (-size * sin_val - size * cos_val) / 100;
        int x4 = cx + (-size * cos_val - size * sin_val) / 100;
        int y4 = cy + (-size * sin_val + size * cos_val) / 100;

        service::Framebuffer::fill_rect(x1 - 3, y1 - 3, 7, 7, 0xFF0000);
        service::Framebuffer::fill_rect(x2 - 3, y2 - 3, 7, 7, 0x00FF00);
        service::Framebuffer::fill_rect(x3 - 3, y3 - 3, 7, 7, 0x0000FF);
        service::Framebuffer::fill_rect(x4 - 3, y4 - 3, 7, 7, 0xFFFF00);
    }

    service::Terminal::set_fg(0xFFFFFF);
    service::Terminal::write("\nRotation fertig.\n");
}

void demo_main() {
    service::Terminal::clear();
    service::Terminal::set_fg(0x00FF00);
    service::Terminal::write("=== NexIOS RTOS Demo ===\n\n");
    service::Terminal::set_fg(0xC0C0C0);
    service::Terminal::write("Starte Demos...\n\n");

    draw_mandelbrot();

    char c;
    while (!arch::Keyboard::getchar(c)) {
        for (int _pd = 0; _pd < 1000; ++_pd) { arch::pause(); }
    }

    draw_spinning_rect();

    service::Terminal::clear();
    service::Terminal::set_fg(0x00FF00);
    service::Terminal::write("Demo beendet.\n");
    service::Terminal::set_fg(0xC0C0C0);
}

uint64_t bench_cpu() {
    uint64_t result = 0;
    for (int iter = 0; iter < 100; ++iter) {
        for (int py = 0; py < 256; py += 2) {
            for (int px = 0; px < 256; px += 2) {
                int x0 = (px * 35 - 25 * 256) / 10;
                int y0 = (py * 20 - 10 * 256) / 10;
                int x = 0, y = 0, n = 0;
                while (x * x + y * y <= 40000 && n < 64) {
                    int xt = (x * x - y * y) / 100 + x0;
                    y = (2 * x * y) / 100 + y0;
                    x = xt;
                    ++n;
                }
                result += n;
            }
        }
    }
    return result;
}

} // namespace programs
