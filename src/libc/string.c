#include <string.h>

void* memset(void* s, int c, size_t n) {
    unsigned char* p = (unsigned char*)s;
    for (size_t i = 0; i < n; ++i) p[i] = (unsigned char)c;
    return s;
}

void* memcpy(void* dest, const void* src, size_t n) {
    unsigned char* d = (unsigned char*)dest;
    const unsigned char* s = (const unsigned char*)src;
    for (size_t i = 0; i < n; ++i) d[i] = s[i];
    return dest;
}

void* memmove(void* dest, const void* src, size_t n) {
    unsigned char* d = (unsigned char*)dest;
    const unsigned char* s = (const unsigned char*)src;
    if (d < s) {
        for (size_t i = 0; i < n; ++i) d[i] = s[i];
    } else if (d > s) {
        for (size_t i = n; i > 0; --i) d[i - 1] = s[i - 1];
    }
    return dest;
}

int memcmp(const void* s1, const void* s2, size_t n) {
    const unsigned char* a = (const unsigned char*)s1;
    const unsigned char* b = (const unsigned char*)s2;
    for (size_t i = 0; i < n; ++i) {
        if (a[i] != b[i]) return (int)a[i] - (int)b[i];
    }
    return 0;
}

size_t strlen(const char* s) {
    size_t n = 0;
    while (*s) { ++n; ++s; }
    return n;
}

int strcmp(const char* s1, const char* s2) {
    while (*s1 && *s2 && *s1 == *s2) { ++s1; ++s2; }
    return (unsigned char)*s1 - (unsigned char)*s2;
}

int strncmp(const char* s1, const char* s2, size_t n) {
    for (size_t i = 0; i < n; ++i) {
        if (s1[i] != s2[i]) return (unsigned char)s1[i] - (unsigned char)s2[i];
        if (!s1[i]) return 0;
    }
    return 0;
}

char* strcpy(char* dest, const char* src) {
    char* d = dest;
    while (*src) *d++ = *src++;
    *d = '\0';
    return dest;
}

char* strncpy(char* dest, const char* src, size_t n) {
    size_t i;
    for (i = 0; i < n && src[i]; ++i) dest[i] = src[i];
    for (; i < n; ++i) dest[i] = '\0';
    return dest;
}

char* strcat(char* dest, const char* src) {
    char* d = dest + strlen(dest);
    while (*src) *d++ = *src++;
    *d = '\0';
    return dest;
}

char* strchr(const char* s, int c) {
    while (*s) {
        if (*s == (char)c) return (char*)s;
        ++s;
    }
    if (c == '\0') return (char*)s;
    return 0;
}

char* strrchr(const char* s, int c) {
    const char* last = 0;
    while (*s) {
        if (*s == (char)c) last = s;
        ++s;
    }
    if (c == '\0') return (char*)s;
    return (char*)last;
}

size_t strspn(const char* s, const char* accept) {
    size_t n = 0;
    while (*s) {
        int found = 0;
        for (const char* a = accept; *a; ++a) {
            if (*s == *a) { found = 1; break; }
        }
        if (!found) break;
        ++n; ++s;
    }
    return n;
}

size_t strcspn(const char* s, const char* reject) {
    size_t n = 0;
    while (*s) {
        for (const char* r = reject; *r; ++r) {
            if (*s == *r) return n;
        }
        ++n; ++s;
    }
    return n;
}

char* strstr(const char* haystack, const char* needle) {
    if (!*needle) return (char*)haystack;
    while (*haystack) {
        const char* h = haystack;
        const char* n = needle;
        while (*h && *n && *h == *n) { ++h; ++n; }
        if (!*n) return (char*)haystack;
        ++haystack;
    }
    return 0;
}

char* strdup(const char* s) {
    size_t n = strlen(s);
    char* p = (char*)0;
    extern void* malloc(size_t);
    p = (char*)malloc(n + 1);
    if (p) {
        for (size_t i = 0; i <= n; ++i) p[i] = s[i];
    }
    return p;
}

// Issue #339: reentrant tokenizer. NULL delim/saveptr fail closed
// (POSIX leaves NULL-delim undefined); empty tokens are skipped like
// consecutive delimiters; end-of-string parks saveptr at 0.
char* strtok_r(char* s, const char* delim, char** saveptr) {
    if (!delim || !saveptr)
        return 0;
    char* cur = s ? s : *saveptr;
    if (!cur)
        return 0;
    cur += strspn(cur, delim);
    if (*cur == '\0') {
        *saveptr = 0;
        return 0;
    }
    char* tok = cur;
    cur += strcspn(cur, delim);
    if (*cur == '\0') {
        *saveptr = 0;
    } else {
        *cur = '\0';
        *saveptr = cur + 1;
    }
    return tok;
}

// Issue #339: classic strtok over a process-private save cell (NOT
// thread-safe — threaded code must use strtok_r; safe under fork since
// the cell copies with the address space).
char* strtok(char* s, const char* delim) {
    static char* save = 0;
    return strtok_r(s, delim, &save);
}
