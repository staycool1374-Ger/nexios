#include <unistd.h>

/* issue #91 — exc_table err_macro_consistency probe.
 *
 * Executes a real `div` with a zero divisor → #DE (vector 0).  Inline asm is
 * mandatory: GCC replaces `1UL / runtime_value` with a compare (1/x == (x==1))
 * and never emits a division, so the fault would silently vanish.  The
 * explicit divq with RCX=0 cannot be folded.
 */
int main(void) {
#if defined(__x86_64__)
    __asm__ volatile(
        "xor %%rax, %%rax\n\t"
        "xor %%rdx, %%rdx\n\t"
        "xor %%rcx, %%rcx\n\t"
        "divq %%rcx\n\t" ::: "rax", "rdx", "rcx");
#endif
    _exit(0);
}