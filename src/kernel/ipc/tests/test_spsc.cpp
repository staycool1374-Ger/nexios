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

/// @file test_spsc.cpp
/// @brief SPSC (Single-Producer Single-Consumer) queue tests.

#include <test.hpp>
#include <logger.hpp>
#include <kernel/sync/spsc_ring.hpp>

using namespace kernel;

// Runmode: kernel
// Testidea: Push then pop a single item from the ring.
// Input: SPSCRing<char, 4> initialized, push 'A'
// Expect: pop returns 'A', buffer empty after
// Depends: SPSCRing
JARVIS_TEST(spsc_push_pop_single, "PRE: none | POST: none") {
    SPSCRing<char, 4> ring;
    JARVIS_ASSERT(ring.try_push('A'));
    JARVIS_ASSERT(ring.empty() == false);
    char c = 0;
    JARVIS_ASSERT(ring.try_pop(c));
    JARVIS_ASSERT(c == 'A');
    JARVIS_ASSERT(ring.empty());
}

// Runmode: kernel
// Testidea: Fill the buffer, verify it reports full.
// Input: SPSCRing<char, 4> initialized, push 3 items
// Expect: 4th push fails (ring has capacity N-1 = 3)
// Depends: SPSCRing
JARVIS_TEST(spsc_buffer_full, "PRE: none | POST: none") {
    SPSCRing<char, 4> ring;
    JARVIS_ASSERT(ring.try_push('a'));
    JARVIS_ASSERT(ring.try_push('b'));
    JARVIS_ASSERT(ring.try_push('c'));
    JARVIS_ASSERT(ring.try_push('d') == false);
}

// Runmode: kernel
// Testidea: Pop from empty buffer returns false.
// Input: SPSCRing<char, 4> initialized empty
// Expect: try_pop returns false
// Depends: SPSCRing
JARVIS_TEST(spsc_pop_empty, "PRE: none | POST: none") {
    SPSCRing<char, 4> ring;
    char c;
    JARVIS_ASSERT(ring.try_pop(c) == false);
}

// Runmode: kernel
// Testidea: Push items then pop them back in FIFO order.
// Input: Push 'x', 'y', 'z'
// Expect: Pop 'x', 'y', 'z' in order, then empty
// Depends: SPSCRing
JARVIS_TEST(spsc_fifo_order, "PRE: none | POST: none") {
    SPSCRing<char, 4> ring;
    JARVIS_ASSERT(ring.try_push('x'));
    JARVIS_ASSERT(ring.try_push('y'));
    JARVIS_ASSERT(ring.try_push('z'));
    char c;
    JARVIS_ASSERT(ring.try_pop(c) && c == 'x');
    JARVIS_ASSERT(ring.try_pop(c) && c == 'y');
    JARVIS_ASSERT(ring.try_pop(c) && c == 'z');
    JARVIS_ASSERT(ring.empty());
}

// Runmode: kernel
// Testidea: Wrap-around (head wraps past end of array).
// Input: Fill and drain so head wraps around
// Expect: Correct FIFO ordering after wrap
// Depends: SPSCRing
JARVIS_TEST(spsc_wrap_around, "PRE: none | POST: none") {
    SPSCRing<char, 4> ring;
    char c;
    // Fill to capacity (3 items)
    JARVIS_ASSERT(ring.try_push('a'));
    JARVIS_ASSERT(ring.try_push('b'));
    JARVIS_ASSERT(ring.try_push('c'));
    // Drain
    JARVIS_ASSERT(ring.try_pop(c) && c == 'a');
    JARVIS_ASSERT(ring.try_pop(c) && c == 'b');
    // head = 3, tail = 2 — next push wraps head to 0
    JARVIS_ASSERT(ring.try_push('d'));
    JARVIS_ASSERT(ring.try_push('e'));
    // Read remaining
    JARVIS_ASSERT(ring.try_pop(c) && c == 'c');
    JARVIS_ASSERT(ring.try_pop(c) && c == 'd');
    JARVIS_ASSERT(ring.try_pop(c) && c == 'e');
    JARVIS_ASSERT(ring.empty());
}

// Runmode: kernel
// Testidea: Reset restores empty state.
// Input: Push items, reset
// Expect: empty() returns true, pop fails
// Depends: SPSCRing
JARVIS_TEST(spsc_reset, "PRE: none | POST: none") {
    SPSCRing<char, 4> ring;
    JARVIS_ASSERT(ring.try_push('a'));
    JARVIS_ASSERT(ring.try_push('b'));
    ring.reset();
    JARVIS_ASSERT(ring.empty());
    char c;
    JARVIS_ASSERT(ring.try_pop(c) == false);
}

// Runmode: kernel
// Testidea: Integer type in SPSC ring.
// Input: SPSCRing<uint64_t, 4>
// Expect: Push/pop works correctly for non-char types
// Depends: SPSCRing
JARVIS_TEST(spsc_integer_type, "PRE: none | POST: none") {
    SPSCRing<uint64_t, 4> ring;
    JARVIS_ASSERT(ring.try_push(42));
    JARVIS_ASSERT(ring.try_push(99));
    uint64_t v;
    JARVIS_ASSERT(ring.try_pop(v) && v == 42);
    JARVIS_ASSERT(ring.try_pop(v) && v == 99);
}

// Runmode: kernel
// Testidea: Large power-of-2 buffer (1024) still works.
// Input: Push 1023 items
// Expect: All pushes succeed, all pops return correct values
// Depends: SPSCRing
JARVIS_TEST(spsc_large_buffer, "PRE: none | POST: none") {
    SPSCRing<size_t, 1024> ring;
    for (size_t i = 0; i < 1023; ++i)
        JARVIS_ASSERT(ring.try_push(i));
    for (size_t i = 0; i < 1023; ++i) {
        size_t v;
        JARVIS_ASSERT(ring.try_pop(v) && v == i);
    }
    JARVIS_ASSERT(ring.empty());
}

void register_spsc_tests() {
    Logger::info("Registering SPSC ring tests");
    JARVIS_REGISTER_TEST(spsc_push_pop_single);
    JARVIS_REGISTER_TEST(spsc_buffer_full);
    JARVIS_REGISTER_TEST(spsc_pop_empty);
    JARVIS_REGISTER_TEST(spsc_fifo_order);
    JARVIS_REGISTER_TEST(spsc_wrap_around);
    JARVIS_REGISTER_TEST(spsc_reset);
    JARVIS_REGISTER_TEST(spsc_integer_type);
    JARVIS_REGISTER_TEST(spsc_large_buffer);
}
