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

#if defined(CONFIG_GCOV_LINE)
// ---------------------------------------------------------------------------
// Phase A (issue #123) — real gcov line/branch coverage.
//
// With -fprofile-arcs every translation unit registers its profile data by
// calling __gcov_init() from a static constructor.  libgcov normally keeps
// that list private, so we supply our own __gcov_init() (the archive copy is
// then never pulled) and keep the gcov_info pointers in a bounded array.  At
// shutdown each info is serialised with __gcov_info_to_gcda() and written to
// COM1 as one framed record per translation unit; the host reconstructs the
// .gcda files, runs x86_64-elf-gcov and merges per-class runs.
//
// Wire format (see docs/specs/coverage.md):
// @code
//   "\n@@GCDABEGIN@@\n"
//   "GCOV:" <len> ":" <filename> "\n" <len bytes of gcda payload>
//   ... one record per translation unit ...
//   "STAT:" <tus> ":" <dropped> ":" <checksum> "\n"
//   "\n@@GCDAEND@@\n"
// @endcode
// A stalled UART emits "@@GCDAABORT@@" instead of the end sentinel so the
// host rejects the capture instead of trusting a truncated stream.
// ---------------------------------------------------------------------------

#include <gcov.h>

/// @brief Maximum number of instrumented translation units tracked.
static constexpr uint32_t MAX_GCOV_TUS = 1024;

/// @brief Bounded poll for TX-ready; a stuck UART must not hang shutdown.
static constexpr uint32_t GCDA_POLL_LIMIT = 2000000;

/// @brief Scratch buffer backing the allocate callback (no heap on RT paths).
static constexpr uint32_t GCDA_ALLOC_SIZE = 4096;

/// @brief Registered translation-unit profile objects.
static const struct gcov_info *gcov_infos[MAX_GCOV_TUS];
/// @brief Number of registered translation units (claimed atomically).
static uint32_t gcov_info_count = 0;
/// @brief Translation units rejected because the table was full (fail-closed).
static uint32_t gcov_info_dropped = 0;
/// @brief Constructor-table entries visited by gcov_run_ctors().
static uint32_t gcov_ctors_seen = 0;
/// @brief Constructor-table entries actually invoked (non-null).
static uint32_t gcov_ctors_invoked = 0;

/// @brief libgcov registration hook — we replace the archive's copy so the
/// per-TU info pointers stay reachable from the kernel.
/// @param info Profile object for one translation unit.
extern "C" void __attribute__((no_profile_instrument_function))
__gcov_init(struct gcov_info *info) { // NOLINT(bugprone-reserved-identifier)
    if (!info)
        return;
    uint32_t slot = __atomic_fetch_add(&gcov_info_count, 1, __ATOMIC_RELAXED);
    if (slot >= MAX_GCOV_TUS) {
        __atomic_fetch_add(&gcov_info_dropped, 1, __ATOMIC_RELAXED);
        return;
    }
    gcov_infos[slot] = info;
}

/// @brief Sentinel symbols around the collected constructor table (linker
/// script).  Only referenced by CONFIG_GCOV_LINE builds.
extern "C" {
extern void (*__ctors_start)();
extern void (*__ctors_end)();
}

/// @brief Run the static constructor table so every instrumented translation
/// unit registers its gcov profile.
///
/// The kernel deliberately does not run constructors in normal builds — all
/// global state is POD or explicitly initialised by <init>() functions — so
/// -fprofile-arcs' per-TU registration hook would otherwise never fire and the
/// profile dump would be empty.  Only the linker-collected table is walked,
/// bounded by its own sentinels.
extern "C" void __attribute__((no_profile_instrument_function))
gcov_run_ctors() {
    for (void (**entry)() = &__ctors_start; entry < &__ctors_end; ++entry) {
        ++gcov_ctors_seen;
        if (*entry) {
            ++gcov_ctors_invoked;
            (*entry)();
        }
    }
}

/// @brief libgcov teardown hook — each instrumented translation unit's static
/// destructor calls it.  In a hosted build this would write the .gcda through
/// the C library; here the profile is streamed explicitly by
/// gcov_line_dump_to_serial() before QEMU exits, so this is intentionally a
/// no-op (and keeps libgcov's copy out of the link).
extern "C" void __attribute__((no_profile_instrument_function))
__gcov_exit(void) { // NOLINT(bugprone-reserved-identifier)
}

namespace {

/// @brief Context threaded through the __gcov_info_to_gcda() callbacks.
struct GcdaContext {
    const char *name = nullptr;   ///< Filename captured from the name callback.
    uint32_t payload_length = 0;  ///< Bytes seen (count) or emitted (send).
    bool counting = false;        ///< True on the measuring pass.
    uint32_t *checksum = nullptr; ///< Optional accumulator over payload bytes.
    bool stalled = false;         ///< UART stopped accepting bytes.
};

/// @brief Emit one byte over COM1 with a bounded wait for TX-ready.
bool __attribute__((no_profile_instrument_function))
gcda_put(uint8_t byte) {
    for (uint32_t poll = 0; poll < GCDA_POLL_LIMIT; ++poll) {
        if ((arch::inb(arch::COM1 + 5) & 0x20) != 0) {
            arch::outb(arch::COM1, byte);
            return true;
        }
    }
    return false;
}

/// @brief Emit a NUL-terminated string over COM1.
bool __attribute__((no_profile_instrument_function))
gcda_puts(const char *text) {
    for (const char *cursor = text; *cursor != '\0'; ++cursor) {
        if (!gcda_put(static_cast<uint8_t>(*cursor)))
            return false;
    }
    return true;
}

/// @brief Emit an unsigned value in decimal over COM1.
bool __attribute__((no_profile_instrument_function))
gcda_put_dec(uint32_t value) {
    char digits[12];
    uint32_t pos = 0;
    if (value == 0) {
        digits[pos++] = '0';
    } else {
        while (value > 0) {
            digits[pos++] = static_cast<char>('0' + (value % 10));
            value /= 10;
        }
    }
    while (pos > 0) {
        if (!gcda_put(static_cast<uint8_t>(digits[--pos])))
            return false;
    }
    return true;
}

/// @brief gcov filename callback — captures the name, emits nothing.
void __attribute__((no_profile_instrument_function))
gcda_filename_cb(const char *filename, void *arg) {
    auto *ctx = static_cast<GcdaContext *>(arg);
    ctx->name = filename;
}

/// @brief gcov dump callback — counts on the measuring pass, emits otherwise.
void __attribute__((no_profile_instrument_function))
gcda_dump_cb(const void *data, unsigned length, void *arg) {
    auto *ctx = static_cast<GcdaContext *>(arg);
    const auto *bytes = static_cast<const uint8_t *>(data);
    for (unsigned i = 0; i < length; ++i) {
        if (ctx->counting) {
            ++ctx->payload_length;
            continue;
        }
        if (!gcda_put(bytes[i])) {
            ctx->stalled = true;
            return;
        }
        ++ctx->payload_length;
        if (ctx->checksum)
            *ctx->checksum += bytes[i];
    }
}

/// @brief gcov allocate callback — static scratch buffer, never the heap.
void *__attribute__((no_profile_instrument_function))
gcda_alloc_cb(unsigned length, void *) {
    static uint8_t scratch[GCDA_ALLOC_SIZE];
    static unsigned used = 0;
    if (used + length > sizeof(scratch))
        return nullptr;
    void *block = &scratch[used];
    used += static_cast<unsigned>(length);
    return block;
}

} // namespace

/// @brief Serialise every translation unit's gcda stream over COM1.
/// Two passes per unit: the first measures the payload so the frame can carry
/// its length, the second emits it.  __gcov_info_to_gcda() is deterministic,
/// so the measured length is authoritative — a mismatch aborts the capture.
extern "C" void __attribute__((no_profile_instrument_function))
gcov_line_dump_to_serial() {
    uint32_t count = __atomic_load_n(&gcov_info_count, __ATOMIC_RELAXED);
    if (count > MAX_GCOV_TUS)
        count = MAX_GCOV_TUS;

    uint32_t checksum = 0;
    bool ok = gcda_puts("\n@@GCDABEGIN@@\n");

    for (uint32_t idx = 0; ok && idx < count; ++idx) {
        const struct gcov_info *info = gcov_infos[idx];
        if (!info)
            continue;

        GcdaContext measure{};
        measure.counting = true;
        __gcov_info_to_gcda(info, gcda_filename_cb, gcda_dump_cb,
                            gcda_alloc_cb, &measure);
        if (!measure.name || measure.payload_length == 0)
            continue;

        const uint32_t declared = measure.payload_length;

        ok = gcda_puts("GCOV:");
        if (ok)
            ok = gcda_put_dec(declared);
        if (ok)
            ok = gcda_puts(":");
        if (ok)
            ok = gcda_puts(measure.name);
        if (ok)
            ok = gcda_puts("\n");
        if (!ok)
            break;

        GcdaContext send{};
        send.checksum = &checksum;
        __gcov_info_to_gcda(info, gcda_filename_cb, gcda_dump_cb,
                            gcda_alloc_cb, &send);

        if (send.stalled || send.payload_length != declared) {
            ok = false;
            break;
        }
    }

    if (ok)
        ok = gcda_puts("STAT:");
    if (ok)
        ok = gcda_put_dec(count);
    if (ok)
        ok = gcda_puts(":");
    if (ok)
        ok = gcda_put_dec(
            __atomic_load_n(&gcov_info_dropped, __ATOMIC_RELAXED));
    if (ok)
        ok = gcda_puts(":");
    if (ok)
        ok = gcda_put_dec(checksum);
    if (ok)
        ok = gcda_puts(":");
    if (ok)
        ok = gcda_put_dec(
            __atomic_load_n(&gcov_ctors_seen, __ATOMIC_RELAXED));
    if (ok)
        ok = gcda_puts(":");
    if (ok)
        ok = gcda_put_dec(
            __atomic_load_n(&gcov_ctors_invoked, __ATOMIC_RELAXED));
    if (ok)
        ok = gcda_puts("\n");

    if (ok)
        ok = gcda_puts("\n@@GCDAEND@@\n");
    if (!ok)
        (void)gcda_puts("\n@@GCDAABORT@@\n");
}

#endif // CONFIG_GCOV_LINE

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
