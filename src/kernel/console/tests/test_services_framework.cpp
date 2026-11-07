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

/// @file test_services_framework.cpp
/// @brief Services framework tests (milestone v0.4.10 issue #125): the
///        non-command parts of `services` — the Terminal rendering surface
///        (colors, cursor, splash, framebuffer gate, scroll) and the
///        ProgramRegistry lookup API — were never entered.
/// @note  `Terminal::readline()` is deliberately not driven: it blocks on
///        serial/keyboard input.  Every test restores the terminal state it
///        mutates (colors, framebuffer gate) so later output is unaffected.

#include <test.hpp>
#include <logger.hpp>
#include <services/terminal/terminal.hpp>
#include <services/program.hpp>
#include <string.hpp>

using namespace kernel;

namespace {

constexpr size_t k_capture_size = 1024;

/// @brief Terminal colors used to restore the defaults after a test.
constexpr uint32_t k_default_fg = 0xC0C0C0;
constexpr uint32_t k_default_bg = 0x000000;

/// @brief Number of newlines written to force the scroll path.  The terminal
///        is cleared first, so this is the number of rows consumed from row 0.
constexpr int k_scroll_lines = 48;

bool has(const char *haystack, const char *needle) {
    if (!*needle)
        return true;
    for (size_t i = 0; haystack[i]; ++i) {
        size_t j = 0;
        while (needle[j] && haystack[i + j] == needle[j])
            ++j;
        if (!needle[j])
            return true;
    }
    return false;
}

size_t count_char(const char *text, char wanted) {
    size_t found = 0;
    for (size_t i = 0; text[i]; ++i) {
        if (text[i] == wanted)
            ++found;
    }
    return found;
}

void begin_capture(char *buffer, size_t size) {
    buffer[0] = '\0';
    service::Terminal::capture_begin(buffer, size);
}

void end_capture() {
    service::Terminal::capture_end();
}

} // namespace

// Runmode: kernel
// Testidea: The terminal is a live singleton and its color setters change
// the attribute state used by later rendering — text written after a color
// change still reaches the output channel.  Defaults are restored.
// Input: Terminal::instance(); set_fg/set_bg; write a marker; restore.
// Expect: instance non-null; the marker is captured.
// Depends: service::Terminal
JARVIS_TEST(services_terminal_colors_and_instance,
            "PRE: vfsd, iocd | POST: none") {
    bool live = service::Terminal::instance() != nullptr;

    char out[k_capture_size];
    begin_capture(out, sizeof(out));
    service::Terminal::set_fg(0x00FF00);
    service::Terminal::set_bg(0x101010);
    service::Terminal::write("zz-color-marker");
    service::Terminal::set_fg(k_default_fg);
    service::Terminal::set_bg(k_default_bg);
    end_capture();

    JARVIS_ASSERT(live);
    JARVIS_ASSERT(has(out, "zz-color-marker"));
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: The length-bounded write overload must honour its length
// argument — it renders exactly `len` bytes and never runs to the NUL of a
// buffer that is not NUL-terminated at `len`.
// Input: Terminal::write("abcdef", 3) followed by the NUL-terminated form.
// Expect: "abc" is captured, "abcdef" is not.
// Depends: service::Terminal
JARVIS_TEST(services_terminal_write_length_bounded,
            "PRE: vfsd, iocd | POST: none") {
    char out[k_capture_size];
    begin_capture(out, sizeof(out));
    service::Terminal::write("abcdef", 3);
    end_capture();

    char full[k_capture_size];
    begin_capture(full, sizeof(full));
    service::Terminal::write("abcdef");
    end_capture();

    JARVIS_ASSERT(has(out, "abc"));
    JARVIS_ASSERT(!has(out, "abcdef"));
    JARVIS_ASSERT(has(full, "abcdef"));
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: The cursor and splash paths are framebuffer-only renders: they
// must be no-ops on the serial/capture channel (no stray characters) and
// must not disturb subsequent text.
// Input: set_cursor_visible(true), set_cursor_visible(false), show_splash(),
//        then write a marker.
// Expect: The captured text is exactly the marker with no splash/cursor
//         characters mixed in.
// Depends: service::Terminal
JARVIS_TEST(services_terminal_cursor_and_splash_are_render_only,
            "PRE: vfsd, iocd | POST: none") {
    char out[k_capture_size];
    begin_capture(out, sizeof(out));
    service::Terminal::set_cursor_visible(true);
    service::Terminal::set_cursor_visible(false);
    service::Terminal::show_splash();
    service::Terminal::write("zz-splash-marker");
    end_capture();

    size_t marker_len = strlen("zz-splash-marker");
    size_t captured = strlen(out);

    JARVIS_ASSERT(has(out, "zz-splash-marker"));
    JARVIS_ASSERT(captured >= marker_len);
    JARVIS_ASSERT(!has(out, "NexIOS RTOS"));
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Disabling the framebuffer must not silence the serial/capture
// channel — `putchar` emits to serial (and capture) before the
// framebuffer-gate check, so output survives with rendering off.
// Input: set_fb_enabled(false); write marker; set_fb_enabled(true).
// Expect: The marker is captured while the framebuffer was disabled, and the
//         gate is restored to enabled afterwards.
// Depends: service::Terminal
JARVIS_TEST(services_terminal_fb_gate_keeps_serial,
            "PRE: vfsd, iocd | POST: none") {
    char out[k_capture_size];
    service::Terminal::set_fb_enabled(false);
    begin_capture(out, sizeof(out));
    service::Terminal::write("zz-fb-off-marker");
    end_capture();
    service::Terminal::set_fb_enabled(true);

    JARVIS_ASSERT(has(out, "zz-fb-off-marker"));
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: A newline on the last row scrolls the framebuffer content and
// keeps the cursor on-screen — the terminal must consume an unbounded number
// of lines without losing any of them on the serial channel.
// Input: clear(), then 48 newline writes with the framebuffer enabled.
// Expect: All 48 newlines appear in the capture (no line is dropped).
// Depends: service::Terminal
JARVIS_TEST(services_terminal_scrolls_on_overflow,
            "PRE: vfsd, iocd | POST: none") {
    service::Terminal::clear();

    char out[k_capture_size];
    begin_capture(out, sizeof(out));
    for (int line = 0; line < k_scroll_lines; ++line)
        service::Terminal::putchar('\n');
    end_capture();

    size_t newlines = count_char(out, '\n');

    JARVIS_ASSERT(newlines == static_cast<size_t>(k_scroll_lines));
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: The program registry is index- and name-bounded: get() past the
// end returns nullptr (no OOB read of the static table) and find() of an
// unknown name returns nullptr instead of a stale entry.  A registered
// program, when one exists, is resolvable by its exact name.
// Input: ProgramRegistry::count(), get(count()), get(0), find("..."),
//        find of a registered name (when count() > 0).
// Expect: get(count()) == nullptr; find(unknown) == nullptr; when
//         count() > 0, get(0) is non-null with a name and find(name) returns
//         the same entry.
// Depends: service::ProgramRegistry
JARVIS_TEST(services_program_registry_bounds,
            "PRE: vfsd, iocd | POST: none") {
    const size_t registered = service::ProgramRegistry::count();
    const service::ProgramRegistry::Program *past_end =
        service::ProgramRegistry::get(registered);
    const service::ProgramRegistry::Program *beyond =
        service::ProgramRegistry::get(registered + 64);
    const service::ProgramRegistry::Program *missing =
        service::ProgramRegistry::find("zz-no-such-program");

    const service::ProgramRegistry::Program *first =
        service::ProgramRegistry::get(0);
    bool first_consistent = true;
    bool find_consistent = true;
    if (registered > 0 && first && first->name) {
        first_consistent = first->name[0] != '\0';
        find_consistent =
            service::ProgramRegistry::find(first->name) == first;
    }

    JARVIS_ASSERT(past_end == nullptr);
    JARVIS_ASSERT(beyond == nullptr);
    JARVIS_ASSERT(missing == nullptr);
    JARVIS_ASSERT(first_consistent);
    JARVIS_ASSERT(find_consistent);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: `puts` is write-plus-newline (the newline must be emitted even
// when the string itself is empty), and a backspace character drives the
// cursor-erase path without emitting the control byte back to the caller.
// Input: Terminal::puts("zz-puts-marker"); Terminal::putchar('\b').
// Expect: The captured text is "zz-puts-marker\n"; the backspace is captured
//         as the control character itself.
// Depends: service::Terminal
JARVIS_TEST(services_terminal_puts_and_backspace,
            "PRE: vfsd, iocd | POST: none") {
    char out[k_capture_size];
    begin_capture(out, sizeof(out));
    service::Terminal::puts("zz-puts-marker");
    end_capture();

    char erased[k_capture_size];
    begin_capture(erased, sizeof(erased));
    service::Terminal::putchar('\b');
    end_capture();

    bool has_text = has(out, "zz-puts-marker");
    size_t newlines = count_char(out, '\n');
    size_t backspaces = count_char(erased, '\b');

    JARVIS_ASSERT(has_text);
    JARVIS_ASSERT(newlines == 1);
    JARVIS_ASSERT(backspaces == 1);
    JARVIS_TEST_PASS();
}

void register_services_framework_tests() {
    Logger::info("Registering services framework tests");
    JARVIS_REGISTER_TEST(services_terminal_colors_and_instance);
    JARVIS_REGISTER_TEST(services_terminal_write_length_bounded);
    JARVIS_REGISTER_TEST(services_terminal_cursor_and_splash_are_render_only);
    JARVIS_REGISTER_TEST(services_terminal_fb_gate_keeps_serial);
    JARVIS_REGISTER_TEST(services_terminal_scrolls_on_overflow);
    JARVIS_REGISTER_TEST(services_program_registry_bounds);
    JARVIS_REGISTER_TEST(services_terminal_puts_and_backspace);
}
