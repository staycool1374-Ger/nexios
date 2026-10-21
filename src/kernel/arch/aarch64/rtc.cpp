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
/// @brief AArch64 RTC driver using the PL031 wall-clock RTC (QEMU virt).

#include <kernel/arch/rtc.hpp>
#include <kernel/arch/hal/io.hpp>
#include <kernel/log/dmesg.hpp>
#include <logger.hpp>
#include <constants.hpp>

using namespace kernel;

namespace arch {

// QEMU virt PL031 RTC (inside the boot-mapped 0x09000000 2 MiB Device block).
inline constexpr uint64_t PL031_BASE = 0x09010000ULL;
inline constexpr uint32_t PL031_DATA = 0x0000; ///< Current time (Unix secs)
inline constexpr uint32_t PL031_CR = 0x00C;    ///< Control register
inline constexpr uint32_t PL031_CR_RTCEN = 1u << 0;
inline constexpr uint32_t PL031_PERIPHID0 = 0xFE0;
inline constexpr uint32_t PL031_PERIPHID1 = 0xFE4;
inline constexpr uint32_t PL031_PERIPHID2 = 0xFE8;
inline constexpr uint32_t PL031_PERIPHID3 = 0xFEC;
inline constexpr uint32_t PL031_ID_MAGIC = 0x00041031; // "PL031" peripheral ID
// Peripheral-ID revision mask: PID2[7:4] is the revision nibble (QEMU
// reports rev 1 → 0x00141031); identity ignores it (live-proven).
inline constexpr uint32_t PL031_ID_MASK = 0xFF0FFFFFU;
inline constexpr uint64_t PL031_WINDOW_BASE = 0x09000000ULL;
inline constexpr uint64_t PL031_WINDOW_SIZE = 0x200000ULL;
inline constexpr uint64_t PL031_REGION_SIZE = 0x1000ULL;
static_assert(PL031_BASE + PL031_REGION_SIZE <=
              PL031_WINDOW_BASE + PL031_WINDOW_SIZE);

// Informational dmesg code (outside the catalog; the message carries
// the payload). Matches the virtio-mmio source-line precedent.
inline constexpr uint64_t kDmesgDriverRtcSource = 950;

/// @brief Runtime PL031 base (0 = uninitialized → PL031_BASE).
static uint64_t s_pl031_base = 0;

/// @brief 32-bit MMIO read through the HHDM alias.
static uint32_t pl031_read32(uint64_t phys, uint32_t reg) {
    // NOLINTNEXTLINE(performance-no-int-to-ptr)
    auto *ptr = reinterpret_cast<volatile uint32_t *>(arch::HHDM_OFFSET +
                                                      phys + reg);
    return *ptr;
}

/// @brief Probe the PL031 signature, start the counter, latch the base.
/// Idempotent: the first successful (or failed) probe wins; the base is
/// published once. Never fails closed into a panic — worst case the
/// counter reads while stopped (still monotonic from the reset value).
void RTC::init() {
    if (s_pl031_base != 0)
        return;
    uint64_t base = PL031_BASE;
    uint32_t ctrl = pl031_read32(base, PL031_CR);
    if ((ctrl & PL031_CR_RTCEN) == 0) {
        // NOLINTNEXTLINE(performance-no-int-to-ptr)
        auto *ctrl_reg = reinterpret_cast<volatile uint32_t *>(
            arch::HHDM_OFFSET + base + PL031_CR);
        *ctrl_reg = ctrl | PL031_CR_RTCEN;
        (void)pl031_read32(base, PL031_CR);
    }
    uint32_t id = pl031_read32(base, PL031_PERIPHID0) |
                  (pl031_read32(base, PL031_PERIPHID1) << 8) |
                  (pl031_read32(base, PL031_PERIPHID2) << 16) |
                  (pl031_read32(base, PL031_PERIPHID3) << 24);
    if ((id & PL031_ID_MASK) == (PL031_ID_MAGIC & PL031_ID_MASK)) {
        s_pl031_base = base;
        Logger::info("rtc: source=pl031 base=0x%lx",
                     static_cast<uint64_t>(base));
    } else {
        // Loud mismatch, fail-open on the architected address: the leaf
        // is boot-mapped by construction, so reads stay defined.
        s_pl031_base = base;
        Logger::info("rtc: PL031 ID mismatch 0x%x (want 0x%x), using base",
                     id, PL031_ID_MAGIC);
    }
    kernel::log::dmesg_push_sev(kernel::log::ErrorSubsystem::DRIVER,
                                kernel::log::kDmesgBase_DRIVER + 950,
                                kernel::log::LogSeverity::INFO,
                                "rtc source pl031", 0);
}

/// @brief PL031 is boot-mapped by construction (2 MiB Device leaf).
bool RTC::available() {
    return true;
}

const char *RTC::source_name() {
    return "pl031";
}

/// @brief Read the wall-clock time in seconds since the Unix epoch.
/// @return Seconds since 1970-01-01 (Unix epoch).
uint64_t RTC::read_seconds() {
    // PL031 RTCDR is one 32-bit seconds counter — no latch, no tear.
    uint64_t base = (s_pl031_base != 0) ? s_pl031_base : PL031_BASE;
    // NOLINTNEXTLINE(performance-no-int-to-ptr)
    auto *rtc = reinterpret_cast<volatile uint32_t *>(arch::HHDM_OFFSET +
                                                      base);
    return static_cast<uint64_t>(rtc[PL031_DATA / 4]);
}

/// @brief Read and decompose the current time into a tm structure.
/// @param[out] out Pointer to tm struct to fill. If null, returns immediately.
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
