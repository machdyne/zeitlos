/*
 * libz -- the formatter.
 *
 * %d %i %u %x %X %o %c %s %p %%, with flags '-' and '0', a field
 * width, and a precision. About 1KB of code.
 *
 * -- Why not newlib's --
 *
 * docs/app_runtime.md is unusually emphatic on this point, and it is
 * worth repeating rather than referencing: newlib's vfprintf costs on
 * the order of 100KB, and anything touching a FILE (even `fputs(s,
 * stdout)`, which formats nothing) costs about 40KB more. The tree's
 * own standing advice is to format numbers by hand and emit them with
 * puts() rather than call printf at all, and sw/apps/read hit that
 * trap twice in one week, both times as an unexplained tripling of the
 * binary.
 *
 * zcc output is already about four and a half times GCC's size. Adding
 * a 100KB formatter to that would make a hello-world that does not fit
 * the loader's space. So printf here is small enough that using it is
 * never the wrong call, which is worth more than supporting %e.
 *
 * -- What is absent --
 *
 * No floating point, because there is none in the compiler or the SOC.
 * No length modifiers (%ld, %zu): on rv32/ilp32 every integer type
 * that exists is 32 bits, so they would all mean the same thing --
 * they are ACCEPTED and ignored rather than refused, since real format
 * strings in the tree contain them.
 */

#include <stdarg.h>

#include "libz.h"

typedef struct {
    char *buf;          /* NULL means "write to the console" */
    size_t cap;
    size_t len;         /* what WOULD have been written, C99-style */
} sink_t;

static void sink_put(sink_t *s, char c) {
    if (!s->buf) {
        putchar(c);
        s->len++;
        return;
    }
    /* cap - 1 because the NUL is always reserved. snprintf returning
     * the untruncated length is what lets a caller size a buffer with
     * one speculative call, so len keeps counting past the end. */
    if (s->len + 1 < s->cap) s->buf[s->len] = c;
    s->len++;
}

static void sink_pad(sink_t *s, char c, int n) {
    while (n-- > 0) sink_put(s, c);
}

static const char *digits_lower = "0123456789abcdef";
static const char *digits_upper = "0123456789ABCDEF";

static int fmt_number(sink_t *s, unsigned v, int base, int is_neg,
                      int width, int prec, int left, int zero,
                      const char *digits) {

    char tmp[36];
    int n = 0;

    if (v == 0 && prec != 0) tmp[n++] = '0';
    while (v) {
        tmp[n++] = digits[v % (unsigned)base];
        v /= (unsigned)base;
    }
    while (n < prec) tmp[n++] = '0';

    int total = n + (is_neg ? 1 : 0);
    int pad = width - total;

    /* A precision suppresses zero padding -- "%08.3d" is spaces then
     * three digits, not eight. Left justification suppresses it too.
     * Both are easy to get wrong and both appear in real format
     * strings. */
    if (zero && !left && prec < 0) {
        if (is_neg) sink_put(s, '-');
        sink_pad(s, '0', pad);
        while (n) sink_put(s, tmp[--n]);
        return total > width ? total : width;
    }

    if (!left) sink_pad(s, ' ', pad);
    if (is_neg) sink_put(s, '-');
    while (n) sink_put(s, tmp[--n]);
    if (left) sink_pad(s, ' ', pad);

    return total > width ? total : width;
}

static int fmt_core(sink_t *s, const char *fmt, va_list ap) {

    for (; *fmt; fmt++) {

        if (*fmt != '%') { sink_put(s, *fmt); continue; }

        fmt++;
        if (*fmt == '%') { sink_put(s, '%'); continue; }

        int left = 0, zero = 0;
        for (;; fmt++) {
            if (*fmt == '-') left = 1;
            else if (*fmt == '0') zero = 1;
            else if (*fmt == '+' || *fmt == ' ' || *fmt == '#') ;
            else break;
        }

        int width = 0;
        if (*fmt == '*') { width = va_arg(ap, int); fmt++; }
        else while (*fmt >= '0' && *fmt <= '9') width = width * 10 + (*fmt++ - '0');
        if (width < 0) { left = 1; width = -width; }

        int prec = -1;
        if (*fmt == '.') {
            fmt++;
            prec = 0;
            if (*fmt == '*') { prec = va_arg(ap, int); fmt++; }
            else while (*fmt >= '0' && *fmt <= '9') prec = prec * 10 + (*fmt++ - '0');
        }

        /* Length modifiers: accepted and ignored. Every integer type on
         * rv32/ilp32 is 32 bits, so %ld and %d are the same
         * instruction sequence -- and refusing them would reject format
         * strings that are already all over this tree. */
        while (*fmt == 'l' || *fmt == 'h' || *fmt == 'z' || *fmt == 'j' ||
               *fmt == 't' || *fmt == 'L')
            fmt++;

        switch (*fmt) {
        case 'd':
        case 'i': {
            int v = va_arg(ap, int);
            unsigned u = (v < 0) ? (unsigned)(-(long)v) : (unsigned)v;
            fmt_number(s, u, 10, v < 0, width, prec, left, zero, digits_lower);
            break;
        }
        case 'u':
            fmt_number(s, va_arg(ap, unsigned), 10, 0, width, prec, left, zero, digits_lower);
            break;
        case 'x':
            fmt_number(s, va_arg(ap, unsigned), 16, 0, width, prec, left, zero, digits_lower);
            break;
        case 'X':
            fmt_number(s, va_arg(ap, unsigned), 16, 0, width, prec, left, zero, digits_upper);
            break;
        case 'o':
            fmt_number(s, va_arg(ap, unsigned), 8, 0, width, prec, left, zero, digits_lower);
            break;
        case 'c': {
            int pad = width - 1;
            if (!left) sink_pad(s, ' ', pad);
            sink_put(s, (char)va_arg(ap, int));
            if (left) sink_pad(s, ' ', pad);
            break;
        }
        case 'p':
            sink_put(s, '0');
            sink_put(s, 'x');
            fmt_number(s, (unsigned)(unsigned long)va_arg(ap, void *), 16, 0, 8, 8, 0, 1, digits_lower);
            break;
        case 's': {
            const char *str = va_arg(ap, const char *);
            if (!str) str = "(null)";
            int n = 0;
            while (str[n] && (prec < 0 || n < prec)) n++;
            int pad = width - n;
            if (!left) sink_pad(s, ' ', pad);
            for (int i = 0; i < n; i++) sink_put(s, str[i]);
            if (left) sink_pad(s, ' ', pad);
            break;
        }
        case 0:
            /* A trailing '%' is a malformed format string. Emitting it
             * literally and stopping is friendlier than reading past
             * the end of the string looking for a conversion. */
            sink_put(s, '%');
            return (int)s->len;
        default:
            sink_put(s, '%');
            sink_put(s, *fmt);
            break;
        }
    }

    return (int)s->len;
}

/*
 * Variadic entry points.
 *
 * <stdarg.h> is a compiler header, not a libc one, so it is available
 * in a freestanding build -- and it is the right thing to use here
 * even though the callers are zcc-generated. The RISC-V ABI passes a
 * variadic call's first eight words in a0..a7 and the rest on the
 * stack, and zcc passes arguments in a0..a7 and refuses more than
 * eight. So a zcc call and a GCC va_arg walk agree exactly, for every
 * call zcc is able to emit.
 *
 * The consequence is a real ceiling: seven conversions for printf,
 * five for snprintf, because the format string and the buffer occupy
 * argument slots too. It is the same ceiling zcc already enforces on
 * every call, so a program that compiles cannot exceed it -- which is
 * a better place for the limit to live than in this file.
 */
int printf(const char *fmt, ...) {
    va_list ap;
    sink_t s;
    int n;

    s.buf = NULL;
    s.cap = 0;
    s.len = 0;

    va_start(ap, fmt);
    n = fmt_core(&s, fmt, ap);
    va_end(ap);
    return n;
}

int vsnprintf(char *buf, size_t cap, const char *fmt, void *ap) {
    sink_t s;
    s.buf = buf;
    s.cap = cap;
    s.len = 0;

    int n = fmt_core(&s, fmt, *(va_list *)&ap);
    if (buf && cap) buf[s.len < cap ? s.len : cap - 1] = 0;
    return n;
}

int snprintf(char *buf, size_t cap, const char *fmt, ...) {
    va_list ap;
    sink_t s;

    s.buf = buf;
    s.cap = cap;
    s.len = 0;

    va_start(ap, fmt);
    int n = fmt_core(&s, fmt, ap);
    va_end(ap);

    if (buf && cap) buf[s.len < cap ? s.len : cap - 1] = 0;
    return n;
}

int puts(const char *s) {
    while (*s) putchar(*s++);
    putchar('\n');
    return 0;
}

int fputs_stdout(const char *s) {
    while (*s) putchar(*s++);
    return 0;
}
