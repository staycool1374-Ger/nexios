#include <unistd.h>
#include <syscall.h>

/* issue #107 — external-pager roundtrip probe.
 *
 * Registers PID 1 (the test harness / init) as its pager, then dereferences a
 * deliberately unmapped user VA (0x50000000).  The kernel delegates the #PF
 * to the registered pager (the harness), which maps a FrameCap at that VA;
 * the client then resumes and the write succeeds.  Writes 0xCAFE to the page
 * and _exit(0).  Without a pager this would SIGSEGV-terminate (fault-probe.c
 * behaviour).
 *
 * VA choice (issue #527): 0x10000000 aliased the riscv64 L0[0] 0-1GB identity
 * window (QEMU virt UART), so the no-PTE premise failed there.  0x50000000
 * is clear on all arches (riscv64 L0[1], above the 0x400000 image, below the
 * 0x70000000 stack — same precedent as #352's 0x50000000 isolation VA).
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
        (volatile unsigned long *)0x50000000UL;
    *p = 0xCAFEUL;
    _exit(0);
}