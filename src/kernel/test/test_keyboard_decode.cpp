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
#endif // CONFIG_ARCH_X86_64
