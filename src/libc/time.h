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

#pragma once

#ifndef __LIBK_TIME_H
#define __LIBK_TIME_H

#include <sys/types.h>

#define CLOCKS_PER_SEC 1000000

// Issue #76 — POSIX clock IDs (match kernel PosixClock).
#define CLOCK_REALTIME  0
#define CLOCK_MONOTONIC 1

struct tm {
    int tm_sec;     // seconds [0,60]
    int tm_min;     // minutes [0,59]
    int tm_hour;    // hours [0,23]
    int tm_mday;    // day of month [1,31]
    int tm_mon;     // month [0,11]
    int tm_year;    // years since 1900
    int tm_wday;    // day of week [0,6] (Sunday = 0)
    int tm_yday;    // day of year [0,365]
    int tm_isdst;   // DST flag
};

struct timeval {
    time_t tv_sec;     // seconds
    suseconds_t tv_usec; // microseconds
};

struct timespec {
    time_t tv_sec;   // seconds
    long tv_nsec;    // nanoseconds
};

struct itimerspec {
    struct timespec it_interval; // period (0 = one-shot)
    struct timespec it_value;    // initial expiry (0 = disarm)
};

int clock_gettime(int clockid, struct timespec* tp);

time_t time(time_t* tloc);
int gettimeofday(struct timeval* tv, void* tz);
int nanosleep(const struct timespec* req, struct timespec* rem);
unsigned int sleep(unsigned int seconds);

#endif // __LIBK_TIME_H