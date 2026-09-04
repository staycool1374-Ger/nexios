#include <unistd.h>
#include <syscall.h>

/* issue #107 — external-pager roundtrip probe.
 *
 * Registers PID 1 (the test harness / init) as its pager, then dereferences a
 * deliberately unmapped user VA (0x10000000).  The kernel delegates the #PF
 * to the registered pager (the harness), which maps a FrameCap at that VA;
 * the client then resumes and the write succeeds.  Writes 0xCAFE to the page
 * and _exit(0).  Without a pager this would SIGSEGV-terminate (fault-probe.c
 * behaviour).
 */
#define SYS_PAGER_REGISTER 69

static inline long pager_register(long pid) {
    return __syscall5(SYS_PAGER_REGISTER, pid, 0, 0, 0);
}

int main(void) {
    long reg = pager_register(1);
    if (reg != 0)
        _exit(2);
    volatile unsigned long *p =
        (volatile unsigned long *)0x10000000UL;
    *p = 0xCAFEUL;
    _exit(0);
}