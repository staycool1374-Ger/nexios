#include <unistd.h>

/* v0.4.0 MP-2/MP-5 memory-isolation probe.
 *
 * Dereferences a deliberately unmapped user VA (0x10000000).  Kernel tests
 * map that VA in ONE task's page table (the write then succeeds) and leave
 * it unmapped for the faulting partner: a live user-mode #PF (page
 * not-present) → SIGSEGV → task TERMINATED — never a kernel panic.
 */
int main(void) {
    volatile unsigned char *p = (volatile unsigned char *)0x10000000UL;
    *p = 0xAB;
    _exit(0);
}
