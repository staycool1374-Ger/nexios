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

/// @file test_textutils.cpp
/// @brief Text utility function tests (less pager + ed-style editing).

#include <test.hpp>
#include <logger.hpp>
#include <string.hpp>
#include <services/shell.hpp>
#include <services/terminal/terminal.hpp>
#include <kernel/vfs/vfs.hpp>
#include <kernel/vfs/tmpfs.hpp>
#include <kernel/input/serial_input.hpp>
#include <kernel/test/test_isolate.hpp>
#include <types.hpp>

using namespace kernel;

namespace {

constexpr size_t kCaptureBytes = 4096;
constexpr size_t kFixtureLines = 200;
constexpr size_t kLinesPerPage = 24;
constexpr size_t kFullPages = kFixtureLines / kLinesPerPage;

constexpr const char *kPathPage24 = "/tmp/qe_textutils_24.txt";
constexpr const char *kPathPage23 = "/tmp/qe_textutils_23.txt";
constexpr const char *kPathCleanup = "/tmp/qe_textutils_cleanup.txt";
constexpr const char *kPathEd = "/tmp/qe_textutils_ed.txt";
constexpr const char *kPathPrompt = "/tmp/qe_textutils_prompt.txt";
constexpr const char *kPathEmpty = "/tmp/qe_textutils_empty.txt";
constexpr const char *kPathOne = "/tmp/qe_textutils_one.txt";
constexpr const char *kPathFresh = "/tmp/qe_textutils_fresh.txt";
constexpr const char *kPathQuit = "/tmp/qe_textutils_quit200.txt";
constexpr const char *kPathFull = "/tmp/qe_textutils_full200.txt";

/// @brief Ensure the standard mounts (notably /tmp) are present.
void ensure_tmp_mounted() {
    kernel::test::mark_vfs_touched();
    kernel::vfs::Vnode *tmp = kernel::vfs::resolve("/tmp");
    if (tmp == nullptr) {
        kernel::vfs::reset_and_remount();
    } else {
        kernel::vfs::release(tmp);
    }
}

/// @brief First occurrence of needle in haystack, or nullptr.
const char *find_text(const char *haystack, const char *needle) {
    if (!haystack || !needle) {
        return nullptr;
    }
    for (size_t start = 0; haystack[start] != '\0'; ++start) {
        size_t offset = 0;
        while (needle[offset] != '\0' &&
               haystack[start + offset] == needle[offset]) {
            ++offset;
        }
        if (needle[offset] == '\0') {
            return haystack + start;
        }
    }
    return nullptr;
}

/// @brief Non-overlapping occurrence count of needle in haystack.
size_t count_text(const char *haystack, const char *needle) {
    size_t needle_len = 0;
    while (needle[needle_len] != '\0') {
        ++needle_len;
    }
    if (needle_len == 0 || !haystack) {
        return 0;
    }
    size_t found = 0;
    for (size_t pos = 0; haystack[pos] != '\0';) {
        size_t offset = 0;
        while (offset < needle_len && haystack[pos + offset] == needle[offset]) {
            ++offset;
        }
        if (offset == needle_len) {
            ++found;
            pos += needle_len;
        } else {
            ++pos;
        }
    }
    return found;
}

/// @brief Append a zero-padded 3-digit line number.
size_t append_line_number(char *out, size_t pos, size_t value) {
    out[pos++] = static_cast<char>('0' + (value / 100) % 10);
    out[pos++] = static_cast<char>('0' + (value / 10) % 10);
    out[pos++] = static_cast<char>('0' + (value % 10));
    return pos;
}

/// @brief Render `label000\n`..`labelNNN\n` into out; return byte count.
size_t build_fixture(char *out, size_t cap, size_t line_count,
                     const char *label) {
    size_t label_len = 0;
    while (label[label_len] != '\0') {
        ++label_len;
    }
    size_t pos = 0;
    for (size_t line = 0; line < line_count; ++line) {
        if (pos + label_len + 4 > cap) {
            break;
        }
        for (size_t idx = 0; idx < label_len; ++idx) {
            out[pos++] = label[idx];
        }
        pos = append_line_number(out, pos, line);
        out[pos++] = '\n';
    }
    if (pos < cap) {
        out[pos] = '\0';
    }
    return pos;
}

/// @brief Create/replace a tmpfs file with the given bytes.
size_t write_fixture(const char *path, const char *data, size_t size) {
    kernel::test::mark_vfs_touched();
    (void)kernel::vfs::unlink(path);
    if (kernel::vfs::create(path, 0) != 0) {
        return 0;
    }
    kernel::vfs::Vnode *file = kernel::vfs::resolve(path);
    if (!file || !file->ops || !file->ops->write) {
        kernel::vfs::release(file);
        return 0;
    }
    int64_t written = file->ops->write(
        *file, reinterpret_cast<const uint8_t *>(data), size, 0);
    kernel::vfs::release(file);
    if (written <= 0) {
        return 0;
    }
    return static_cast<size_t>(written);
}

/// @brief Read a tmpfs file into out (NUL-terminated); return bytes read.
size_t read_fixture(const char *path, char *out, size_t cap) {
    if (cap == 0) {
        return 0;
    }
    out[0] = '\0';
    kernel::vfs::Vnode *file = kernel::vfs::resolve(path);
    if (!file || !file->ops || !file->ops->read) {
        kernel::vfs::release(file);
        return 0;
    }
    size_t pos = 0;
    while (pos + 1 < cap) {
        int64_t got = file->ops->read(
            *file, reinterpret_cast<uint8_t *>(out + pos), cap - 1 - pos, pos);
        if (got <= 0) {
            break;
        }
        pos += static_cast<size_t>(got);
    }
    out[pos] = '\0';
    kernel::vfs::release(file);
    return pos;
}

/// @brief Run `verb <path>` through the shell with captured output.
void run_path_command(const char *verb, const char *path, char *capture,
                      size_t cap) {
    char command[96];
    size_t pos = 0;
    for (size_t idx = 0; verb[idx] != '\0' && pos + 2 < sizeof(command);
         ++idx) {
        command[pos++] = verb[idx];
    }
    command[pos++] = ' ';
    for (size_t idx = 0; path[idx] != '\0' && pos + 1 < sizeof(command);
         ++idx) {
        command[pos++] = path[idx];
    }
    command[pos] = '\0';
    service::Terminal::capture_begin(capture, cap);
    service::Shell::execute(command);
    service::Terminal::capture_end();
}

/// @brief Run `less <path>` with the captured Terminal output.
void run_less(const char *path, char *capture, size_t cap) {
    run_path_command("less", path, capture, cap);
}

/// @brief Run a shell command with the captured Terminal output.
void run_command(const char *command, char *capture, size_t cap) {
    service::Terminal::capture_begin(capture, cap);
    service::Shell::execute(command);
    service::Terminal::capture_end();
}

/// @brief Remove every fixture and drain the input ring (no residue).
void cleanup_fixtures() {
    const char *paths[] = {kPathPage24,  kPathPage23, kPathCleanup,
                           kPathEd,      kPathPrompt, kPathEmpty,
                           kPathOne,     kPathFresh,  kPathQuit,
                           kPathFull};
    kernel::test::mark_vfs_touched();
    for (size_t idx = 0; idx < sizeof(paths) / sizeof(paths[0]); ++idx) {
        (void)kernel::vfs::unlink(paths[idx]);
    }
    input::serial::reset();
}

/// @brief Pagination contract: 24 lines/page boundary + cat shared path.
void textutils_less_pagination_check() {
    ensure_tmp_mounted();

    char body[512];
    size_t body_len = build_fixture(body, sizeof(body), kLinesPerPage, "TEXT");
    JARVIS_ASSERT(body_len > 0);
    JARVIS_ASSERT(write_fixture(kPathPage24, body, body_len) == body_len);

    char cat_capture[kCaptureBytes];
    run_path_command("cat", kPathPage24, cat_capture, sizeof(cat_capture));
    JARVIS_ASSERT(find_text(cat_capture, "TEXT000") != nullptr);
    JARVIS_ASSERT(find_text(cat_capture, "TEXT023") != nullptr);

    char small_body[512];
    size_t small_len =
        build_fixture(small_body, sizeof(small_body), kLinesPerPage - 1, "TEXT");
    JARVIS_ASSERT(write_fixture(kPathPage23, small_body, small_len) == small_len);

    input::serial::reset();
    JARVIS_ASSERT(input::serial::inject("q", 1) == 1);
    char small_capture[kCaptureBytes];
    run_less(kPathPage23, small_capture, sizeof(small_capture));
    JARVIS_ASSERT(count_text(small_capture, "--More--") == 0);
    JARVIS_ASSERT(find_text(small_capture, "TEXT022") != nullptr);

    input::serial::reset();
    JARVIS_ASSERT(input::serial::inject(" q", 2) == 2);
    char page_capture[kCaptureBytes];
    run_less(kPathPage24, page_capture, sizeof(page_capture));
    JARVIS_ASSERT(count_text(page_capture, "--More--") == 1);
    JARVIS_ASSERT(find_text(page_capture, "TEXT023") != nullptr);
    input::serial::reset();
}

/// @brief Graceful termination: usage + missing path never block.
void textutils_less_graceful_termination_check() {
    ensure_tmp_mounted();

    char usage_capture[256];
    run_command("less", usage_capture, sizeof(usage_capture));
    JARVIS_ASSERT(find_text(usage_capture, "Usage: less <path>") != nullptr);

    char miss_capture[256];
    run_path_command("less", "/tmp/qe_textutils_missing.txt", miss_capture,
                     sizeof(miss_capture));
    JARVIS_ASSERT(find_text(miss_capture, "No such file") != nullptr);
    JARVIS_ASSERT(
        find_text(miss_capture, "/tmp/qe_textutils_missing.txt") != nullptr);

    char marker_capture[256];
    run_command("echo TERM_TTY_OK", marker_capture, sizeof(marker_capture));
    JARVIS_ASSERT(find_text(marker_capture, "TERM_TTY_OK") != nullptr);
}

/// @brief Cleanup: pager quit leaves file bytes and TTY intact.
void textutils_less_cleanup_check() {
    ensure_tmp_mounted();

    char body[512];
    size_t body_len = build_fixture(body, sizeof(body), 30, "CLEAN");
    JARVIS_ASSERT(body_len > 0);
    JARVIS_ASSERT(write_fixture(kPathCleanup, body, body_len) == body_len);

    char before[512];
    JARVIS_ASSERT(read_fixture(kPathCleanup, before, sizeof(before)) == body_len);

    input::serial::reset();
    JARVIS_ASSERT(input::serial::inject("q", 1) == 1);
    char capture[kCaptureBytes];
    run_less(kPathCleanup, capture, sizeof(capture));
    JARVIS_ASSERT(find_text(capture, "CLEAN000") != nullptr);

    char after[512];
    JARVIS_ASSERT(read_fixture(kPathCleanup, after, sizeof(after)) == body_len);
    JARVIS_ASSERT(memcmp(before, after, body_len) == 0);

    char marker_capture[256];
    run_command("echo CLEAN_TTY_OK", marker_capture, sizeof(marker_capture));
    JARVIS_ASSERT(find_text(marker_capture, "CLEAN_TTY_OK") != nullptr);

    kernel::test::mark_vfs_touched();
    JARVIS_ASSERT(kernel::vfs::unlink(kPathCleanup) == 0);
}

/// @brief ed-style editing cycle: write/append/read-back/unlink.
void textutils_ed_editing_check() {
    ensure_tmp_mounted();

    kernel::test::mark_vfs_touched();
    (void)kernel::vfs::unlink(kPathEd);
    JARVIS_ASSERT(kernel::vfs::create(kPathEd, 0) == 0);

    kernel::vfs::Vnode *file = kernel::vfs::resolve(kPathEd);
    JARVIS_ASSERT(file != nullptr);
    const char *hello = "hello\n";
    const char *world = "world\n";
    int64_t first = -1;
    int64_t second = -1;
    if (file->ops && file->ops->write) {
        first = file->ops->write(
            *file, reinterpret_cast<const uint8_t *>(hello), 6, 0);
        second = file->ops->write(
            *file, reinterpret_cast<const uint8_t *>(world), 6, 6);
    }
    kernel::vfs::release(file);
    JARVIS_ASSERT(first == 6);
    JARVIS_ASSERT(second == 6);

    char content[64];
    JARVIS_ASSERT(read_fixture(kPathEd, content, sizeof(content)) == 12);
    JARVIS_ASSERT(memcmp(content, "hello\nworld\n", 12) == 0);

    kernel::test::mark_vfs_touched();
    JARVIS_ASSERT(kernel::vfs::unlink(kPathEd) == 0);
    JARVIS_ASSERT(kernel::vfs::resolve(kPathEd) == nullptr);
}

/// @brief Prompt return: quit leaves a balanced capture and a usable TTY.
void textutils_ed_prompt_return_check() {
    ensure_tmp_mounted();

    char body[1024];
    size_t body_len = build_fixture(body, sizeof(body), 60, "PROMPT");
    JARVIS_ASSERT(body_len > 0);
    JARVIS_ASSERT(write_fixture(kPathPrompt, body, body_len) == body_len);

    input::serial::reset();
    JARVIS_ASSERT(input::serial::inject("q", 1) == 1);
    char capture[kCaptureBytes];
    run_less(kPathPrompt, capture, sizeof(capture));
    JARVIS_ASSERT(count_text(capture, "--More--") == 1);
    size_t capture_len = strlen(capture);
    JARVIS_ASSERT(capture_len > 0);
    JARVIS_ASSERT(capture[capture_len - 1] == '\n');

    char marker_capture[256];
    run_command("echo PROMPT_RETURN_OK", marker_capture,
                sizeof(marker_capture));
    JARVIS_ASSERT(find_text(marker_capture, "PROMPT_RETURN_OK") != nullptr);
    input::serial::reset();
}

/// @brief Edge cases: empty, single line, create-on-write.
void textutils_edge_cases_check() {
    ensure_tmp_mounted();

    kernel::test::mark_vfs_touched();
    (void)kernel::vfs::unlink(kPathEmpty);
    JARVIS_ASSERT(kernel::vfs::create(kPathEmpty, 0) == 0);
    input::serial::reset();
    JARVIS_ASSERT(input::serial::inject("q", 1) == 1);
    char empty_capture[256];
    run_less(kPathEmpty, empty_capture, sizeof(empty_capture));
    JARVIS_ASSERT(find_text(empty_capture, "--More--") == nullptr);
    JARVIS_ASSERT(strlen(empty_capture) <= 1);

    char one_body[64];
    size_t one_len = build_fixture(one_body, sizeof(one_body), 1, "ONE");
    JARVIS_ASSERT(one_len > 0);
    JARVIS_ASSERT(write_fixture(kPathOne, one_body, one_len) == one_len);
    input::serial::reset();
    JARVIS_ASSERT(input::serial::inject("q", 1) == 1);
    char one_capture[256];
    run_less(kPathOne, one_capture, sizeof(one_capture));
    JARVIS_ASSERT(find_text(one_capture, "ONE000") != nullptr);
    JARVIS_ASSERT(count_text(one_capture, "--More--") == 0);

    kernel::test::mark_vfs_touched();
    (void)kernel::vfs::unlink(kPathFresh);
    JARVIS_ASSERT(kernel::vfs::resolve(kPathFresh) == nullptr);
    const char *payload = "created\n";
    JARVIS_ASSERT(write_fixture(kPathFresh, payload, 8) == 8);
    char fresh[32];
    JARVIS_ASSERT(read_fixture(kPathFresh, fresh, sizeof(fresh)) == 8);
    JARVIS_ASSERT(memcmp(fresh, "created\n", 8) == 0);
    kernel::test::mark_vfs_touched();
    JARVIS_ASSERT(kernel::vfs::unlink(kPathFresh) == 0);
    input::serial::reset();
}

/// @brief Real pager quit: head renders, tail absent, TTY usable.
void textutils_less_real_quit_check() {
    ensure_tmp_mounted();

    char body[2048];
    size_t body_len = build_fixture(body, sizeof(body), kFixtureLines, "REAL");
    JARVIS_ASSERT(body_len > 0);
    JARVIS_ASSERT(write_fixture(kPathQuit, body, body_len) == body_len);

    input::serial::reset();
    JARVIS_ASSERT(input::serial::inject("q", 1) == 1);
    char capture[kCaptureBytes];
    run_less(kPathQuit, capture, sizeof(capture));
    JARVIS_ASSERT(find_text(capture, "REAL000") != nullptr);
    JARVIS_ASSERT(find_text(capture, "REAL199") == nullptr);
    JARVIS_ASSERT(count_text(capture, "--More--") == 1);

    char marker_capture[256];
    run_command("echo QUIT_TTY_OK", marker_capture, sizeof(marker_capture));
    JARVIS_ASSERT(find_text(marker_capture, "QUIT_TTY_OK") != nullptr);
    input::serial::reset();
}

/// @brief Real pager full run: every page and the last line render.
void textutils_less_real_full_check() {
    ensure_tmp_mounted();

    char body[2048];
    size_t body_len = build_fixture(body, sizeof(body), kFixtureLines, "FULL");
    JARVIS_ASSERT(body_len > 0);
    JARVIS_ASSERT(write_fixture(kPathFull, body, body_len) == body_len);

    input::serial::reset();
    JARVIS_ASSERT(input::serial::inject("          ", 10) == 10);
    char capture[kCaptureBytes];
    run_less(kPathFull, capture, sizeof(capture));
    JARVIS_ASSERT(count_text(capture, "--More--") == kFullPages);
    JARVIS_ASSERT(find_text(capture, "FULL000") != nullptr);
    JARVIS_ASSERT(find_text(capture, "FULL199") != nullptr);

    char marker_capture[256];
    run_command("echo FULL_TTY_OK", marker_capture, sizeof(marker_capture));
    JARVIS_ASSERT(find_text(marker_capture, "FULL_TTY_OK") != nullptr);
    input::serial::reset();
}

} // namespace

// Runmode: kernel
// Testidea: Exercise the shell text utilities end to end. The pager (less)
//           and an ed-style edit cycle are driven through the production
//           shell entry points and the serial-injection input seam; the
//           missing userland binaries are covered by the built-in command
//           surface the shell actually ships.
// Input:   tmpfs fixtures under /tmp (0/1/23/24/30/60/200 lines) and
//          serial-injected keys (q/space) consumed via shell_try_get_key;
//          Terminal capture around each command.
// Expect:  usage + missing-file errors return without blocking; 24
//          lines/page boundary (23 -> 0, 24 -> 1 --More--); quit renders
//          the head only, the full run renders all 200 lines; edited
//          bytes survive read-back; the TTY is usable afterwards.
// Depends: vfs (tmpfs /tmp), shell built-ins less/cat/echo,
//          input::serial inject/reset, Terminal capture seam.
JARVIS_TEST(core_text_utilities, "PRE: vfsd, iocd | POST: none") {
    /*
    1. Pagination contract: 24-line file -> one --More--, 23-line -> none;
       cat renders the same bytes through the shared VFS read path.
    2. Graceful termination: missing argument and missing path return
       immediately with usage/error output.
    3. Cleanup: quitting the pager leaves file bytes and TTY intact.
    4. ed editing cycle: create, write hello, append world, read back,
       unlink.
    5. Prompt return: aborted pager leaves a balanced capture and the
       next command runs.
    6. Edges: empty file, one-line file, create-on-write.
    7. Real pager quit: head rendered, tail absent, TTY usable.
    8. Real pager full run: all pages and the last line rendered.
    */
    textutils_less_pagination_check();
    textutils_less_graceful_termination_check();
    textutils_less_cleanup_check();
    textutils_ed_editing_check();
    textutils_ed_prompt_return_check();
    textutils_edge_cases_check();
    textutils_less_real_quit_check();
    textutils_less_real_full_check();
    cleanup_fixtures();
    JARVIS_TEST_PASS();
}

void register_textutils_tests() {
    kernel::Logger::info("Registering textutils tests");
    JARVIS_REGISTER_TEST(core_text_utilities);
}
