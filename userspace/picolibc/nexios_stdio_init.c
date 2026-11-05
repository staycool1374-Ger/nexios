/* NexIOS hosted stdio init (issue #455 item 5).
 *
 * Picolibc's posix-console layer defaults both stdout and stderr to
 * line-buffered (__BLBF in posixiob_stdout.c/posixiob_stderr.c); stdout
 * line-buffering is kept (newline flush + exit destructor), but stderr
 * must never delay diagnostics past a newline — a crash mid-line would
 * lose the partial line.
 *
 * Runs as a constructor: crt0's step-4b .init_array walk invokes it
 * post-TLS_SET pre-main on all three arches (no asm, no syscalls beyond
 * the setvbuf setup itself), before any app I/O.
 *
 * Separate TU compiled sysroot-only (no -I src/libc): the in-tree
 * stdarg.h stub shadows the compiler builtin and breaks picolibc
 * stdio.h (__gnuc_va_list) — the same latent conflict documented on the
 * verify_elf target in packaging/meson.build. Links into every hosted
 * program (see the link recipe there).
 */

#include <stdio.h>
#include <stddef.h>

__attribute__((constructor)) static void nexios_stdio_init(void) {
    setvbuf(stderr, NULL, _IONBF, 0);
}
