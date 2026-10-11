/*
 * NexIOS RTOS — runelf 3-arg main / envp probe (issue #311)
 *
 * A runelf-loaded ELF is launched with an EMPTY environment.  This probe has a
 * 3-argument main, so crt0 derives `envp = rsp + 16 + argc*8` and the probe
 * reads it — the exact pattern that faulted with an instant #GP in issue #311
 * (the stack "after" canary overwrote the envp[0] NULL terminator).
 * Exit codes:
 *   0 = envp read OK (NULL-terminated empty array, or a NULL envp pointer)
 *   2 = envp[0] non-NULL (unexpected for a runelf load)
 * A user-mode fault kills the task instead of returning (exit_code != 0).
 */

#include <stdint.h>
#include <unistd.h>

int main(int argc, char **argv, char **envp) {
    (void)argc;
    (void)argv;
    if (envp == 0)
        _exit(0);
    if (envp[0] != 0)
        _exit(2);
    for (int i = 0; envp[i]; ++i)
        (void)envp[i][0];
    _exit(0);
}
