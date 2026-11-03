#include <stdint.h>
#include <stdio.h>
#include <time.h>

#define BENCH_N 1000

int main(void) {
    struct timespec t0;
    struct timespec t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    for (int i = 0; i < BENCH_N; ++i) {
        struct timespec tp;
        clock_gettime(CLOCK_MONOTONIC, &tp);
    }
    clock_gettime(CLOCK_MONOTONIC, &t1);
    uint64_t ns0 = (uint64_t)t0.tv_sec * 1000000000ULL + (uint64_t)t0.tv_nsec;
    uint64_t ns1 = (uint64_t)t1.tv_sec * 1000000000ULL + (uint64_t)t1.tv_nsec;
    uint64_t per = (ns1 - ns0) / (uint64_t)(BENCH_N + 2);
    printf("S TMRBENCH op=gettime n=%d ns_per_call=%lu\n", BENCH_N,
           (unsigned long)per);

    struct timespec zero;
    zero.tv_sec = 0;
    zero.tv_nsec = 0;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    for (int i = 0; i < BENCH_N; ++i) {
        nanosleep(&zero, 0);
    }
    clock_gettime(CLOCK_MONOTONIC, &t1);
    ns0 = (uint64_t)t0.tv_sec * 1000000000ULL + (uint64_t)t0.tv_nsec;
    ns1 = (uint64_t)t1.tv_sec * 1000000000ULL + (uint64_t)t1.tv_nsec;
    per = (ns1 - ns0) / (uint64_t)(BENCH_N + 2);
    printf("S TMRBENCH op=nanosleep0 n=%d ns_per_call=%lu\n", BENCH_N,
           (unsigned long)per);
    return 0;
}
