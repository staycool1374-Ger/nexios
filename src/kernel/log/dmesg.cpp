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

/// @file dmesg.cpp
/// @brief DmesgService singleton — the private DmesgBuffer ring is defined
/// inline in dmesg.hpp (friend-tested, no explicit instantiation needed).

#include <kernel/log/dmesg.hpp>
#include <kernel/log/dmesg_catalog.hpp>
#include <kernel/arch/timer.hpp>
#include <kernel/task/scheduler.hpp>
#include <kernel/core/global_state.hpp>
#include <kernel/kernel.hpp>

namespace kernel::log {

/// @brief The one and only DmesgService (Meyers singleton). Defined here so
/// the buffer has no external linkage — all access flows through instance().
DmesgService &DmesgService::instance() noexcept {
    static DmesgService service{};
    return service;
}

uint64_t current_wall_ms() noexcept {
    const uint64_t epoch = kernel::gs::get_boot_epoch();
    if (epoch == 0) {
        return 0; // no RTC/boot-epoch yet — renderer uses tick count
    }
    return epoch * 1000ULL + arch::Timer::ticks();
}

namespace {

/// @brief Append a NUL-terminated string, truncated at end.
void append_str(char *&pos, char *end, const char *str) {
    while (*str && pos < end) {
        *pos++ = *str++;
    }
}

/// @brief Append a uint64 in decimal, truncated at end.
void append_dec(char *&pos, char *end, uint64_t val) {
    char tmp[24];
    size_t len = 0;
    if (val == 0) {
        tmp[len++] = '0';
    } else {
        while (val > 0 && len < sizeof(tmp)) {
            tmp[len++] = static_cast<char>('0' + (val % 10));
            val /= 10;
        }
    }
    while (len > 0 && pos < end) {
        *pos++ = tmp[--len];
    }
}

/// @brief Append a uintptr in lowercase hex without prefix, truncated.
void append_hex(char *&pos, char *end, uintptr_t val) {
    const size_t digits = sizeof(uintptr_t) * 2;
    for (size_t idx = 0; idx < digits && pos < end; ++idx) {
        const size_t shift = (digits - 1 - idx) * 4;
        const uint8_t nib = static_cast<uint8_t>((val >> shift) & 0xFU);
        *pos++ = static_cast<char>(nib < 10 ? '0' + nib : 'a' + (nib - 10));
    }
}

} // namespace

size_t format_dmesg_entry(char *out_buf, size_t out_cap,
                          const LogEntry &entry) noexcept {
    if (!out_buf || out_cap == 0) {
        return 0;
    }
    char *pos = out_buf;
    char *end = out_buf + out_cap - 1; // reserve NUL

    append_str(pos, end, "[DMESG ");
    // Issue #234 time rule: wall-clock datetime (incl. ms) when available,
    // else the boot-tick count in ms.
    if (entry.wall_ms != 0) {
        char stamp[24] = {};
        ::format_datetime(stamp, sizeof(stamp), entry.wall_ms * 1000000ULL);
        append_str(pos, end, stamp);
    } else {
        append_dec(pos, end, entry.timestamp);
        append_str(pos, end, "ms tick");
    }
    append_str(pos, end, "]: ");
    append_str(pos, end, severity_name(entry.severity));
    append_str(pos, end, " ");
    append_str(pos, end, subsystem_name(entry.subsystem));
    append_str(pos, end, " ");
    append_dec(pos, end, entry.error_code);
    append_str(pos, end, " ");
    append_str(pos, end, error_string(entry.subsystem, entry.error_code));
    append_str(pos, end, ": ");
    append_str(pos, end, entry.message); // owned array, never null
    append_str(pos, end, " [task=");
    append_dec(pos, end, entry.task_id);
    append_str(pos, end, " ctx=0x");
    append_hex(pos, end, entry.context);
    append_str(pos, end, "]\n");
    *pos = '\0';
    return static_cast<size_t>(pos - out_buf);
}

} // namespace kernel::log