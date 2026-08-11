#include <unistd.h>

/* v0.4.0 MP-2 heap red-zone probe.
 *
 * Grows the heap to the kernel cap (HEAP_VADDR + INITIAL_HEAP_SIZE =
 * 0x60004000) via brk(), then writes one byte past the new break.  The
 * byte past the break is unmapped → live user-mode #PF → SIGSEGV → task
 * TERMINATED.
 */
int main(void) {
    void *cap = (void *)0x60004000UL;
    brk(cap);
    volatile unsigned char *p = (volatile unsigned char *)cap;
    *p = 0xEE;
    _exit(0);
}
