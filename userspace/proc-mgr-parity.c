/* Proc-mgr parity probe (issue #564, F11 strangler step b).
 * Deterministic single-shot S-lines (no timestamps, no fork/exec: forking
 * from the probe would leave a task behind and perturb the suite — the
 * parity class drives fork/exec parity from the kernel side). OFF
 * (knob 0): exercises the in-kernel process contracts; ON: the same
 * script runs against the PD verdict path and the filtered sets must be
 * byte-identical per op. Raw sys_* inlines (kill/signal have no libc
 * wrappers — only declarations, no definitions — so the probe issues the
 * syscalls directly, like the kernel-side parity assertions do).
 */
#include <stdint.h>
#include <stdio.h>
#include <unistd.h>
#include <signal.h>
#include <syscall.h>

int main(void) {
    long pid = (long)getpid();
    printf("S PROCMGR seq=%d op=%s rc=%d\n", 0, "getpid",
           pid > 0 ? 0 : -1);
    long kr = sys_kill((pid_t)pid, 0);
    printf("S PROCMGR seq=%d op=%s rc=%d\n", 1, "kill_self_null",
           kr == 0 ? 0 : -1);
    int st = 0;
    /* WNOHANG is bit 0 of the wait flags (no userspace macro; keep the
     * literal local so the line stays deterministic). */
    long wr = (long)waitpid((pid_t)-1, &st, 1);
    printf("S PROCMGR seq=%d op=%s rc=%d\n", 2, "waitpid_nochild",
           wr < 0 ? -1 : 0);
    long sr = sys_signal(SIGKILL, SIG_DFL);
    printf("S PROCMGR seq=%d op=%s rc=%d\n", 3, "signal_kill",
           sr == 0 ? 0 : -1);
    printf("S PROCMGR seq=%d op=%s rc=%d\n", 4, "done", 0);
    return 0;
}
