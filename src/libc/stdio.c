#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <sys/types.h>
#include <stdarg.h>

int putchar(int c) {
    char ch = (char)c;
    write(STDOUT_FILENO, &ch, 1);
    return c;
}

int puts(const char* s) {
    write(STDOUT_FILENO, s, strlen(s));
    write(STDOUT_FILENO, "\n", 1);
    return 1;
}

int getchar(void) {
    unsigned char c;
    if (read(STDIN_FILENO, &c, 1) == 1) return c;
    return -1;
}

/// @brief Emit one hex digit sequence char (issue #444: total counts
///        every would-be char; the store is bounded by end).
static void print_hex(unsigned long n, char** p, char* end, int storing,
                      int* total) {
    static const char hex[] = "0123456789abcdef";
    char buf[17];
    int i = 16;
    buf[i] = '\0';
    if (n == 0) buf[--i] = '0';
    else while (n && i) { buf[--i] = hex[n & 0xF]; n >>= 4; }
    while (buf[i]) {
        if (storing && *p < end) *(*p)++ = buf[i];
        ++(*total);
        ++i;
    }
}

/// @brief Emit one decimal digit sequence char (issue #444: see print_hex).
static void print_dec(unsigned long n, char** p, char* end, int storing,
                      int* total) {
    char buf[21];
    int i = 20;
    buf[i] = '\0';
    if (n == 0) buf[--i] = '0';
    else while (n && i) { buf[--i] = '0' + (n % 10); n /= 10; }
    while (buf[i]) {
        if (storing && *p < end) *(*p)++ = buf[i];
        ++(*total);
        ++i;
    }
}

/// @brief Store one char when room remains; always count it (issue #444).
/// Note: the stored VALUE must be side-effect-free — skipped stores do
/// not evaluate it, so call sites advance pointers and consume va_arg
/// BEFORE calling EMIT (a *p++=(*fmt++) shape would wedge the scan and
/// desync ap on truncation).
#define EMIT(c)                                \
    do {                                       \
        if (storing && p < end) *p++ = (c);    \
        ++total;                               \
    } while (0)

int vsnprintf(char* buf, size_t n, const char* fmt, va_list ap) {
    // Issue #444: buf==NULL || n==0 is count-only — zero stores and NO
    // buf/end pointer arithmetic on NULL (end stays NULL, never buf+n-1).
    const int storing = (buf != 0 && n > 0);
    char* p = buf;
    char* end = 0;
    if (storing) {
        end = buf + n - 1;
    }
    int total = 0;

    // Full-format iteration even when truncated: the return is the
    // would-be length (S3-1: the unprocessed tail on truncation is
    // dropped by the bounded stores, per C truncated-output semantics).
    while (*fmt) {
        if (*fmt != '%') {
            EMIT(*fmt);
            ++fmt;
            continue;
        }
        ++fmt;
        int zero_pad = 0;
        int width = 0;
        if (*fmt == '0') {
            zero_pad = 1;
            ++fmt;
        }
        while (*fmt >= '0' && *fmt <= '9') {
            width = width * 10 + (*fmt - '0');
            ++fmt;
        }
        // S3-2 contract: width/zero_pad are parsed and SKIPPED — no
        // padding is emitted (a future change adds padding explicitly).
        (void)zero_pad;
        (void)width;
        long lval = 0;
        unsigned long ulval = 0;
        int is_long = 0;
        while (*fmt == 'l') { is_long = 1; ++fmt; }

        switch (*fmt) {
        case 'd':
        case 'i':
            if (is_long) lval = va_arg(ap, long);
            else lval = (long)va_arg(ap, int);
            if (lval < 0) { EMIT('-'); ulval = (unsigned long)(-lval); }
            else ulval = (unsigned long)lval;
            print_dec(ulval, &p, end, storing, &total);
            break;
        case 'u':
            ulval = is_long ? va_arg(ap, unsigned long) : (unsigned long)va_arg(ap, unsigned int);
            print_dec(ulval, &p, end, storing, &total);
            break;
        case 'x':
        case 'X':
            ulval = is_long ? va_arg(ap, unsigned long) : (unsigned long)va_arg(ap, unsigned int);
            print_hex(ulval, &p, end, storing, &total);
            break;
        case 's': {
            const char* s = va_arg(ap, const char*);
            if (!s) s = "(null)";
            while (*s) {
                EMIT(*s);
                ++s;
            }
            break;
        }
        case 'c': {
            int cval = va_arg(ap, int);
            EMIT((char)cval);
            break;
        }
        case 'p': {
            unsigned long ptrval = (unsigned long)va_arg(ap, void*);
            EMIT('0');
            EMIT('x');
            print_hex(ptrval, &p, end, storing, &total);
            break;
        }
        case '%':
            EMIT('%');
            break;
        default:
            EMIT('%');
            EMIT(*fmt);
            break;
        }
        ++fmt;
    }
    if (storing) {
        // p advanced only while p < end, so p <= end: the NUL lands
        // inside the buffer (at most buf[n-1]).
        *p = '\0';
    }
    return total;
}

int snprintf(char* buf, size_t n, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int r = vsnprintf(buf, n, fmt, ap);
    va_end(ap);
    return r;
}

int sprintf(char* buf, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int r = vsnprintf(buf, 0x7FFFFFFF, fmt, ap);
    va_end(ap);
    return r;
}

int printf(const char* fmt, ...) {
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    int r = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    // Issue #444 S2: r is the would-be length and may exceed what was
    // stored — clamp the write to the buffer (stored <= 255 by the
    // NUL-slot construction); the return stays would-be r.
    size_t stored = (r < 0) ? 0 : (size_t)r;
    if (stored > sizeof(buf) - 1) {
        stored = sizeof(buf) - 1;
    }
    write(STDOUT_FILENO, buf, stored);
    return r;
}
