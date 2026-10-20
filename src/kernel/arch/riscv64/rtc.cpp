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

/// @file rtc.cpp
/// @brief RISC-V64 RTC driver — Goldfish wall clock when the DTB provides
///        it (`google,goldfish-rtc`), mtime uptime fallback otherwise
///        (issue #355; supersedes the uptime-only driver for the
///        DTB-present case).

#include <kernel/arch/rtc.hpp>
#include <kernel/arch/hal/io.hpp>
#include <kernel/boot/dtb.hpp>
#include <kernel/log/dmesg.hpp>
#include <logger.hpp>

using namespace kernel;

namespace arch {

// QEMU virt Goldfish RTC (DTB `google,goldfish-rtc`, reg 0x101000/0x1000;
// proven via dumpdtb). Reached through the 4 KiB identity page at
// VA == PA (boot.S L2[0] → _boot_pt_l3_rtc, L3[0x101]).
inline constexpr uint64_t GOLDFISH_RTC_REGION_SIZE = 0x1000ULL;
inline constexpr uint32_t GOLDFISH_TIME_LOW = 0x00;
inline constexpr uint32_t GOLDFISH_TIME_HIGH = 0x04;
inline constexpr int GOLDFISH_LATCH_TRIES = 8;
// Plausibility floor (2001-09-09): rejects absent/zero/all-ones reads.
// Valid until the u32 nanosecond counter wraps (year 2106).
inline constexpr uint64_t GOLDFISH_MIN_PLAUSIBLE_NS =
    1000000000ULL * 1000000000ULL;
inline constexpr uint64_t MTIME_FREQ_HZ = 10000000; // QEMU virt default

/// @brief Discovered Goldfish base (0 = none). Published once at boot.
static uint64_t s_base = 0;
/// @brief Wall-clock availability latch (release/acquire with s_base).
static bool s_available = false;
/// @brief init() ran (idempotence guard).
static bool s_ready = false;
/// @brief Latch-exhaustion warning emitted (warn-once).
static bool s_latch_warned = false;

/// @brief Read uptime seconds (mtime ticks / frequency).
static uint64_t read_seconds_mtime() {
    uint64_t cnt{};
    asm volatile("csrr %0, time" : "=r"(cnt));
    return cnt / MTIME_FREQ_HZ;
}

/// @brief Read Goldfish TIME (nanoseconds) with HIGH/LOW/HIGH latch.
/// @return Nanoseconds, or 0 after bounded exhaustion (warn-once).
static uint64_t read_ns_goldfish(uint64_t base) {
    for (int attempt = 0; attempt < GOLDFISH_LATCH_TRIES; ++attempt) {
        const volatile void *hi_addr =
            reinterpret_cast<const volatile void *>(base + GOLDFISH_TIME_HIGH);
        const volatile void *lo_addr =
            reinterpret_cast<const volatile void *>(base + GOLDFISH_TIME_LOW);
        uint32_t hi_first = arch::mmio_read32(hi_addr);
        uint32_t lo = arch::mmio_read32(lo_addr);
        uint32_t hi_second = arch::mmio_read32(hi_addr);
        if (hi_first == hi_second)
            return (static_cast<uint64_t>(hi_first) << 32) | lo;
    }
    if (!__atomic_exchange_n(&s_latch_warned, true, __ATOMIC_ACQ_REL)) {
        Logger::warn("rtc: goldfish latch torn %d times, reading 0",
                     GOLDFISH_LATCH_TRIES);
    }
    return 0;
}

/// @brief Discover the Goldfish RTC from the DTB, validate, and probe.
/// Idempotent; never panics; fail-loud (one WARN) to mtime fallback.
void RTC::init() {
    if (s_ready)
        return;
    s_ready = true;
    const void *fdt = kernel::boot::dtb_from_boot_info();
    if (fdt) {
        int nodes[kernel::boot::DTB_MAX_MATCHES];
        int found = kernel::boot::dtb_find_compatible_nodes(
            fdt, "google,goldfish-rtc", nodes, kernel::boot::DTB_MAX_MATCHES);
        for (int idx = 0; idx < found; ++idx) {
            uint64_t base = 0;
            uint64_t size = 0;
            if (!kernel::boot::dtb_node_reg(fdt, nodes[idx], base, size))
                continue;
            // End-contained in the boot-mapped 4 KiB page (overflow-safe,
            // same form as the virtio-MMIO window check).
            if (base < 0x101000ULL ||
                base + GOLDFISH_RTC_REGION_SIZE > 0x101000ULL + 0x1000ULL)
                continue;
            if (size < GOLDFISH_RTC_REGION_SIZE)
                continue;
            if ((base % 4ULL) != 0)
                continue;
            uint64_t nanos = read_ns_goldfish(base);
            if (nanos < GOLDFISH_MIN_PLAUSIBLE_NS)
                continue;
            s_base = base;
            __atomic_store_n(&s_available, true, __ATOMIC_RELEASE);
            Logger::info("rtc: source=goldfish base=0x%lx",
                         static_cast<uint64_t>(base));
            kernel::log::dmesg_push_sev(
                kernel::log::ErrorSubsystem::DRIVER,
                kernel::log::kDmesgBase_DRIVER + 950,
                kernel::log::LogSeverity::INFO, "rtc source goldfish", 0);
            return;
        }
    }
    __atomic_store_n(&s_available, false, __ATOMIC_RELEASE);
    Logger::warn("rtc: goldfish-rtc not found/valid — wall clock "
                 "unavailable, mtime uptime fallback (epoch 1970)");
    kernel::log::dmesg_push_sev(kernel::log::ErrorSubsystem::DRIVER,
                                kernel::log::kDmesgBase_DRIVER + 950,
                                kernel::log::LogSeverity::WARN,
                                "rtc mtime fallback", 0);
}

bool RTC::available() {
    return __atomic_load_n(&s_available, __ATOMIC_ACQUIRE);
}

const char *RTC::source_name() {
    return available() ? "goldfish" : "mtime-fallback";
}

/// @brief Read seconds: Goldfish wall clock when available, else mtime
///        uptime rendered from the 1970 epoch (documented fallback).
uint64_t RTC::read_seconds() {
    if (available()) {
        uint64_t base = s_base;
        if (base != 0)
            return read_ns_goldfish(base) / 1000000000ULL;
    }
    return read_seconds_mtime();
}

/// @brief Populate a tm structure (goldfish or mtime seconds).
/// @param[out] out Pointer to a tm struct to fill (no-op if nullptr).
void RTC::read_time(tm *out) {
    if (!out)
        return;
    uint64_t secs = read_seconds();

    uint64_t days = secs / 86400;
    uint64_t rem = secs % 86400;

    out->tm_hour = rem / 3600;
    rem %= 3600;
    out->tm_min = rem / 60;
    out->tm_sec = rem % 60;

    int y = 1970;
    while (days >= 365) {
        bool leap = (y % 4 == 0 && (y % 100 != 0 || y % 400 == 0));
        days -= leap ? 366 : 365;
        ++y;
    }
    out->tm_year = y - 1900;

    static const uint8_t mdays[12] = {31, 28, 31, 30, 31, 30,
                                      31, 31, 30, 31, 30, 31};
    bool leap = (y % 4 == 0 && (y % 100 != 0 || y % 400 == 0));
    int m = 0;
    for (; m < 12; ++m) {
        uint8_t d = mdays[m];
        if (m == 1 && leap)
            d = 29;
        if (days < d)
            break;
        days -= d;
    }
    out->tm_mon = m;
    out->tm_mday = days + 1;
    out->tm_wday = 0;
    out->tm_yday = 0;
    out->tm_isdst = 0;
}

} // namespace arch
