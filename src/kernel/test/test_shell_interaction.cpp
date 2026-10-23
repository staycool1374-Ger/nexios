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

/// @file test_shell_interaction.cpp
/// @brief Shell interaction / command execution tests.

#if defined(CONFIG_ARCH_X86_64)
#include <test.hpp>
#include <logger.hpp>
#include <services/shell.hpp>
#include <services/terminal/terminal.hpp>
#include <kernel/arch/io.hpp>
#include <kernel/arch/timer.hpp>
#include <kernel/arch/keyboard.hpp>
#include <kernel/driver/iocd.hpp>
#include <kernel/input/serial_input.hpp>
#include <kernel/console/urc.hpp>
#include <kernel/ipc/ipc.hpp>
#include <constants.hpp>

using namespace kernel;

static void serial_flush_rx() {
    while (arch::inb(arch::COM1_LSR) & 1) {
        arch::inb(arch::COM1);
    }
}

static bool serial_read_all(char *buf, size_t max, int iterations) {
    size_t pos = 0;
    for (int i = 0; i < iterations && pos < max - 1; ++i) {
        if (arch::inb(arch::COM1_LSR) & 1) {
            buf[pos++] = arch::inb(arch::COM1);
        }
        arch::pause();
    }
    buf[pos] = '\0';
    return pos > 0;
}

static bool serial_contains(const char *buf, const char *needle) {
    for (size_t i = 0; buf[i] != '\0'; ++i) {
        size_t j;
        for (j = 0; needle[j] != '\0'; ++j) {
            if (buf[i + j] != needle[j])
                break;
        }
        if (needle[j] == '\0')
            return true;
    }
    return false;
}

static constexpr size_t kSubstrNotFound = static_cast<size_t>(-1);

// Index of the first occurrence of needle in buf, or kSubstrNotFound.
static size_t substr_index(const char *buf, const char *needle) {
    for (size_t start = 0; buf[start] != '\0'; ++start) {
        size_t offset = 0;
        for (; needle[offset] != '\0'; ++offset) {
            if (buf[start + offset] != needle[offset])
                break;
        }
        if (needle[offset] == '\0')
            return start;
    }
    return kSubstrNotFound;
}

// Non-overlapping occurrence count of needle in buf.
static size_t count_occurrences(const char *buf, const char *needle) {
    size_t found = 0;
    size_t needle_len = 0;
    while (needle[needle_len] != '\0')
        ++needle_len;
    if (needle_len == 0)
        return 0;
    for (size_t pos = 0; buf[pos] != '\0';) {
        size_t offset = 0;
        while (offset < needle_len && buf[pos + offset] == needle[offset])
            ++offset;
        if (offset == needle_len) {
            ++found;
            pos += needle_len;
        } else {
            ++pos;
        }
    }
    return found;
}

JARVIS_TEST(shell_loopback_manual_string, "PRE: vfsd, iocd | POST: none") {
    uint8_t mcr = arch::inb(arch::COM1 + 4);
    arch::outb(arch::COM1 + 4, mcr | 0x10);
    serial_flush_rx();

    const char *test_str = "HelloWorld";
    for (const char *p = test_str; *p; ++p) {
        while ((arch::inb(arch::COM1_LSR) & 0x20) == 0)
            ;
        arch::outb(arch::COM1, *p);
    }

    char buf[64];
    bool ok = serial_read_all(buf, sizeof(buf), 100000);
    arch::outb(arch::COM1 + 4, mcr);

    JARVIS_ASSERT(ok);
    JARVIS_ASSERT(serial_contains(buf, "HelloWorld"));
}

JARVIS_TEST(shell_terminal_write, "PRE: vfsd, iocd | POST: none") {
    uint8_t mcr = arch::inb(arch::COM1 + 4);
    arch::outb(arch::COM1 + 4, mcr | 0x10);
    serial_flush_rx();

    service::Terminal::write("TESTMARKER");

    char buf[256];
    bool ok = serial_read_all(buf, sizeof(buf), 200000);
    arch::outb(arch::COM1 + 4, mcr);

    JARVIS_ASSERT(ok);
    JARVIS_ASSERT(serial_contains(buf, "TESTMARKER"));
}

JARVIS_TEST(shell_simple_write, "PRE: vfsd, iocd | POST: none") {
    uint8_t mcr = arch::inb(arch::COM1 + 4);
    arch::outb(arch::COM1 + 4, mcr | 0x10);
    serial_flush_rx();

    service::Terminal::write("SIMPLE_TEST");

    char buf[64];
    bool ok = serial_read_all(buf, sizeof(buf), 200000);
    arch::outb(arch::COM1 + 4, mcr);

    JARVIS_ASSERT(ok);
    JARVIS_ASSERT(serial_contains(buf, "SIMPLE_TEST"));
}

JARVIS_TEST(shell_write_with_newline, "PRE: vfsd, iocd | POST: none") {
    uint8_t mcr = arch::inb(arch::COM1 + 4);
    arch::outb(arch::COM1 + 4, mcr | 0x10);
    serial_flush_rx();

    service::Terminal::write("LINE1\nLINE2\n");

    char buf[512];
    bool ok = serial_read_all(buf, sizeof(buf), 500000);
    arch::outb(arch::COM1 + 4, mcr);

    JARVIS_ASSERT(ok);
    JARVIS_ASSERT(serial_contains(buf, "LINE1"));
    JARVIS_ASSERT(serial_contains(buf, "LINE2"));
}

JARVIS_TEST(shell_echo_and_write, "PRE: vfsd, iocd | POST: none") {
    uint8_t mcr = arch::inb(arch::COM1 + 4);
    arch::outb(arch::COM1 + 4, mcr | 0x10);
    serial_flush_rx();

    service::Terminal::write("ALPHA");
    service::Terminal::write("BETA");

    char buf[512];
    bool ok = serial_read_all(buf, sizeof(buf), 500000);
    arch::outb(arch::COM1 + 4, mcr);

    JARVIS_ASSERT(ok);
    JARVIS_ASSERT(serial_contains(buf, "ALPHA"));
    JARVIS_ASSERT(serial_contains(buf, "BETA"));
}

JARVIS_TEST(shell_heap_delete, "PRE: vfsd, iocd | POST: none") {
    uint8_t mcr = arch::inb(arch::COM1 + 4);
    arch::outb(arch::COM1 + 4, mcr | 0x10);
    serial_flush_rx();

    service::Terminal::write("BEFORE");
    char *tmp = new char[8];
    delete[] tmp;
    service::Terminal::write("END");

    char buf[512];
    bool ok = serial_read_all(buf, sizeof(buf), 500000);
    arch::outb(arch::COM1 + 4, mcr);

    JARVIS_ASSERT(ok);
    JARVIS_ASSERT(serial_contains(buf, "BEFORE"));
    JARVIS_ASSERT(serial_contains(buf, "END"));
}

JARVIS_TEST(shell_execute_serial, "PRE: vfsd, iocd | POST: none") {
    uint8_t orig_mcr = arch::inb(arch::COM1 + 4);
    arch::outb(arch::COM1 + 4, orig_mcr | 0x10);
    serial_flush_rx();

    service::Terminal::write("BEFORE");
    service::Shell::execute("garbing");

    char buf1[128];
    size_t pos = 0;
    for (int i = 0; i < 100000 && pos < sizeof(buf1) - 1; ++i) {
        if (arch::inb(arch::COM1_LSR) & 1) {
            buf1[pos++] = arch::inb(arch::COM1);
        }
        arch::pause();
    }
    buf1[pos] = '\0';

    service::Terminal::write("AFTER");

    char buf2[128];
    bool ok = serial_read_all(buf2, sizeof(buf2), 500000);
    arch::outb(arch::COM1 + 4, orig_mcr);

    JARVIS_ASSERT(ok);
    JARVIS_ASSERT(serial_contains(buf1, "BEFORE"));
    JARVIS_ASSERT(serial_contains(buf2, "AFTER"));
    JARVIS_ASSERT(serial_contains(buf1, "Unknown"));
}

JARVIS_TEST(shell_help_command, "PRE: vfsd, iocd | POST: none") {
    uint8_t orig_mcr = arch::inb(arch::COM1 + 4);
    arch::outb(arch::COM1 + 4, orig_mcr | 0x10);
    serial_flush_rx();

    service::Terminal::write("BEFORE");
    service::Shell::execute("help");

    char buf1[128];
    size_t pos = 0;
    for (int i = 0; i < 200000 && pos < sizeof(buf1) - 1; ++i) {
        if (arch::inb(arch::COM1_LSR) & 1) {
            buf1[pos++] = arch::inb(arch::COM1);
        }
        arch::pause();
    }
    buf1[pos] = '\0';

    service::Terminal::write("AFTER");

    char buf2[128];
    bool ok = serial_read_all(buf2, sizeof(buf2), 500000);
    arch::outb(arch::COM1 + 4, orig_mcr);

    JARVIS_ASSERT(ok);
    JARVIS_ASSERT(serial_contains(buf1, "BEFORE"));
    JARVIS_ASSERT(serial_contains(buf2, "AFTER"));
    // Loopback FIFO window: buf1 holds BEFORE(6) + CRLF(2) + the first
    // 8 header chars only — assert that prefix (as "Verfueg" did).
    JARVIS_ASSERT(serial_contains(buf1, "Availab"));
}

JARVIS_TEST(shell_echo_command, "PRE: vfsd, iocd | POST: none") {
    uint8_t orig_mcr = arch::inb(arch::COM1 + 4);
    arch::outb(arch::COM1 + 4, orig_mcr | 0x10);
    serial_flush_rx();

    service::Terminal::write("BEFORE");
    service::Shell::execute("echo Hello World");

    char buf1[128];
    size_t pos = 0;
    for (int i = 0; i < 200000 && pos < sizeof(buf1) - 1; ++i) {
        if (arch::inb(arch::COM1_LSR) & 1) {
            buf1[pos++] = arch::inb(arch::COM1);
        }
        arch::pause();
    }
    buf1[pos] = '\0';

    service::Terminal::write("AFTER");

    char buf2[128];
    bool ok = serial_read_all(buf2, sizeof(buf2), 500000);
    arch::outb(arch::COM1 + 4, orig_mcr);

    JARVIS_ASSERT(ok);
    JARVIS_ASSERT(serial_contains(buf1, "BEFORE"));
    JARVIS_ASSERT(serial_contains(buf2, "AFTER"));
    JARVIS_ASSERT(serial_contains(buf1, "Hello"));
}

JARVIS_TEST(shell_uptime_command, "PRE: vfsd, iocd | POST: none") {
    uint8_t orig_mcr = arch::inb(arch::COM1 + 4);
    arch::outb(arch::COM1 + 4, orig_mcr | 0x10);
    serial_flush_rx();

    service::Terminal::write("BEFORE");
    service::Shell::execute("uptime");

    char buf1[128];
    size_t pos = 0;
    for (int i = 0; i < 200000 && pos < sizeof(buf1) - 1; ++i) {
        if (arch::inb(arch::COM1_LSR) & 1) {
            buf1[pos++] = arch::inb(arch::COM1);
        }
        arch::pause();
    }
    buf1[pos] = '\0';

    service::Terminal::write("AFTER");

    char buf2[128];
    bool ok = serial_read_all(buf2, sizeof(buf2), 500000);
    arch::outb(arch::COM1 + 4, orig_mcr);

    JARVIS_ASSERT(ok);
    JARVIS_ASSERT(serial_contains(buf1, "BEFORE"));
    JARVIS_ASSERT(serial_contains(buf2, "AFTER"));
    JARVIS_ASSERT(serial_contains(buf1, "Uptime"));
}

// Runmode: kernel (release)
// Testidea: The `tasks` command reports per-task memory info: stack
//           high-water/low-water, stack size, and text/data/bss segments.
// Input: Shell::execute("tasks") with Terminal capture enabled
// Expect: Captured output contains the new memory columns
// Depends: service::Shell, service::Terminal, kernel::TaskControlBlock
JARVIS_TEST(shell_tasks_memory_columns, "PRE: vfsd, iocd | POST: none") {
    // Capture must hold MAX_TASKS rows (~108 B each) + headers.
    char cap[CONFIG_MAX_TASKS * 128u + 512u];
    service::Terminal::capture_begin(cap, sizeof(cap));
    service::Shell::execute("tasks");
    service::Terminal::capture_end();

    // New memory columns present in the header.
    JARVIS_ASSERT(serial_contains(cap, "/STACK"));
    JARVIS_ASSERT(serial_contains(cap, "/TEXT"));
    JARVIS_ASSERT(serial_contains(cap, "/DATA"));
    JARVIS_ASSERT(serial_contains(cap, "/BSS"));
    JARVIS_ASSERT(serial_contains(cap, "/HEAP"));
    JARVIS_ASSERT(serial_contains(cap, "MEM_PG"));
    JARVIS_ASSERT(serial_contains(cap, "CPU_TIME"));
}

JARVIS_TEST(shell_pwd_command, "PRE: vfsd, iocd | POST: none") {
    uint8_t orig_mcr = arch::inb(arch::COM1 + 4);
    arch::outb(arch::COM1 + 4, orig_mcr | 0x10);
    serial_flush_rx();

    service::Terminal::write("BEFORE");
    service::Shell::execute("pwd");

    char buf1[128];
    size_t pos = 0;
    for (int i = 0; i < 200000 && pos < sizeof(buf1) - 1; ++i) {
        if (arch::inb(arch::COM1_LSR) & 1) {
            buf1[pos++] = arch::inb(arch::COM1);
        }
        arch::pause();
    }
    buf1[pos] = '\0';

    service::Terminal::write("AFTER");

    char buf2[128];
    bool ok = serial_read_all(buf2, sizeof(buf2), 500000);
    arch::outb(arch::COM1 + 4, orig_mcr);

    JARVIS_ASSERT(ok);
    JARVIS_ASSERT(serial_contains(buf1, "BEFORE"));
    JARVIS_ASSERT(serial_contains(buf2, "AFTER"));
    JARVIS_ASSERT(serial_contains(buf1, "/"));
}

JARVIS_TEST(shell_which_command, "PRE: vfsd, iocd | POST: none") {
    uint8_t orig_mcr = arch::inb(arch::COM1 + 4);
    arch::outb(arch::COM1 + 4, orig_mcr | 0x10);
    serial_flush_rx();

    service::Terminal::write("BEFORE");
    service::Shell::execute("locate help");

    char buf1[128];
    size_t pos = 0;
    for (int i = 0; i < 200000 && pos < sizeof(buf1) - 1; ++i) {
        if (arch::inb(arch::COM1_LSR) & 1) {
            buf1[pos++] = arch::inb(arch::COM1);
        }
        arch::pause();
    }
    buf1[pos] = '\0';

    service::Terminal::write("AFTER");

    char buf2[128];
    bool ok = serial_read_all(buf2, sizeof(buf2), 500000);
    arch::outb(arch::COM1 + 4, orig_mcr);

    JARVIS_ASSERT(ok);
    JARVIS_ASSERT(serial_contains(buf1, "BEFORE"));
    JARVIS_ASSERT(serial_contains(buf2, "AFTER"));
    JARVIS_ASSERT(serial_contains(buf1, " ("));
    JARVIS_ASSERT(serial_contains(buf1, "help"));
}

JARVIS_TEST(shell_help_shows_which, "PRE: vfsd, iocd | POST: none") {
    // Verify "locate" is registered and would appear in help output.
    // Can't capture full help via serial loopback (16-byte RX FIFO limit),
    // so instead: run "locate locate" — dispatch proves registration,
    // and cmd_which finding itself proves it's in the command list.
    uint8_t orig_mcr = arch::inb(arch::COM1 + 4);
    arch::outb(arch::COM1 + 4, orig_mcr | 0x10);
    serial_flush_rx();

    service::Terminal::write("BEFORE");
    service::Shell::execute("locate locate");

    char buf1[128];
    size_t pos = 0;
    for (int i = 0; i < 200000 && pos < sizeof(buf1) - 1; ++i) {
        if (arch::inb(arch::COM1_LSR) & 1) {
            buf1[pos++] = arch::inb(arch::COM1);
        }
        arch::pause();
    }
    buf1[pos] = '\0';

    service::Terminal::write("AFTER");

    char buf2[128];
    bool ok = serial_read_all(buf2, sizeof(buf2), 500000);
    arch::outb(arch::COM1 + 4, orig_mcr);

    JARVIS_ASSERT(ok);
    JARVIS_ASSERT(serial_contains(buf1, "BEFORE"));
    JARVIS_ASSERT(serial_contains(buf2, "AFTER"));
    JARVIS_ASSERT(serial_contains(buf1, " ("));
    JARVIS_ASSERT(serial_contains(buf1, "locate"));
}

JARVIS_TEST(shell_which_notfound, "PRE: vfsd, iocd | POST: none") {
    uint8_t orig_mcr = arch::inb(arch::COM1 + 4);
    arch::outb(arch::COM1 + 4, orig_mcr | 0x10);
    serial_flush_rx();

    service::Terminal::write("BEFORE");
    service::Shell::execute("locate xyzzy");

    char buf1[128];
    size_t pos = 0;
    for (int i = 0; i < 200000 && pos < sizeof(buf1) - 1; ++i) {
        if (arch::inb(arch::COM1_LSR) & 1) {
            buf1[pos++] = arch::inb(arch::COM1);
        }
        arch::pause();
    }
    buf1[pos] = '\0';

    service::Terminal::write("AFTER");

    char buf2[128];
    bool ok = serial_read_all(buf2, sizeof(buf2), 500000);
    arch::outb(arch::COM1 + 4, orig_mcr);

    JARVIS_ASSERT(ok);
    JARVIS_ASSERT(serial_contains(buf1, "BEFORE"));
    JARVIS_ASSERT(serial_contains(buf2, "AFTER"));
    JARVIS_ASSERT(serial_contains(buf1, ": not"));
    JARVIS_ASSERT(serial_contains(buf1, "xyzzy"));
}

JARVIS_TEST(shell_env_command, "PRE: vfsd, iocd | POST: none") {
    uint8_t orig_mcr = arch::inb(arch::COM1 + 4);
    arch::outb(arch::COM1 + 4, orig_mcr | 0x10);
    serial_flush_rx();

    service::Terminal::write("BEFORE");
    service::Shell::execute("env");

    char buf1[128];
    size_t pos = 0;
    for (int i = 0; i < 200000 && pos < sizeof(buf1) - 1; ++i) {
        if (arch::inb(arch::COM1_LSR) & 1) {
            buf1[pos++] = arch::inb(arch::COM1);
        }
        arch::pause();
    }
    buf1[pos] = '\0';

    service::Terminal::write("AFTER");

    char buf2[128];
    bool ok = serial_read_all(buf2, sizeof(buf2), 500000);
    arch::outb(arch::COM1 + 4, orig_mcr);

    JARVIS_ASSERT(ok);
    JARVIS_ASSERT(serial_contains(buf1, "BEFORE"));
    JARVIS_ASSERT(serial_contains(buf2, "AFTER"));
}

JARVIS_TEST(shell_export_command, "PRE: vfsd, iocd | POST: none") {
    uint8_t orig_mcr = arch::inb(arch::COM1 + 4);
    arch::outb(arch::COM1 + 4, orig_mcr | 0x10);
    serial_flush_rx();

    service::Terminal::write("BEFORE");
    service::Shell::execute("export FOO=bar");

    char buf1[128];
    size_t pos = 0;
    for (int i = 0; i < 200000 && pos < sizeof(buf1) - 1; ++i) {
        if (arch::inb(arch::COM1_LSR) & 1) {
            buf1[pos++] = arch::inb(arch::COM1);
        }
        arch::pause();
    }
    buf1[pos] = '\0';

    service::Terminal::write("AFTER");

    char buf2[128];
    bool ok = serial_read_all(buf2, sizeof(buf2), 500000);
    arch::outb(arch::COM1 + 4, orig_mcr);

    JARVIS_ASSERT(ok);
    JARVIS_ASSERT(serial_contains(buf1, "BEFORE"));
    JARVIS_ASSERT(serial_contains(buf2, "AFTER"));
}

JARVIS_TEST(shell_sleep_zero, "PRE: vfsd, iocd | POST: none") {
    uint8_t orig_mcr = arch::inb(arch::COM1 + 4);
    arch::outb(arch::COM1 + 4, orig_mcr | 0x10);
    serial_flush_rx();

    service::Terminal::write("BEFORE");
    service::Shell::execute("sleep 0");

    char buf1[128];
    size_t pos = 0;
    for (int i = 0; i < 200000 && pos < sizeof(buf1) - 1; ++i) {
        if (arch::inb(arch::COM1_LSR) & 1) {
            buf1[pos++] = arch::inb(arch::COM1);
        }
        arch::pause();
    }
    buf1[pos] = '\0';

    service::Terminal::write("AFTER");

    char buf2[128];
    bool ok = serial_read_all(buf2, sizeof(buf2), 500000);
    arch::outb(arch::COM1 + 4, orig_mcr);

    JARVIS_ASSERT(ok);
    JARVIS_ASSERT(serial_contains(buf1, "BEFORE"));
    JARVIS_ASSERT(serial_contains(buf2, "AFTER"));
}

// Runmode: kernel
// Testidea: Verifies the "clear" built-in command executes without crash.
// Input: Shell::execute("clear")
// Expect: No crash, terminal cleared
// Depends: service::Shell, service::Terminal
JARVIS_TEST(shell_clear_command, "PRE: vfsd, iocd | POST: none") {
    service::Shell::execute("clear");
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: shell P2 key-delivery consumes a daemon-owned byte through
//           the production poll (issue #303 item 3 seam; the #340 P2 path
//           the tracker left manual-only).
// Input: ownership on; inject 'a' press (set-1 0x1E) via 0xD2 + genuine
//        handle_irq; tick-spin until the daemon queues it; drive the SEAM
//        (service::shell_try_get_key) twice. Ownership cleared before
//        asserts so an abort cannot leak the routing.
// Expect: first seam call true + 'a'; second call false (exactly-once);
//         kernel hardware ring empty (no double delivery).
// Depends: shell_try_get_key seam + iocd decode/queue + ownership routing.
JARVIS_TEST(shell_key_delivery_via_seam, "PRE: vfsd, iocd | POST: none") {
    uint64_t iocd_pid = kernel::iocd::get_iocd_pid();
    JARVIS_ASSERT_FMT(iocd_pid != 0, "iocd not running");
    serial_flush_rx();
    arch::Keyboard::flush();

    kernel::iocd::set_keyboard_owned(true);
    arch::outb(0x64, 0xD2); // i8042: write keyboard output buffer
    arch::outb(0x60, 0x1E); // 'a' press
    arch::Keyboard::handle_irq();

    // The daemon needs timer ticks to run; spin until a seam poll
    // observes the byte (bounded: 50-tick window, cross_boundary idiom).
    char got = 0;
    bool delivered = false;
    const uint64_t deadline = arch::Timer::ticks() + 50;
    while (!delivered && arch::Timer::ticks() < deadline) {
        if (service::shell_try_get_key(got))
            delivered = true;
        else
            arch::pause();
    }
    // Exactly-once + single ownership, collected before asserts.
    char second = 0;
    const bool second_empty = !service::shell_try_get_key(second);
    char ring_byte = 0;
    const bool ring_empty = !arch::Keyboard::getchar(ring_byte);
    kernel::iocd::set_keyboard_owned(false);

    JARVIS_ASSERT_FMT(delivered, "seam saw no byte within 50 ticks");
    JARVIS_ASSERT_FMT(got == 'a', "seam byte 0x%x != 'a'", got);
    JARVIS_ASSERT_FMT(second_empty, "seam delivered twice");
    JARVIS_ASSERT_FMT(ring_empty, "byte duplicated into kernel ring");
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: shell_peek_input/shell_consume_input drive the exact
// readline fast-path seam (pump + peek + consume): injected bytes
// surface as one contiguous run and consume drops exactly that run.
// Input: flush RX; ring reset; inject "abc"; shell_peek_input;
//        shell_consume_input(3); shell_peek_input.
// Expect: peek true, len 3, bytes "abc"; consume returns 3; second
//         peek false.
// Depends: service shell seam + input::serial ring (issue #407)
JARVIS_TEST(shell_peek_slice_seam, "PRE: vfsd, iocd | POST: none") {
    serial_flush_rx();
    kernel::input::serial::reset();
    size_t accepted = kernel::input::serial::inject("abc", 3);
    const char *data = nullptr;
    size_t len = 0;
    bool exposed = service::shell_peek_input(data, len);
    bool bytes_match = exposed && len == 3 && memcmp(data, "abc", 3) == 0;
    size_t consumed = service::shell_consume_input(3);
    const char *tail = nullptr;
    size_t tail_len = 0;
    bool empty = !service::shell_peek_input(tail, tail_len);
    kernel::input::serial::reset();
    JARVIS_ASSERT_EQ(static_cast<uint64_t>(3),
                      static_cast<uint64_t>(accepted));
    JARVIS_ASSERT(bytes_match);
    JARVIS_ASSERT_EQ(static_cast<uint64_t>(3),
                      static_cast<uint64_t>(consumed));
    JARVIS_ASSERT(empty);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: CR normalizes to newline through the shell seam
// (anti-#245): readline's scan stops at '\n'; a raw '\r' reaching the
// echo path would trip the async-output guard (#248). The seam must
// expose the decoded newline, never the raw carriage return.
// Input: flush RX; ring reset; inject "a\r"; shell_peek_input;
//        shell_consume_input(2); shell_peek_input.
// Expect: peek true, len 2, bytes {'a','\n'}; consume returns 2;
//         second peek false.
// Depends: service shell seam + push_decoded (issue #407)
JARVIS_TEST(shell_slice_normalized_newline, "PRE: vfsd, iocd | POST: none") {
    serial_flush_rx();
    kernel::input::serial::reset();
    size_t accepted = kernel::input::serial::inject("a\r", 2);
    const char *data = nullptr;
    size_t len = 0;
    bool exposed = service::shell_peek_input(data, len);
    bool bytes_match = exposed && len == 2 && data[0] == 'a' &&
                       data[1] == '\n';
    size_t consumed = service::shell_consume_input(2);
    const char *tail = nullptr;
    size_t tail_len = 0;
    bool empty = !service::shell_peek_input(tail, tail_len);
    kernel::input::serial::reset();
    JARVIS_ASSERT_EQ(static_cast<uint64_t>(2),
                      static_cast<uint64_t>(accepted));
    JARVIS_ASSERT(bytes_match);
    JARVIS_ASSERT_EQ(static_cast<uint64_t>(2),
                      static_cast<uint64_t>(consumed));
    JARVIS_ASSERT(empty);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: URC queue preserves FIFO order with per-tag prefixes (#407-2).
// Input: urc reset; emit Elf/Kill/Test; pop x4.
// Expect: "[ELF] a", "[KILL] b", "[TST] c" in order; 4th pop false;
//         all counters zero, queue not pending.
// Depends: console::urc emit/pop (issue #407)
JARVIS_TEST(urc_emit_pop_fifo, "PRE: none | POST: none") {
    console::urc::reset();
    bool queued_elf = console::urc::emit(console::urc::Tag::Elf, "a");
    bool queued_kill = console::urc::emit(console::urc::Tag::Kill, "b");
    bool queued_test = console::urc::emit(console::urc::Tag::Test, "c");
    bool order_ok = queued_elf && queued_kill && queued_test;
    char line_buf[console::urc::kLineMax] = {};
    order_ok = order_ok && console::urc::pop(line_buf, sizeof(line_buf));
    order_ok = order_ok && memcmp(line_buf, "[ELF] a", 8) == 0;
    order_ok = order_ok && console::urc::pop(line_buf, sizeof(line_buf));
    order_ok = order_ok && memcmp(line_buf, "[KILL] b", 9) == 0;
    order_ok = order_ok && console::urc::pop(line_buf, sizeof(line_buf));
    order_ok = order_ok && memcmp(line_buf, "[TST] c", 8) == 0;
    bool drained = !console::urc::pop(line_buf, sizeof(line_buf));
    bool counters_clean = console::urc::dropped_count() == 0 &&
                          console::urc::truncated_count() == 0 &&
                          console::urc::rejected_count() == 0;
    bool queue_idle = !console::urc::pending();
    console::urc::reset();
    JARVIS_ASSERT(order_ok);
    JARVIS_ASSERT(drained);
    JARVIS_ASSERT(counters_clean);
    JARVIS_ASSERT(queue_idle);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: oversize payloads truncate with "..." and count once (#407-2).
// Input: urc reset; emit one 199-char payload; pop.
// Expect: popped len kLineMax-1, starts "[ELF] ", ends "...",
//         truncated_count 1.
// Depends: console::urc emit/pop (issue #407)
JARVIS_TEST(urc_payload_truncation_counted, "PRE: none | POST: none") {
    console::urc::reset();
    char long_payload[200] = {};
    for (size_t fill = 0; fill < sizeof(long_payload) - 1; ++fill)
        long_payload[fill] = 'x';
    bool queued = console::urc::emit(console::urc::Tag::Elf, long_payload);
    char line_buf[console::urc::kLineMax] = {};
    bool popped = console::urc::pop(line_buf, sizeof(line_buf));
    size_t line_len = 0;
    while (line_buf[line_len] != '\0' && line_len < sizeof(line_buf))
        ++line_len;
    bool head_ok = memcmp(line_buf, "[ELF] ", 6) == 0;
    bool tail_ok = line_len == console::urc::kLineMax - 1 &&
                   line_buf[line_len - 1] == '.' &&
                   line_buf[line_len - 2] == '.' &&
                   line_buf[line_len - 3] == '.';
    bool counted_once = console::urc::truncated_count() == 1;
    console::urc::reset();
    JARVIS_ASSERT(queued);
    JARVIS_ASSERT(popped);
    JARVIS_ASSERT(head_ok);
    JARVIS_ASSERT(tail_ok);
    JARVIS_ASSERT(counted_once);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: full queue drops newest, keeps order, counts drops (#407-2).
// Input: urc reset; emit kQueueDepth+3 single-char lines; drain.
// Expect: dropped_count 3; first 16 pop in emission order;
//         17th pop false.
// Depends: console::urc emit/pop (issue #407)
JARVIS_TEST(urc_overflow_drop_newest_keeps_order, "PRE: none | POST: none") {
    console::urc::reset();
    for (size_t emit_idx = 0;
         emit_idx < console::urc::kQueueDepth + 3; ++emit_idx) {
        char payload[2] = {static_cast<char>('A' + emit_idx), '\0'};
        (void)console::urc::emit(console::urc::Tag::Test, payload);
    }
    bool drops_counted = console::urc::dropped_count() == 3;
    bool order_kept = true;
    char line_buf[console::urc::kLineMax] = {};
    for (size_t pop_idx = 0; pop_idx < console::urc::kQueueDepth; ++pop_idx) {
        char want[8] = {'[', 'T', 'S', 'T', ']', ' ', '?', '\0'};
        want[6] = static_cast<char>('A' + pop_idx);
        if (!console::urc::pop(line_buf, sizeof(line_buf)) ||
            memcmp(line_buf, want, sizeof(want)) != 0)
            order_kept = false;
    }
    bool drained = !console::urc::pop(line_buf, sizeof(line_buf));
    console::urc::reset();
    JARVIS_ASSERT(drops_counted);
    JARVIS_ASSERT(order_kept);
    JARVIS_ASSERT(drained);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: seal() freezes the prefix table; known tags still queue,
// unknown ids and late registration are counted rejects (#407-2).
// Input: urc reset; seal; register "NEW"; emit Elf; emit id 0xFE.
// Expect: register returns kNoPrefix; Elf queues and pops;
//         rejected_count 2 (late register + unknown id).
// Depends: console::urc seal/register/emit (issue #407)
JARVIS_TEST(urc_seal_rejects_new_prefix, "PRE: none | POST: none") {
    console::urc::reset();
    console::urc::seal();
    bool was_sealed = console::urc::sealed();
    uint8_t late_id = console::urc::register_prefix("NEW");
    bool late_rejected = late_id == console::urc::kNoPrefix;
    bool known_queues = console::urc::emit(console::urc::Tag::Elf, "ok");
    bool unknown_rejected =
        !console::urc::emit(static_cast<console::urc::Tag>(0xFE), "bad");
    bool rejects_counted = console::urc::rejected_count() == 2;
    char line_buf[console::urc::kLineMax] = {};
    bool known_pops = console::urc::pop(line_buf, sizeof(line_buf)) &&
                      memcmp(line_buf, "[ELF] ok", 9) == 0;
    bool drained = !console::urc::pop(line_buf, sizeof(line_buf));
    console::urc::reset();
    bool seal_cleared = !console::urc::sealed();
    JARVIS_ASSERT(was_sealed);
    JARVIS_ASSERT(late_rejected);
    JARVIS_ASSERT(known_queues);
    JARVIS_ASSERT(unknown_rejected);
    JARVIS_ASSERT(rejects_counted);
    JARVIS_ASSERT(known_pops);
    JARVIS_ASSERT(drained);
    JARVIS_ASSERT(seal_cleared);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: the production drain prints a drop notice plus at most
// kDrainMaxPerPass lines per pass, oldest first (#407-2).
// Input: urc reset; capture; emit 10 lines; flush; flush; capture end.
// Expect: first flush true, contains line0..line7 in order, no
//         line8/line9; pending true; second flush drains 2, pending
//         false. Overflow leg: 17 emits, flush shows "[URC] 1 dropped"
//         before the first payload line.
// Depends: shell_flush_urcs_for_test + console::urc (issue #407)
JARVIS_TEST(urc_flush_frames_and_bounds, "PRE: none | POST: none") {
    console::urc::reset();
    char capture[1024] = {};
    service::Terminal::capture_begin(capture, sizeof(capture));
    for (size_t line_idx = 0; line_idx < 10; ++line_idx) {
        char payload[8] = {'l', 'i', 'n', 'e', '?', '\0', '\0', '\0'};
        payload[4] = static_cast<char>('0' + line_idx);
        (void)console::urc::emit(console::urc::Tag::Test, payload);
    }
    bool flushed = service::shell_flush_urcs_for_test();
    service::Terminal::capture_end();
    bool bound_ok = flushed;
    size_t prev_pos = 0;
    for (size_t want_idx = 0; want_idx < 8 && bound_ok; ++want_idx) {
        char want[7] = {'l', 'i', 'n', 'e', '?', '\0', '\0'};
        want[4] = static_cast<char>('0' + want_idx);
        size_t found_at = substr_index(capture, want);
        bound_ok = found_at != kSubstrNotFound && found_at >= prev_pos;
        prev_pos = found_at;
    }
    bound_ok = bound_ok && !serial_contains(capture, "line8");
    bound_ok = bound_ok && !serial_contains(capture, "line9");
    bool still_pending = console::urc::pending();
    char capture_second[512] = {};
    service::Terminal::capture_begin(capture_second, sizeof(capture_second));
    bool flushed_rest = service::shell_flush_urcs_for_test();
    service::Terminal::capture_end();
    bool rest_ok = flushed_rest && !console::urc::pending() &&
                   serial_contains(capture_second, "line8") &&
                   serial_contains(capture_second, "line9");
    console::urc::reset();
    for (size_t emit_idx = 0; emit_idx < console::urc::kQueueDepth + 1;
         ++emit_idx) {
        char payload[8] = {'o', 'v', 'f', '?', '\0', '\0', '\0', '\0'};
        payload[3] = static_cast<char>('a' + emit_idx);
        (void)console::urc::emit(console::urc::Tag::Elf, payload);
    }
    char capture_over[1024] = {};
    service::Terminal::capture_begin(capture_over, sizeof(capture_over));
    bool flushed_over = service::shell_flush_urcs_for_test();
    service::Terminal::capture_end();
    size_t notice_at = substr_index(capture_over, "[URC] 1 dropped");
    size_t first_line_at = substr_index(capture_over, "ovfa");
    bool notice_ok = flushed_over && notice_at != kSubstrNotFound &&
                     first_line_at != kSubstrNotFound &&
                     notice_at < first_line_at;
    console::urc::reset();
    JARVIS_ASSERT(bound_ok);
    JARVIS_ASSERT(still_pending);
    JARVIS_ASSERT(rest_ok);
    JARVIS_ASSERT(notice_ok);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: empty-queue drain prints nothing and asks for no redraw —
// pins the prompt-loop fix (commit 2d2695233): the prompt sitting
// mid-line is normal state, not async output (#407-2).
// Input: urc reset; capture; flush; capture end.
// Expect: flush false, capture empty.
// Depends: shell_flush_urcs_for_test (issue #407)
JARVIS_TEST(urc_empty_flush_prints_nothing, "PRE: none | POST: none") {
    console::urc::reset();
    char capture[256] = {};
    service::Terminal::capture_begin(capture, sizeof(capture));
    bool flushed = service::shell_flush_urcs_for_test();
    service::Terminal::capture_end();
    console::urc::reset();
    JARVIS_ASSERT(!flushed);
    JARVIS_ASSERT(capture[0] == '\0');
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: prompt redraw after a drain reprints the typed buffer
// exactly once below the async text (#407-2).
// Input: urc reset; emit Kill line; capture; flush; redraw "abc".
// Expect: URC text above the final prompt; tail holds "$ " before
//         exactly one "abc".
// Depends: shell_flush/redraw_for_test seams (issue #407)
JARVIS_TEST(urc_redraw_keeps_typed_buffer, "PRE: none | POST: none") {
    console::urc::reset();
    (void)console::urc::emit(console::urc::Tag::Kill, "reaped");
    char capture[1024] = {};
    service::Terminal::capture_begin(capture, sizeof(capture));
    bool flushed = service::shell_flush_urcs_for_test();
    service::shell_redraw_for_test("abc", 3, 0);
    service::Terminal::capture_end();
    size_t scan = 0;
    size_t last_nl = kSubstrNotFound;
    while (capture[scan] != '\0') {
        if (capture[scan] == '\n')
            last_nl = scan;
        ++scan;
    }
    bool has_block = last_nl != kSubstrNotFound;
    bool urc_above =
        has_block && substr_index(capture, "reaped") < last_nl;
    const char *tail = has_block ? capture + last_nl + 1 : capture;
    size_t prompt_at = substr_index(tail, "$ ");
    size_t buffer_at = substr_index(tail, "abc");
    bool order_ok = prompt_at != kSubstrNotFound &&
                    buffer_at != kSubstrNotFound && prompt_at < buffer_at;
    bool buffer_once = count_occurrences(tail, "abc") == 1;
    console::urc::reset();
    JARVIS_ASSERT(flushed);
    JARVIS_ASSERT(has_block);
    JARVIS_ASSERT(urc_above);
    JARVIS_ASSERT(order_ok);
    JARVIS_ASSERT(buffer_once);
    JARVIS_TEST_PASS();
}

void register_shell_interaction_tests() {
    Logger::info("Registering shell interaction tests");
    JARVIS_REGISTER_RELEASE_TEST(shell_loopback_manual_string);
    JARVIS_REGISTER_RELEASE_TEST(shell_terminal_write);
    JARVIS_REGISTER_RELEASE_TEST(shell_simple_write);
    JARVIS_REGISTER_RELEASE_TEST(shell_write_with_newline);
    JARVIS_REGISTER_RELEASE_TEST(shell_echo_and_write);
    JARVIS_REGISTER_RELEASE_TEST(shell_heap_delete);
    JARVIS_REGISTER_RELEASE_TEST(shell_execute_serial);
    JARVIS_REGISTER_RELEASE_TEST(shell_help_command);
    JARVIS_REGISTER_RELEASE_TEST(shell_echo_command);
    JARVIS_REGISTER_RELEASE_TEST(shell_uptime_command);
    JARVIS_REGISTER_RELEASE_TEST(shell_pwd_command);
    JARVIS_REGISTER_RELEASE_TEST(shell_help_shows_which);
    JARVIS_REGISTER_RELEASE_TEST(shell_which_command);
    JARVIS_REGISTER_RELEASE_TEST(shell_which_notfound);
    JARVIS_REGISTER_RELEASE_TEST(shell_env_command);
    JARVIS_REGISTER_RELEASE_TEST(shell_export_command);
    JARVIS_REGISTER_RELEASE_TEST(shell_sleep_zero);
    JARVIS_REGISTER_RELEASE_TEST(shell_clear_command);
    JARVIS_REGISTER_RELEASE_TEST(shell_tasks_memory_columns);
    JARVIS_REGISTER_TEST(shell_key_delivery_via_seam);
    JARVIS_REGISTER_TEST(shell_peek_slice_seam); // issue #407
    JARVIS_REGISTER_TEST(shell_slice_normalized_newline); // issue #407
    JARVIS_REGISTER_TEST(urc_emit_pop_fifo); // issue #407 item 2
    JARVIS_REGISTER_TEST(urc_payload_truncation_counted); // issue #407 item 2
    JARVIS_REGISTER_TEST(urc_overflow_drop_newest_keeps_order); // issue #407 item 2
    JARVIS_REGISTER_TEST(urc_seal_rejects_new_prefix); // issue #407 item 2
    JARVIS_REGISTER_TEST(urc_flush_frames_and_bounds); // issue #407 item 2
    JARVIS_REGISTER_TEST(urc_empty_flush_prints_nothing); // issue #407 item 2
    JARVIS_REGISTER_TEST(urc_redraw_keeps_typed_buffer); // issue #407 item 2
}
#elif defined(CONFIG_ARCH_AARCH64) || defined(CONFIG_ARCH_RISCV64)
#include <test.hpp>
#include <logger.hpp>
#include <services/shell.hpp>
#include <services/terminal/terminal.hpp>
#include <kernel/arch/keyboard.hpp>
#include <kernel/arch/serial.hpp>
#include <kernel/input/serial_input.hpp>
#include <kernel/console/urc.hpp>
#include <kernel/arch/io.hpp>
#include <kernel/arch/timer.hpp>
#include <string.hpp>

using namespace kernel;
using arch::Keyboard;
using arch::Serial;

static bool serial_contains(const char *buf, const char *needle) {
    for (size_t i = 0; buf[i] != '\0'; ++i) {
        size_t j;
        for (j = 0; needle[j] != '\0'; ++j) {
            if (buf[i + j] != needle[j])
                break;
        }
        if (needle[j] == '\0')
            return true;
    }
    return false;
}

static constexpr size_t kSubstrNotFound = static_cast<size_t>(-1);

// Index of the first occurrence of needle in buf, or kSubstrNotFound.
static size_t substr_index(const char *buf, const char *needle) {
    for (size_t start = 0; buf[start] != '\0'; ++start) {
        size_t offset = 0;
        for (; needle[offset] != '\0'; ++offset) {
            if (buf[start + offset] != needle[offset])
                break;
        }
        if (needle[offset] == '\0')
            return start;
    }
    return kSubstrNotFound;
}

// Non-overlapping occurrence count of needle in buf.
static size_t count_occurrences(const char *buf, const char *needle) {
    size_t found = 0;
    size_t needle_len = 0;
    while (needle[needle_len] != '\0')
        ++needle_len;
    if (needle_len == 0)
        return 0;
    for (size_t pos = 0; buf[pos] != '\0';) {
        size_t offset = 0;
        while (offset < needle_len && buf[pos + offset] == needle[offset])
            ++offset;
        if (offset == needle_len) {
            ++found;
            pos += needle_len;
        } else {
            ++pos;
        }
    }
    return found;
}

// Runmode: kernel
// Testidea: Terminal::write output lands in the capture buffer (issue
// #355 H3: off-x86 port of the COM1-loopback write test onto
// Terminal::capture_begin/end — no UART loopback on these arches).
// Input: capture_begin; write("HelloWorld"); capture_end.
// Expect: capture contains "HelloWorld".
// Depends: service::Terminal
JARVIS_TEST(shell_loopback_manual_string, "PRE: vfsd, iocd | POST: none") {
    char cap[64];
    service::Terminal::capture_begin(cap, sizeof(cap));
    service::Terminal::write("HelloWorld");
    service::Terminal::capture_end();
    JARVIS_ASSERT(serial_contains(cap, "HelloWorld"));
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Capture port of shell_terminal_write (H3): Terminal output
// is observable in-memory without UART loopback.
// Input: capture_begin; write("TESTMARKER"); capture_end.
// Expect: capture contains "TESTMARKER".
// Depends: service::Terminal
JARVIS_TEST(shell_terminal_write, "PRE: vfsd, iocd | POST: none") {
    char cap[256];
    service::Terminal::capture_begin(cap, sizeof(cap));
    service::Terminal::write("TESTMARKER");
    service::Terminal::capture_end();
    JARVIS_ASSERT(serial_contains(cap, "TESTMARKER"));
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Capture port of shell_simple_write (H3).
// Input: capture_begin; write("SIMPLE_TEST"); capture_end.
// Expect: capture contains "SIMPLE_TEST".
// Depends: service::Terminal
JARVIS_TEST(shell_simple_write, "PRE: vfsd, iocd | POST: none") {
    char cap[64];
    service::Terminal::capture_begin(cap, sizeof(cap));
    service::Terminal::write("SIMPLE_TEST");
    service::Terminal::capture_end();
    JARVIS_ASSERT(serial_contains(cap, "SIMPLE_TEST"));
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Capture port of shell_write_with_newline (H3): multi-line
// writes land in order.
// Input: capture_begin; write("LINE1\nLINE2\n"); capture_end.
// Expect: capture contains LINE1 and LINE2.
// Depends: service::Terminal
JARVIS_TEST(shell_write_with_newline, "PRE: vfsd, iocd | POST: none") {
    char cap[512];
    service::Terminal::capture_begin(cap, sizeof(cap));
    service::Terminal::write("LINE1\nLINE2\n");
    service::Terminal::capture_end();
    JARVIS_ASSERT(serial_contains(cap, "LINE1"));
    JARVIS_ASSERT(serial_contains(cap, "LINE2"));
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Capture port of shell_echo_and_write (H3).
// Input: capture_begin; write ALPHA + BETA; capture_end.
// Expect: capture contains both.
// Depends: service::Terminal
JARVIS_TEST(shell_echo_and_write, "PRE: vfsd, iocd | POST: none") {
    char cap[512];
    service::Terminal::capture_begin(cap, sizeof(cap));
    service::Terminal::write("ALPHA");
    service::Terminal::write("BETA");
    service::Terminal::capture_end();
    JARVIS_ASSERT(serial_contains(cap, "ALPHA"));
    JARVIS_ASSERT(serial_contains(cap, "BETA"));
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Capture port of shell_heap_delete (H3): heap churn between
// writes loses nothing.
// Input: capture_begin; write BEFORE; new/delete; write END;
//        capture_end.
// Expect: capture contains BEFORE and END.
// Depends: service::Terminal
JARVIS_TEST(shell_heap_delete, "PRE: vfsd, iocd | POST: none") {
    char cap[512];
    service::Terminal::capture_begin(cap, sizeof(cap));
    service::Terminal::write("BEFORE");
    char *tmp = new char[8];
    delete[] tmp;
    service::Terminal::write("END");
    service::Terminal::capture_end();
    JARVIS_ASSERT(serial_contains(cap, "BEFORE"));
    JARVIS_ASSERT(serial_contains(cap, "END"));
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Capture port of shell_execute_serial (H3): an unknown command
// reports through the shared path while markers bracket it.
// Input: capture_begin; write BEFORE; execute("garbing"); write AFTER;
//        capture_end.
// Expect: capture contains BEFORE, AFTER and "Unknown".
// Depends: service::Shell, service::Terminal
JARVIS_TEST(shell_execute_serial, "PRE: vfsd, iocd | POST: none") {
    char cap[1024];
    service::Terminal::capture_begin(cap, sizeof(cap));
    service::Terminal::write("BEFORE");
    service::Shell::execute("garbing");
    service::Terminal::write("AFTER");
    service::Terminal::capture_end();
    JARVIS_ASSERT(serial_contains(cap, "BEFORE"));
    JARVIS_ASSERT(serial_contains(cap, "AFTER"));
    JARVIS_ASSERT(serial_contains(cap, "Unknown"));
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Capture port of shell_help_command (H3): the full help text
// is capturable in-memory (no 16-byte FIFO window — the whole header is
// present, so the full "Available commands" needle applies).
// Input: capture_begin; write BEFORE; execute("help"); write AFTER;
//        capture_end.
// Expect: capture contains BEFORE, AFTER and "Available commands".
// Depends: service::Shell, service::Terminal
JARVIS_TEST(shell_help_command, "PRE: vfsd, iocd | POST: none") {
    char cap[4096];
    service::Terminal::capture_begin(cap, sizeof(cap));
    service::Terminal::write("BEFORE");
    service::Shell::execute("help");
    service::Terminal::write("AFTER");
    service::Terminal::capture_end();
    JARVIS_ASSERT(serial_contains(cap, "BEFORE"));
    JARVIS_ASSERT(serial_contains(cap, "AFTER"));
    JARVIS_ASSERT(serial_contains(cap, "Available commands"));
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Capture port of shell_echo_command (H3).
// Input: capture_begin; write BEFORE; execute("echo Hello World");
//        write AFTER; capture_end.
// Expect: capture contains BEFORE, AFTER and "Hello".
// Depends: service::Shell, service::Terminal
JARVIS_TEST(shell_echo_command, "PRE: vfsd, iocd | POST: none") {
    char cap[1024];
    service::Terminal::capture_begin(cap, sizeof(cap));
    service::Terminal::write("BEFORE");
    service::Shell::execute("echo Hello World");
    service::Terminal::write("AFTER");
    service::Terminal::capture_end();
    JARVIS_ASSERT(serial_contains(cap, "BEFORE"));
    JARVIS_ASSERT(serial_contains(cap, "AFTER"));
    JARVIS_ASSERT(serial_contains(cap, "Hello"));
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Capture port of shell_uptime_command (H3).
// Input: capture_begin; write BEFORE; execute("uptime"); write AFTER;
//        capture_end.
// Expect: capture contains BEFORE, AFTER and "Uptime".
// Depends: service::Shell, service::Terminal
JARVIS_TEST(shell_uptime_command, "PRE: vfsd, iocd | POST: none") {
    char cap[1024];
    service::Terminal::capture_begin(cap, sizeof(cap));
    service::Terminal::write("BEFORE");
    service::Shell::execute("uptime");
    service::Terminal::write("AFTER");
    service::Terminal::capture_end();
    JARVIS_ASSERT(serial_contains(cap, "BEFORE"));
    JARVIS_ASSERT(serial_contains(cap, "AFTER"));
    JARVIS_ASSERT(serial_contains(cap, "Uptime"));
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Capture port of shell_pwd_command (H3).
// Input: capture_begin; write BEFORE; execute("pwd"); write AFTER;
//        capture_end.
// Expect: capture contains BEFORE, AFTER and "/".
// Depends: service::Shell, service::Terminal
JARVIS_TEST(shell_pwd_command, "PRE: vfsd, iocd | POST: none") {
    char cap[1024];
    service::Terminal::capture_begin(cap, sizeof(cap));
    service::Terminal::write("BEFORE");
    service::Shell::execute("pwd");
    service::Terminal::write("AFTER");
    service::Terminal::capture_end();
    JARVIS_ASSERT(serial_contains(cap, "BEFORE"));
    JARVIS_ASSERT(serial_contains(cap, "AFTER"));
    JARVIS_ASSERT(serial_contains(cap, "/"));
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Capture port of shell_help_shows_which (H3): dispatch of
// "locate locate" proves registration; the entry format carries " (".
// Input: capture_begin; write BEFORE; execute("locate locate");
//        write AFTER; capture_end.
// Expect: capture contains BEFORE, AFTER, " (" and "locate".
// Depends: service::Shell, service::Terminal
JARVIS_TEST(shell_help_shows_which, "PRE: vfsd, iocd | POST: none") {
    char cap[1024];
    service::Terminal::capture_begin(cap, sizeof(cap));
    service::Terminal::write("BEFORE");
    service::Shell::execute("locate locate");
    service::Terminal::write("AFTER");
    service::Terminal::capture_end();
    JARVIS_ASSERT(serial_contains(cap, "BEFORE"));
    JARVIS_ASSERT(serial_contains(cap, "AFTER"));
    JARVIS_ASSERT(serial_contains(cap, " ("));
    JARVIS_ASSERT(serial_contains(cap, "locate"));
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Capture port of shell_which_command (H3).
// Input: capture_begin; write BEFORE; execute("locate help");
//        write AFTER; capture_end.
// Expect: capture contains BEFORE, AFTER, " (" and "help".
// Depends: service::Shell, service::Terminal
JARVIS_TEST(shell_which_command, "PRE: vfsd, iocd | POST: none") {
    char cap[1024];
    service::Terminal::capture_begin(cap, sizeof(cap));
    service::Terminal::write("BEFORE");
    service::Shell::execute("locate help");
    service::Terminal::write("AFTER");
    service::Terminal::capture_end();
    JARVIS_ASSERT(serial_contains(cap, "BEFORE"));
    JARVIS_ASSERT(serial_contains(cap, "AFTER"));
    JARVIS_ASSERT(serial_contains(cap, " ("));
    JARVIS_ASSERT(serial_contains(cap, "help"));
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Capture port of shell_which_notfound (H3).
// Input: capture_begin; write BEFORE; execute("locate xyzzy");
//        write AFTER; capture_end.
// Expect: capture contains BEFORE, AFTER, ": not" and "xyzzy".
// Depends: service::Shell, service::Terminal
JARVIS_TEST(shell_which_notfound, "PRE: vfsd, iocd | POST: none") {
    char cap[1024];
    service::Terminal::capture_begin(cap, sizeof(cap));
    service::Terminal::write("BEFORE");
    service::Shell::execute("locate xyzzy");
    service::Terminal::write("AFTER");
    service::Terminal::capture_end();
    JARVIS_ASSERT(serial_contains(cap, "BEFORE"));
    JARVIS_ASSERT(serial_contains(cap, "AFTER"));
    JARVIS_ASSERT(serial_contains(cap, ": not"));
    JARVIS_ASSERT(serial_contains(cap, "xyzzy"));
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Capture port of shell_env_command (H3).
// Input: capture_begin; write BEFORE; execute("env"); write AFTER;
//        capture_end.
// Expect: capture contains BEFORE and AFTER (env runs clean).
// Depends: service::Shell, service::Terminal
JARVIS_TEST(shell_env_command, "PRE: vfsd, iocd | POST: none") {
    char cap[1024];
    service::Terminal::capture_begin(cap, sizeof(cap));
    service::Terminal::write("BEFORE");
    service::Shell::execute("env");
    service::Terminal::write("AFTER");
    service::Terminal::capture_end();
    JARVIS_ASSERT(serial_contains(cap, "BEFORE"));
    JARVIS_ASSERT(serial_contains(cap, "AFTER"));
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Capture port of shell_export_command (H3).
// Input: capture_begin; write BEFORE; execute("export FOO=bar");
//        write AFTER; capture_end.
// Expect: capture contains BEFORE and AFTER (export runs clean).
// Depends: service::Shell, service::Terminal
JARVIS_TEST(shell_export_command, "PRE: vfsd, iocd | POST: none") {
    char cap[1024];
    service::Terminal::capture_begin(cap, sizeof(cap));
    service::Terminal::write("BEFORE");
    service::Shell::execute("export FOO=bar");
    service::Terminal::write("AFTER");
    service::Terminal::capture_end();
    JARVIS_ASSERT(serial_contains(cap, "BEFORE"));
    JARVIS_ASSERT(serial_contains(cap, "AFTER"));
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Capture port of shell_sleep_zero (H3).
// Input: capture_begin; write BEFORE; execute("sleep 0"); write AFTER;
//        capture_end.
// Expect: capture contains BEFORE and AFTER (returns immediately).
// Depends: service::Shell, service::Terminal
JARVIS_TEST(shell_sleep_zero, "PRE: vfsd, iocd | POST: none") {
    char cap[1024];
    service::Terminal::capture_begin(cap, sizeof(cap));
    service::Terminal::write("BEFORE");
    service::Shell::execute("sleep 0");
    service::Terminal::write("AFTER");
    service::Terminal::capture_end();
    JARVIS_ASSERT(serial_contains(cap, "BEFORE"));
    JARVIS_ASSERT(serial_contains(cap, "AFTER"));
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Verifies the "clear" built-in command executes without crash
// (arch-neutral — no I/O asserted).
// Input: Shell::execute("clear")
// Expect: No crash, terminal cleared
// Depends: service::Shell, service::Terminal
JARVIS_TEST(shell_clear_command, "PRE: vfsd, iocd | POST: none") {
    service::Shell::execute("clear");
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: The `tasks` command reports per-task memory info: stack
// high-water/low-water, stack size, and text/data/bss segments
// (arch-neutral — capture-based).
// Input: Shell::execute("tasks") with Terminal capture enabled
// Expect: Captured output contains the new memory columns
// Depends: service::Shell, service::Terminal, kernel::TaskControlBlock
JARVIS_TEST(shell_tasks_memory_columns, "PRE: vfsd, iocd | POST: none") {
    // Capture must hold MAX_TASKS rows (~108 B each) + headers.
    char cap[CONFIG_MAX_TASKS * 128u + 512u];
    service::Terminal::capture_begin(cap, sizeof(cap));
    service::Shell::execute("tasks");
    service::Terminal::capture_end();

    // New memory columns present in the header.
    JARVIS_ASSERT(serial_contains(cap, "/STACK"));
    JARVIS_ASSERT(serial_contains(cap, "/TEXT"));
    JARVIS_ASSERT(serial_contains(cap, "/DATA"));
    JARVIS_ASSERT(serial_contains(cap, "/BSS"));
    JARVIS_ASSERT(serial_contains(cap, "MEM_PG"));
    JARVIS_ASSERT(serial_contains(cap, "CPU_TIME"));
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Decoder seam delivery (issue #355 H3): bytes injected via
// Keyboard::inject_serial travel the production shell_try_get_key path
// (pump → decoder → raw-UART fallback → iocd) exactly once. The "\r"
// normalizes to "\n" in the decoder, and the UART stays idle throughout
// (proving the bytes arrived via inject, not the wire).
// Input: drain UART RX (bounded); inject "echo hi\r" (8 bytes); collect
//        via shell_try_get_key (50-tick bound); two extra polls + a raw
//        UART poll.
// Expect: exact sequence e,c,h,o,' ',h,i,'\n'; both extra polls false;
//         raw UART poll false.
// Depends: shell_try_get_key seam, arch::Keyboard, arch::Serial
JARVIS_TEST(shell_key_via_decoder_seam, "PRE: vfsd, iocd | POST: none") {
    for (int i = 0; i < 64; ++i) {
        char junk = 0;
        if (!Serial::poll_getchar(junk))
            break;
    }
    Keyboard::init();
    const char keys[8] = {'e', 'c', 'h', 'o', ' ', 'h', 'i', '\r'};
    size_t accepted = Keyboard::inject_serial(keys, sizeof(keys));
    JARVIS_ASSERT_EQ(static_cast<uint64_t>(8),
                     static_cast<uint64_t>(accepted));
    char got[8] = {};
    size_t n = 0;
    const uint64_t deadline = arch::Timer::ticks() + 50;
    while (n < 8 && arch::Timer::ticks() < deadline) {
        char k = 0;
        if (service::shell_try_get_key(k))
            got[n++] = k;
        else
            arch::pause();
    }
    char extra1 = 0;
    char extra2 = 0;
    bool empty1 = !service::shell_try_get_key(extra1);
    bool empty2 = !service::shell_try_get_key(extra2);
    char uart = 0;
    bool uart_idle = !Serial::poll_getchar(uart);
    JARVIS_ASSERT_EQ(static_cast<uint64_t>(8),
                     static_cast<uint64_t>(n));
    JARVIS_ASSERT(memcmp(got, "echo hi\n", 8) == 0);
    JARVIS_ASSERT(empty1);
    JARVIS_ASSERT(empty2);
    JARVIS_ASSERT(uart_idle);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: shell_peek_input/shell_consume_input drive the exact
// readline fast-path seam (pump + peek + consume): injected bytes
// surface as one contiguous run and consume drops exactly that run.
// Input: drain UART; ring reset; inject "abc"; shell_peek_input;
//        shell_consume_input(3); shell_peek_input.
// Expect: peek true, len 3, bytes "abc"; consume returns 3; second
//         peek false.
// Depends: service shell seam + input::serial ring (issue #407)
JARVIS_TEST(shell_peek_slice_seam, "PRE: vfsd, iocd | POST: none") {
    for (int i = 0; i < 64; ++i) {
        char junk = 0;
        if (!Serial::poll_getchar(junk))
            break;
    }
    kernel::input::serial::reset();
    size_t accepted = kernel::input::serial::inject("abc", 3);
    const char *data = nullptr;
    size_t len = 0;
    bool exposed = service::shell_peek_input(data, len);
    bool bytes_match = exposed && len == 3 && memcmp(data, "abc", 3) == 0;
    size_t consumed = service::shell_consume_input(3);
    const char *tail = nullptr;
    size_t tail_len = 0;
    bool empty = !service::shell_peek_input(tail, tail_len);
    kernel::input::serial::reset();
    JARVIS_ASSERT_EQ(static_cast<uint64_t>(3),
                      static_cast<uint64_t>(accepted));
    JARVIS_ASSERT(bytes_match);
    JARVIS_ASSERT_EQ(static_cast<uint64_t>(3),
                      static_cast<uint64_t>(consumed));
    JARVIS_ASSERT(empty);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: CR normalizes to newline through the shell seam
// (anti-#245): readline's scan stops at '\n'; a raw '\r' reaching the
// echo path would trip the async-output guard (#248). The seam must
// expose the decoded newline, never the raw carriage return.
// Input: drain UART; ring reset; inject "a\r"; shell_peek_input;
//        shell_consume_input(2); shell_peek_input.
// Expect: peek true, len 2, bytes {'a','\n'}; consume returns 2;
//         second peek false.
// Depends: service shell seam + push_decoded (issue #407)
JARVIS_TEST(shell_slice_normalized_newline, "PRE: vfsd, iocd | POST: none") {
    for (int i = 0; i < 64; ++i) {
        char junk = 0;
        if (!Serial::poll_getchar(junk))
            break;
    }
    kernel::input::serial::reset();
    size_t accepted = kernel::input::serial::inject("a\r", 2);
    const char *data = nullptr;
    size_t len = 0;
    bool exposed = service::shell_peek_input(data, len);
    bool bytes_match = exposed && len == 2 && data[0] == 'a' &&
                       data[1] == '\n';
    size_t consumed = service::shell_consume_input(2);
    const char *tail = nullptr;
    size_t tail_len = 0;
    bool empty = !service::shell_peek_input(tail, tail_len);
    kernel::input::serial::reset();
    JARVIS_ASSERT_EQ(static_cast<uint64_t>(2),
                      static_cast<uint64_t>(accepted));
    JARVIS_ASSERT(bytes_match);
    JARVIS_ASSERT_EQ(static_cast<uint64_t>(2),
                      static_cast<uint64_t>(consumed));
    JARVIS_ASSERT(empty);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: URC queue preserves FIFO order with per-tag prefixes (#407-2).
// Input: urc reset; emit Elf/Kill/Test; pop x4.
// Expect: "[ELF] a", "[KILL] b", "[TST] c" in order; 4th pop false;
//         all counters zero, queue not pending.
// Depends: console::urc emit/pop (issue #407)
JARVIS_TEST(urc_emit_pop_fifo, "PRE: none | POST: none") {
    console::urc::reset();
    bool queued_elf = console::urc::emit(console::urc::Tag::Elf, "a");
    bool queued_kill = console::urc::emit(console::urc::Tag::Kill, "b");
    bool queued_test = console::urc::emit(console::urc::Tag::Test, "c");
    bool order_ok = queued_elf && queued_kill && queued_test;
    char line_buf[console::urc::kLineMax] = {};
    order_ok = order_ok && console::urc::pop(line_buf, sizeof(line_buf));
    order_ok = order_ok && memcmp(line_buf, "[ELF] a", 8) == 0;
    order_ok = order_ok && console::urc::pop(line_buf, sizeof(line_buf));
    order_ok = order_ok && memcmp(line_buf, "[KILL] b", 9) == 0;
    order_ok = order_ok && console::urc::pop(line_buf, sizeof(line_buf));
    order_ok = order_ok && memcmp(line_buf, "[TST] c", 8) == 0;
    bool drained = !console::urc::pop(line_buf, sizeof(line_buf));
    bool counters_clean = console::urc::dropped_count() == 0 &&
                          console::urc::truncated_count() == 0 &&
                          console::urc::rejected_count() == 0;
    bool queue_idle = !console::urc::pending();
    console::urc::reset();
    JARVIS_ASSERT(order_ok);
    JARVIS_ASSERT(drained);
    JARVIS_ASSERT(counters_clean);
    JARVIS_ASSERT(queue_idle);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: oversize payloads truncate with "..." and count once (#407-2).
// Input: urc reset; emit one 199-char payload; pop.
// Expect: popped len kLineMax-1, starts "[ELF] ", ends "...",
//         truncated_count 1.
// Depends: console::urc emit/pop (issue #407)
JARVIS_TEST(urc_payload_truncation_counted, "PRE: none | POST: none") {
    console::urc::reset();
    char long_payload[200] = {};
    for (size_t fill = 0; fill < sizeof(long_payload) - 1; ++fill)
        long_payload[fill] = 'x';
    bool queued = console::urc::emit(console::urc::Tag::Elf, long_payload);
    char line_buf[console::urc::kLineMax] = {};
    bool popped = console::urc::pop(line_buf, sizeof(line_buf));
    size_t line_len = 0;
    while (line_buf[line_len] != '\0' && line_len < sizeof(line_buf))
        ++line_len;
    bool head_ok = memcmp(line_buf, "[ELF] ", 6) == 0;
    bool tail_ok = line_len == console::urc::kLineMax - 1 &&
                   line_buf[line_len - 1] == '.' &&
                   line_buf[line_len - 2] == '.' &&
                   line_buf[line_len - 3] == '.';
    bool counted_once = console::urc::truncated_count() == 1;
    console::urc::reset();
    JARVIS_ASSERT(queued);
    JARVIS_ASSERT(popped);
    JARVIS_ASSERT(head_ok);
    JARVIS_ASSERT(tail_ok);
    JARVIS_ASSERT(counted_once);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: full queue drops newest, keeps order, counts drops (#407-2).
// Input: urc reset; emit kQueueDepth+3 single-char lines; drain.
// Expect: dropped_count 3; first 16 pop in emission order;
//         17th pop false.
// Depends: console::urc emit/pop (issue #407)
JARVIS_TEST(urc_overflow_drop_newest_keeps_order, "PRE: none | POST: none") {
    console::urc::reset();
    for (size_t emit_idx = 0;
         emit_idx < console::urc::kQueueDepth + 3; ++emit_idx) {
        char payload[2] = {static_cast<char>('A' + emit_idx), '\0'};
        (void)console::urc::emit(console::urc::Tag::Test, payload);
    }
    bool drops_counted = console::urc::dropped_count() == 3;
    bool order_kept = true;
    char line_buf[console::urc::kLineMax] = {};
    for (size_t pop_idx = 0; pop_idx < console::urc::kQueueDepth; ++pop_idx) {
        char want[8] = {'[', 'T', 'S', 'T', ']', ' ', '?', '\0'};
        want[6] = static_cast<char>('A' + pop_idx);
        if (!console::urc::pop(line_buf, sizeof(line_buf)) ||
            memcmp(line_buf, want, sizeof(want)) != 0)
            order_kept = false;
    }
    bool drained = !console::urc::pop(line_buf, sizeof(line_buf));
    console::urc::reset();
    JARVIS_ASSERT(drops_counted);
    JARVIS_ASSERT(order_kept);
    JARVIS_ASSERT(drained);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: seal() freezes the prefix table; known tags still queue,
// unknown ids and late registration are counted rejects (#407-2).
// Input: urc reset; seal; register "NEW"; emit Elf; emit id 0xFE.
// Expect: register returns kNoPrefix; Elf queues and pops;
//         rejected_count 2 (late register + unknown id).
// Depends: console::urc seal/register/emit (issue #407)
JARVIS_TEST(urc_seal_rejects_new_prefix, "PRE: none | POST: none") {
    console::urc::reset();
    console::urc::seal();
    bool was_sealed = console::urc::sealed();
    uint8_t late_id = console::urc::register_prefix("NEW");
    bool late_rejected = late_id == console::urc::kNoPrefix;
    bool known_queues = console::urc::emit(console::urc::Tag::Elf, "ok");
    bool unknown_rejected =
        !console::urc::emit(static_cast<console::urc::Tag>(0xFE), "bad");
    bool rejects_counted = console::urc::rejected_count() == 2;
    char line_buf[console::urc::kLineMax] = {};
    bool known_pops = console::urc::pop(line_buf, sizeof(line_buf)) &&
                      memcmp(line_buf, "[ELF] ok", 9) == 0;
    bool drained = !console::urc::pop(line_buf, sizeof(line_buf));
    console::urc::reset();
    bool seal_cleared = !console::urc::sealed();
    JARVIS_ASSERT(was_sealed);
    JARVIS_ASSERT(late_rejected);
    JARVIS_ASSERT(known_queues);
    JARVIS_ASSERT(unknown_rejected);
    JARVIS_ASSERT(rejects_counted);
    JARVIS_ASSERT(known_pops);
    JARVIS_ASSERT(drained);
    JARVIS_ASSERT(seal_cleared);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: the production drain prints a drop notice plus at most
// kDrainMaxPerPass lines per pass, oldest first (#407-2).
// Input: urc reset; capture; emit 10 lines; flush; flush; capture end.
// Expect: first flush true, contains line0..line7 in order, no
//         line8/line9; pending true; second flush drains 2, pending
//         false. Overflow leg: 17 emits, flush shows "[URC] 1 dropped"
//         before the first payload line.
// Depends: shell_flush_urcs_for_test + console::urc (issue #407)
JARVIS_TEST(urc_flush_frames_and_bounds, "PRE: none | POST: none") {
    console::urc::reset();
    char capture[1024] = {};
    service::Terminal::capture_begin(capture, sizeof(capture));
    for (size_t line_idx = 0; line_idx < 10; ++line_idx) {
        char payload[8] = {'l', 'i', 'n', 'e', '?', '\0', '\0', '\0'};
        payload[4] = static_cast<char>('0' + line_idx);
        (void)console::urc::emit(console::urc::Tag::Test, payload);
    }
    bool flushed = service::shell_flush_urcs_for_test();
    service::Terminal::capture_end();
    bool bound_ok = flushed;
    size_t prev_pos = 0;
    for (size_t want_idx = 0; want_idx < 8 && bound_ok; ++want_idx) {
        char want[7] = {'l', 'i', 'n', 'e', '?', '\0', '\0'};
        want[4] = static_cast<char>('0' + want_idx);
        size_t found_at = substr_index(capture, want);
        bound_ok = found_at != kSubstrNotFound && found_at >= prev_pos;
        prev_pos = found_at;
    }
    bound_ok = bound_ok && !serial_contains(capture, "line8");
    bound_ok = bound_ok && !serial_contains(capture, "line9");
    bool still_pending = console::urc::pending();
    char capture_second[512] = {};
    service::Terminal::capture_begin(capture_second, sizeof(capture_second));
    bool flushed_rest = service::shell_flush_urcs_for_test();
    service::Terminal::capture_end();
    bool rest_ok = flushed_rest && !console::urc::pending() &&
                   serial_contains(capture_second, "line8") &&
                   serial_contains(capture_second, "line9");
    console::urc::reset();
    for (size_t emit_idx = 0; emit_idx < console::urc::kQueueDepth + 1;
         ++emit_idx) {
        char payload[8] = {'o', 'v', 'f', '?', '\0', '\0', '\0', '\0'};
        payload[3] = static_cast<char>('a' + emit_idx);
        (void)console::urc::emit(console::urc::Tag::Elf, payload);
    }
    char capture_over[1024] = {};
    service::Terminal::capture_begin(capture_over, sizeof(capture_over));
    bool flushed_over = service::shell_flush_urcs_for_test();
    service::Terminal::capture_end();
    size_t notice_at = substr_index(capture_over, "[URC] 1 dropped");
    size_t first_line_at = substr_index(capture_over, "ovfa");
    bool notice_ok = flushed_over && notice_at != kSubstrNotFound &&
                     first_line_at != kSubstrNotFound &&
                     notice_at < first_line_at;
    console::urc::reset();
    JARVIS_ASSERT(bound_ok);
    JARVIS_ASSERT(still_pending);
    JARVIS_ASSERT(rest_ok);
    JARVIS_ASSERT(notice_ok);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: empty-queue drain prints nothing and asks for no redraw —
// pins the prompt-loop fix (commit 2d2695233): the prompt sitting
// mid-line is normal state, not async output (#407-2).
// Input: urc reset; capture; flush; capture end.
// Expect: flush false, capture empty.
// Depends: shell_flush_urcs_for_test (issue #407)
JARVIS_TEST(urc_empty_flush_prints_nothing, "PRE: none | POST: none") {
    console::urc::reset();
    char capture[256] = {};
    service::Terminal::capture_begin(capture, sizeof(capture));
    bool flushed = service::shell_flush_urcs_for_test();
    service::Terminal::capture_end();
    console::urc::reset();
    JARVIS_ASSERT(!flushed);
    JARVIS_ASSERT(capture[0] == '\0');
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: prompt redraw after a drain reprints the typed buffer
// exactly once below the async text (#407-2).
// Input: urc reset; emit Kill line; capture; flush; redraw "abc".
// Expect: URC text above the final prompt; tail holds "$ " before
//         exactly one "abc".
// Depends: shell_flush/redraw_for_test seams (issue #407)
JARVIS_TEST(urc_redraw_keeps_typed_buffer, "PRE: none | POST: none") {
    console::urc::reset();
    (void)console::urc::emit(console::urc::Tag::Kill, "reaped");
    char capture[1024] = {};
    service::Terminal::capture_begin(capture, sizeof(capture));
    bool flushed = service::shell_flush_urcs_for_test();
    service::shell_redraw_for_test("abc", 3, 0);
    service::Terminal::capture_end();
    size_t scan = 0;
    size_t last_nl = kSubstrNotFound;
    while (capture[scan] != '\0') {
        if (capture[scan] == '\n')
            last_nl = scan;
        ++scan;
    }
    bool has_block = last_nl != kSubstrNotFound;
    bool urc_above =
        has_block && substr_index(capture, "reaped") < last_nl;
    const char *tail = has_block ? capture + last_nl + 1 : capture;
    size_t prompt_at = substr_index(tail, "$ ");
    size_t buffer_at = substr_index(tail, "abc");
    bool order_ok = prompt_at != kSubstrNotFound &&
                    buffer_at != kSubstrNotFound && prompt_at < buffer_at;
    bool buffer_once = count_occurrences(tail, "abc") == 1;
    console::urc::reset();
    JARVIS_ASSERT(flushed);
    JARVIS_ASSERT(has_block);
    JARVIS_ASSERT(urc_above);
    JARVIS_ASSERT(order_ok);
    JARVIS_ASSERT(buffer_once);
    JARVIS_TEST_PASS();
}

void register_shell_interaction_tests() {
    Logger::info("Registering shell interaction tests");
    JARVIS_REGISTER_RELEASE_TEST(shell_loopback_manual_string);
    JARVIS_REGISTER_RELEASE_TEST(shell_terminal_write);
    JARVIS_REGISTER_RELEASE_TEST(shell_simple_write);
    JARVIS_REGISTER_RELEASE_TEST(shell_write_with_newline);
    JARVIS_REGISTER_RELEASE_TEST(shell_echo_and_write);
    JARVIS_REGISTER_RELEASE_TEST(shell_heap_delete);
    JARVIS_REGISTER_RELEASE_TEST(shell_execute_serial);
    JARVIS_REGISTER_RELEASE_TEST(shell_help_command);
    JARVIS_REGISTER_RELEASE_TEST(shell_echo_command);
    JARVIS_REGISTER_RELEASE_TEST(shell_uptime_command);
    JARVIS_REGISTER_RELEASE_TEST(shell_pwd_command);
    JARVIS_REGISTER_RELEASE_TEST(shell_help_shows_which);
    JARVIS_REGISTER_RELEASE_TEST(shell_which_command);
    JARVIS_REGISTER_RELEASE_TEST(shell_which_notfound);
    JARVIS_REGISTER_RELEASE_TEST(shell_env_command);
    JARVIS_REGISTER_RELEASE_TEST(shell_export_command);
    JARVIS_REGISTER_RELEASE_TEST(shell_sleep_zero);
    JARVIS_REGISTER_RELEASE_TEST(shell_clear_command);
    JARVIS_REGISTER_RELEASE_TEST(shell_tasks_memory_columns);
    JARVIS_REGISTER_TEST(shell_key_via_decoder_seam);
    JARVIS_REGISTER_TEST(shell_peek_slice_seam); // issue #407
    JARVIS_REGISTER_TEST(shell_slice_normalized_newline); // issue #407
    JARVIS_REGISTER_TEST(urc_emit_pop_fifo); // issue #407 item 2
    JARVIS_REGISTER_TEST(urc_payload_truncation_counted); // issue #407 item 2
    JARVIS_REGISTER_TEST(urc_overflow_drop_newest_keeps_order); // issue #407 item 2
    JARVIS_REGISTER_TEST(urc_seal_rejects_new_prefix); // issue #407 item 2
    JARVIS_REGISTER_TEST(urc_flush_frames_and_bounds); // issue #407 item 2
    JARVIS_REGISTER_TEST(urc_empty_flush_prints_nothing); // issue #407 item 2
    JARVIS_REGISTER_TEST(urc_redraw_keeps_typed_buffer); // issue #407 item 2
}
#endif // CONFIG_ARCH_X86_64
