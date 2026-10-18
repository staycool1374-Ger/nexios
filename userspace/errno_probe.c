/* NexIOS in-tree errno proof probe (issue #338).
 * Hosted on the in-tree libc only (no sysroot headers): exercises the
 * thread-local errno cell end to end — crt0 publishes the block via
 * TLS_SET, __errno_location() reads TP+0, the errno macro round-trips.
 * A faulting TP (unregistered/unreadable) dies before printing, so a
 * clean exit with the exact marker line proves the whole chain.
 * stdout goes to serial via the WRITE path. */

#include <stdio.h>
#include <errno.h>
#include <string.h>

int main(void) {
    int e0 = errno; /* fresh .bss block: expect 0 */
    errno = 42;
    int e1 = errno; /* TP cell round-trip: expect 42 */
    errno = 0;
    int e2 = errno; /* write-back, no stickiness: expect 0 */
    printf("ERRNO_PROBE: %d %d %d\n", e0, e1, e2);

    /* ---- #339: in-tree strtok/strtok_r from Ring 3 ---- */
    {
        char a[] = "a,b;c";
        char *s1 = 0;
        char *t0 = strtok_r(a, ",;", &s1);
        char *t1 = strtok_r(0, ",;", &s1);
        char *t2 = strtok_r(0, ",;", &s1);
        char *t3 = strtok_r(0, ",;", &s1);
        int ok = (t0 && t1 && t2 && !t3 && strcmp(t0, "a") == 0 &&
                  strcmp(t1, "b") == 0 && strcmp(t2, "c") == 0)
                     ? 1
                     : 0;
        printf("STRTOK_PROBE: %d\n", ok);
    }
    return 0;
}
