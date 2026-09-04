#include <syscall.h>

// Issue #92 fastpath probe: performs only FAST-class syscalls (YIELD +
// GETPID), never touching user pointers or FULL syscalls.  Used by
// fast_path_skips_canary to prove the FAST path never consults canaries even
// when the caller's segment canaries are tampered.  Parks forever at the end
// so the harness can kill it.
int main(void) {
    for (int i = 0; i < 64; ++i) {
        (void)__syscall5(SYS_YIELD, 0, 0, 0, 0);
        (void)__syscall5(SYS_GETPID, 0, 0, 0, 0);
    }
    for (;;) {
        (void)__syscall5(SYS_YIELD, 0, 0, 0, 0);
    }
    return 0;
}