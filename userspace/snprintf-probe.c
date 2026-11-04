/*
 * NexIOS RTOS — snprintf regression probe (issue #444).
 * Exercises the in-tree vsnprintf fix live: NULL,0 would-be length (was
 * SIGSEGV), truncation bounds, width parsed-not-applied, %s NULL, and the
 * printf 255-byte clamp (300-char line, counted in the serial capture).
 * Markers ("SNPRINTF_VERIFY: ...") are parsed by the testbed class.
 */

#include <stdio.h>

int main(void) {
    int fails = 0;

    int r0 = snprintf(0, 0, "hello");
    printf("SNPRINTF_VERIFY: null0 %d\n", r0);
    if (r0 != 5) {
        printf("SNPRINTF_VERIFY: null0 FAIL\n");
        ++fails;
    }

    char tb[4];
    int rt = snprintf(tb, sizeof(tb), "hello world");
    int tl = 0;
    while (tb[tl] != '\0' && tl < 4)
        ++tl;
    printf("SNPRINTF_VERIFY: trunc %d %d %s\n", rt, tl, tb);
    if (rt != 11 || tl != 3) {
        printf("SNPRINTF_VERIFY: trunc FAIL\n");
        ++fails;
    }

    char wb[64];
    int rw = snprintf(wb, sizeof(wb), "%5d|%03x", 42, 7);
    printf("SNPRINTF_VERIFY: width %d %s\n", rw, wb);
    if (rw != 4) {
        printf("SNPRINTF_VERIFY: width FAIL\n");
        ++fails;
    }

    char nb[64];
    int rn = snprintf(nb, sizeof(nb), "%s", (const char *)0);
    printf("SNPRINTF_VERIFY: nullstr %d %s\n", rn, nb);
    if (rn != 6) {
        printf("SNPRINTF_VERIFY: nullstr FAIL\n");
        ++fails;
    }

    char big[301];
    for (int i = 0; i < 300; ++i)
        big[i] = 'z';
    big[300] = '\0';
    char sb[64];
    int rb = snprintf(sb, sizeof(sb), "%s", big);
    /* printf clamp wire check: one 300-char print must arrive as
       exactly 255 bytes (S2). Counted between markers (async serial
       output may split the run, so total — not longest run). */
    printf("SNPRINTF_VERIFY: big %d\n", rb);
    if (rb != 300) {
        printf("SNPRINTF_VERIFY: big FAIL\n");
        ++fails;
    }
    printf("%s\n", big);
    printf("SNPRINTF_VERIFY: clamp done\n");

    if (fails == 0) {
        printf("SNPRINTF_VERIFY: PASS\n");
    } else {
        printf("SNPRINTF_VERIFY: FAIL %d\n", fails);
    }
    return fails;
}
