#include <unistd.h>
#include <stdint.h>

/* v0.4.0 MP-4 SMEP probe.
 *
 * Calls the kernel-text VA stored in g_kva from ring 3.  The kernel test
 * patches g_kva (via HHDM) before dispatch.  With SMEP enabled the ring-3
 * instruction fetch of a supervisor page faults (#PF, IF=1) -> SIGSEGV ->
 * task TERMINATED — never a kernel panic.
 */
volatile uint64_t g_kva = 0;

int main(void) {
    void (*fn)(void) = (void (*)(void))(uintptr_t)g_kva;
    fn();
    _exit(0);
}
