#include <unistd.h>

/* v0.4.0 MP-2 user red-zone probe.
 *
 * Writes just below the user stack guard (STACK_VADDR = 0x70000000; the
 * guard page at STACK_VADDR and everything below it are unmapped).  The
 * write faults in user mode → SIGSEGV → task TERMINATED.
 */
int main(void) {
    volatile unsigned char *p = (volatile unsigned char *)0x6FFFFFF8UL;
    *p = 0xCD;
    _exit(0);
}
