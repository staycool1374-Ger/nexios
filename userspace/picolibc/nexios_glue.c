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
#include <errno.h> // sysroot errno: BSS cell pre-rebuild, TP cell after
                   // (#303 item 2, errno-function knob)

/* #71 stubs (build/libc/picolib_stubs.o, linked explicitly — NOT the
 * whole in-tree archive, which would duplicate errno/_exit).
 * Signatures MUST match the int-returning definitions byte-for-byte:
 * declaring them long (as here historically) suppresses the call-site
 * sign-extension, so a -1 return arrives as +4294967295 and the errno
 * sets below never fire (issue #303 item 1 root cause). */
int _read(int fd, char *buf, int count);
int _write(int fd, char *buf, int count);
int _close(int fd);
long _lseek(int fd, long offset, int whence);
int _open(const char *path, int flags, int mode);

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

/* Issue #303 item 1 (hosted errno mapping): the `_` stubs return -1 on
 * failure but the kernel reports only fail-closed GENERIC_FAIL, so no
 * specific code survives the trap. The glue therefore records a
 * documented generic errno on every failed wrapper (open → ENOENT,
 * read/write/lseek/close → EIO), overwriting deterministically. Known
 * approximation: the stub's O_TRUNC-on-nonempty ENOSYS is superseded by
 * ENOENT here — revisit when the kernel returns specific negated codes.
 * Via <errno.h> this hits the BSS cell pre-rebuild and the TP cell after
 * the errno-function rebuild (item 2) with zero source change. */
long read(int fd, void *buf, unsigned long count) {
    long ret = _read(fd, (char *)buf, (int)count);
    if (ret < 0)
        errno = EIO;
    return ret;
}

long write(int fd, const void *buf, unsigned long count) {
    long ret = _write(fd, (char *)buf, (int)count);
    if (ret < 0)
        errno = EIO;
    return ret;
}

long lseek(int fd, long offset, int whence) {
    long ret = _lseek(fd, offset, whence);
    if (ret < 0)
        errno = EIO;
    return ret;
}

long close(int fd) {
    long ret = _close(fd);
    if (ret < 0)
        errno = EIO;
    return ret;
}

long open(const char *path, int flags) {
    long ret = _open(path, flags, 0);
    if (ret < 0)
        errno = ENOENT;
    return ret;
}

/* Issue #303 item 2 (hosted TP-wiring): errno-cell provider for the
 * picolibc errno-function path (sysroot rebuilt with
 * -Derrno-function=__errno; thread-local-storage stays off, so no
 * PT_TLS — locality comes from TP alone). Reads the task's
 * TP-published control
 * block — errno_value at offset 0, layout shared by value with struct
 * nexios_tcb in src/libc/errno.h (separate lib domain: the TU cannot be
 * shared, the 3-arch sequence mirrors src/libc/errno.c:read_tp).
 * TP == 0 (pre-TLS_SET) falls back to a function-static cell, never a
 * null dereference. Dormant while the sysroot uses BSS errno. */
static unsigned long glue_read_tp(void) {
#if defined(__x86_64__)
    unsigned long v = 0;
    __asm__ volatile("rdfsbase %0" : "=r"(v));
    return v;
#elif defined(__aarch64__)
    unsigned long v = 0;
    __asm__ volatile("mrs %0, tpidr_el0" : "=r"(v));
    return v;
#elif defined(__riscv)
    unsigned long v = 0;
    __asm__ volatile("mv %0, tp" : "=r"(v));
    return v;
#else
    return 0;
#endif
}

int *__errno(void) {
    unsigned long tp = glue_read_tp();
    if (tp == 0) {
        static int fallback_errno = 0;
        return &fallback_errno;
    }
    return (int *)tp; // offset-0 errno_value (see src/libc/errno.h)
}
