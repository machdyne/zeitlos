/*
 * zcc -- allocation and diagnostics.
 *
 * -- The allocator never frees --
 *
 * zalloc() is calloc() with an out-of-memory check and no counterpart.
 * That is deliberate and it is a decision about the target, not
 * laziness: a compiler runs once, produces one output and exits, so
 * every allocation it makes is live until it does. Freeing them buys
 * nothing except the opportunity to free one twice.
 *
 * It stops being free on the device, where the process's whole heap is
 * its stack allowance (Z_PROC_STACK_SIZE_HUGE, 4MB -- see
 * sw/os/kernel.h) and there is no OS to reclaim anything until the
 * process exits. It still holds there, but the number that matters
 * becomes peak use rather than steady state; docs/zcc.md's memory
 * budget is written in those terms for that reason. If it ever stops
 * fitting, the fix is arena allocation per function body, not
 * scattered free() calls.
 */

#include <string.h>
#include <stdarg.h>

#include "zcc.h"

int zcc_verbose;

/*
 * A bump arena over large blocks.
 *
 * zalloc() used to be a straight malloc(), which was correct and, on
 * the device, quadratic. libz's malloc is a first-fit walk over a free
 * list -- fine for an app that holds a few dozen allocations, ruinous
 * for a compiler that makes one per token and never frees. Compiling a
 * ten-line file that includes zeitlos.h took 729 MILLION instructions,
 * most of it walking a list half a million entries long.
 *
 * A bump allocator removes the walk entirely, and it costs nothing to
 * use one here because this allocator already never frees: a compiler
 * runs once, produces one output and exits. That was written down as
 * the eventual fix before it was needed; it was needed sooner than
 * expected, and only the device build could have shown it.
 *
 * Blocks are large and grow, so a big translation unit does not pay a
 * malloc per 64KB either. Nothing is ever returned to malloc, which is
 * the point.
 */
#define ARENA_MIN (64 * 1024)

static char *arena_ptr;
static size_t arena_left;
static size_t arena_total;

size_t zalloc_total(void) { return arena_total; }

void *zalloc(size_t n) {

    n = (n + 7u) & ~(size_t)7u;      /* keep every allocation 8-aligned */

    if (n > arena_left) {
        /* A request larger than a block gets its own block, rather
         * than being refused or rounded up to the next power of two.
         * The token stream is made of small objects; the code and data
         * buffers in emit.c are not. */
        size_t want = ARENA_MIN;
        while (want < n) want *= 2;

        arena_ptr = malloc(want);
        if (!arena_ptr) {
            /* Not zcc_fatal(): that formats into a buffer, and the
             * reason we are here is that memory ran out. A fixed
             * string cannot fail. On the device this is the message
             * that means the 4MB heap tier was not enough -- see
             * docs/zcc.md, "Memory". */
            zio_out("zcc: out of memory\n");
            zio_exit(1);
        }
        arena_left = want;
        arena_total += want;
    }

    char *p = arena_ptr;
    arena_ptr += n;
    arena_left -= n;

    /* Zeroed, because every caller relies on it -- the parser builds
     * structs field by field and leaves the rest at zero. */
    memset(p, 0, n);
    return p;
}

char *zstrdup(const char *s) {
    size_t n = strlen(s);
    char *p = zalloc(n + 1);
    memcpy(p, s, n + 1);
    return p;
}

char *zstrndup(const char *s, size_t n) {
    char *p = zalloc(n + 1);
    memcpy(p, s, n);
    p[n] = 0;
    return p;
}

/*
 * -- The formatter --
 *
 * zcc formats its own messages rather than calling vsnprintf().
 *
 * docs/app_runtime.md is emphatic about why: newlib's formatter costs
 * on the order of 100KB on this machine, which is more than the whole
 * compiler. The tree's standing advice is to format by hand rather
 * than call printf, and a compiler -- which is made of diagnostics
 * with line numbers in them -- is exactly the case that advice is
 * about.
 *
 * The second reason is subtler and was learned the hard way: with our
 * own formatter, the host build and the device build format
 * IDENTICALLY. A diagnostic that reads one way on a development
 * machine and another way on the board is a bug report nobody can
 * follow, and the differential test suite would not catch it because
 * it compares compiler OUTPUT, not compiler messages.
 *
 * %s %c %d %i %u %x %% with a field width, zero-padding, and a
 * precision on %s including `%.*s`.
 *
 * That list is not a guess -- it is what `grep -oh '%[^ ]' *.c` says
 * this compiler actually uses, and it was checked that way after
 * getting it wrong: the first version omitted precision, so `%.*s`
 * printed "%." literally, never consumed its int argument, and the
 * following %s read that int as a pointer. Every compile segfaulted,
 * from cpp_define_cli() formatting `-D` on the command line.
 *
 * Anything still unsupported is printed literally rather than
 * silently swallowed -- but note what that episode showed: printing
 * it literally does NOT make the argument list safe, because the
 * argument goes unconsumed and every conversion after it is reading
 * the wrong thing. A formatter has to know every conversion its
 * callers use; there is no graceful degradation.
 */

static void fmt_num(char *buf, int cap, int *n, uint32_t v, int base,
                    int neg, int width, int zero) {

    char tmp[12];
    int t = 0;

    if (!v) tmp[t++] = '0';
    while (v) {
        int d = (int)(v % (uint32_t)base);
        tmp[t++] = (char)(d < 10 ? '0' + d : 'a' + d - 10);
        v /= (uint32_t)base;
    }

    int len = t + (neg ? 1 : 0);
    if (neg && zero && *n < cap - 1) buf[(*n)++] = '-';
    while (len < width && *n < cap - 1) { buf[(*n)++] = zero ? '0' : ' '; len++; }
    if (neg && !zero && *n < cap - 1) buf[(*n)++] = '-';
    while (t && *n < cap - 1) buf[(*n)++] = tmp[--t];
}

int zcc_vfmt(char *buf, int cap, const char *fmt, va_list ap) {

    int n = 0;

    if (cap <= 0) return 0;

    for (; *fmt && n < cap - 1; fmt++) {

        if (*fmt != '%') { buf[n++] = *fmt; continue; }

        fmt++;
        if (*fmt == '%') { buf[n++] = '%'; continue; }

        int zero = 0, width = 0, left = 0, prec = -1;
        if (*fmt == '-') { left = 1; fmt++; }
        if (*fmt == '0') { zero = 1; fmt++; }
        if (*fmt == '*') { width = va_arg(ap, int); fmt++; }
        else while (*fmt >= '0' && *fmt <= '9') width = width * 10 + (*fmt++ - '0');
        if (width < 0) { left = 1; width = -width; }

        if (*fmt == '.') {
            fmt++;
            prec = 0;
            if (*fmt == '*') { prec = va_arg(ap, int); fmt++; }
            else while (*fmt >= '0' && *fmt <= '9') prec = prec * 10 + (*fmt++ - '0');
        }

        while (*fmt == 'l' || *fmt == 'z' || *fmt == 'h') fmt++;

        switch (*fmt) {
        case 's': {
            const char *s = va_arg(ap, const char *);
            if (!s) s = "(null)";
            int len = 0;
            while (s[len] && (prec < 0 || len < prec)) len++;
            int pad = width - len;
            if (!left) while (pad-- > 0 && n < cap - 1) buf[n++] = ' ';
            for (int k = 0; k < len && n < cap - 1; k++) buf[n++] = s[k];
            if (left) while (pad-- > 0 && n < cap - 1) buf[n++] = ' ';
            break;
        }
        case 'c':
            buf[n++] = (char)va_arg(ap, int);
            break;
        case 'd':
        case 'i': {
            int v = va_arg(ap, int);
            fmt_num(buf, cap, &n,
                    v < 0 ? (uint32_t)(-(int64_t)v) : (uint32_t)v,
                    10, v < 0, width, zero);
            break;
        }
        case 'u':
            fmt_num(buf, cap, &n, va_arg(ap, unsigned), 10, 0, width, zero);
            break;
        case 'x':
            fmt_num(buf, cap, &n, va_arg(ap, unsigned), 16, 0, width, zero);
            break;
        default:
            /* An unsupported conversion prints as itself. Swallowing
             * it would make the message quietly wrong, which is worse
             * than making it quietly ugly. */
            if (n < cap - 1) buf[n++] = '%';
            if (*fmt && n < cap - 1) buf[n++] = *fmt;
            break;
        }
    }

    buf[n] = 0;
    return n;
}

/*
 * Formats into a fresh allocation.
 */
static char zfmt_buf[ZCC_MSG_MAX];

char *zformat(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    zcc_vfmt(zfmt_buf, (int)sizeof(zfmt_buf), fmt, ap);
    va_end(ap);

    size_t n = strlen(zfmt_buf);
    char *p = zalloc(n + 1);
    memcpy(p, zfmt_buf, n + 1);
    return p;
}

/* One buffer for every message the compiler emits. 1KB is generous
 * for `file:line:col: error: ...` and bounded, which matters more on
 * the device: a diagnostic path that allocates can fail exactly when
 * it is most needed. */
static char zmsg_buf[ZCC_MSG_MAX];

void zcc_printf(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    zcc_vfmt(zmsg_buf, (int)sizeof(zmsg_buf), fmt, ap);
    va_end(ap);
    zio_out(zmsg_buf);
}

int zcc_snprintf(char *buf, int cap, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int n = zcc_vfmt(buf, cap, fmt, ap);
    va_end(ap);
    return n;
}

void zcc_fatal(const char *fmt, ...) {
    va_list ap;
    zio_out("zcc: ");
    va_start(ap, fmt);
    zcc_vfmt(zmsg_buf, (int)sizeof(zmsg_buf), fmt, ap);
    va_end(ap);
    zio_out(zmsg_buf);
    zio_out("\n");
    zio_exit(1);
}

/*
 * Diagnostics.
 *
 * Format is `file:line:col: error: message`, which is what every
 * editor's error parser already understands -- including `te`, which
 * is the editor this compiler will most often be driven from once it
 * runs on the device.
 *
 * There is no error recovery. A one-pass compiler that continues past
 * a parse error is generating code from a token stream it has lost its
 * place in, and every message after the first is invented. One true
 * message beats twenty plausible ones, especially on a screen that
 * holds twenty-five lines.
 */
void zcc_error(const char *file, int line, int col, const char *fmt, ...) {
    va_list ap;

    zcc_printf("%s:%d:%d: error: ", file ? file : "<input>", line, col);

    va_start(ap, fmt);
    zcc_vfmt(zmsg_buf, (int)sizeof(zmsg_buf), fmt, ap);
    va_end(ap);
    zio_out(zmsg_buf);
    zio_out("\n");

    zio_exit(1);
}
