/// @file src/lib/fmt/term_fmt.cpp
/// @brief Unified terminal output core implementation (issue #286).
#include <lib/fmt/term_fmt.hpp>

namespace fmt {
namespace {

// Bounded per-conversion scratch (20 digits max for 64-bit + NUL).
constexpr size_t kNumBuf = 24;

/// @brief Single accounting path: `need` always counts, store only
/// in-bounds, NUL handled once by the caller. All emitters below go
/// through here — no inline accounting duplicates.
struct Sink {
    char *out = nullptr;
    size_t cap = 0;
    size_t at = 0;
    size_t need = 0;
    void put(char c) {
        if (cap > 0 && at < cap)
            out[at] = c;
        ++at;
        ++need;
    }
};

/// @brief Render unsigned magnitude in `base` (lowercase digits).
/// @return Digit count; digits land in `buf` (truncated, NUL-terminated).
size_t render_uint(char *buf, size_t cap, unsigned long long val,
                   unsigned int base) {
    static const char kDigits[] = "0123456789abcdef";
    char rev[kNumBuf] = {};
    size_t len = 0;
    if (val == 0)
        rev[len++] = '0';
    while (val > 0 && len < sizeof(rev) - 1) {
        rev[len++] = kDigits[val % base];
        val /= base;
    }
    size_t n = 0;
    while (len > 0 && n + 1 < cap) {
        --len;
        buf[n++] = rev[len];
    }
    if (cap > 0)
        buf[(n < cap) ? n : cap - 1] = '\0';
    return n + len; // stored + dropped remainder = exact count
}

void put_pad(Sink &sink, char ch, size_t count) {
    for (size_t k = 0; k < count; ++k)
        sink.put(ch);
}

} // namespace

int mini_vsnprintf(char *out, size_t cap, const char *fmt, VaList args) {
    Sink sink{};
    sink.out = out;
    sink.cap = cap;
    size_t i = 0;
    if (out == nullptr || fmt == nullptr)
        return -1;
    while (fmt[i] != '\0') {
        if (fmt[i] != '%') {
            sink.put(fmt[i]);
            ++i;
            continue;
        }
        ++i; // consume '%'
        bool left = false;
        bool zero = false;
        while (fmt[i] == '-' || fmt[i] == '0') {
            if (fmt[i] == '-')
                left = true;
            else
                zero = true;
            ++i;
        }
        if (left)
            zero = false;
        size_t width = 0;
        while (fmt[i] >= '0' && fmt[i] <= '9' &&
               width < kMaxWidth + 16) {
            width = width * 10 + static_cast<size_t>(fmt[i] - '0');
            ++i;
        }
        if (width > kMaxWidth)
            width = kMaxWidth;
        size_t prec = 0;
        bool have_prec = false;
        if (fmt[i] == '.') {
            ++i;
            have_prec = true;
            while (fmt[i] >= '0' && fmt[i] <= '9' &&
                   prec < kMaxPrecision + 16) {
                prec = prec * 10 + static_cast<size_t>(fmt[i] - '0');
                ++i;
            }
            if (prec > kMaxPrecision)
                prec = kMaxPrecision;
        }
        bool wide = false; // 32-bit default, 64-bit on l/ll/z
        if (fmt[i] == 'l') {
            wide = true;
            ++i;
            if (fmt[i] == 'l')
                ++i; // ll collapses to l (LP64 identical widths)
        } else if (fmt[i] == 'z') {
            wide = true; // size_t is 64-bit LP64 (libc delta documented)
            ++i;
        } else if (fmt[i] == 'h') {
            ++i;
            if (fmt[i] == 'h')
                ++i; // hh/h ignored: default promotion applies
        }
        char conv = fmt[i];
        if (conv == '\0')
            break; // trailing '%' dropped (fail-closed)
        ++i;
        if (conv == '%') {
            sink.put('%');
            continue;
        }
        if (conv == 'c') {
            int val = __builtin_va_arg(args, int);
            size_t pad = (width > 1) ? width - 1 : 0;
            if (!left)
                put_pad(sink, ' ', pad);
            sink.put(static_cast<char>(val));
            if (left)
                put_pad(sink, ' ', pad);
            continue;
        }
        if (conv == 's') {
            const char *str = __builtin_va_arg(args, const char *);
            if (str == nullptr)
                str = "(null)";
            size_t slen = 0;
            while (str[slen] != '\0')
                ++slen; // NUL-terminated contract (house strlen idiom)
            if (have_prec && slen > prec)
                slen = prec;
            size_t pad = (width > slen) ? width - slen : 0;
            if (!left)
                put_pad(sink, ' ', pad);
            for (size_t k = 0; k < slen; ++k)
                sink.put(str[k]);
            if (left)
                put_pad(sink, ' ', pad);
            continue;
        }
        if (conv == 'd' || conv == 'i' || conv == 'u' || conv == 'x' ||
            conv == 'X' || conv == 'p') {
            unsigned int base = 10;
            if (conv == 'x' || conv == 'X' || conv == 'p')
                base = 16;
            bool neg = false;
            unsigned long long mag = 0;
            if (conv == 'd' || conv == 'i') {
                long long sval = 0;
                if (wide)
                    sval = __builtin_va_arg(args, long long);
                else
                    sval = __builtin_va_arg(args, int);
                if (sval < 0) {
                    neg = true;
                    mag = static_cast<unsigned long long>(-(sval + 1)) +
                          1ULL; // INT64_MIN-safe magnitude
                } else {
                    mag = static_cast<unsigned long long>(sval);
                }
            } else if (conv == 'p') {
                void *ptr = __builtin_va_arg(args, void *);
                mag = static_cast<unsigned long long>(
                    reinterpret_cast<uintptr_t>(ptr));
            } else if (wide) {
                mag = __builtin_va_arg(args, unsigned long long);
            } else {
                mag = __builtin_va_arg(args, unsigned int);
            }
            char num[kNumBuf] = {};
            size_t nlen = render_uint(num, sizeof(num), mag, base);
            size_t head = (conv == 'p') ? 2 : 0; // "0x"
            size_t sign = neg ? 1 : 0;
            size_t total = sign + head + nlen;
            size_t pad = (width > total) ? width - total : 0;
            if (!left && !zero)
                put_pad(sink, ' ', pad);
            if (neg)
                sink.put('-');
            if (conv == 'p') {
                sink.put('0');
                sink.put('x');
            }
            if (!left && zero)
                put_pad(sink, '0', pad);
            for (size_t k = 0; k < nlen && num[k] != '\0'; ++k)
                sink.put(num[k]);
            if (left)
                put_pad(sink, ' ', pad);
            continue;
        }
        // Unknown specifier: fail-closed literal "%?" copy.
        sink.put('%');
        sink.put(conv);
    }
    if (cap > 0)
        out[(sink.at < cap) ? sink.at : cap - 1] = '\0';
    return static_cast<int>(sink.need);
}

void emit_field(Emit emit, const char *text, size_t width, bool right) {
    if (emit == nullptr)
        return;
    if (text == nullptr)
        text = "(null)";
    if (width > kTermWidth)
        width = kTermWidth;
    size_t len = 0;
    while (text[len] != '\0' && len < width)
        ++len;
    if (right) {
        for (size_t k = len; k < width; ++k)
            emit(' ');
    }
    for (size_t k = 0; k < len; ++k)
        emit(text[k]);
    if (!right) {
        for (size_t k = len; k < width; ++k)
            emit(' ');
    }
}

void emit_separator(Emit emit, char ch, size_t count) {
    if (emit == nullptr)
        return;
    if (count > kTermWidth)
        count = kTermWidth;
    for (size_t k = 0; k < count; ++k)
        emit(ch);
}

bool frame_due(unsigned long now_ms, unsigned long &last_ms,
               unsigned long interval_ms) {
    if (now_ms - last_ms >= interval_ms) {
        last_ms = now_ms;
        return true;
    }
    return false;
}

} // namespace fmt
