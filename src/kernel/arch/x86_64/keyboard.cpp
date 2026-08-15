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

/// @file keyboard.cpp
/// @brief PS/2 keyboard driver — translates scancodes to ASCII characters and
/// manages a ring-buffer input queue.

#include <kernel/arch/keyboard.hpp>
#include <kernel/arch/io.hpp>

namespace arch {

/// @brief Ring buffer for queued input characters.
SPSCRing<char, Keyboard::RING_SIZE> Keyboard::ring_;
/// @brief Packed Shift/Ctrl/Alt/Caps state (byte-atomic).
constinit uint8_t Keyboard::mods_ = 0;

/// @brief Scancode-to-ASCII lookup table for unshifted (lowercase) keys.
/// @note Indexed by scancode & 0x7F. Zero entries indicate unmapped or special
/// keys.
static const char scancode_lower[128] = {
    0,   0,   '1', '2', '3', '4', '5', '6', '7', '8', '9',  '0', '-', '=',  0,
    0,   'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p',  '[', ']', 0,    0,
    'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', ';', '\'', '`', 0,   '\\', 'z',
    'x', 'c', 'v', 'b', 'n', 'm', ',', '.', '/', 0,   '*',  0,   ' ', 0,    0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,    0,   0,   0,    0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,    0,   0,   0,    0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,    0,   0,   0,    0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,    0,   0,   0,    0,
    0,   0,   0,   0,   0,   0,   0,   0,
};

/// @brief Scancode-to-ASCII lookup table for shifted (uppercase) keys.
/// @note Indexed by scancode & 0x7F. Zero entries indicate unmapped or special
/// keys.
static const char scancode_upper[128] = {
    0,   0,   '!', '@', '#', '$', '%', '^', '&', '*', '(', ')', '_', '+', 0,
    0,   'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P', '{', '}', 0,   0,
    'A', 'S', 'D', 'F', 'G', 'H', 'J', 'K', 'L', ':', '"', '~', 0,   '|', 'Z',
    'X', 'C', 'V', 'B', 'N', 'M', '<', '>', '?', 0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,
};

/// @brief Initialise the PS/2 keyboard controller.
/// Enables the keyboard interface, resets the scancode ring buffer, and clears
/// all modifier key states.
void Keyboard::init() {
    ring_.reset();
    __atomic_store_n(&mods_, 0, __ATOMIC_RELEASE);

    outb(0x64, 0xAE);
    io_wait();
    // Drain any pending output (e.g. an ACK to a prior command) so the first
    // keyboard IRQ does not read a stale byte as a scancode.  Bounded
    // (FLAW-10): cap at the i8042 output-buffer depth so a stuck controller
    // cannot hang boot.
    for (int i = 0; i < 16; ++i) {
        if ((inb(STATUS_PORT) & 0x01) == 0)
            break;
        inb(DATA_PORT);
        arch::pause();
    }
    outb(0x60, 0xF4);
    io_wait();
    // The 0xF4 enable command is ACKed with 0xFA; drain it so it is not
    // consumed as a fake scancode by the keyboard ISR.
    for (int i = 0; i < 4; ++i) {
        if ((inb(STATUS_PORT) & 0x01) == 0)
            break;
        inb(DATA_PORT);
        arch::pause();
    }
}

/// @brief Push a single character onto the input ring buffer.
/// @param c Character to enqueue.
/// @return true if the character was successfully enqueued, false if the buffer
/// was full.
bool Keyboard::push_ring(char c) {
    return ring_.try_push(c);
}

/// @brief IRQ handler for keyboard interrupts.
/// Reads the scancode from the PS/2 data port, updates modifier key states,
/// translates the scancode to an ASCII character, and pushes it onto the ring
/// buffer.
void Keyboard::handle_irq() {
    uint8_t status = inb(STATUS_PORT);
    if (!(status & 0x01))
        return;

    uint8_t scancode = inb(DATA_PORT);

    bool pressed = !(scancode & 0x80);
    uint8_t code = scancode & 0x7F;

    update_modifiers(scancode, pressed);

    if (!pressed)
        return;

    if (code == 0x1C) {
        push_ring('\n');
        return;
    }
    if (code == 0x0E) {
        push_ring('\b');
        return;
    }
    if (code == 0x0F) {
        push_ring('\t');
        return;
    }

    uint8_t m = atomic_mods();
    bool use_shift = ((m & MOD_SHIFT) != 0) != ((m & MOD_CAPS) != 0);
    char c = use_shift ? scancode_upper[code] : scancode_lower[code];
    if (c)
        push_ring(c);
}

/// @brief Dequeue a single character from the input ring buffer.
/// @param[out] c Reference that receives the retrieved character.
/// @return true if a character was available and dequeued, false if the buffer
/// was empty.
bool Keyboard::getchar(char &c) {
    return ring_.try_pop(c);
}

/// @brief Read up to @p len characters from the ring buffer into a
/// caller-supplied buffer.
/// @param[out] buf Destination buffer.
/// @param len Maximum number of characters to read.
/// @return The number of characters actually dequeued.
size_t Keyboard::read(char *buf, size_t len) {
    size_t count = 0;
    while (count < len && ring_.try_pop(buf[count]))
        ++count;
    return count;
}

/// @brief Discard all characters currently in the ring buffer.
void Keyboard::flush() {
    ring_.reset();
}

/// @brief Update the keyboard modifier key state based on a scancode.
/// @param scancode Raw scancode read from the PS/2 data port.
/// @param pressed true if the key was pressed, false if released.
void Keyboard::update_modifiers(uint8_t scancode, bool pressed) {
    uint8_t code = scancode & 0x7F;
    switch (code) {
    case 0x2A:
    case 0x36:
        if (pressed)
            __atomic_fetch_or(&mods_, MOD_SHIFT, __ATOMIC_RELEASE);
        else
            __atomic_fetch_and(&mods_, static_cast<uint8_t>(~MOD_SHIFT),
                               __ATOMIC_RELEASE);
        break;
    case 0x1D:
        if (pressed)
            __atomic_fetch_or(&mods_, MOD_CTRL, __ATOMIC_RELEASE);
        else
            __atomic_fetch_and(&mods_, static_cast<uint8_t>(~MOD_CTRL),
                               __ATOMIC_RELEASE);
        break;
    case 0x38:
        if (pressed)
            __atomic_fetch_or(&mods_, MOD_ALT, __ATOMIC_RELEASE);
        else
            __atomic_fetch_and(&mods_, static_cast<uint8_t>(~MOD_ALT),
                               __ATOMIC_RELEASE);
        break;
    case 0x3A:
        if (pressed) {
            uint8_t old = atomic_mods();
            __atomic_store_n(&mods_, old ^ MOD_CAPS, __ATOMIC_RELEASE);
        }
        break;
    default:
        break;
    }
}

} // namespace arch