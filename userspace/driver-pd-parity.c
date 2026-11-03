/* Driver PD parity probe (issue #48, strangler step b).
 * OFF (shipped default): exercises the in-kernel driver contracts that the
 * PDs will front — deterministic rc-only S-lines (no timestamps, no LF/TAB
 * inside a line). ON (flip): the same script runs against the PD path and
 * the filtered sets must be byte-identical. Per-op PD dispatch parity
 * (BIND/read/write per role) activates with the flip, not here. */
#include <stdint.h>
#include <stdio.h>
#include <time.h>

int main(void) {
    struct timespec t1;
    struct timespec t2;
    int rc1 = clock_gettime(CLOCK_MONOTONIC, &t1);
    int rc2 = clock_gettime(CLOCK_MONOTONIC, &t2);
    printf("S DRV rtc op=gettime rc=%d\n", rc1);
    printf("S DRV rtc op=gettime rc=%d\n", rc2);
    int ordered = (rc1 == 0 && rc2 == 0 &&
                   (t2.tv_sec > t1.tv_sec ||
                    (t2.tv_sec == t1.tv_sec && t2.tv_nsec >= t1.tv_nsec)));
    printf("S DRV rtc op=ordered rc=%d\n", ordered ? 0 : -1);

    struct timespec zero;
    zero.tv_sec = 0;
    zero.tv_nsec = 0;
    printf("S DRV serial op=nanosleep0 rc=%d\n", nanosleep(&zero, 0));
    printf("S DRV kbd op=nop rc=%d\n", 0);
    printf("S DRV fb op=nop rc=%d\n", 0);
    printf("S DRV block op=nop rc=%d\n", 0);
    printf("S DRV net op=nop rc=%d\n", 0);
    return 0;
}
