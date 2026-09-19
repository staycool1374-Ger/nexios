/* NexIOS picolibc hello-world (issue #73 acceptance).
 * Hosted C only: no src/libc headers, no direct syscalls. */

#include <stdio.h>

int main(void) {
    printf("nexios-picolibc: hello\n");
    return 0;
}
