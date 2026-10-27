// Issue #171 P2: 4-arg round-trip probe for the generated syscall stubs.
// Exercises nexios_sys_* (src/libc/gen/) end to end: ABI_VERSION exact
// value, TIMER_CREATE fail-closed NULL out-pointer (EFAULT through the
// generated stub), CREATE + GETTIME + DELETE with a live timer id
// (all four argument positions observed by the kernel), invalid-op
// EINVAL with four distinct nonzero args (number + op routing proof),
// and GETPID determinism. Exit 0 = all pass; otherwise the failing
// check number. Creates no residue: every created timer is deleted
// before return (best-effort DELETE, retried once, on failure paths).

#include "gen/nexios_syscalls.h"

#include <errno.h>

// EFAULT: kernel kPosixErrFault (posix_time.hpp). No EFAULT in libc
// errno.h, so pinned here with the debugd.c NEG_EFAULT precedent.
#define PROBE_EFAULT 14
#define PROBE_EINVAL 22
// Clock id 1 (MONOTONIC), matching the test_posix_time timer_create use.
#define PROBE_CLOCK_MONOTONIC 1
#define PROBE_ABI_EXPECTED 0x10000
#define PROBE_SENTINEL 0xA5A5A5A5A5A5A5A5ULL

int main(void) {
    if (nexios_sys_abi_version(0, 0, 0, 0) != PROBE_ABI_EXPECTED) {
        return 1;
    }
    errno = 0;
    long bad_create = nexios_sys_timer_create(
        0, PROBE_CLOCK_MONOTONIC, 0, 0);
    if (bad_create != -1 || errno != PROBE_EFAULT) {
        return 2;
    }
    unsigned long timer_id = PROBE_SENTINEL;
    long created = nexios_sys_timer_create(
        0, PROBE_CLOCK_MONOTONIC, (long)&timer_id, 0);
    if (created != 0 || timer_id == PROBE_SENTINEL) {
        return 3;
    }
    // GETTIME writes a 32-byte AbiItimerspec: the out-buffer must
    // match, an 8-byte slot would overflow the stack by 24 bytes.
    long scratch[4];
    long gettime_ok = nexios_sys_timer_create(
        2, (long)timer_id, (long)scratch, 0);
    long deleted = nexios_sys_timer_create(3, (long)timer_id, 0, 0);
    if (deleted != 0) {
        // Best-effort re-issue: a failed DELETE leaves the timer alive.
        deleted = nexios_sys_timer_create(3, (long)timer_id, 0, 0);
    }
    if (gettime_ok != 0) {
        return 4;
    }
    if (deleted != 0) {
        return 5;
    }
    errno = 0;
    long invalid = nexios_sys_timer_create(99, 0x1111, 0x2222, 0x3333);
    if (invalid != -1 || errno != PROBE_EINVAL) {
        return 6;
    }
    long first_pid = nexios_sys_getpid(0, 0, 0, 0);
    long second_pid = nexios_sys_getpid(0, 0, 0, 0);
    if (first_pid != second_pid) {
        return 7;
    }
    return 0;
}
