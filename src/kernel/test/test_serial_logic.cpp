/*
 * NexIOS RTOS — Development Roadmap / Kernel Core
 * Copyright (C) 2026 Arnold Hasshold
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3, as
 * published by the Free Software Foundation.
 */

/// @file test_serial_logic.cpp
/// @brief UART (16550 COM1) driver logic tests (milestone v0.4.3 #118):
///        init register state (FIFO enable, DLAB, baud divisor) verified
///        by register readback on the real QEMU 16550, and TX→RX loopback
///        round-trips through Serial::putchar/getchar (16550 MCR loopback
///        mode — bytes stay inside the UART, never reach the host console).

#if defined(CONFIG_ARCH_X86_64)
#include <test.hpp>
#include <logger.hpp>
#include <kernel/arch/serial.hpp>
#include <kernel/arch/hal/io.hpp>
#include <string.hpp>

using namespace kernel;
using arch::Serial;
using arch::inb;
using arch::outb;

namespace {

constexpr uint16_t COM1 = 0x3F8;
// Register offsets from COM1.
constexpr uint16_t REG_DATA = 0;    // THR / RBR / DLL (DLAB=1)
constexpr uint16_t REG_IER = 1;     // IER / DLM (DLAB=1)
constexpr uint16_t REG_FCR_IIR = 2; // FCR (write) / IIR (read)
constexpr uint16_t REG_LCR = 3;
constexpr uint16_t REG_MCR = 4;
constexpr uint16_t REG_LSR = 5;
// LCR bits.
constexpr uint8_t LCR_DLAB = 0x80;
// MCR bits.
constexpr uint8_t MCR_LOOPBACK = 0x10;
constexpr uint8_t MCR_OUT1 = 0x04;
constexpr uint8_t MCR_OUT2 = 0x08;
constexpr uint8_t MCR_RTS = 0x02;
constexpr uint8_t MCR_DTR = 0x01;

/// @brief Restores the driver's own init state (also re-arms a clean UART
/// after DLAB/loopback experiments).
void restore_uart() {
    outb(COM1 + REG_MCR, MCR_OUT1 | MCR_OUT2 | MCR_RTS | MCR_DTR); // 0x0F
    Serial::init();
}

} // namespace

// Runmode: kernel
// Testidea: Serial::init() programs the documented 8N1/115200 state: LCR
// ends at 0x03 (8 data bits, no parity, 1 stop, DLAB cleared) and the
// divisor latches hold 1 (115200 baud from the 1.8432 MHz clock).
// Input: Serial::init(), then LCR readback and DLAB-gated DLL/DLM reads.
// Expect: LCR == 0x03; DLL == 0x01; DLM == 0x00; DLAB restored to 0 after
//         the divisor probe.
// Depends: arch::Serial
JARVIS_TEST(serial_init_register_state, "PRE: iocd | POST: none") {
    Serial::init();
    uint8_t lcr = inb(COM1 + REG_LCR);
    JARVIS_ASSERT_EQ(static_cast<uint64_t>(0x03), static_cast<uint64_t>(lcr));

    // Probe the divisor latches via DLAB, then restore the 8N1 state.
    outb(COM1 + REG_LCR, LCR_DLAB);
    uint8_t dll = inb(COM1 + REG_DATA);
    uint8_t dlm = inb(COM1 + REG_IER);
    outb(COM1 + REG_LCR, 0x03);
    restore_uart();

    JARVIS_ASSERT_EQ(static_cast<uint64_t>(0x01), static_cast<uint64_t>(dll));
    JARVIS_ASSERT_EQ(static_cast<uint64_t>(0x00), static_cast<uint64_t>(dlm));
    JARVIS_ASSERT_EQ(static_cast<uint64_t>(0x03),
                     static_cast<uint64_t>(inb(COM1 + REG_LCR)));
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Serial::init() enables the 16550 FIFOs — the FCR write of
// 0xC7 is observable in the IIR's FIFO-status bits (bit 6|7 set on read
// when FIFOs are enabled and usable).
// Input: Serial::init(), read IIR.
// Expect: (IIR & 0xC0) == 0xC0 (FIFO enabled).
// Depends: arch::Serial
JARVIS_TEST(serial_init_fifo_enabled, "PRE: iocd | POST: none") {
    Serial::init();
    uint8_t iir = inb(COM1 + REG_FCR_IIR);
    uint8_t fifo_bits = static_cast<uint8_t>(iir & 0xC0);
    restore_uart();
    JARVIS_ASSERT_EQ(static_cast<uint64_t>(0xC0),
                     static_cast<uint64_t>(fifo_bits));
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Loopback round-trip — with MCR loopback set, every byte
// written via Serial::putchar appears on the receive side via
// Serial::getchar (real 16550 path: TX shift register loops into RX).
// Input: MCR |= 0x10; putchar('A'); getchar().
// Expect: getchar returns 'A' (bounded wait, never hangs).
// Depends: arch::Serial
JARVIS_TEST(serial_loopback_roundtrip, "PRE: iocd | POST: none") {
    Serial::init();
    outb(COM1 + REG_MCR, inb(COM1 + REG_MCR) | MCR_LOOPBACK);
    Serial::putchar('A');
    char c = Serial::getchar();
    outb(COM1 + REG_MCR, inb(COM1 + REG_MCR) & static_cast<uint8_t>(~MCR_LOOPBACK));
    restore_uart();
    JARVIS_ASSERT_EQ('A', c);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Newline expansion over loopback — putchar('\n') transmits the
// '\r' '\n' pair, both receivable in order.
// Input: Loopback on; putchar('\n'); two getchar calls.
// Expect: First '\r', then '\n'.
// Depends: arch::Serial
JARVIS_TEST(serial_loopback_newline_expansion, "PRE: iocd | POST: none") {
    Serial::init();
    outb(COM1 + REG_MCR, inb(COM1 + REG_MCR) | MCR_LOOPBACK);
    Serial::putchar('\n');
    char first = Serial::getchar();
    char second = Serial::getchar();
    outb(COM1 + REG_MCR, inb(COM1 + REG_MCR) & static_cast<uint8_t>(~MCR_LOOPBACK));
    restore_uart();
    JARVIS_ASSERT_EQ('\r', first);
    JARVIS_ASSERT_EQ('\n', second);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: puts() transmits every byte in order over loopback and the
// write counter advances by the transmitted byte count (the '\n'
// expansion adds to the count too).
// Input: Loopback on; snapshot write_count; puts("hi\n"); drain 4 bytes.
// Expect: Drained bytes "hi\r\n"; write_count advanced by AT LEAST 3 —
//         one increment per putchar call ('h','i','\n'); the '\r'
//         escape byte is not counted, and the counter is global so
//         concurrent console output may only ADD to the delta.
// Depends: arch::Serial
JARVIS_TEST(serial_puts_and_write_count, "PRE: iocd | POST: none") {
    Serial::init();
    outb(COM1 + REG_MCR, inb(COM1 + REG_MCR) | MCR_LOOPBACK);
    uint64_t before = Serial::write_count();
    Serial::puts("hi\n");
    uint64_t after = Serial::write_count();
    char buf[5] = {};
    for (int i = 0; i < 4; ++i)
        buf[i] = Serial::getchar();
    outb(COM1 + REG_MCR, inb(COM1 + REG_MCR) & static_cast<uint8_t>(~MCR_LOOPBACK));
    restore_uart();
    JARVIS_ASSERT(memcmp(buf, "hi\r\n", 4) == 0);
    JARVIS_ASSERT(after >= before + 3);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: getchar() bounded-wait contract (FLAW-08) — with no incoming
// data and loopback off, getchar returns the '\0' sentinel after the
// bounded poll instead of hanging.
// Input: Loopback off; drain LSR; getchar().
// Expect: Returns '\0'; the call terminates (test reaching the assert
//         proves the bound).
// Depends: arch::Serial
JARVIS_TEST(serial_getchar_idle_returns_nul, "PRE: iocd | POST: none") {
    restore_uart();
    // Drain any pending receive byte (bounded — FIFO is 16 deep).
    for (int i = 0; i < 16 && (inb(COM1 + REG_LSR) & 0x01); ++i)
        (void)inb(COM1 + REG_DATA);
    char c = Serial::getchar();
    JARVIS_ASSERT_EQ('\0', c);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Serial::poll_getchar() non-blocking contract (issue #245) —
// false immediately on an empty FIFO, true + the byte after a loopback
// transmit, false again once drained. The shell's input poll on every
// architecture.
// Input: Loopback on; drain; poll (expect false); putchar('Q');
// poll (expect true 'Q'); poll (expect false); loopback off.
// Expect: false/true('Q')/false sequence; bounded (no waits anywhere).
// Depends: arch::Serial
JARVIS_TEST(serial_poll_getchar_loopback, "PRE: iocd | POST: none") {
    Serial::init();
    outb(COM1 + REG_MCR, inb(COM1 + REG_MCR) | MCR_LOOPBACK);
    for (int i = 0; i < 16 && (inb(COM1 + REG_LSR) & 0x01); ++i)
        (void)inb(COM1 + REG_DATA);
    char idle = 0;
    bool idle_got = Serial::poll_getchar(idle);
    Serial::putchar('Q');
    char c = 0;
    bool got = Serial::poll_getchar(c);
    char drained = 0;
    bool drained_got = Serial::poll_getchar(drained);
    outb(COM1 + REG_MCR, inb(COM1 + REG_MCR) & static_cast<uint8_t>(~MCR_LOOPBACK));
    restore_uart();
    JARVIS_ASSERT(!idle_got);
    JARVIS_ASSERT(got);
    JARVIS_ASSERT_EQ('Q', c);
    JARVIS_ASSERT(!drained_got);
    JARVIS_TEST_PASS();
}

void register_serial_logic_tests() {
    Logger::info("Registering serial logic tests");
    JARVIS_REGISTER_TEST(serial_init_register_state);
    JARVIS_REGISTER_TEST(serial_init_fifo_enabled);
    JARVIS_REGISTER_TEST(serial_loopback_roundtrip);
    JARVIS_REGISTER_TEST(serial_loopback_newline_expansion);
    JARVIS_REGISTER_TEST(serial_puts_and_write_count);
    JARVIS_REGISTER_TEST(serial_getchar_idle_returns_nul);
    JARVIS_REGISTER_TEST(serial_poll_getchar_loopback); // issue #245
}
#elif defined(CONFIG_ARCH_AARCH64)
#include <test.hpp>
#include <logger.hpp>
#include <kernel/arch/serial.hpp>

using namespace kernel;
using arch::Serial;

// Runmode: kernel
// Testidea: Serial::init() programs the documented PL011 state: CR 0x301
// (RXE+TXE+UARTEN), 8N1+FIFO LCR_H 0x70, 115200-baud divisors (IBRD 26,
// FBRD 3) and IMSC 0 (polling only — no UART IRQ exists, issue #355).
// The PL011 has no loopback mode, so init-state readback is the parity
// contract (no fabricated loopback).
// Input: Serial::init(), then pl011_snapshot().
// Expect: CR == 0x301; LCR_H == 0x70; IBRD == 26; FBRD == 3; IMSC == 0;
//         FR shows TX FIFO empty (TXFE set, idle UART).
// Depends: arch::Serial::pl011_snapshot
JARVIS_TEST(serial_pl011_init_registers, "PRE: iocd | POST: none") {
    Serial::init();
    arch::Serial::Pl011Snapshot snap = Serial::pl011_snapshot();
    JARVIS_ASSERT_EQ(static_cast<uint64_t>(0x301),
                     static_cast<uint64_t>(snap.cr));
    JARVIS_ASSERT_EQ(static_cast<uint64_t>(0x70),
                     static_cast<uint64_t>(snap.lcr_h));
    JARVIS_ASSERT_EQ(static_cast<uint64_t>(26),
                     static_cast<uint64_t>(snap.ibrd));
    JARVIS_ASSERT_EQ(static_cast<uint64_t>(3),
                     static_cast<uint64_t>(snap.fbrd));
    JARVIS_ASSERT_EQ(static_cast<uint64_t>(0),
                     static_cast<uint64_t>(snap.imsc));
    JARVIS_ASSERT((snap.fr & (1u << 7)) != 0); // TXFE: FIFO empty when idle
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: The IMSC==0 no-interrupt pin holds across transmit traffic —
// the polled driver never arms a UART IRQ source (issue #355, polling
// only by design).
// Input: pl011_snapshot(); putchar('Z'); pl011_snapshot().
// Expect: IMSC == 0 before and after; the transmit completes (no hang
//         proves TXFE polling still drains).
// Depends: arch::Serial::pl011_snapshot
JARVIS_TEST(serial_pl011_irqs_never_armed, "PRE: iocd | POST: none") {
    arch::Serial::Pl011Snapshot before = Serial::pl011_snapshot();
    Serial::putchar('Z');
    arch::Serial::Pl011Snapshot after = Serial::pl011_snapshot();
    JARVIS_ASSERT_EQ(static_cast<uint64_t>(0),
                     static_cast<uint64_t>(before.imsc));
    JARVIS_ASSERT_EQ(static_cast<uint64_t>(0),
                     static_cast<uint64_t>(after.imsc));
    JARVIS_TEST_PASS();
}

void register_serial_logic_tests() {
    Logger::info("Registering serial logic tests");
    JARVIS_REGISTER_TEST(serial_pl011_init_registers);
    JARVIS_REGISTER_TEST(serial_pl011_irqs_never_armed);
}
#elif defined(CONFIG_ARCH_RISCV64)
#include <test.hpp>
#include <logger.hpp>
#include <kernel/arch/serial.hpp>
#include <kernel/input/serial_input.hpp>

using namespace kernel;
using arch::Serial;

// Runmode: kernel
// Testidea: The virt 16550A snapshot observes the OpenSBI/QEMU live
// configuration: IER stays 0 (RX is polled, never interrupt-driven —
// the kernel never writes IER, issue #355) and DLAB stays clear.
// Input: Serial::uart_snapshot() on the live UART.
// Expect: IER == 0; LCR DLAB bit (0x80) clear; MCR LOOP bit clear in
//         steady state (no test left the window open).
// Depends: arch::Serial::uart_snapshot
JARVIS_TEST(serial_16550_snapshot_init_state, "PRE: iocd | POST: none") {
    arch::Serial::Uart16550Snapshot snap = Serial::uart_snapshot();
    JARVIS_ASSERT_EQ(static_cast<uint64_t>(0),
                     static_cast<uint64_t>(snap.ier));
    JARVIS_ASSERT((snap.lcr & 0x80) == 0); // DLAB clear
    JARVIS_ASSERT((snap.mcr & arch::Serial::UART_MCR_LOOP) == 0);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: MCR loopback round-trip — with LOOP set, an SBI-transmitted
// byte (OpenSBI writes the same THR the MMIO path reads) returns via RBR
// instead of reaching the host. All observations happen inside the
// window; every assert runs after unconditional restore (a failed assert
// inside the window would strand LOOP and silence the console suite).
// Newline note: putchar('\n') expands to CR+LF in the kernel AND OpenSBI
// expands '\n' again, so only the first looped byte ('\r', ours) is
// asserted — the remainder is drained boundedly (firmware composition is
// not kernel contract).
// Input: loopback_begin; putchar('A'); getchar; putchar('\n'); getchar;
//        bounded drain; loopback_end (unconditional).
// Expect: 'A', then '\r'; drain terminates; MCR after == MCR before.
// Depends: arch::Serial MCR accessors, input::serial loopback window
JARVIS_TEST(serial_16550_loopback_roundtrip, "PRE: iocd | POST: none") {
    uint8_t mcr_before = Serial::uart_mcr();
    bool entered = kernel::input::serial::loopback_begin();
    char got_a = 0;
    char got_cr = 0;
    bool drained = false;
    if (entered) {
        Serial::putchar('A');
        got_a = Serial::getchar();
        Serial::putchar('\n');
        got_cr = Serial::getchar();
        // Drain the firmware-composed remainder (bounded by FIFO depth).
        drained = true;
        for (int i = 0; i < 16; ++i) {
            char junk = 0;
            if (!Serial::poll_getchar(junk))
                break;
            if (i == 15)
                drained = false;
        }
    }
    kernel::input::serial::loopback_end();
    uint8_t mcr_after = Serial::uart_mcr();
    JARVIS_ASSERT(entered);
    JARVIS_ASSERT_EQ('A', got_a);
    JARVIS_ASSERT_EQ('\r', got_cr);
    JARVIS_ASSERT(drained);
    JARVIS_ASSERT_EQ(static_cast<uint64_t>(mcr_before),
                     static_cast<uint64_t>(mcr_after));
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: The loopback window is fail-closed — nested begin refuses,
// a foreign LOOP bit refuses without touching MCR, and the window can be
// re-entered after a clean exit (issue #355 Slice C contract).
// Input: begin; begin again; end; set LOOP manually; begin; restore
//        manually; begin; end. All asserts after the windows close.
// Expect: first begin true; nested and foreign begins false with MCR
//         unmodified by the refusals; re-entry true; final MCR == pre-test.
// Depends: arch::Serial MCR accessors, input::serial loopback window
JARVIS_TEST(serial_16550_loopback_fail_closed, "PRE: iocd | POST: none") {
    uint8_t pre = Serial::uart_mcr();
    bool first = kernel::input::serial::loopback_begin();
    bool nested = kernel::input::serial::loopback_begin();
    kernel::input::serial::loopback_end();
    Serial::uart_mcr_set(
        static_cast<uint8_t>(pre | arch::Serial::UART_MCR_LOOP));
    uint8_t forced = Serial::uart_mcr();
    bool foreign = kernel::input::serial::loopback_begin();
    Serial::uart_mcr_set(pre);
    bool reopen = kernel::input::serial::loopback_begin();
    kernel::input::serial::loopback_end();
    uint8_t post = Serial::uart_mcr();
    JARVIS_ASSERT(first);
    JARVIS_ASSERT(!nested);
    JARVIS_ASSERT((forced & arch::Serial::UART_MCR_LOOP) != 0);
    JARVIS_ASSERT(!foreign);
    JARVIS_ASSERT(reopen);
    JARVIS_ASSERT_EQ(static_cast<uint64_t>(pre),
                     static_cast<uint64_t>(post));
    JARVIS_TEST_PASS();
}

void register_serial_logic_tests() {
    Logger::info("Registering serial logic tests");
    JARVIS_REGISTER_TEST(serial_16550_snapshot_init_state);
    JARVIS_REGISTER_TEST(serial_16550_loopback_roundtrip);
    JARVIS_REGISTER_TEST(serial_16550_loopback_fail_closed);
}
#endif // CONFIG_ARCH_X86_64
