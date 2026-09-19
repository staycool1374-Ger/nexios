/* NexIOS libos glue for picolibc programs (issue #73).
 * Provides what picolibc's posix-console FILE layer needs and its
 * exit path requires, over the NexIOS syscall header (this file is
 * NexIOS-specific, not hosted C):
 *   _exit ............ kernel-ABI exit (picolibc exit.c needs it)
 *   read/write/lseek/close/open ... POSIX names delegating to the #71
 *     `_`-stubs (single syscall-mapping owner; errno mapping included).
 * Everything else comes from the picolibc sysroot. */

#include <sys/time.h> // first: declares struct timeval for syscall.h
                       // sys_gettod (sysroot header; LP64 layout matches)
#include <syscall.h>

/* #71 stubs (build/libc/picolib_stubs.o, linked explicitly — NOT the
 * whole in-tree archive, which would duplicate errno/_exit). */
long _read(int fd, char *buf, int count);
long _write(int fd, char *buf, int count);
long _close(int fd);
long _lseek(int fd, long offset, int whence);
long _open(const char *path, int flags, int mode);

void _exit(int status) {
    __syscall5(SYS_EXIT, (long)status, 0, 0, 0);
    for (;;)
        __builtin_trap();
}

/* Break owner for the picolibc link domain: mirrors sbrk() (stdlib.c)
 * over sys_brk (STACK_VADDR red-zone cap enforced kernel-side, MP-2.2).
 * In-tree stdlib.o cannot be pulled (it would drag malloc/free into
 * collision with picolibc's); defining sbrk here keeps picolibc's
 * __fallback_sbrk (and its __heap_start/__heap_end anchors) unlinked. */
void *sbrk(long increment) {
    void *old = sys_brk(0);
    if (old == (void *)-1)
        return (void *)-1;
    if (increment == 0)
        return old;
    void *new_brk = (void *)((unsigned long)old + (long)increment);
    void *ret = sys_brk(new_brk);
    return (ret == new_brk) ? old : (void *)-1;
}

long read(int fd, void *buf, unsigned long count) {
    return _read(fd, (char *)buf, (int)count);
}

long write(int fd, const void *buf, unsigned long count) {
    return _write(fd, (char *)buf, (int)count);
}

long lseek(int fd, long offset, int whence) {
    return _lseek(fd, offset, whence);
}

long close(int fd) {
    return _close(fd);
}

long open(const char *path, int flags) {
    return _open(path, flags, 0);
}
