/*
 * NexIOS RTOS — userspace prime benchmark (first 100000 primes)
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

/*
 * prime — bounded prime benchmark for NexIOS user space.
 *
 * Computes the first 100000 primes (trial division), prints every 1000th
 * prime, then reports:
 *   - wall-clock time (CLOCK_MONOTONIC, ms),
 *   - CPU time consumed by this task (SYS_TIMES exec_ns_total, ms),
 *   - scheduling stats: wall ticks elapsed, executed ticks, ticks spent
 *     descheduled (= wall - executed, i.e. preempted by timer/others),
 *     plus voluntary yield count (this program never yields mid-compute).
 *
 * Intended invocation:
 *   loadelf prime.c.elf
 *   runelf
 *
 * Safety notes (see userspace/user-app.c):
 *   - All syscall out-params live in file-scope statics (.bss, fully
 *     mapped and zero-filled by the loader) — never pass stack-local
 *     buffers to syscalls (the initial user stack is only partially
 *     mapped and the kernel copy path would fault).
 *   - All output goes through unbuffered write() (never stdio): each
 *     line hits the screen the moment it is produced, including the
 *     startup banner below.  No sys_yield() inside the compute loop:
 *     the benchmark must measure natural preemption, and each print is
 *     already a syscall boundary for signal delivery.
 */

#include <syscall.h>
#include <time.h>
#include <unistd.h>
#include <stddef.h>

/* Must match kernel struct TaskTimes (src/kernel/task/task.hpp) exactly:
 * four consecutive uint64_t fields. */
struct prime_task_times {
    unsigned long exec_ns_total;
    unsigned long exec_period_ns;
    unsigned long executed_ticks;
    unsigned long wcet_ticks;
};

#define PRIME_COUNT 100000UL
#define PRIME_REPORT_EVERY 1000UL

static struct timespec g_wall_start;
static struct timespec g_wall_end;
static struct prime_task_times g_cpu_start;
static struct prime_task_times g_cpu_end;
static char g_out[96];

static void emit_str(const char *s) {
    size_t len = 0;

    while (s[len])
        ++len;
    write(STDOUT_FILENO, s, len);
}

/* Unbuffered decimal emitter: "<prefix><num><suffix>\n" via one write(). */
static void emit_num_line(const char *prefix, unsigned long num,
                          const char *suffix) {
    size_t pos = 0;
    char rev[24];
    int r = 0;

    while (prefix[pos] && pos < sizeof(g_out) - 32) {
        g_out[pos] = prefix[pos];
        ++pos;
    }
    if (num == 0) {
        rev[r++] = '0';
    } else {
        while (num > 0 && r < 23) {
            rev[r++] = (char)('0' + (num % 10));
            num /= 10;
        }
    }
    while (r > 0 && pos < sizeof(g_out) - 2)
        g_out[pos++] = rev[--r];
    while (*suffix && pos < sizeof(g_out) - 2) {
        g_out[pos++] = *suffix++;
    }
    g_out[pos++] = '\n';
    write(STDOUT_FILENO, g_out, pos);
}

/* Unbuffered milestone emitter: "prime #<count> = <value>\n" in one
 * write() so the record never splits across lines. */
static void emit_prime_record(unsigned long count, unsigned long value) {
    size_t pos = 0;
    const char *prefix = "prime #";
    const char *mid = " = ";
    char rev[24];
    int r = 0;

    while (prefix[pos] && pos < sizeof(g_out) - 48) {
        g_out[pos] = prefix[pos];
        ++pos;
    }
    if (count == 0) {
        rev[r++] = '0';
    } else {
        while (count > 0 && r < 23) {
            rev[r++] = (char)('0' + (count % 10));
            count /= 10;
        }
    }
    while (r > 0 && pos < sizeof(g_out) - 32)
        g_out[pos++] = rev[--r];
    r = 0;
    while (mid[r] && pos < sizeof(g_out) - 24) {
        g_out[pos++] = mid[r++];
    }
    r = 0;
    if (value == 0) {
        rev[r++] = '0';
    } else {
        while (value > 0 && r < 23) {
            rev[r++] = (char)('0' + (value % 10));
            value /= 10;
        }
    }
    while (r > 0 && pos < sizeof(g_out) - 2)
        g_out[pos++] = rev[--r];
    g_out[pos++] = '\n';
    write(STDOUT_FILENO, g_out, pos);
}

/*
 * Trial-division primality test.  Overflow-safe bound: d <= n / d
 * (d * d <= n overflows at 64-bit for d > 2^32).
 */
static int is_prime(unsigned long n) {
    unsigned long d;

    if (n < 2)
        return 0;
    if (n % 2 == 0)
        return n == 2;
    for (d = 3; d <= n / d; d += 2) {
        if (n % d == 0)
            return 0;
    }
    return 1;
}

int main(void) {
    unsigned long found = 0;
    unsigned long n = 2;
    unsigned long last = 0;
    long wall_ticks_start = 0;
    long wall_ticks_end = 0;
    long times_ok_start = 0;
    long times_ok_end = 0;
    long wall_ms = 0;
    long cpu_ms = 0;
    unsigned long exec_ticks = 0;
    unsigned long wall_ticks = 0;
    unsigned long descheduled_ticks = 0;

    emit_str("prime: computing first 100000 primes...\n");

    clock_gettime(CLOCK_MONOTONIC, &g_wall_start);
    wall_ticks_start = __syscall5(SYS_GET_TICKS, 0, 0, 0, 0);
    times_ok_start = __syscall5(SYS_TIMES, 0, (long)&g_cpu_start, 0, 0);

    while (found < PRIME_COUNT) {
        if (is_prime(n)) {
            ++found;
            last = n;
            if (found % PRIME_REPORT_EVERY == 0)
                emit_prime_record(found, n);
        }
        ++n;
    }

    times_ok_end = __syscall5(SYS_TIMES, 0, (long)&g_cpu_end, 0, 0);
    wall_ticks_end = __syscall5(SYS_GET_TICKS, 0, 0, 0, 0);
    clock_gettime(CLOCK_MONOTONIC, &g_wall_end);

    wall_ms = (long)(g_wall_end.tv_sec - g_wall_start.tv_sec) * 1000L +
              (long)(g_wall_end.tv_nsec - g_wall_start.tv_nsec) / 1000000L;

    emit_num_line("primes computed: ", found, "");
    emit_num_line("last prime: ", last, "");
    emit_num_line("wall time ms: ", (unsigned long)wall_ms, "");

    if (times_ok_start == 0 && times_ok_end == 0) {
        cpu_ms =
            (long)(g_cpu_end.exec_ns_total - g_cpu_start.exec_ns_total) /
            1000000L;
        exec_ticks = g_cpu_end.executed_ticks - g_cpu_start.executed_ticks;
        emit_num_line("cpu time ms: ", (unsigned long)cpu_ms, "");
        emit_num_line("executed ticks: ", exec_ticks, "");
    } else {
        emit_str("cpu time: unavailable (SYS_TIMES failed)\n");
    }

    if (wall_ticks_end >= wall_ticks_start) {
        wall_ticks = (unsigned long)(wall_ticks_end - wall_ticks_start);
        emit_num_line("wall ticks: ", wall_ticks, "");
        if (times_ok_start == 0 && times_ok_end == 0) {
            descheduled_ticks =
                (wall_ticks > exec_ticks) ? (wall_ticks - exec_ticks) : 0;
            emit_num_line("descheduled ticks: ", descheduled_ticks, "");
        }
    }
    emit_str("voluntary yields: 0 (no sys_yield in compute loop)\n");

    return 0;
}
