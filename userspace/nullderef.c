/* NexIOS demonstration program: handled user-mode MMU fault.
 *
 * Loaded via `load /nullderef.c.elf` + bare `runelf`: prints a greeting,
 * then dereferences address zero.  The unmapped access raises a user-mode
 * #PF (page not-present) which the kernel handles as SIGSEGV: the task is
 * TERMINATED, never a kernel panic (same path as fault-probe.c).
 */
#include <stdio.h>

int main(void) {
    printf("Hey. This is NexIOS, the appication access memory which doesnt belongs to it!\n");
    volatile unsigned char *p = (volatile unsigned char *)0UL;
    *p = 0xAB;
    return 0;
}
