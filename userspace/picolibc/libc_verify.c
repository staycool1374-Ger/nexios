/* NexIOS picolibc libc-verify program (issue #75 acceptance).
 * Hosted C only: no src/libc headers, no direct syscalls. Exercises
 * printf (WRITE), malloc/free via _sbrk growth + free-list reuse, and
 * scanf (READ) from a Ring 3 task. stdin is wired by the kernel test
 * (tmpfs-backed fd 0); stdout goes to serial via the WRITE path.
 *
 * Issue #303 (Phase 6) extensions, appended after the #75 block (whose
 * markers stay byte-identical and first, so the original tests keep
 * passing on a truncated 512 B capture): format specifiers, snprintf
 * bounds, sscanf mismatch/hex, fprintf-to-fd, malloc edge cases,
 * string/memory sweeps, getpid-loop syscall timing, single-write
 * crossing timing, a staged-file open/read/write chain (cross-boundary),
 * atomics and setjmp/longjmp. New tests use a 2 KiB capture.
 *
 * NOTE (verified ABI wart, #303 M1/M9): picolibc O_CREAT (0x40) does NOT
 * equal kernel VFS O_CREAT (0x200), so this program never creates files
 * itself — every /tmp path it opens is pre-created by the kernel test
 * and opened O_RDONLY/O_WRONLY only (both match the kernel values).
 *
 * Issue #341 extends this: the #341 probe block below (placed right
 * after the #75 scanf block so its markers land inside the kernel
 * test's 512 B stdout capture) exercises the picolibc→kernel flag
 * translation in picolib_stubs _open — Ring-3 file creation, O_TRUNC
 * on an empty file (no-op success), and O_TRUNC refusal on a
 * non-empty file (no kernel truncate primitive). */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <setjmp.h>
#include <errno.h>

static unsigned long rdtsc_now(void) {
    unsigned lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((unsigned long)hi << 32) | lo;
}

int main(void) {
    /* Stash entry errno BEFORE any syscall: proves the cell starts clean
     * on a fresh dispatch (BSS-fresh today; TP-cell when the hosted path
     * is wired — see test_libc_thread.cpp). Printed at the end so the
     * #75 512 B prefix window is untouched. */
    int errno_at_entry = errno;
    printf("LIBC_VERIFY: hello\n");

    void *b0 = sbrk(0);
    void *p1 = malloc(64);
    void *b1 = sbrk(0);
    free(p1);
    void *b2 = sbrk(0);
    void *p2 = malloc(64);
    void *b3 = sbrk(0);
    printf("LIBC_VERIFY: brk %lu %lu %lu %lu\n",
           (unsigned long)b0, (unsigned long)b1,
           (unsigned long)b2, (unsigned long)b3);
    printf("LIBC_VERIFY: ptr %lu %lu\n",
           (unsigned long)p1, (unsigned long)p2);

    int v = 0;
    if (scanf("%d", &v) == 1)
        printf("LIBC_VERIFY: scanf %d\n", v);
    else
        printf("LIBC_VERIFY: scanf FAIL\n");

    /* ---- #341: Ring-3 file creation + O_TRUNC semantics ---- */
    {
        int cfd = open("/tmp/libc_created.txt", O_WRONLY | O_CREAT);
        int wrok = 0;
        if (cfd >= 0) {
            const char *msg = "created-by-ring3";
            long w = write(cfd, msg, strlen(msg));
            wrok = (w == (long)strlen(msg)) ? 1 : 0;
            close(cfd);
        }
        printf("LIBC_VERIFY: created %d %d\n", (cfd >= 0) ? 1 : 0, wrok);
        /* Non-empty file + O_TRUNC must fail (no kernel truncate
         * primitive) — proves the 0x200 bit is translated, not passed
         * through as a bogus kernel O_CREAT. */
        int efd = open("/tmp/libc_created.txt", O_WRONLY | O_TRUNC);
        printf("LIBC_VERIFY: trunc_full %d\n", (efd < 0) ? 1 : 0);
        if (efd >= 0)
            close(efd);
        /* Empty file + O_TRUNC is a no-op and must succeed. */
        int zfd0 = open("/tmp/libc_empty.txt", O_WRONLY | O_CREAT);
        if (zfd0 >= 0)
            close(zfd0);
        int zfd = open("/tmp/libc_empty.txt", O_WRONLY | O_TRUNC);
        printf("LIBC_VERIFY: trunc_empty %d\n", (zfd >= 0) ? 1 : 0);
        if (zfd >= 0)
            close(zfd);
    }

    /* ---- #303 M9: format specifiers ---- */
    printf("LIBC_VERIFY: fmt %d %x %s %c %p\n", -42, 0xabcd, "hi", 'Q',
           (void *)0x400000);

    /* ---- #303 M9: snprintf respects the bound ---- */
    {
        char snb[16];
        int r = snprintf(snb, sizeof(snb), "0123456789ABCDEFGHIJ");
        printf("LIBC_VERIFY: snprintf %d %u %s\n", r,
               (unsigned)strlen(snb), snb);
    }

    /* ---- #303 M9: sscanf mismatch + hex/string ---- */
    {
        int sv = 12345;
        int n0 = sscanf("abc", "%d", &sv);
        unsigned hv = 0;
        char sw[16] = {0};
        int n1 = sscanf("0x10 hello", "%x %15s", &hv, sw);
        printf("LIBC_VERIFY: sscanf %d %d %u %s\n", n0, n1, hv, sw);
    }

    /* ---- #303 M9: fprintf to a pre-created fd ---- */
    {
        int fdf = open("/tmp/libc_fprintf.txt", O_WRONLY);
        printf("LIBC_VERIFY: fprintf_fd %d\n", fdf);
        if (fdf >= 0) {
            FILE *f = fdopen(fdf, "w");
            if (f != NULL) {
                fprintf(f, "LIBC_VERIFY file %d\n", 7);
                fclose(f);
            } else {
                close(fdf);
            }
        }
    }

    /* ---- #303 M10: malloc edge cases ---- */
    {
        void *z = malloc(0);
        printf("LIBC_VERIFY: malloc0 %lu\n", (unsigned long)z);
        free(NULL); /* must not crash; survival is the assert */
        int *cz = (int *)calloc(4, 8);
        unsigned csum = 0;
        if (cz != NULL) {
            unsigned char *cb = (unsigned char *)cz;
            for (unsigned i = 0; i < 32; ++i)
                csum += cb[i];
        }
        printf("LIBC_VERIFY: calloc %lu %u\n", (unsigned long)cz, csum);
        free(cz);
        unsigned char *rp = (unsigned char *)malloc(32);
        unsigned rok = 0;
        if (rp != NULL) {
            for (unsigned i = 0; i < 32; ++i)
                rp[i] = (unsigned char)(i + 1);
            unsigned char *rq = (unsigned char *)realloc(rp, 64);
            if (rq != NULL) {
                rok = 1;
                for (unsigned i = 0; i < 32; ++i)
                    if (rq[i] != (unsigned char)(i + 1))
                        rok = 0;
                free(rq);
            } else {
                free(rp);
            }
        }
        void *oom = malloc(1UL << 31);
        printf("LIBC_VERIFY: realloc %u %lu\n", rok,
               (unsigned long)oom);
        free(oom);
    }

    /* ---- #303 M11: string/memory ---- */
    {
        char mv[64];
        for (unsigned i = 0; i < 64; ++i)
            mv[i] = (char)('A' + (i % 26));
        memmove(mv + 8, mv, 32); /* forward overlap */
        memmove(mv, mv + 8, 32); /* backward overlap (restores) */
        unsigned msum = 0;
        for (unsigned i = 0; i < 64; ++i)
            msum += (unsigned char)mv[i];
        char sc[32];
        strcpy(sc, "foo");
        strcat(sc, "bar");
        const char *hay = "abccbxcc";
        long p1off = (long)(strchr(hay, 'x') - hay);
        long p2off = (long)(strrchr(hay, 'c') - hay);
        const char *psub = strstr(hay, "bxc");
        long p3off = (psub != NULL) ? (long)(psub - hay) : -1;
        char stok[32];
        strcpy(stok, "a,b;c");
        char *t1 = strtok(stok, ",;");
        char *t2 = strtok(NULL, ",;");
        char *t3 = strtok(NULL, ",;");
        printf("LIBC_VERIFY: string %u %s %ld %ld %ld %s %s %s\n", msum,
               sc, p1off, p2off, p3off, t1 ? t1 : "-",
               t2 ? t2 : "-", t3 ? t3 : "-");
        /* 0..256 sweep checksum over memcpy/memcmp */
        unsigned long sweep = 0;
        char ssrc[256], sdst[256];
        for (unsigned i = 0; i < 256; ++i)
            ssrc[i] = (char)(i * 7 + 3);
        for (unsigned n = 0; n <= 256; ++n) {
            memset(sdst, 0, n);
            memcpy(sdst, ssrc, n);
            if (memcmp(sdst, ssrc, n) == 0)
                sweep += n;
        }
        printf("LIBC_VERIFY: sweep %lu\n", sweep);
    }

    /* ---- #303 M4: syscall entry/exit via getpid loop ---- */
    {
        unsigned long mn = ~0UL, mx = 0, sum = 0;
        for (unsigned i = 0; i < 2000; ++i) {
            unsigned long t0 = rdtsc_now();
            (void)getpid();
            unsigned long dt = rdtsc_now() - t0;
            if (dt < mn)
                mn = dt;
            if (dt > mx)
                mx = dt;
            sum += dt;
        }
        printf("LIBC_VERIFY: cs_getpid %lu %lu %lu\n", mn, mx,
               sum / 2000);
    }

    /* ---- #303 M4: kernel<->user crossing via single-byte writes ---- */
    {
        unsigned long mn = ~0UL, mx = 0, sum = 0;
        for (unsigned i = 0; i < 200; ++i) {
            unsigned long t0 = rdtsc_now();
            if (write(1, "W", 1) != 1)
                printf("LIBC_VERIFY: cs_write FAIL\n");
            unsigned long dt = rdtsc_now() - t0;
            if (dt < mn)
                mn = dt;
            if (dt > mx)
                mx = dt;
            sum += dt;
        }
        printf("\nLIBC_VERIFY: cs_writecross %lu %lu %lu\n", mn, mx,
               sum / 200);
    }

    /* ---- #303 M1: staged-file open/read/write chain ---- */
    {
        int rdf = open("/tmp/libc_xbound.txt", O_RDONLY);
        long rbytes = -1;
        int rok = 0;
        if (rdf >= 0) {
            char rb[64] = {0};
            long total = 0;
            long r;
            while ((r = read(rdf, rb + total, 32)) > 0)
                total += r;
            rbytes = total;
            if (total == 32 &&
                memcmp(rb, "XBOUND-0123456789ABCDEFGH", 24) == 0)
                rok = 1;
            close(rdf);
        }
        printf("LIBC_VERIFY: xbound %d %ld %d\n", rdf, rbytes, rok);
        int wrf = open("/tmp/libc_xbound_reply.txt", O_WRONLY);
        long wbytes = -1;
        if (wrf >= 0) {
            wbytes = write(wrf, "XBOUND-REPLY-42\n", 16);
            close(wrf);
        }
        printf("LIBC_VERIFY: xbound_reply %d %ld\n", wrf, wbytes);
    }

    /* ---- #303 M12: atomics + setjmp/longjmp ---- */
    {
        volatile int av = 0;
        for (unsigned i = 0; i < 1000; ++i)
            __sync_fetch_and_add(&av, 1);
        int aok = (av == 1000) ? 1 : 0;
        int jok = 0;
        jmp_buf jb;
        int jr = setjmp(jb);
        if (jr == 0) {
            longjmp(jb, 7);
        } else if (jr == 7) {
            jok = 1;
        }
        printf("LIBC_VERIFY: atomic %d %d %d\n", av, aok, jok);
    }

    /* ---- #303 M12: hosted errno probe (test_libc_thread.cpp) ----
     * Writable-cell check + failing-open probe. failed==1 pins fd<0
     * (parse_tag_line is unsigned-only, so the sign is folded here);
     * eno is observed only — 2026-10-18 the hosted open() failure leaves
     * errno at 0 (no kernel→errno mapping in the hosted path; the TP
     * cell exists but picolibc was built without __PICOLIBC_ERRNO_FUNCTION,
     * so hosted errno is plain BSS). Saved/restored so earlier markers'
     * errno state is untouched. */
    {
        int saved = errno;
        errno = 0;
        int fd = open("/nonexistent_libc_thread_probe", O_RDONLY);
        int eno = errno;
        printf("LIBC_VERIFY: errno_probe %d %d %d\n",
               errno_at_entry, (fd < 0) ? 1 : 0, eno);
        if (fd >= 0)
            close(fd);
        errno = saved;
    }

    return 0;
}
