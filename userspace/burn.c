/*
 * NexIOS RTOS — silent prime burner for monitor verification (issue #293)
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
 * burn — silent long-running prime crunch for monstat verification.
 *
 * Computes the first 500000 primes (trial division, same core as
 * prime.c) with ZERO output, then exits 0. Purpose: a live,
 * observable ring-3 CPU load so `monstat user` shows real exec/util
 * numbers while it runs, and real post-mortem totals after it exits.
 *
 * Intended invocation:
 *   loadelf burn.c.elf
 *   runelf            (returns immediately; the burner runs concurrently)
 *   monstat user      (live stats while burning, totals after exit)
 *
 * Safety notes (see userspace/user-app.c): all state in file-scope
 * statics (.bss) — never stack buffers near syscalls; no output at
 * all (not even a banner); no yields inside the compute loop (natural
 * preemption is what the monitor measures).
 */

#include <syscall.h>
#include <unistd.h>
#include <stddef.h>

#define BURN_PRIME_COUNT 500000UL

/* Small prime table speeds trial division; static .bss per discipline. */
#define BURN_TABLE_MAX 8192UL

static unsigned long g_table[BURN_TABLE_MAX];
static unsigned long g_table_len;
static unsigned long g_found;
static unsigned long g_candidate;

static int is_prime(unsigned long n) {
    unsigned long d;

    for (d = 0; d < g_table_len; ++d) {
        unsigned long p = g_table[d];

        if (p * p > n)
            break;
        if (n % p == 0)
            return 0;
    }
    return 1;
}

int main(void) {
    g_table[0] = 2;
    g_table[1] = 3;
    g_table_len = 2;
    g_found = 2;
    g_candidate = 5;

    while (g_found < BURN_PRIME_COUNT) {
        if (is_prime(g_candidate)) {
            if (g_table_len < BURN_TABLE_MAX)
                g_table[g_table_len++] = g_candidate;
            ++g_found;
        }
        g_candidate += 2;
    }

    _exit(0);
    return 0;
}
