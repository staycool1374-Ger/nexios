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

/// @file gcov_handler.cpp
/// @brief Dynamic function-level coverage — GCC `-finstrument-functions`
/// entry hook plus the framed serial dump of the executed-function set.
///
/// Registration IS the coverage signal: a function appears in the dump if
/// and only if it was entered at least once.  The "not covered" set is
/// derived host-side from the ELF symbol universe
/// (tools/coverage_report.py), because a kernel cannot know which functions
/// exist in files it never executes.
///
/// Wire format (all little-endian) — see docs/specs/coverage.md:
/// @code
///   "\n@@COVBEGIN@@\n"
///   "FUNC" u32 count
///   count * (u64 addr, u8 called)      // called is always 1 (wire compat)
///   "STAT" u32 registered u32 dropped u32 checksum
///   "END!"
///   "\n@@COVEND@@\n"
/// @endcode
/// A capture is only valid when both sentinels and the checksum are intact;
/// a truncated dump emits "@@COVABORT@@" instead of the end sentinel.

#include <kernel/gcov/gcov_handler.hpp>
#include <kernel/arch/io.hpp>
#include <constants.hpp>

extern "C" {

#ifdef CONFIG_COVERAGE
#ifndef CONFIG_COVERAGE_MAX_FUNCS
/// @brief Default table capacity for the coverage build (overridable).
#define CONFIG_COVERAGE_MAX_FUNCS 16384
#endif
/// @brief Capacity of the executed-function table (coverage build).
static constexpr uint32_t MAX_FUNCS = CONFIG_COVERAGE_MAX_FUNCS;
#elif defined(CONFIG_PROFILING)
/// @brief Capacity of the executed-function table (instrumented profiling).
static constexpr uint32_t MAX_FUNCS = 4096;
#else
/// @brief No -finstrument-functions: the table has no producer, so it is kept
/// minimal to avoid growing the production BSS.
static constexpr uint32_t MAX_FUNCS = 256;
#endif

/// @brief Open-addressing slots — power of two, load factor <= 0.5.
static constexpr uint32_t HASH_SLOTS = MAX_FUNCS * 2;
/// @brief Sentinel for "function not registered" (table full / probe exhausted).
static constexpr uint32_t NO_INDEX = 0xFFFFFFFFu;
/// @brief Linear probe bound — keeps the hook O(1) and bounded.  At 50 % load
/// linear probing forms ~log2(n) clusters, so the bound must exceed that or a
/// healthy run would report spurious drops.
static constexpr uint32_t MAX_PROBE = 32;
/// @brief Bounded COM1 poll count; a stuck UART must not hang shutdown.
static constexpr uint32_t SERIAL_POLL_LIMIT = 2000000;

static_assert((HASH_SLOTS & (HASH_SLOTS - 1)) == 0,
              "HASH_SLOTS must be a power of two");

/// @brief Executed function addresses, indexed 0..func_count-1.
static uint64_t func_addrs[MAX_FUNCS];
/// @brief Slot -> index+1 map (0 == empty).
static uint32_t hash_slots[HASH_SLOTS];
/// @brief Number of registered functions (claimed atomically).
static uint32_t func_count = 0;
/// @brief Functions rejected because the table was full (fail-closed).
static uint32_t dropped_funcs = 0;

/// @brief Hash a function address to a slot index.
/// @param addr Function entry address (4-byte aligned, so >>4 keeps entropy).
/// @return Slot index in [0, HASH_SLOTS).
static uint32_t __attribute__((no_instrument_function))
slot_for(uint64_t addr) {
    uint32_t mixed = static_cast<uint32_t>(addr >> 4) * 2654435761u;
    uint32_t high = static_cast<uint32_t>(addr >> 32) * 40503u;
    return (mixed ^ high) & (HASH_SLOTS - 1);
}

/// @brief Register a function address, or return its existing index.
/// @param func_addr Function entry address.
/// @return Index in func_addrs, or NO_INDEX when the table is full or the
/// probe sequence is exhausted (never mis-attributes to index 0).
static uint32_t __attribute__((no_instrument_function))
find_or_add_func(uint64_t func_addr) {
    uint32_t slot = slot_for(func_addr);
    for (uint32_t probe = 0; probe < MAX_PROBE; ++probe) {
        uint32_t idx = (slot + probe) & (HASH_SLOTS - 1);
        uint32_t entry = hash_slots[idx];
        if (entry != 0) {
            if (func_addrs[entry - 1] == func_addr)
                return entry - 1;
            continue;
        }
        uint32_t next = __atomic_fetch_add(&func_count, 1, __ATOMIC_RELAXED);
        if (next >= MAX_FUNCS) {
            __atomic_fetch_add(&dropped_funcs, 1, __ATOMIC_RELAXED);
            return NO_INDEX;
        }
        func_addrs[next] = func_addr;
        hash_slots[idx] = next + 1;
        return next;
    }
    __atomic_fetch_add(&dropped_funcs, 1, __ATOMIC_RELAXED);
    return NO_INDEX;
}

/// @brief GCC instrumentation hook — called on every instrumented function entry.
/// @param func   Address of the entered function.
/// @param caller Address of the call site (unused).
void __attribute__((no_instrument_function)) __cyg_profile_func_enter(
    void *func, void *caller) { // NOLINT(bugprone-reserved-identifier,
                                // bugprone-easily-swappable-parameters)
    (void)caller;
    // A concurrent entry from IRQ context can only produce a duplicate slot
    // (each claimant gets its own index from the atomic counter); the host
    // unions addresses, so no sample is lost and no entry is corrupted.
    (void)find_or_add_func(reinterpret_cast<uint64_t>(func));
}

/// @brief GCC instrumentation hook — called on every function exit (no-op).
/// @param func   Address of the exited function (unused).
/// @param caller Address of the call site (unused).
void __attribute__((no_instrument_function))
__cyg_profile_func_exit(void *func,
                        void *caller) { // NOLINT(bugprone-reserved-identifier,
                                        // bugprone-easily-swappable-parameters)
    (void)func;
    (void)caller;
}

#if defined(CONFIG_ARCH_X86_64)

/// @brief Emit one byte over COM1 with a bounded wait for TX-ready.
/// @param byte Byte to transmit.
/// @return true when the byte was written, false when the UART stalled.
static bool __attribute__((no_instrument_function)) serial_put(uint8_t byte) {
    for (uint32_t poll = 0; poll < SERIAL_POLL_LIMIT; ++poll) {
        if ((arch::inb(arch::COM1 + 5) & 0x20) != 0) {
            arch::outb(arch::COM1, byte);
            return true;
        }
    }
    return false;
}

/// @brief Emit a NUL-terminated string over COM1.
/// @param text String to transmit.
/// @return false as soon as one byte could not be written.
static bool __attribute__((no_instrument_function))
serial_puts(const char *text) {
    for (const char *cursor = text; *cursor != '\0'; ++cursor) {
        if (!serial_put(static_cast<uint8_t>(*cursor)))
            return false;
    }
    return true;
}

/// @brief Emit a 32-bit value little-endian, folding it into @p checksum.
static bool __attribute__((no_instrument_function))
serial_put_u32(uint32_t value, uint32_t &checksum) {
    for (uint32_t shift = 0; shift < 32; shift += 8) {
        uint8_t byte = static_cast<uint8_t>((value >> shift) & 0xFFu);
        checksum += byte;
        if (!serial_put(byte))
            return false;
    }
    return true;
}

/// @brief Emit a 64-bit value little-endian, folding it into @p checksum.
static bool __attribute__((no_instrument_function))
serial_put_u64(uint64_t value, uint32_t &checksum) {
    for (uint32_t shift = 0; shift < 64; shift += 8) {
        uint8_t byte = static_cast<uint8_t>((value >> shift) & 0xFFu);
        checksum += byte;
        if (!serial_put(byte))
            return false;
    }
    return true;
}

/// @brief Serialise the executed-function set over COM1 (framed + checksummed).
/// Never blocks unbounded: a stalled UART emits the abort sentinel instead of
/// the end sentinel so the host rejects the capture.
void __attribute__((no_instrument_function)) gcov_flush_to_serial() {
    uint32_t count = __atomic_load_n(&func_count, __ATOMIC_RELAXED);
    if (count > MAX_FUNCS)
        count = MAX_FUNCS;

    uint32_t checksum = 0;
    bool ok = serial_puts("\n@@COVBEGIN@@\n");
    if (ok)
        ok = serial_puts("FUNC");
    if (ok)
        ok = serial_put_u32(count, checksum);
    for (uint32_t i = 0; ok && i < count; ++i) {
        ok = serial_put_u64(func_addrs[i], checksum);
        if (ok) {
            // Wire-compat "called" byte: registration already implies called.
            checksum += 1u;
            ok = serial_put(1u);
        }
    }
    if (ok)
        ok = serial_puts("STAT");
    if (ok) {
        uint32_t trailing = __atomic_load_n(&func_count, __ATOMIC_RELAXED);
        ok = serial_put_u32(trailing, checksum);
    }
    if (ok)
        ok = serial_put_u32(__atomic_load_n(&dropped_funcs, __ATOMIC_RELAXED),
                            checksum);
    if (ok)
        ok = serial_put_u32(checksum, checksum);
    if (ok)
        ok = serial_puts("END!");
    if (ok)
        ok = serial_puts("\n@@COVEND@@\n");
    if (!ok)
        (void)serial_puts("\n@@COVABORT@@\n");
}

#else // !CONFIG_ARCH_X86_64

void __attribute__((no_instrument_function)) gcov_flush_to_serial() {
    (void)func_addrs;
    (void)hash_slots;
    (void)func_count;
    (void)dropped_funcs;
}

#endif // CONFIG_ARCH_X86_64

} // extern "C"
