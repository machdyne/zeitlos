/*
 * zfpga -- allocation, formatting, diagnostics, buffered I/O.
 */

#include <string.h>

#include "zfpga.h"

/* -- arena ------------------------------------------------------------ */

#define ARENA_BLOCK (64 * 1024)

static uint8_t *arena_p;
static size_t arena_left;

/* Every block is on a list, so that `zfpga build` can hand a stage's
 * memory back before the next (zf_mark / zf_release) and all four
 * stages fit in the one 4MB process the kernel gives zfpga. The counts
 * are what -v and `build` report. */
typedef struct blk { struct blk *next; size_t size; size_t pad; } blk_t;
static blk_t *blocks;
static size_t mem_now, mem_peak;

static void *block(size_t n) {
    blk_t *b = zio_block(sizeof(blk_t) + n);
    if (!b) zf_fatal("out of memory (%u bytes more, %u in use); zfpga needs the kernel's "
        "4MB tier (sw/os/kernel.h, docs/zfpga-test.md sec. 1)", (unsigned)n, (unsigned)mem_now);
    b->next = blocks;
    b->size = n;
    blocks = b;
    mem_now += n + sizeof(blk_t);
    if (mem_now > mem_peak) mem_peak = mem_now;
    return b + 1;
}

zf_mark_t zf_mark(void) {
    zf_mark_t m;
    m.blocks = blocks;
    m.p = arena_p;
    m.left = arena_left;
    return m;
}

void zf_release(zf_mark_t m) {
    while (blocks && blocks != (blk_t *)m.blocks) {
        blk_t *b = blocks;
        blocks = b->next;
        mem_now -= b->size + sizeof(blk_t);
        zdb_forget_if(b + 1);           /* a released database leaves the cache too */
        zio_free(b);
    }
    arena_p = m.p;
    arena_left = m.left;
}

size_t zf_mem_peak(void) { return mem_peak; }

/* Unzeroed, for a buffer about to be filled entirely (the database). */
void *zf_alloc_raw(size_t n) {
    return block((n + 3) & ~(size_t)3);
}

void *zf_alloc(size_t n) {
    void *p;
    n = (n + 3) & ~(size_t)3;
    if (n > ARENA_BLOCK / 2) {
        /* Big objects (the database, the CRAM) get a block of their
         * own rather than wasting the tail of the current one. */
        p = block(n);
        zf_memset(p, 0, n);
        return p;
    }
    if (n > arena_left) {
        arena_p = block(ARENA_BLOCK);
        arena_left = ARENA_BLOCK;
    }
    p = arena_p;
    arena_p += n;
    arena_left -= n;
    zf_memset(p, 0, n);
    return p;
}

char *zf_strdup(const char *s) {
    size_t n = zf_strlen(s) + 1;
    char *d = zf_alloc(n);
    zf_memcpy(d, s, n);
    return d;
}

/* -- strings ---------------------------------------------------------- */

size_t zf_strlen(const char *s) {
    size_t n = 0;
    while (s[n]) n++;
    return n;
}

/* Bytewise, unsigned: the order mkzdb.py sorts in, which is std::map's. */
int zf_strcmp(const char *a, const char *b) {
    /* C's strcmp compares as unsigned char, which is the order required;
     * libc's is word-at-a-time and was 15% of a run as a byte loop. */
    return strcmp(a, b);
}

int zf_streq(const char *a, const char *b) {
    return zf_strcmp(a, b) == 0;
}

/* newlib's, which are word-at-a-time and pull in nothing else. A byte
 * loop here was a measurable share of a device run: the CRAM alone is
 * 560KB to clear. */
void zf_memset(void *p, int c, size_t n) {
    memset(p, c, n);
}

void zf_memcpy(void *d, const void *s, size_t n) {
    memcpy(d, s, n);
}

int zf_parse_uint(const char *s, uint32_t *out) {
    uint32_t v = 0;
    int base = 10, any = 0;
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) { base = 16; s += 2; }
    for (; *s; s++) {
        int d;
        if (*s >= '0' && *s <= '9') d = *s - '0';
        else if (base == 16 && *s >= 'a' && *s <= 'f') d = *s - 'a' + 10;
        else if (base == 16 && *s >= 'A' && *s <= 'F') d = *s - 'A' + 10;
        else return 0;
        v = v * (uint32_t)base + (uint32_t)d;
        any = 1;
    }
    if (!any) return 0;
    *out = v;
    return 1;
}

/* -- formatting ------------------------------------------------------- */

static void put(char *out, int outlen, int *n, char c) {
    if (*n < outlen - 1) out[*n] = c;
    (*n)++;
}

static void put_uint(char *out, int outlen, int *n, uint32_t v, int base,
        int upper, int width, int zero) {
    char tmp[12];
    int i = 0;
    const char *dig = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    do { tmp[i++] = dig[v % (uint32_t)base]; v /= (uint32_t)base; } while (v);
    while (i < width) tmp[i++] = zero ? '0' : ' ';
    while (i) put(out, outlen, n, tmp[--i]);
}

int zf_vfmt(char *out, int outlen, const char *fmt, va_list ap) {
    int n = 0;
    for (; *fmt; fmt++) {
        int width = 0, zero = 0, left = 0;
        if (*fmt != '%') { put(out, outlen, &n, *fmt); continue; }
        fmt++;
        if (*fmt == '-') { left = 1; fmt++; }
        if (*fmt == '0') { zero = 1; fmt++; }
        while (*fmt >= '0' && *fmt <= '9') width = width * 10 + (*fmt++ - '0');
        switch (*fmt) {
        case 's': {
            const char *s = va_arg(ap, const char *);
            int len = 0;
            if (!s) s = "(null)";
            while (s[len]) len++;
            if (!left) while (len < width--) put(out, outlen, &n, ' ');
            while (*s) put(out, outlen, &n, *s++);
            if (left) while (len < width--) put(out, outlen, &n, ' ');
            break;
        }
        case 'd': {
            int v = va_arg(ap, int);
            if (v < 0) { put(out, outlen, &n, '-'); v = -v; if (width) width--; }
            put_uint(out, outlen, &n, (uint32_t)v, 10, 0, width, zero);
            break;
        }
        case 'u':
            put_uint(out, outlen, &n, va_arg(ap, unsigned), 10, 0, width, zero);
            break;
        case 'x':
            put_uint(out, outlen, &n, va_arg(ap, unsigned), 16, 0, width, zero);
            break;
        case 'X':
            put_uint(out, outlen, &n, va_arg(ap, unsigned), 16, 1, width, zero);
            break;
        case 'c':
            put(out, outlen, &n, (char)va_arg(ap, int));
            break;
        case '%':
            put(out, outlen, &n, '%');
            break;
        default:
            put(out, outlen, &n, '%');
            put(out, outlen, &n, *fmt);
            break;
        }
        if (!*fmt) break;
    }
    if (outlen > 0) out[n < outlen ? n : outlen - 1] = 0;
    return n;
}

int zf_fmt(char *out, int outlen, const char *fmt, ...) {
    va_list ap;
    int n;
    va_start(ap, fmt);
    n = zf_vfmt(out, outlen, fmt, ap);
    va_end(ap);
    return n;
}

void zf_print(const char *fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    zf_vfmt(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    zio_out(buf);
}

void zf_note(const char *fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    zf_vfmt(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    zio_out("zfpga: ");
    zio_out(buf);
    zio_out("\n");
}

void zf_fatal_at(const char *file, int line, const char *fmt, ...) {
    char buf[512], pre[256];
    va_list ap;
    va_start(ap, fmt);
    zf_vfmt(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    zf_fmt(pre, sizeof(pre), "%s:%d: error: ", file, line);
    zio_out(pre);
    zio_out(buf);
    zio_out("\n");
    zio_console("zfpga: ");
    zio_console(pre);
    zio_console(buf);
    zio_console("\n");
    zio_exit(1);
}

void zf_fatal(const char *fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    zf_vfmt(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    zio_out("zfpga: error: ");
    zio_out(buf);
    zio_out("\n");
    zio_console("zfpga: error: ");
    zio_console(buf);
    zio_console("\n");
    zio_exit(1);
}

/* -- reading ---------------------------------------------------------- */

void zf_reader_open(zf_reader_t *r, const char *path) {
    zf_memset(r, 0, sizeof(*r));
    r->path = path;
    r->f = zio_open_read(path);
    if (!r->f) zf_fatal("cannot open %s%s", path, zf_83_hint(path));
}

static int reader_fill(zf_reader_t *r) {
    int n;
    if (r->eof) return 0;
    n = zio_read(r->f, r->buf, (int)sizeof(r->buf));
    if (n < 0) zf_fatal("read error on %s", r->path);
    if (n == 0) { r->eof = 1; return 0; }
    r->len = n;
    r->pos = 0;
    return 1;
}

char *zf_reader_line(zf_reader_t *r) {
    int n = 0, got = 0;
    for (;;) {
        const char *nl;
        int take, avail;
        if (r->pos >= r->len && !reader_fill(r)) break;
        got = 1;
        avail = r->len - r->pos;
        nl = memchr(r->buf + r->pos, '\n', (size_t)avail);
        take = nl ? (int)(nl - (r->buf + r->pos)) : avail;
        if (n + take > ZF_LINE_MAX - 1)
            zf_fatal_at(r->path, r->lineno + 1, "line longer than %d bytes",
                ZF_LINE_MAX - 1);
        zf_memcpy(r->line + n, r->buf + r->pos, (size_t)take);
        n += take;
        r->pos += take;
        if (nl) { r->pos++; break; }
    }
    if (!got) return NULL;
    while (n && r->line[n - 1] == '\r') n--;
    r->line[n] = 0;
    r->lineno++;
    return r->line;
}

void zf_reader_close(zf_reader_t *r) {
    if (r->f) zio_close(r->f);
    r->f = NULL;
}

uint8_t *zf_read_all(const char *path, uint32_t *len) {
    /* Grows by doubling into fresh arena blocks. Only the database uses
     * this, and it knows its own size from its header, so in practice
     * zdb_load() reads the header first and calls the sized path. */
    zio_file_t *f = zio_open_read(path);
    uint32_t cap = 64 * 1024, n = 0;
    uint8_t *buf;
    if (!f) return NULL;
    buf = zf_alloc(cap);
    for (;;) {
        int r;
        if (n == cap) {
            uint8_t *nb = zf_alloc(cap * 2);
            zf_memcpy(nb, buf, n);
            buf = nb;
            cap *= 2;
        }
        r = zio_read(f, buf + n, (int)(cap - n));
        if (r < 0) zf_fatal("read error on %s", path);
        if (r == 0) break;
        n += (uint32_t)r;
    }
    zio_close(f);
    *len = n;
    return buf;
}

/* -- writing ---------------------------------------------------------- */

void zf_writer_open(zf_writer_t *w, const char *path) {
    zf_memset(w, 0, sizeof(*w));
    w->path = path;
    w->f = zio_open_write(path);
    if (!w->f) zf_fatal("cannot create %s%s", path, zf_83_hint(path));
}

static void writer_flush(zf_writer_t *w) {
    if (w->len && zio_write(w->f, w->buf, w->len) != w->len)
        zf_fatal("write error on %s", w->path);
    w->len = 0;
}

void zf_writer_byte(zf_writer_t *w, uint8_t b) {
    if (w->len == (int)sizeof(w->buf)) writer_flush(w);
    w->buf[w->len++] = b;
    w->total++;
}

void zf_writer_bytes(zf_writer_t *w, const uint8_t *p, uint32_t n) {
    while (n) {
        uint32_t room = (uint32_t)sizeof(w->buf) - (uint32_t)w->len, k;
        if (!room) { writer_flush(w); continue; }
        k = n < room ? n : room;
        zf_memcpy(w->buf + w->len, p, k);
        w->len += (int)k;
        w->total += k;
        p += k;
        n -= k;
    }
}

void zf_writer_close(zf_writer_t *w) {
    writer_flush(w);
    if (zio_close(w->f) != 0) zf_fatal("error closing %s", w->path);
    w->f = NULL;
}

/* -- tokens ----------------------------------------------------------- */

int zf_tokens(char *line, char **tok, int max) {
    int n = 0;
    char *p = line;
    for (;;) {
        while (*p == ' ' || *p == '\t') p++;
        if (!*p || *p == '#') break;
        if (n == max) return n;
        /* '#' is a comment only at the start of a token, as in
         * libtrellis, where `>>` reads "foo#bar" as one word. */
        tok[n++] = p;
        while (*p && *p != ' ' && *p != '\t') p++;
        if (*p) *p++ = 0;
    }
    return n;
}

/* 8.3: every component of the path an 8-character name with at most a
 * 3-character extension. The card's FatFs has no long names, and a name
 * that does not fit simply cannot be found or made -- which the first
 * on-board run met as "no database" for a file that was plainly there
 * (docs/zfpga.md sec. 22). */
int zf_is_83(const char *path) {
    const char *p = path;
    while (*p) {
        int stem = 0, ext = 0, dot = 0;
        if (*p == '/') { p++; continue; }
        for (; *p && *p != '/'; p++) {
            if (*p == '.') {
                if (dot || !stem) return 0;
                dot = 1;
            } else if (dot) {
                ext++;
            } else {
                stem++;
            }
        }
        if (stem > 8 || ext > 3) return 0;
    }
    return 1;
}

/* The end of an error message about a path the card cannot hold. */
const char *zf_83_hint(const char *path) {
    if (!zio_fat83() || zf_is_83(path)) return "";
    return " -- not an 8.3 name, and the card's filesystem has no long names";
}

int zf_memcmp(const void *a, const void *b, size_t n) {
    const uint8_t *p = a, *q = b;
    size_t i;
    for (i = 0; i < n; i++) if (p[i] != q[i]) return p[i] < q[i] ? -1 : 1;
    return 0;
}
