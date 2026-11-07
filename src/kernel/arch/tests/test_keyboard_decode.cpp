/*
 * NexIOS RTOS — Development Roadmap / Kernel Core
 * Copyright (C) 2026 Arnold Hasshold
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3, as
 * published by the Free Software Foundation.
 */

/// @file test_keyboard_decode.cpp
/// @brief PS/2 keyboard scancode decode tests (milestone v0.4.3 issue #110).
///        Feeds synthetic scancodes through the REAL i8042 controller using
///        the 0xD2 "write keyboard output buffer" command (the byte then
///        appears in the output buffer exactly as if the keyboard had sent
///        it), then runs the genuine Keyboard::handle_irq() decode path.

#if defined(CONFIG_ARCH_X86_64)
#include <test.hpp>
#include <logger.hpp>
#include <kernel/arch/keyboard.hpp>
#include <kernel/arch/io.hpp>
#include <string.hpp>

using namespace kernel;
using arch::inb;
using arch::outb;
using arch::Keyboard;

namespace {

constexpr uint16_t KBD_CMD_PORT = 0x64;
constexpr uint16_t KBD_DATA_PORT = 0x60;
constexpr uint8_t CCMD_WRITE_KBD_OUTBUF = 0xD2;

/// @brief Places @p scancode in the i8042 output buffer (real controller
/// behaviour, 0xD2 + data write) and runs the driver's IRQ handler, which
/// polls the output-buffer-full status bit and decodes the scancode.
void inject_scancode(uint8_t scancode) {
    outb(KBD_CMD_PORT, CCMD_WRITE_KBD_OUTBUF);
    outb(KBD_DATA_PORT, scancode);
    Keyboard::handle_irq();
}

/// @brief Drains every pending controller output byte (bounded — the i8042
/// output buffer holds at most a few bytes) without decoding them.
void drain_controller() {
    for (int i = 0; i < 16; ++i) {
        if ((inb(KBD_CMD_PORT) & 0x01) == 0)
            return;
        (void)inb(KBD_DATA_PORT);
        arch::pause();
    }
}

/// @brief Resets the decode pipeline: drains the controller, reinitialises
/// the driver (clears ring + modifiers) and flushes the ring.
void reset_keyboard() {
    drain_controller();
    Keyboard::init();
    Keyboard::flush();
}

} // namespace

// Runmode: kernel
// Testidea: Lower-table scancodes decode to the expected ASCII characters
// through the real handle_irq path.
// Input: Inject 0x02 ('1'), 0x0A ('9'), 0x10 ('q'), 0x1E ('a'), 0x2C ('z').
// Expect: getchar returns exactly those characters, in order.
// Depends: arch::Keyboard
JARVIS_TEST(kbd_lower_table_chars, "PRE: iocd | POST: none") {
    reset_keyboard();
    struct {
        uint8_t sc;
        char expected;
    } vectors[] = {{0x02, '1'}, {0x0A, '9'}, {0x10, 'q'},
                   {0x1E, 'a'}, {0x2C, 'z'}};
    for (const auto &v : vectors) {
        inject_scancode(v.sc);
        char c = 0;
        JARVIS_ASSERT(Keyboard::getchar(c));
        JARVIS_ASSERT_EQ(v.expected, c);
    }
    char extra = 0;
    JARVIS_ASSERT(!Keyboard::getchar(extra));
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Shift (0x2A make / 0xAA break) selects the upper table while
// held and the lower table after release — the modifier latch must track
// the real make/break codes.
// Input: 0x2A down, inject 0x1E and 0x02, 0xAA up, inject 0x1E again.
// Expect: While shifted: 'A' and '!'; is_shifted() true; after break: 'a'
//         and is_shifted() false.
// Depends: arch::Keyboard
JARVIS_TEST(kbd_shift_selects_upper_table, "PRE: iocd | POST: none") {
    reset_keyboard();
    inject_scancode(0x2A); // shift down
    char c = 0;
    JARVIS_ASSERT(Keyboard::is_shifted());
    inject_scancode(0x1E); // 'a'
    JARVIS_ASSERT(Keyboard::getchar(c));
    JARVIS_ASSERT_EQ('A', c);
    inject_scancode(0x02); // '1'
    JARVIS_ASSERT(Keyboard::getchar(c));
    JARVIS_ASSERT_EQ('!', c);
    inject_scancode(0xAA); // shift up
    JARVIS_ASSERT(!Keyboard::is_shifted());
    inject_scancode(0x1E);
    JARVIS_ASSERT(Keyboard::getchar(c));
    JARVIS_ASSERT_EQ('a', c);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Ctrl (0x1D make / 0x9D break) toggles the ctrl modifier without
// emitting characters and without corrupting the ring.
// Input: 0x1D down, 0x9D up, polling is_ctrl().
// Expect: is_ctrl() true while held, false after break; no characters
//         decoded for either code.
// Depends: arch::Keyboard
JARVIS_TEST(kbd_ctrl_modifier_latch, "PRE: iocd | POST: none") {
    reset_keyboard();
    inject_scancode(0x1D); // ctrl down
    JARVIS_ASSERT(Keyboard::is_ctrl());
    inject_scancode(0x9D); // ctrl up
    JARVIS_ASSERT(!Keyboard::is_ctrl());
    char c = 0;
    JARVIS_ASSERT(!Keyboard::getchar(c)); // no chars from modifier codes
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Alt (0x38 make / 0xB8 break) toggles the alt modifier.
// Input: 0x38 down, 0xB8 up, polling is_alt().
// Expect: is_alt() true while held, false after break.
// Depends: arch::Keyboard
JARVIS_TEST(kbd_alt_modifier_latch, "PRE: iocd | POST: none") {
    reset_keyboard();
    inject_scancode(0x38); // alt down
    JARVIS_ASSERT(Keyboard::is_alt());
    inject_scancode(0xB8); // alt up
    JARVIS_ASSERT(!Keyboard::is_alt());
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Break codes (0x80 | scancode) of mapped keys never emit a
// character — only the key-release state update happens.
// Input: Inject break codes of 'a' (0x9E), 'z' (0xBC), '1' (0x82).
// Expect: Ring stays empty; no OOB access on the 128-entry tables.
// Depends: arch::Keyboard
JARVIS_TEST(kbd_break_codes_emit_nothing, "PRE: iocd | POST: none") {
    reset_keyboard();
    inject_scancode(0x9E); // break of 0x1E 'a'
    inject_scancode(0xBC); // break of 0x3C 'z'
    inject_scancode(0x82); // break of 0x02 '1'
    char c = 0;
    JARVIS_ASSERT(!Keyboard::getchar(c));
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Unmapped scancodes (table entry 0) are ignored safely — no
// character, no out-of-bounds access on the 128-entry tables, and the
// decoder keeps working for the next mapped key.
// Input: Inject 0x77, 0x7C, 0x00 (unmapped / zero entries), then 'q'.
// Expect: No characters from the unmapped codes; 'q' decodes afterwards.
// Depends: arch::Keyboard
JARVIS_TEST(kbd_unmapped_scancodes_ignored, "PRE: iocd | POST: none") {
    reset_keyboard();
    inject_scancode(0x77);
    inject_scancode(0x7C);
    inject_scancode(0x00);
    char c = 0;
    JARVIS_ASSERT(!Keyboard::getchar(c));
    inject_scancode(0x10); // 'q'
    JARVIS_ASSERT(Keyboard::getchar(c));
    JARVIS_ASSERT_EQ('q', c);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Control keys bypass the character tables: Enter (0x1C) emits
// '\n', Backspace (0x0E) emits '\b', Tab (0x0F) emits '\t'.
// Input: Inject the three control scancodes.
// Expect: The ring holds exactly those three characters in order.
// Depends: arch::Keyboard
JARVIS_TEST(kbd_control_keys_bypass_tables, "PRE: iocd | POST: none") {
    reset_keyboard();
    inject_scancode(0x1C); // enter
    inject_scancode(0x0E); // backspace
    inject_scancode(0x0F); // tab
    char c = 0;
    JARVIS_ASSERT(Keyboard::getchar(c));
    JARVIS_ASSERT_EQ('\n', c);
    JARVIS_ASSERT(Keyboard::getchar(c));
    JARVIS_ASSERT_EQ('\b', c);
    JARVIS_ASSERT(Keyboard::getchar(c));
    JARVIS_ASSERT_EQ('\t', c);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Caps-lock (0x3A) is a toggle; combined shift+caps uses XOR
// semantics — shift while caps is on returns to lowercase.
// Input: 0x3A make, 'a' → 'A'; 0x2A make with caps still on, 'a' → 'a';
//        release both (0x2A break only — caps stays latched), then clear
//        caps with a second 0x3A make.
// Expect: 'A' then 'a' then 'a', matching XOR selection logic.
// Depends: arch::Keyboard
JARVIS_TEST(kbd_caps_xor_shift_semantics, "PRE: iocd | POST: none") {
    reset_keyboard();
    char c = 0;
    inject_scancode(0x3A); // caps on
    inject_scancode(0x1E); // 'a'
    JARVIS_ASSERT(Keyboard::getchar(c));
    JARVIS_ASSERT_EQ('A', c);
    inject_scancode(0x2A); // shift down (caps still on)
    inject_scancode(0x1E);
    JARVIS_ASSERT(Keyboard::getchar(c));
    JARVIS_ASSERT_EQ('a', c);
    inject_scancode(0xAA); // shift up — caps alone restores 'A'
    inject_scancode(0x1E);
    JARVIS_ASSERT(Keyboard::getchar(c));
    JARVIS_ASSERT_EQ('A', c);
    inject_scancode(0x3A); // caps off
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Keyboard::read() drains multiple queued characters into a
// caller buffer in FIFO order and reports the exact count.
// Input: Inject 'h','e','l','p'; read into a 8-byte buffer.
// Expect: Returns 4, buffer contains "help".
// Depends: arch::Keyboard
JARVIS_TEST(kbd_read_drains_fifo, "PRE: iocd | POST: none") {
    reset_keyboard();
    inject_scancode(0x23); // 'h'
    inject_scancode(0x12); // 'e'
    inject_scancode(0x26); // 'l'
    inject_scancode(0x19); // 'p'
    char buf[8] = {};
    size_t n = Keyboard::read(buf, sizeof(buf));
    JARVIS_ASSERT_EQ(static_cast<size_t>(4), n);
    JARVIS_ASSERT(memcmp(buf, "help", 4) == 0);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: flush() discards every queued character — after flush the ring
// is empty and later keys still decode (flush must not wedge the ring).
// Input: Inject two keys, flush(), then inject 'x' (0x1D? no — 0x1D is
//        ctrl; use 0x2D 'x') and read.
// Expect: getchar false after flush; 'x' decodes after.
// Depends: arch::Keyboard
JARVIS_TEST(kbd_flush_clears_ring, "PRE: iocd | POST: none") {
    reset_keyboard();
    inject_scancode(0x1E); // 'a'
    inject_scancode(0x30); // 'b'
    Keyboard::flush();
    char c = 0;
    JARVIS_ASSERT(!Keyboard::getchar(c));
    inject_scancode(0x2D); // 'x'
    JARVIS_ASSERT(Keyboard::getchar(c));
    JARVIS_ASSERT_EQ('x', c);
    JARVIS_TEST_PASS();
}

void register_keyboard_decode_tests() {
    Logger::info("Registering keyboard decode tests");
    JARVIS_REGISTER_TEST(kbd_lower_table_chars);
    JARVIS_REGISTER_TEST(kbd_shift_selects_upper_table);
    JARVIS_REGISTER_TEST(kbd_ctrl_modifier_latch);
    JARVIS_REGISTER_TEST(kbd_alt_modifier_latch);
    JARVIS_REGISTER_TEST(kbd_break_codes_emit_nothing);
    JARVIS_REGISTER_TEST(kbd_unmapped_scancodes_ignored);
    JARVIS_REGISTER_TEST(kbd_control_keys_bypass_tables);
    JARVIS_REGISTER_TEST(kbd_caps_xor_shift_semantics);
    JARVIS_REGISTER_TEST(kbd_read_drains_fifo);
    JARVIS_REGISTER_TEST(kbd_flush_clears_ring);
}
#elif defined(CONFIG_ARCH_AARCH64) || defined(CONFIG_ARCH_RISCV64)
#include <test.hpp>
#include <logger.hpp>
#include <kernel/arch/keyboard.hpp>
#include <kernel/arch/serial.hpp>
#include <kernel/arch/io.hpp>
#include <kernel/input/serial_input.hpp>
#include <string.hpp>

using namespace kernel;
using arch::Keyboard;
using arch::Serial;

// Runmode: kernel
// Testidea: Serial-injected printable bytes arrive in order through the
// decoder facade (issue #355: UART is the only input source off-x86).
// Input: Keyboard::init(); inject_serial("hello", 5).
// Expect: 5 accepted; read() returns 5 with "hello";
//         ring empty afterwards.
// Depends: arch::Keyboard::inject_serial/read
JARVIS_TEST(kbd_dec_inject_printable_read, "PRE: iocd | POST: none") {
    Keyboard::init();
    size_t accepted = Keyboard::inject_serial("hello", 5);
    JARVIS_ASSERT_EQ(static_cast<uint64_t>(5),
                     static_cast<uint64_t>(accepted));
    char buf[8] = {};
    size_t n = Keyboard::read(buf, sizeof(buf));
    JARVIS_ASSERT_EQ(static_cast<uint64_t>(5),
                     static_cast<uint64_t>(n));
    JARVIS_ASSERT(memcmp(buf, "hello", 5) == 0);
    char extra = 0;
    JARVIS_ASSERT(!Keyboard::getchar(extra));
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Decoder normalization (issue #355): CR and LF both become
// newline; BS and DEL both become backspace; TAB passes through.
// Input: inject "\r\n\x08\x7F\tA" (6 bytes).
// Expect: pops '\n','\n','\b','\b','\t','A' in order.
// Depends: arch::Keyboard::inject_serial/getchar
JARVIS_TEST(kbd_dec_crlf_normalize, "PRE: iocd | POST: none") {
    Keyboard::init();
    const char seq[6] = {'\r', '\n', '\b', '\x7F', '\t', 'A'};
    size_t accepted = Keyboard::inject_serial(seq, sizeof(seq));
    JARVIS_ASSERT_EQ(static_cast<uint64_t>(6),
                     static_cast<uint64_t>(accepted));
    const char want[6] = {'\n', '\n', '\b', '\b', '\t', 'A'};
    for (int i = 0; i < 6; ++i) {
        char c = 0;
        JARVIS_ASSERT(Keyboard::getchar(c));
        JARVIS_ASSERT_EQ(want[i], c);
    }
    char extra = 0;
    JARVIS_ASSERT(!Keyboard::getchar(extra));
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: NUL is never a key — it is dropped and counts as dropped,
// while surrounding bytes are unaffected (issue #355).
// Input: inject "a\0b" (3 bytes, one NUL).
// Expect: 2 accepted; dropped delta 1; pops 'a','b'.
// Depends: input::serial::dropped_count
JARVIS_TEST(kbd_dec_nul_dropped_counts, "PRE: iocd | POST: none") {
    Keyboard::init();
    size_t dropped_before = kernel::input::serial::dropped_count();
    const char seq[3] = {'a', 0x00, 'b'};
    size_t accepted = Keyboard::inject_serial(seq, sizeof(seq));
    size_t dropped_after = kernel::input::serial::dropped_count();
    JARVIS_ASSERT_EQ(static_cast<uint64_t>(2),
                     static_cast<uint64_t>(accepted));
    JARVIS_ASSERT_EQ(static_cast<uint64_t>(1), dropped_after - dropped_before);
    char c = 0;
    JARVIS_ASSERT(Keyboard::getchar(c));
    JARVIS_ASSERT_EQ('a', c);
    JARVIS_ASSERT(Keyboard::getchar(c));
    JARVIS_ASSERT_EQ('b', c);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: High bytes pass through verbatim — a UART has no modifier
// channel, so 0x80..0xFF are data, never control (issue #355).
// Input: inject 0x80, 0xFF, 0x41.
// Expect: 3 accepted; read returns the identical bytes.
// Depends: arch::Keyboard::inject_serial/read
JARVIS_TEST(kbd_dec_high_bytes_verbatim, "PRE: iocd | POST: none") {
    Keyboard::init();
    const uint8_t seq[3] = {0x80, 0xFF, 0x41};
    size_t accepted = Keyboard::inject_serial(
        reinterpret_cast<const char *>(seq), sizeof(seq));
    JARVIS_ASSERT_EQ(static_cast<uint64_t>(3),
                     static_cast<uint64_t>(accepted));
    uint8_t buf[4] = {};
    size_t n = Keyboard::read(reinterpret_cast<char *>(buf), sizeof(buf));
    JARVIS_ASSERT_EQ(static_cast<uint64_t>(3),
                     static_cast<uint64_t>(n));
    JARVIS_ASSERT(memcmp(buf, seq, sizeof(seq)) == 0);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: The 256-slot SPSC ring holds 255 keys (one slot sacrificed
// for the full/empty distinction); the remainder counts as dropped and
// the drain preserves FIFO order (issue #355 H4: 255+45).
// Input: inject 255 x's; inject 45 y's; drain.
// Expect: 255 then 0 accepted; dropped delta 45; 255 pops, all 'x'.
// Depends: arch::Keyboard::inject_serial/getchar
JARVIS_TEST(kbd_dec_ring_full_drops_remainder, "PRE: iocd | POST: none") {
    Keyboard::init();
    char fill[255];
    for (int i = 0; i < 255; ++i)
        fill[i] = 'x';
    size_t accepted_fill =
        Keyboard::inject_serial(fill, sizeof(fill));
    size_t dropped_before = kernel::input::serial::dropped_count();
    char extra[45];
    for (int i = 0; i < 45; ++i)
        extra[i] = 'y';
    size_t accepted_extra =
        Keyboard::inject_serial(extra, sizeof(extra));
    size_t dropped_after = kernel::input::serial::dropped_count();
    JARVIS_ASSERT_EQ(static_cast<uint64_t>(255),
                     static_cast<uint64_t>(accepted_fill));
    JARVIS_ASSERT_EQ(static_cast<uint64_t>(0),
                     static_cast<uint64_t>(accepted_extra));
    JARVIS_ASSERT_EQ(static_cast<uint64_t>(45),
                     dropped_after - dropped_before);
    for (int i = 0; i < 255; ++i) {
        char c = 0;
        JARVIS_ASSERT(Keyboard::getchar(c));
        JARVIS_ASSERT_EQ('x', c);
    }
    char tail = 0;
    JARVIS_ASSERT(!Keyboard::getchar(tail));
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Modifiers are always false off-x86 — a UART has no modifier
// channel (Ctrl arrives as 0x01-0x1A, never sticky; issue #355).
// Input: poll is_shifted/is_ctrl/is_alt.
// Expect: all false, before and after injecting a control byte.
// Depends: arch::Keyboard modifiers
JARVIS_TEST(kbd_dec_modifiers_always_false, "PRE: iocd | POST: none") {
    Keyboard::init();
    JARVIS_ASSERT(!Keyboard::is_shifted());
    JARVIS_ASSERT(!Keyboard::is_ctrl());
    JARVIS_ASSERT(!Keyboard::is_alt());
    const char ctrl = 0x03; // ETX — would be Ctrl-C on a terminal
    JARVIS_ASSERT_EQ(static_cast<uint64_t>(1),
                     static_cast<uint64_t>(
                         Keyboard::inject_serial(&ctrl, 1)));
    JARVIS_ASSERT(!Keyboard::is_shifted());
    JARVIS_ASSERT(!Keyboard::is_ctrl());
    JARVIS_ASSERT(!Keyboard::is_alt());
    char c = 0;
    JARVIS_ASSERT(Keyboard::getchar(c));
    JARVIS_ASSERT_EQ(0x03, c);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: flush() discards queued keys and later injects still decode
// (flush must not wedge the ring).
// Input: inject "ab"; flush(); inject "c".
// Expect: getchar false after flush; then 'c'.
// Depends: arch::Keyboard::flush
JARVIS_TEST(kbd_dec_flush_clears_ring, "PRE: iocd | POST: none") {
    Keyboard::init();
    JARVIS_ASSERT_EQ(static_cast<uint64_t>(2),
                     static_cast<uint64_t>(
                         Keyboard::inject_serial("ab", 2)));
    Keyboard::flush();
    char c = 0;
    JARVIS_ASSERT(!Keyboard::getchar(c));
    JARVIS_ASSERT_EQ(static_cast<uint64_t>(1),
                     static_cast<uint64_t>(
                         Keyboard::inject_serial("c", 1)));
    JARVIS_ASSERT(Keyboard::getchar(c));
    JARVIS_ASSERT_EQ('c', c);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: read() drains multiple queued keys in FIFO order and reports
// the exact count.
// Input: inject "help"; read into an 8-byte buffer.
// Expect: returns 4, buffer holds "help".
// Depends: arch::Keyboard::read
JARVIS_TEST(kbd_dec_read_drains_fifo, "PRE: iocd | POST: none") {
    Keyboard::init();
    JARVIS_ASSERT_EQ(static_cast<uint64_t>(4),
                     static_cast<uint64_t>(
                         Keyboard::inject_serial("help", 4)));
    char buf[8] = {};
    size_t n = Keyboard::read(buf, sizeof(buf));
    JARVIS_ASSERT_EQ(static_cast<uint64_t>(4),
                     static_cast<uint64_t>(n));
    JARVIS_ASSERT(memcmp(buf, "help", 4) == 0);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: handle_irq is a contract-parity no-op off-x86 — no UART IRQ
// exists, so it must neither consume nor produce keys (issue #355).
// Input: inject "q"; handle_irq(); read.
// Expect: the key survives; exactly one pop.
// Depends: arch::Keyboard::handle_irq
JARVIS_TEST(kbd_dec_irq_is_noop, "PRE: iocd | POST: none") {
    Keyboard::init();
    JARVIS_ASSERT_EQ(static_cast<uint64_t>(1),
                     static_cast<uint64_t>(
                         Keyboard::inject_serial("q", 1)));
    Keyboard::handle_irq();
    char c = 0;
    JARVIS_ASSERT(Keyboard::getchar(c));
    JARVIS_ASSERT_EQ('q', c);
    JARVIS_ASSERT(!Keyboard::getchar(c));
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: init() resets the decoder — queued keys and drop counters
// clear, so every test starts from a clean slate (self-cleanup rule).
// Input: inject "zz"; init(); poll.
// Expect: ring empty; a fresh inject decodes afterwards.
// Depends: arch::Keyboard::init
JARVIS_TEST(kbd_dec_init_resets_decoder, "PRE: iocd | POST: none") {
    Keyboard::init();
    JARVIS_ASSERT_EQ(static_cast<uint64_t>(2),
                     static_cast<uint64_t>(
                         Keyboard::inject_serial("zz", 2)));
    Keyboard::init();
    char c = 0;
    JARVIS_ASSERT(!Keyboard::getchar(c));
    JARVIS_ASSERT_EQ(static_cast<uint64_t>(1),
                     static_cast<uint64_t>(
                         Keyboard::inject_serial("w", 1)));
    JARVIS_ASSERT(Keyboard::getchar(c));
    JARVIS_ASSERT_EQ('w', c);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: pump_serial on an idle UART returns 0 and leaves the ring
// empty — the shell nap loop must observe no phantom keys (issue #355).
// The RX FIFO is drained first (bounded), so the test is independent of
// run order within the hal aggregate.
// Input: drain RX via poll_getchar (bounded 64); pump_serial().
// Expect: 0 pushed; ring empty.
// Depends: arch::Keyboard::pump_serial, arch::Serial::poll_getchar
JARVIS_TEST(kbd_dec_pump_idle_returns_zero, "PRE: iocd | POST: none") {
    Keyboard::init();
    for (int i = 0; i < 64; ++i) {
        char junk = 0;
        if (!Serial::poll_getchar(junk))
            break;
    }
    size_t pushed = Keyboard::pump_serial();
    JARVIS_ASSERT_EQ(static_cast<uint64_t>(0),
                     static_cast<uint64_t>(pushed));
    char c = 0;
    JARVIS_ASSERT(!Keyboard::getchar(c));
    JARVIS_TEST_PASS();
}

#if defined(CONFIG_ARCH_RISCV64)
// Runmode: kernel
// Testidea: pump_serial consumes real looped UART bytes through the
// decoder (issue #355 H4, riscv-only: PL011 has no loopback mode).
// pump() is contractually inert inside the loopback window, so this
// test drives MCR LOOP manually OUTSIDE the window protocol: set LOOP,
// SBI-transmit, bounded pump retries, pops, then unconditional MCR
// restore + RX drain with every assert after the restore (a mid-window
// assert would strand LOOP and silence the suite). Registered in debug
// builds only (loopback is a debug facility).
// Input: manual LOOP; putchar("hi"); bounded pump retries; pops;
//        manual restore + drain.
// Expect: 2 total pushed; pops 'h','i'; ring empty; MCR restored.
// Depends: arch::Serial MCR accessors, arch::Keyboard::pump_serial
JARVIS_TEST(kbd_dec_pump_consumes_loopback_bytes,
            "PRE: iocd | POST: none") {
    Keyboard::init();
    uint8_t pre = Serial::uart_mcr();
    Serial::uart_mcr_set(
        static_cast<uint8_t>(pre | arch::Serial::UART_MCR_LOOP));
    bool loop_set = (Serial::uart_mcr() & arch::Serial::UART_MCR_LOOP) != 0;
    size_t pushed_total = 0;
    if (loop_set) {
        Serial::putchar('h');
        Serial::putchar('i');
        for (int i = 0; i < 1000 && pushed_total < 2; ++i) {
            pushed_total += Keyboard::pump_serial();
            arch::pause();
        }
    }
    char first = 0;
    char second = 0;
    bool got_both = false;
    if (loop_set) {
        got_both = Keyboard::getchar(first) && Keyboard::getchar(second);
    }
    Serial::uart_mcr_set(pre);
    Serial::uart_drain_rx();
    uint8_t post = Serial::uart_mcr();
    char tail = 0;
    bool drained = !Keyboard::getchar(tail);
    JARVIS_ASSERT(loop_set);
    JARVIS_ASSERT_EQ(static_cast<uint64_t>(2), pushed_total);
    JARVIS_ASSERT(got_both);
    JARVIS_ASSERT_EQ('h', first);
    JARVIS_ASSERT_EQ('i', second);
    JARVIS_ASSERT(drained);
    JARVIS_ASSERT_EQ(static_cast<uint64_t>(pre),
                     static_cast<uint64_t>(post));
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: pump() is inert while the loopback window owns the UART —
// the window suspends capture so test traffic never enters the decoder
// ring (issue #355 Slice C contract).
// Input: loopback_begin; putchar('X'); pump_serial(); loopback_end.
// Expect: pump returns 0; ring empty after the window closes.
// Depends: input::serial loopback window
JARVIS_TEST(kbd_dec_pump_inert_while_loopback, "PRE: iocd | POST: none") {
    Keyboard::init();
    bool entered = kernel::input::serial::loopback_begin();
    size_t pushed = 0;
    if (entered) {
        Serial::putchar('X');
        pushed = Keyboard::pump_serial();
    }
    kernel::input::serial::loopback_end();
    char c = 0;
    bool empty = !Keyboard::getchar(c);
    JARVIS_ASSERT(entered);
    JARVIS_ASSERT_EQ(static_cast<uint64_t>(0),
                     static_cast<uint64_t>(pushed));
    JARVIS_ASSERT(empty);
    JARVIS_TEST_PASS();
}
#else
// Runmode: kernel
// Testidea: The riscv loopback window is unavailable on aarch64 (PL011
// has no loopback mode) — loopback_begin must refuse fail-closed
// (issue #355 Slice C).
// Input: loopback_begin().
// Expect: false; no window state latched.
// Depends: input::serial::loopback_begin
JARVIS_TEST(kbd_dec_loopback_unsupported_fail_closed,
            "PRE: iocd | POST: none") {
    JARVIS_ASSERT(!kernel::input::serial::loopback_begin());
    JARVIS_ASSERT(!kernel::input::serial::loopback_active());
    JARVIS_TEST_PASS();
}
#endif

void register_keyboard_decode_tests() {
    Logger::info("Registering keyboard decode tests");
    JARVIS_REGISTER_TEST(kbd_dec_inject_printable_read);
    JARVIS_REGISTER_TEST(kbd_dec_crlf_normalize);
    JARVIS_REGISTER_TEST(kbd_dec_nul_dropped_counts);
    JARVIS_REGISTER_TEST(kbd_dec_high_bytes_verbatim);
    JARVIS_REGISTER_TEST(kbd_dec_ring_full_drops_remainder);
    JARVIS_REGISTER_TEST(kbd_dec_modifiers_always_false);
    JARVIS_REGISTER_TEST(kbd_dec_flush_clears_ring);
    JARVIS_REGISTER_TEST(kbd_dec_read_drains_fifo);
    JARVIS_REGISTER_TEST(kbd_dec_irq_is_noop);
    JARVIS_REGISTER_TEST(kbd_dec_init_resets_decoder);
    JARVIS_REGISTER_TEST(kbd_dec_pump_idle_returns_zero);
#if defined(CONFIG_ARCH_RISCV64)
#if defined(CONFIG_DEBUG)
    JARVIS_REGISTER_TEST(kbd_dec_pump_consumes_loopback_bytes);
    JARVIS_REGISTER_TEST(kbd_dec_pump_inert_while_loopback);
#endif
#else
    JARVIS_REGISTER_TEST(kbd_dec_loopback_unsupported_fail_closed);
#endif
}
#endif // CONFIG_ARCH_X86_64
