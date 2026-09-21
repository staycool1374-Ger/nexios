// NexIOS RTOS — aarch64 EL0 fork smoke probe (issue #104).
//
// Parent forks; the child prints CHILD-MARKER (positive execution proof —
// a translation-faulted child terminates without printing) and exits; the
// parent waitpids and prints PARENT-OK. The kernel test asserts both lines
// in the stdout capture file.
#include <unistd.h>

static int puts_fd(const char *s) {
    unsigned long n = 0;
    while (s[n] != '\0')
        ++n;
    long r = (long)write(1, s, n);
    return (r == (long)n) ? 0 : -1;
}

int main(void) {
    pid_t pid = fork();
    if (pid == 0) {
        // Child: positive execution marker, then exit clean.
        puts_fd("CHILD-MARKER\n");
        _exit(0);
    }
    if (pid < 0) {
        puts_fd("FORK-FAILED\n");
        _exit(1);
    }
    int status = -1;
    pid_t c = waitpid(pid, &status, 0);
    if (c == pid && status == 0) {
        puts_fd("PARENT-OK\n");
        _exit(0);
    }
    puts_fd("WAITPID-FAILED\n");
    _exit(1);
}
