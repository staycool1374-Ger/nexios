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

#include <types.hpp>

// All functions in this file are trap/stub handlers.  The intentional infinite
// loops are landing pads for undefined behaviour — if reached the system is
// already in an unrecoverable state.  Suppress the analyzer's infinite-loop
// diagnostic so -fanalyzer in release builds does not false-positive here.
// -Wanalyzer-infinite-loop only exists in GCC 14+; older GCC (e.g. the
// ubuntu-24.04 CI default, GCC 13) rejects the option as unknown.
#if defined(__GNUC__) && !defined(__clang__) && __GNUC__ >= 14
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wanalyzer-infinite-loop"
#endif

extern "C" int __cxa_guard_acquire(uint64_t* g) {
    if (*g == 0) return 1;
    return 0;
}

extern "C" void __cxa_guard_release(uint64_t* g) {
    *g = 1;
}

extern "C" void __cxa_guard_abort(uint64_t* g) {
    (void)g;
}

extern "C" void __cxa_pure_virtual() {
    while (1) {}
}

extern "C" void __stack_chk_fail() {
    while (1) {}
}

extern "C" void _Unwind_Resume() {
    while (1) {}
}

extern "C" void __gxx_personality_v0() {
}

static void div128_trap() {
    while (1) {}
}

extern "C" void __udivti3() { div128_trap(); }
extern "C" void __umodti3() { div128_trap(); }
extern "C" void __divti3() { div128_trap(); }
extern "C" void __modti3() { div128_trap(); }

extern "C" void __chkstk() {
}

extern "C" void __chkstk_ms() {
}

extern "C" void* memset(void* s, int c, size_t n) {
    unsigned char* p = static_cast<unsigned char*>(s);
    while (n--) *p++ = static_cast<unsigned char>(c);
    return s;
}

extern "C" void* memcpy(void* dest, const void* src, size_t n) {
    unsigned char* d = static_cast<unsigned char*>(dest);
    const unsigned char* s = static_cast<const unsigned char*>(src);
    while (n--) *d++ = *s++;
    return dest;
}

extern "C" void* memmove(void* dest, const void* src, size_t n) {
    unsigned char* d = static_cast<unsigned char*>(dest);
    const unsigned char* s = static_cast<const unsigned char*>(src);
    if (d < s) {
        while (n--) *d++ = *s++;
    } else {
        d += n;
        s += n;
        while (n--) *--d = *--s;
    }
    return dest;
}

#if defined(__GNUC__) && !defined(__clang__) && __GNUC__ >= 14
#pragma GCC diagnostic pop
#endif
