/* NexIOS picolibc libc-verify program (issue #75 acceptance).
 * Hosted C only: no src/libc headers, no direct syscalls. Exercises
 * printf (WRITE), malloc/free via _sbrk growth + free-list reuse, and
 * scanf (READ) from a Ring 3 task. stdin is wired by the kernel test
 * (tmpfs-backed fd 0); stdout goes to serial via the WRITE path. */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

int main(void) {
    printf("LIBC_VERIFY: hello\n");

    void *b0 = sbrk(0);
    void *p1 = malloc(64);
    void *b1 = sbrk(0);
    free(p1);
    void *b2 = sbrk(0);
    void *p2 = malloc(64);
    void *b3 = sbrk(0);
    printf("LIBC_VERIFY: brk %lu %lu %lu %lu\n",
           (unsigned long)b0, (unsigned long)b1,
           (unsigned long)b2, (unsigned long)b3);
    printf("LIBC_VERIFY: ptr %lu %lu\n",
           (unsigned long)p1, (unsigned long)p2);

    int v = 0;
    if (scanf("%d", &v) == 1)
        printf("LIBC_VERIFY: scanf %d\n", v);
    else
        printf("LIBC_VERIFY: scanf FAIL\n");
    return 0;
}
