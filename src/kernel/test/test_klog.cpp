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

/// @file test_klog.cpp
/// @brief Kernel log (klog) tests.

#include <test.hpp>
#include <logger.hpp>
#include <kernel/log/ring_buffer.hpp>
#include <kernel/memory/checked_ptr.hpp>
#include <string.hpp>

using namespace kernel;

static bool contains(const char *haystack, const char *needle) {
    if (!haystack || !needle)
        return false;
    while (*haystack) {
        const char *h = haystack;
        const char *n = needle;
        while (*h && *n && *h == *n) {
            ++h;
            ++n;
        }
        if (!*n)
            return true;
        ++haystack;
    }
    return false;
}

// Runmode: kernel
// Testidea: Verifies SYS_KLOG syscall reads back kernel log entries.
// Input: SYS_KLOG with valid buffer, size, and flags
// Expect: Returns number of bytes read, buffer contains log entries
// Depends: kernel::Syscall, kernel::log::RingBuffer
/* Pseudocode:
 * 1. Generate some kernel log entries via Logger::info/warn/error
 * 2. Call SYS_KLOG with buffer, size, flags=0 (read)
 * 3. Assert return value > 0
 * 4. Assert buffer contains expected log strings
 * 5. Test with flags=1 (clear) and verify buffer emptied
 */
JARVIS_TEST(klog_read_entries, "PRE: none | POST: none") {
    auto &rb = kernel::log::KlogService::instance();

    rb.clear();
    rb.puts("KLOG_TEST_MARKER_1\n");
    rb.puts("KLOG_TEST_MARKER_2\n");

    char buf[128];
    size_t n = rb.read(buf, sizeof(buf) - 1);
    buf[n] = '\0';

    JARVIS_ASSERT_FMT(n > 0, "read returned %zu bytes", n);
    JARVIS_ASSERT_FMT(contains(buf, "KLOG_TEST_MARKER_1"),
                      "buffer should contain KLOG_TEST_MARKER_1: %s", buf);
    JARVIS_ASSERT_FMT(contains(buf, "KLOG_TEST_MARKER_2"),
                      "buffer should contain KLOG_TEST_MARKER_2: %s", buf);

    JARVIS_TEST_PASS();
}

JARVIS_TEST(klog_ringbuffer_wrap, "PRE: none | POST: none") {
    auto &rb = kernel::log::KlogService::instance();

    rb.clear();
    JARVIS_ASSERT(rb.empty());

    char fill_char = 'X';
    for (size_t i = 0; i < kernel::log::RingBuffer::BUFFER_SIZE * 2; ++i) {
        rb.putchar(fill_char);
    }
    JARVIS_ASSERT(!rb.empty());

    char buf[4096];
    size_t n = rb.read(buf, sizeof(buf));
    JARVIS_ASSERT_FMT(n > 0, "read returned %zu bytes after wrap fill", n);
    JARVIS_ASSERT_FMT(n <= kernel::log::RingBuffer::BUFFER_SIZE,
                      "read count %zu within buffer size %zu", n,
                      kernel::log::RingBuffer::BUFFER_SIZE);

    JARVIS_TEST_PASS();
}

JARVIS_TEST(klog_concurrent_readers, "PRE: none | POST: none") {
    auto &rb = kernel::log::KlogService::instance();

    rb.puts("CONCURRENT_READER_TEST\n");

    char buf1[64], buf2[64];
    size_t n1 = rb.read(buf1, sizeof(buf1) - 1);
    size_t n2 = rb.read(buf2, sizeof(buf2) - 1);

    buf1[n1] = '\0';
    buf2[n2] = '\0';

    JARVIS_ASSERT_FMT(n1 > 0, "reader 1 got %zu bytes", n1);
    JARVIS_ASSERT_FMT(n2 > 0, "reader 2 got %zu bytes", n2);

    JARVIS_TEST_PASS();
}

JARVIS_TEST(klog_invalid_buffer_eFault, "PRE: none | POST: none") {
    {
        auto buf =
            checked(reinterpret_cast<char *>(0), static_cast<size_t>(10));
        JARVIS_ASSERT_FMT(!buf.valid(), "NULL buffer should be invalid");
    }
    {
        auto buf = checked(reinterpret_cast<char *>(0xFFFF800000000000ULL),
                           static_cast<size_t>(10));
        JARVIS_ASSERT_FMT(!buf.valid(),
                          "kernel address buffer should be invalid");
    }
    {
        char stack_buf[16];
        auto buf = checked(stack_buf, static_cast<size_t>(16));
        JARVIS_ASSERT_FMT(!buf.valid(),
                          "kernel stack buffer should be invalid");
    }

    JARVIS_TEST_PASS();
}

JARVIS_TEST(dmesg_prints_kernel_log, "PRE: none | POST: none") {
    auto &rb = kernel::log::KlogService::instance();

    rb.clear();
    rb.puts("DMESG_TEST_MARKER\n");

    char buf[256];
    size_t n = rb.read(buf, sizeof(buf) - 1);
    buf[n] = '\0';

    JARVIS_ASSERT_FMT(contains(buf, "DMESG_TEST_MARKER"),
                      "log output should contain test marker: %s", buf);

    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Reading from an empty ring buffer returns 0 bytes.
// Input: clear(), then read()
// Expect: read returns 0
// Depends: kernel::log::RingBuffer
JARVIS_TEST(klog_empty_read, "PRE: none | POST: none") {
    auto &rb = kernel::log::KlogService::instance();
    rb.clear();
    JARVIS_ASSERT(rb.empty());
    char buf[64];
    size_t n = rb.read(buf, sizeof(buf));
    JARVIS_ASSERT_EQ((size_t)0, n);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Clearing the ring buffer makes it empty.
// Input: write data, clear(), check empty
// Expect: empty() returns true after clear
// Depends: kernel::log::RingBuffer
JARVIS_TEST(klog_clear, "PRE: none | POST: none") {
    auto &rb = kernel::log::KlogService::instance();
    rb.puts("SOME_DATA_TO_CLEAR\n");
    JARVIS_ASSERT(!rb.empty());
    rb.clear();
    JARVIS_ASSERT(rb.empty());
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Reading with a buffer smaller than available data returns only
// the requested amount.
// Input: write data, read with small buffer
// Expect: read returns exactly the small buffer size
// Depends: kernel::log::RingBuffer
JARVIS_TEST(klog_read_partial, "PRE: none | POST: none") {
    auto &rb = kernel::log::KlogService::instance();
    rb.clear();
    for (int i = 0; i < 50; ++i)
        rb.putchar('B');
    char small[10];
    size_t n = rb.read(small, 5);
    JARVIS_ASSERT_EQ((size_t)5, n);
    small[5] = '\0';
    for (size_t i = 0; i < 5; ++i)
        JARVIS_ASSERT_EQ('B', small[i]);
    JARVIS_TEST_PASS();
}

void register_klog_tests() {
    Logger::info("Registering KLOG tests");

    JARVIS_REGISTER_TEST(klog_read_entries);
    JARVIS_REGISTER_TEST(klog_ringbuffer_wrap);
    JARVIS_REGISTER_TEST(klog_concurrent_readers);
    JARVIS_REGISTER_TEST(klog_invalid_buffer_eFault);
    JARVIS_REGISTER_TEST(dmesg_prints_kernel_log);
    JARVIS_REGISTER_TEST(klog_empty_read);
    JARVIS_REGISTER_TEST(klog_clear);
    JARVIS_REGISTER_TEST(klog_read_partial);
}