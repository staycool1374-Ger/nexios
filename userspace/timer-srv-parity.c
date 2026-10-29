#include <stdint.h>
#include <stdio.h>
#include <time.h>

int main(void) {
    int seq = 0;
    struct timespec t1;
    struct timespec t2;
    int rc1 = clock_gettime(CLOCK_MONOTONIC, &t1);
    int rc2 = clock_gettime(CLOCK_MONOTONIC, &t2);
    printf("S TMR seq=%d op=gettime rc=%d\n", ++seq, rc1);
    printf("S TMR seq=%d op=gettime rc=%d\n", ++seq, rc2);
    int ordered = (rc1 == 0 && rc2 == 0 &&
                   (t2.tv_sec > t1.tv_sec ||
                    (t2.tv_sec == t1.tv_sec && t2.tv_nsec >= t1.tv_nsec)));
    printf("S TMR seq=%d op=ordered rc=%d\n", ++seq, ordered ? 0 : -1);

    struct timespec zero;
    zero.tv_sec = 0;
    zero.tv_nsec = 0;
    int rcz = nanosleep(&zero, 0);
    printf("S TMR seq=%d op=nanosleep0 rc=%d\n", ++seq, rcz);

    struct timespec rt;
    int rcr = clock_gettime(CLOCK_REALTIME, &rt);
    printf("S TMR seq=%d op=gettime_rt rc=%d\n", ++seq, rcr);
    return 0;
}
