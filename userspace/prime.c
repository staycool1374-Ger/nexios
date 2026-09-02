/*
 * NexIOS RTOS — userspace prime generator
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
 * prime — outputs prime numbers forever, one per line, until terminated
 * (Ctrl+C / SIGINT kills it via the kernel default action; SIG_DFL for
 * SIGINT is TERMINATE).
 *
 * Intended invocation (once the "running" roadmap topic lands):
 *   runelf prime.c.elf > prime_output.txt &
 *
 * Safety notes (see docs/specs):
 *   - Output buffer is a file-scope static in .bss, which the ELF loader
 *     maps and zero-fills in full — do NOT use a stack-local buffer (the
 *     initial user stack is only partially mapped; see userspace/user-app.c).
 *   - The loop calls sys_yield() after each printed prime: it keeps the
 *     shell responsive AND creates a syscall boundary so a pending SIGINT
 *     is delivered (signals are processed after the syscall returns).
 */

#include <syscall.h>
#include <unistd.h>
#include <stdio.h>
#include <stddef.h>

/*
 * Trial-division primality test.  Overflow-safe bound: d <= n / d
 * (d * d <= n overflows at 64-bit for d > 2^32).
 */
static int is_prime(unsigned long n) {
    if (n < 2)
        return 0;
    if (n % 2 == 0)
        return n == 2;
    for (unsigned long d = 3; d <= n / d; d += 2) {
        if (n % d == 0)
            return 0;
    }
    return 1;
}

int main(void) {
    static char outbuf[32];
    unsigned long n = 2;

    for (;;) {
        if (is_prime(n)) {
            int len = snprintf(outbuf, sizeof(outbuf), "%lu\n", n);
            if (len > 0)
                write(STDOUT_FILENO, outbuf, (size_t)len);
            /* Yield after each prime: CPU-friendly + a syscall boundary
             * for signal delivery (SIGINT). */
            sys_yield();
        }
        ++n;
    }
}
