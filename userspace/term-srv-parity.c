#include <stdint.h>
#include <stdio.h>
#include <time.h>

int main(void) {
    struct timespec t1;
    struct timespec t2;
    int rc1 = clock_gettime(CLOCK_MONOTONIC, &t1);
    int rc2 = clock_gettime(CLOCK_MONOTONIC, &t2);
    printf("S TM seq=%d op=gettime rc=%d\n", 0, rc1);
    printf("S TM seq=%d op=gettime rc=%d\n", 1, rc2);
    int ordered = (rc1 == 0 && rc2 == 0 &&
                   (t2.tv_sec > t1.tv_sec ||
                    (t2.tv_sec == t1.tv_sec && t2.tv_nsec >= t1.tv_nsec)));
    printf("S TM seq=%d op=ordered rc=%d\n", 2, ordered ? 0 : -1);
    struct timespec zero;
    zero.tv_sec = 0;
    zero.tv_nsec = 0;
    printf("S TM seq=%d op=nanosleep0 rc=%d\n", 3, nanosleep(&zero, 0));
    printf("S TM seq=%d op=done rc=%d\n", 4, 0);
    return 0;
}
