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
/// @brief RISC-V64 RTC driver — uptime clock via the mtime CSR (NOT a wall
///        clock: seconds since boot rendered from a 1970 epoch; a
///        goldfish-rtc wall-clock driver is follow-up work).

#include <kernel/arch/rtc.hpp>
#include <kernel/arch/hal/io.hpp>

namespace arch {

/// @brief Read uptime seconds (mtime ticks / frequency) rendered from a
///        1970 epoch for tm conversion.  NOT wall-clock time.
/// @return Seconds since boot (epoch 1970-01-01 rendering).
/// @note Uses the QEMU virt default mtime frequency of 10 MHz.
uint64_t RTC::read_seconds() {
    uint64_t cnt{};
    asm volatile("csrr %0, time" : "=r"(cnt));
    uint64_t freq = 10000000; // QEMU virt default mtime frequency
    if (freq == 0)
        return 0;
    return cnt / freq;
}

/// @brief Populate a tm structure from the mtime CSR.
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
