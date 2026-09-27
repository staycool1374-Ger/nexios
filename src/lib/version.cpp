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

#include <version.hpp>

namespace kernel {

static const char* const build_date_ = __DATE__;
static const char* const build_time_ = __TIME__;

static char full_buf_[64];

const char* Version::string() {
    return KERNEL_VERSION_STRING;
}

#define NEXIOS_STR_INNER(x) #x
#define NEXIOS_STR(x) NEXIOS_STR_INNER(x)

const char* Version::picolibc_string() {
#ifdef PICOLIBC_VERSION_NUM
    return NEXIOS_STR(PICOLIBC_VERSION_NUM);
#else
    return "unknown";
#endif
}

const char* Version::build_date() {
    return build_date_;
}

const char* Version::build_time() {
    return build_time_;
}

const char* Version::full_string() {
    int pos = 0;
    auto append = [&](const char* s) {
        while (*s && pos < 63) full_buf_[pos++] = *s++;
    };

    append("NexIOS RTOS ");
    append(KERNEL_VERSION_STRING);
    append(" (");
    append(build_date_);
    append(" ");
    append(build_time_);
    append(")");
    full_buf_[pos] = '\0';
    return full_buf_;
}

} // namespace kernel