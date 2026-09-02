#include <unistd.h>

/* issue #91 — exc_table err_macro_consistency probe.
 *
 * Executes an undefined instruction (ud2) → #UD (vector 6).  A correctly
 * built ISR_NOERR frame lets the signal path deliver SIGILL and terminate
 * the task; a misaligned frame (ISR_ERR vector treated as NOERR, or vice
 * versa) would corrupt the iretq frame and triple-fault the kernel.
 */
int main(void) {
#if defined(__x86_64__)
    __asm__ volatile("ud2");
#endif
    _exit(0);
}