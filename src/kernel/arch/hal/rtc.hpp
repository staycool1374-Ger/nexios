#pragma once

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

/// @file rtc.hpp
/// @brief Real-Time Clock (CMOS RTC on x86_64, platform RTC on AArch64/RISC-V).

#include <types.hpp>
#include <kernel/nexios_config.h>

namespace arch {

/// @cond
#if defined(CONFIG_ARCH_X86_64)
/// @endcond

/// @brief Broken-down calendar time (compatible with struct tm).
struct tm {
    int tm_sec;   ///< Seconds (0–59).
    int tm_min;   ///< Minutes (0–59).
    int tm_hour;  ///< Hours (0–23).
    int tm_mday;  ///< Day of month (1–31).
    int tm_mon;   ///< Month (0–11).
    int tm_year;  ///< Years since 1900.
    int tm_wday;  ///< Day of week (0 = Sunday).
    int tm_yday;  ///< Day of year (0–365).
    int tm_isdst; ///< Daylight-saving flag.
};

/// @brief x86-64 CMOS RTC driver.
class RTC {
  public:
    /// @brief Initialise the RTC (enable periodic interrupt if needed).
    static void init();
    /// @brief Read the current UNIX timestamp (seconds since epoch).
    /// @return Seconds since 1970-01-01 00:00:00.
    static uint64_t read_seconds();
    /// @brief Read the current time into a broken-down tm structure.
    /// @param[out] out Destination tm struct.
    static void read_time(tm *out);

    /// @brief Convert a BCD byte to binary.
    /// @param bcd BCD-encoded value.
    /// @return Binary value.
    static constexpr uint8_t bcd_to_bin(uint8_t bcd) {
        return (bcd & 0x0F) + ((bcd >> 4) * 10);
    }
    /// @brief Convert a binary value to BCD.
    /// @param bin Binary value.
    /// @return BCD-encoded value.
    static constexpr uint8_t bin_to_bcd(uint8_t bin) {
        return ((bin / 10) << 4) | (bin % 10);
    }

    /// @brief CMOS index port (0x70).
    static constexpr uint16_t CMOS_INDEX = 0x70;
    /// @brief CMOS data port (0x71).
    static constexpr uint16_t CMOS_DATA = 0x71;

    /// @brief CMOS register addresses.
    enum Register : uint8_t {
        REG_SECONDS = 0x00,
        REG_MINUTES = 0x02,
        REG_HOURS = 0x04,
        REG_DAY_OF_WEEK = 0x06,
        REG_DAY_OF_MONTH = 0x07,
        REG_MONTH = 0x08,
        REG_YEAR = 0x09,
        REG_CENTURY = 0x32,
        REG_STATUS_A = 0x0A,
        REG_STATUS_B = 0x0B,
        REG_STATUS_C = 0x0C,
    };

  private:
    /// @brief Read a CMOS register.
    static uint8_t read_register(Register reg);
    /// @brief Write a CMOS register.
    static void write_register(Register reg, uint8_t value);
    /// @brief Wait for an RTC update to complete.
    static void wait_for_update();
    /// @brief Read raw time components from CMOS.
    static bool read_time_raw(uint8_t &sec, uint8_t &min, uint8_t &hour,
                              uint8_t &day, uint8_t &month, uint16_t &year);
    /// @brief Convert date/time components to a UNIX timestamp.
    static uint64_t make_timestamp(uint16_t year, uint8_t month, uint8_t day,
                                   uint8_t hour, uint8_t min, uint8_t sec);
};

/// @cond
#elif defined(CONFIG_ARCH_AARCH64) || defined(CONFIG_ARCH_RISCV64)
/// @endcond

/// @brief Broken-down calendar time (compatible with struct tm).
struct tm {
    int tm_sec;
    int tm_min;
    int tm_hour;
    int tm_mday;
    int tm_mon;
    int tm_year;
    int tm_wday;
    int tm_yday;
    int tm_isdst;
};

/// @brief Platform RTC driver for AArch64/RISC-V.
class RTC {
  public:
    static void init() {
    }
    static uint64_t read_seconds();
    static void read_time(tm *out);
};

/// @cond
#else
#error "HAL: no rtc implementation for this architecture"
#endif
/// @endcond

} // namespace arch
