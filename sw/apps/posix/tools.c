/*
 * posix -- find, diff, unzip and tar (docs/posix.md, "find, diff,
 * unzip, tar").
 *
 * Builtins like the ones in sh.c, kept in a file of their own because
 * each is a small program rather than a few lines over a syscall. Like
 * sh.c there is no port, message or window code here, so all four run
 * in the host tests (tests/).
 *
 * All four are long-running on a big tree or file, so all four check
 * px_intr() as they go: Ctrl+C stops them at the next entry, line or
 * 4 KB of output, and on the machine that is also where they hand back
 * to the main loop (job.c). Their state is in statics, which is one
 * reason only one command runs at a time. Memory comes from px_malloc()
 * (job.c), so a command stopped without unwinding gives it back.
 */

#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include "zeitlos.h"
#include "zfsapp.h"
#include "../../common/zglob.h"
#include "../../common/formats/zf.h"
#include "../../common/formats/zf_zip.h"
#include "posix.h"

/* ------------------------------------------------------------------ */
/* find                                                                */

/*
 * Depth-first, each directory's entries sorted as `ls` sorts them, the
 * starting point first. No recursion: the walk is a stack of frames,
 * one per directory being listed, and the names a frame has still to
 * visit are kept in one arena, the deepest directory's on top. So the
 * memory is fixed -- FIND_DEPTH frames and FIND_ARENA bytes of names --
 * and a tree that does not fit says where, rather than overflowing a
 * stack nobody can see.
 */
#define FIND_DEPTH  32          /* directories below a starting point */
#define FIND_ARENA  16384       /* names waiting to be visited, all levels */
#define FIND_LIST   256         /* entries one directory can list (as ls) */

typedef struct {
    uint16_t next, end;         /* this directory's names left, in the arena */
    uint16_t plen;              /* length of its path in find_path */
} find_frame_t;

static find_frame_t find_stack[FIND_DEPTH + 1];
static char find_arena[FIND_ARENA];
static char find_list[8192];
static uint8_t find_types[FIND_LIST];
static uint16_t find_order[FIND_LIST];
static char find_path[PX_PATH_MAX];

typedef struct {
    const char *name;           /* -name pattern, or NULL */
    int type;                   /* 0 any, 'f' or 'd' */
    int depth;                  /* frames in use */
    int top;                    /* arena bytes in use */
} find_t;

static const char *base_of(const char *p) {
    const char *s = strrchr(p, '/');
    return (s && s[1]) ? s + 1 : p;
}

static void find_emit(px_shell_t *sh, find_t *f, const char *path, bool dir) {
    if (f->type == 'f' && dir) return;
    if (f->type == 'd' && !dir) return;
    if (f->name && !z_glob_match(f->name, base_of(path))) return;
    px_printf(sh, "%s\n", path);
}

/* Lists find_path into the arena, sorted, as a new frame. Each name is
 * stored as a type byte ('d' or 'f'), the name and a NUL. */
static bool find_push(px_shell_t *sh, find_t *f) {

    char abs[PX_PATH_MAX];
    uint32_t count = 0, truncated = 0;
    const char *names[FIND_LIST];

    if (f->depth >= FIND_DEPTH) {
        px_printf(sh, "find: %s: more than %d levels deep, not entered\n",
                  find_path, FIND_DEPTH);
        return false;
    }
    if (!px_resolve(find_path, abs, sizeof(abs)) ||
        !fs_list_into(abs, find_list, sizeof(find_list), find_types,
                      FIND_LIST, &count, &truncated)) {
        px_printf(sh, "find: %s: cannot read\n", find_path);
        return false;
    }

    const char *p = find_list;
    for (uint32_t i = 0; i < count; i++) {
        names[i] = base_of(p);
        find_order[i] = (uint16_t)i;
        p += strlen(p) + 1;
    }
    for (uint32_t i = 1; i < count; i++) {
        uint16_t k = find_order[i];
        uint32_t j = i;
        while (j > 0 && strcmp(names[find_order[j - 1]], names[k]) > 0) {
            find_order[j] = find_order[j - 1];
            j--;
        }
        find_order[j] = k;
    }

    int start = f->top, o = f->top;
    for (uint32_t i = 0; i < count; i++) {
        const char *n = names[find_order[i]];
        int len = (int)strlen(n);
        if (o + len + 2 > FIND_ARENA) {
            px_printf(sh, "find: %s: too many names waiting (%d bytes), not entered\n",
                      find_path, FIND_ARENA);
            return false;
        }
        find_arena[o++] = find_types[find_order[i]] == Z_FS_TYPE_DIR ? 'd' : 'f';
        memcpy(find_arena + o, n, (size_t)len + 1);
        o += len + 1;
    }

    find_frame_t *fr = &find_stack[f->depth++];
    fr->next = (uint16_t)start;
    fr->end = (uint16_t)o;
    fr->plen = (uint16_t)strlen(find_path);
    f->top = o;

    if (truncated) {
        px_printf(sh, "find: %s: more than %d entries, the rest not listed\n",
                  find_path, FIND_LIST);
        return false;           /* listed what fits, but say so */
    }
    return true;
}

/* One starting point. False if anything along the way was reported. */
static bool find_walk(px_shell_t *sh, find_t *f, const char *start) {

    char abs[PX_PATH_MAX];
    z_fs_info_t fi;
    bool ok = true, dir;
    int n = (int)strlen(start);

    if (n >= PX_PATH_MAX) { px_printf(sh, "find: %s: path too long\n", start); return false; }
    memcpy(find_path, start, (size_t)n + 1);
    while (n > 1 && find_path[n - 1] == '/') find_path[--n] = 0;    /* src/ is src */

    if (!px_resolve(find_path, abs, sizeof(abs))) {
        px_printf(sh, "find: %s: bad path\n", start);
        return false;
    }
    if (!strcmp(abs, "/")) dir = true;
    else if (fs_stat(abs, &fi)) dir = (fi.type == Z_FS_TYPE_DIR);
    else { px_printf(sh, "find: %s: no such file or directory\n", start); return false; }

    find_emit(sh, f, find_path, dir);
    if (!dir) return true;

    f->depth = 0;
    f->top = 0;
    if (!find_push(sh, f)) ok = false;

    while (f->depth > 0) {

        if (px_intr(sh)) return false;

        find_frame_t *fr = &find_stack[f->depth - 1];
        if (fr->next >= fr->end) {
            f->depth--;
            /* The arena is a stack too: this frame's names were the
             * top, and they are all visited. */
            f->top = f->depth ? find_stack[f->depth - 1].end : 0;
            continue;
        }

        const char *e = find_arena + fr->next;
        bool sub = (e[0] == 'd');
        const char *name = e + 1;
        int len = (int)strlen(name);
        fr->next = (uint16_t)(fr->next + len + 2);

        int pl = fr->plen;
        bool slash = !(pl == 1 && find_path[0] == '/');
        if (pl + (slash ? 1 : 0) + len + 1 > PX_PATH_MAX) {
            find_path[pl] = 0;
            px_printf(sh, "find: %s/%s: path too long\n", find_path, name);
            ok = false;
            continue;
        }
        find_path[pl] = 0;
        if (slash) strcat(find_path, "/");
        strcat(find_path, name);

        find_emit(sh, f, find_path, sub);
        if (sub && !find_push(sh, f)) ok = false;
    }
    return ok;
}

void px_bi_find(px_shell_t *sh, int argc, char **argv) {

    find_t f;
    int first = 1, ndirs = 0;

    memset(&f, 0, sizeof(f));

    /* The starting points come first, then the tests. */
    while (first + ndirs < argc && argv[first + ndirs][0] != '-') ndirs++;

    for (int i = first + ndirs; i < argc; i++) {
        if (!strcmp(argv[i], "-name") && i + 1 < argc) {
            f.name = argv[++i];
        } else if (!strcmp(argv[i], "-type") && i + 1 < argc &&
                   (!strcmp(argv[i + 1], "f") || !strcmp(argv[i + 1], "d"))) {
            f.type = argv[++i][0];
        } else {
            px_puts(sh, "usage: find [DIR...] [-name PATTERN] [-type f|d]\n");
            sh->status = 1;
            return;
        }
    }

    if (!ndirs) {
        if (!find_walk(sh, &f, ".")) sh->status = 1;
        return;
    }
    for (int i = 0; i < ndirs && !px_intr(sh); i++)
        if (!find_walk(sh, &f, argv[first + i])) sh->status = 1;
}

/* ------------------------------------------------------------------ */
/* diff                                                                */

/*
 * diff FILE1 FILE2, in unified form with 3 lines of context -- what
 * `diff -u` prints, without the timestamps on the --- and +++ lines.
 *
 * Both files are read into memory (DIFF_BYTES and DIFF_LINES each, so
 * the worst case is known: about 800 KB, malloc()ed for the one command
 * and freed after), and compared with Myers' algorithm in its linear-
 * space form -- the middle snake, as GNU diff does it. Its recursion
 * halves the work each level, so it is a few levels deep, not one per
 * line.
 *
 * A region whose differences take more than DIFF_COST steps to pin down
 * is reported as all removed and all added: still a correct diff, just
 * not the shortest one -- GNU diff gives up early on such regions too.
 * That bounds the time on two files that have nothing in common.
 */
#define DIFF_BYTES  (256u * 1024u)
#define DIFF_LINES  8192
#define DIFF_COST   1024
#define DIFF_CTX    3

typedef struct {
    char        *text;
    uint32_t    *off;           /* line i is text[off[i] .. off[i+1]) */
    uint32_t    *hash;
    int         n;
    bool        nl_end;         /* the last line ends in a newline */
    bool        binary;
} diff_file_t;

typedef struct {
    diff_file_t a, b;
    int         *fd, *bd;       /* the two diagonal vectors */
    int         voff;
    uint8_t     *del, *ins;     /* a's lines removed, b's added */
    px_shell_t  *sh;
    bool        stopped;
} diff_t;

/* 2 on trouble, having said what it was. */
static int diff_read(px_shell_t *sh, const char *path, diff_file_t *f) {

    char abs[PX_PATH_MAX];
    int h, got;
    uint32_t len = 0;

    if (!px_resolve(path, abs, sizeof(abs))) {
        px_printf(sh, "diff: %s: bad path\n", path);
        return 2;
    }
    if ((h = fs_open_read(abs)) < 0) {
        px_printf(sh, "diff: %s: cannot open\n", path);
        return 2;
    }
    f->text = px_malloc(DIFF_BYTES + 1);
    if (!f->text) { fs_close_handle(h); px_puts(sh, "diff: out of memory\n"); return 2; }

    for (;;) {
        uint32_t want = DIFF_BYTES + 1 - len;
        if (want > 4096) want = 4096;
        got = fs_read_chunk(h, f->text + len, (int)want);
        if (got < 0) {
            fs_close_handle(h);
            px_printf(sh, "diff: %s: read failed\n", path);
            return 2;
        }
        if (!got) break;
        len += (uint32_t)got;
        if (len > DIFF_BYTES) {
            fs_close_handle(h);
            px_printf(sh, "diff: %s: larger than %u KB, not compared\n",
                      path, (unsigned)(DIFF_BYTES / 1024));
            return 2;
        }
        if (px_intr(sh)) { fs_close_handle(h); return 2; }
    }
    fs_close_handle(h);

    int n = 0;
    for (uint32_t i = 0; i < len; i++) {
        if (!f->text[i]) f->binary = true;
        if (f->text[i] == '\n') n++;
    }
    f->nl_end = (len == 0 || f->text[len - 1] == '\n');
    if (!f->nl_end) n++;
    if (n > DIFF_LINES) {
        px_printf(sh, "diff: %s: more than %d lines, not compared\n", path, DIFF_LINES);
        return 2;
    }

    f->off = px_malloc(sizeof(uint32_t) * (size_t)(n + 1));
    f->hash = px_malloc(sizeof(uint32_t) * (size_t)(n + 1));
    if (!f->off || !f->hash) { px_puts(sh, "diff: out of memory\n"); return 2; }

    /* FNV-1a over each line, newline included: the last line with no
     * newline is a different line from the same text with one, as it
     * is to GNU diff. */
    uint32_t h32 = 2166136261u, s = 0;
    int k = 0;
    for (uint32_t i = 0; i < len; i++) {
        h32 = (h32 ^ (uint8_t)f->text[i]) * 16777619u;
        if (f->text[i] == '\n' || i == len - 1) {
            f->off[k] = s;
            f->hash[k] = h32;
            k++;
            s = i + 1;
            h32 = 2166136261u;
        }
    }
    f->off[k] = len;
    f->n = k;
    return 0;
}

static bool diff_eq(diff_t *d, int x, int y) {
    const diff_file_t *a = &d->a, *b = &d->b;
    uint32_t la = a->off[x + 1] - a->off[x], lb = b->off[y + 1] - b->off[y];
    return a->hash[x] == b->hash[y] && la == lb &&
           !memcmp(a->text + a->off[x], b->text + b->off[y], la);
}

/*
 * The middle snake of a[xoff..xlim) against b[yoff..ylim): the point
 * where a shortest edit path crosses the middle. As GNU diff's diag(),
 * without its heuristics. -1 if it costs more than DIFF_COST.
 */
static int diff_snake(diff_t *d, int xoff, int xlim, int yoff, int ylim,
                      int *mx, int *my) {

    int *fd = d->fd + d->voff, *bd = d->bd + d->voff;
    int dmin = xoff - ylim, dmax = xlim - yoff;
    int fmid = xoff - yoff, bmid = xlim - ylim;
    int fmin = fmid, fmax = fmid, bmin = bmid, bmax = bmid;
    bool odd = ((fmid - bmid) & 1) != 0;

    fd[fmid] = xoff;
    bd[bmid] = xlim;

    for (int c = 1; ; c++) {

        int k;

        if (c > DIFF_COST) return -1;
        if (px_intr(d->sh)) { d->stopped = true; return -1; }

        if (fmin > dmin) fd[--fmin - 1] = -1; else ++fmin;
        if (fmax < dmax) fd[++fmax + 1] = -1; else --fmax;
        for (k = fmax; k >= fmin; k -= 2) {
            int lo = fd[k - 1], hi = fd[k + 1];
            int x = lo >= hi ? lo + 1 : hi, y;
            y = x - k;
            while (x < xlim && y < ylim && diff_eq(d, x, y)) { x++; y++; }
            fd[k] = x;
            if (odd && bmin <= k && k <= bmax && bd[k] <= x) {
                *mx = x; *my = y;
                return 2 * c - 1;
            }
        }

        if (bmin > dmin) bd[--bmin - 1] = 0x7fffffff; else ++bmin;
        if (bmax < dmax) bd[++bmax + 1] = 0x7fffffff; else --bmax;
        for (k = bmax; k >= bmin; k -= 2) {
            int lo = bd[k - 1], hi = bd[k + 1];
            int x = lo < hi ? lo : hi - 1, y;
            y = x - k;
            while (x > xoff && y > yoff && diff_eq(d, x - 1, y - 1)) { x--; y--; }
            bd[k] = x;
            if (!odd && fmin <= k && k <= fmax && x <= fd[k]) {
                *mx = x; *my = y;
                return 2 * c;
            }
        }
    }
}

/* Marks what changed between a[xoff..xlim) and b[yoff..ylim). Recursive,
 * but each level halves the cost of what is left, so it goes about
 * log2(DIFF_COST) levels deep, not one per line. */
static void diff_seq(diff_t *d, int xoff, int xlim, int yoff, int ylim) {

    while (xoff < xlim && yoff < ylim && diff_eq(d, xoff, yoff)) { xoff++; yoff++; }
    while (xlim > xoff && ylim > yoff && diff_eq(d, xlim - 1, ylim - 1)) { xlim--; ylim--; }

    if (xoff == xlim) { while (yoff < ylim) d->ins[yoff++] = 1; return; }
    if (yoff == ylim) { while (xoff < xlim) d->del[xoff++] = 1; return; }
    if (d->stopped) return;

    int x, y;
    if (diff_snake(d, xoff, xlim, yoff, ylim, &x, &y) < 0) {
        while (xoff < xlim) d->del[xoff++] = 1;
        while (yoff < ylim) d->ins[yoff++] = 1;
        return;
    }
    diff_seq(d, xoff, x, yoff, y);
    diff_seq(d, x, xlim, y, ylim);
}

static void diff_line(px_shell_t *sh, char mark, const diff_file_t *f, int i) {
    uint32_t s = f->off[i], e = f->off[i + 1];
    char m[2] = { mark, 0 };
    px_puts(sh, m);
    /* through px_printf's buffer would cut a long line: written whole */
    while (s < e) {
        char chunk[256];
        uint32_t k = e - s < sizeof(chunk) - 1 ? e - s : (uint32_t)sizeof(chunk) - 1;
        memcpy(chunk, f->text + s, k);
        chunk[k] = 0;
        px_puts(sh, chunk);
        s += k;
    }
    if (i == f->n - 1 && !f->nl_end) px_puts(sh, "\n\\ No newline at end of file\n");
}

static void diff_range(px_shell_t *sh, int start, int count) {
    if (count == 1) px_printf(sh, "%d", start + 1);
    else px_printf(sh, "%d,%d", count ? start + 1 : start, count);
}

/* The hunks. A change is a run of removed lines in a and added lines in
 * b; changes closer than 2 * DIFF_CTX unchanged lines share a hunk. */
static void diff_print(diff_t *d, const char *n1, const char *n2) {

    px_shell_t *sh = d->sh;
    int na = d->a.n, nb = d->b.n;
    int i = 0, j = 0;
    bool header = false;

    for (;;) {

        /* the next change */
        while (i < na && j < nb && !d->del[i] && !d->ins[j]) { i++; j++; }
        if (i >= na && j >= nb) return;
        if (px_intr(sh)) return;

        int hs_a = i - DIFF_CTX < 0 ? 0 : i - DIFF_CTX;
        int hs_b = j - (i - hs_a);

        /* how far the hunk runs: change after change while the
         * unchanged lines between are few enough */
        int ei = i, ej = j;
        for (;;) {
            while (ei < na && d->del[ei]) ei++;
            while (ej < nb && d->ins[ej]) ej++;
            int k = 0;
            while (ei + k < na && ej + k < nb && !d->del[ei + k] && !d->ins[ej + k] &&
                   k <= 2 * DIFF_CTX) k++;
            bool more = (ei + k < na && d->del[ei + k]) || (ej + k < nb && d->ins[ej + k]);
            if (k <= 2 * DIFF_CTX && more) { ei += k; ej += k; continue; }
            break;
        }
        int ctx = 0;
        while (ctx < DIFF_CTX && ei + ctx < na && ej + ctx < nb) ctx++;
        int he_a = ei + ctx, he_b = ej + ctx;

        if (!header) {
            px_printf(sh, "--- %s\n+++ %s\n", n1, n2);
            header = true;
        }
        px_puts(sh, "@@ -");
        diff_range(sh, hs_a, he_a - hs_a);
        px_puts(sh, " +");
        diff_range(sh, hs_b, he_b - hs_b);
        px_puts(sh, " @@\n");

        int x = hs_a, y = hs_b;
        while (x < he_a || y < he_b) {
            if ((x < he_a && d->del[x]) || (y < he_b && d->ins[y])) {
                while (x < he_a && d->del[x]) diff_line(sh, '-', &d->a, x++);
                while (y < he_b && d->ins[y]) diff_line(sh, '+', &d->b, y++);
            } else {
                diff_line(sh, ' ', &d->a, x);
                x++; y++;
            }
        }
        i = he_a;
        j = he_b;
    }
}

static void diff_free(diff_t *d) {
    px_free(d->a.text); px_free(d->a.off); px_free(d->a.hash);
    px_free(d->b.text); px_free(d->b.off); px_free(d->b.hash);
    px_free(d->fd); px_free(d->bd); px_free(d->del); px_free(d->ins);
}

void px_bi_diff(px_shell_t *sh, int argc, char **argv) {

    diff_t d;
    int r;

    if (argc != 3) {
        px_puts(sh, "usage: diff FILE1 FILE2\n");
        sh->status = 2;
        return;
    }

    memset(&d, 0, sizeof(d));
    d.sh = sh;

    if ((r = diff_read(sh, argv[1], &d.a)) != 0 || (r = diff_read(sh, argv[2], &d.b)) != 0) {
        diff_free(&d);
        sh->status = 2;
        return;
    }

    if (d.a.binary || d.b.binary) {
        uint32_t la = d.a.off[d.a.n], lb = d.b.off[d.b.n];
        bool same = la == lb && !memcmp(d.a.text, d.b.text, la);
        if (!same) px_printf(sh, "Binary files %s and %s differ\n", argv[1], argv[2]);
        sh->status = same ? 0 : 1;
        diff_free(&d);
        return;
    }

    int vlen = d.a.n + d.b.n + 3;
    d.fd = px_malloc(sizeof(int) * (size_t)vlen);
    d.bd = px_malloc(sizeof(int) * (size_t)vlen);
    d.del = px_calloc((size_t)d.a.n + 1, 1);
    d.ins = px_calloc((size_t)d.b.n + 1, 1);
    d.voff = d.b.n + 1;
    if (!d.fd || !d.bd || !d.del || !d.ins) {
        px_puts(sh, "diff: out of memory\n");
        diff_free(&d);
        sh->status = 2;
        return;
    }

    diff_seq(&d, 0, d.a.n, 0, d.b.n);

    bool differ = false;
    for (int i = 0; i < d.a.n && !differ; i++) if (d.del[i]) differ = true;
    for (int i = 0; i < d.b.n && !differ; i++) if (d.ins[i]) differ = true;

    if (!d.stopped && differ) diff_print(&d, argv[1], argv[2]);
    sh->status = differ ? 1 : 0;
    diff_free(&d);
}

/* ------------------------------------------------------------------ */
/* unzip and tar                                                       */

/*
 * Through sw/common/formats (zf.h): zf_extract_zip(), zf_extract_tar()
 * and zf_extract_tgz(), with a zf_dir_t that writes under the target
 * directory. The formats check every name before anything is written
 * -- an absolute path or a `..` that climbs out is refused -- cap each
 * entry at ZF_ENTRY_MAX, check CRCs, and remove a file left half-
 * written by an error or by Ctrl+C (zf_cancel_poll, polled every 4 KB).
 *
 * The 32 KB inflate window is zf_zip.c's own, static, the same one
 * zcat and gunzip borrow (zz_window_take()) -- there is one in this
 * program, not two. Nothing here needs zf_heap: only the document and
 * table readers use it.
 *
 * An existing file is never overwritten unless -o is given. unzip
 * checks the whole archive for that before writing anything; tar,
 * which can only be read once from front to back, stops at the first.
 */

typedef struct {
    px_shell_t  *sh;
    char        root[PX_PATH_MAX];  /* absolute */
    bool        list;               /* tar -t: names only, nothing written */
    bool        overwrite;
    const char  *cmd;
    const char  *file;              /* the archive, as typed */
    uint32_t    files, bytes;
    char        exists[ZF_PATH_MAX];
} arch_t;

static arch_t arch;

static bool arch_join(char *out, int cap, const char *rel) {
    int n = snprintf(out, (size_t)cap, "%s%s%s", arch.root,
                     strcmp(arch.root, "/") ? "/" : "", rel);
    return n > 0 && n < cap;
}

static int32_t null_write(zf_io_t *io, const void *buf, uint32_t n) {
    (void)io; (void)buf;
    arch.bytes += n;
    return (int32_t)n;
}

static int32_t file_write(zf_io_t *io, const void *buf, uint32_t n) {
    int h = (int)(intptr_t)io->h;
    int r = fs_write_chunk(h, buf, (int)n);
    if (r != (int)n) return -1;
    arch.bytes += n;
    return (int32_t)n;
}

static bool d_open(zf_dir_t *d, const char *rel, zf_io_t *out) {
    char p[PX_PATH_MAX];
    z_fs_info_t fi;
    (void)d;
    memset(out, 0, sizeof(*out));
    if (!arch.list) {
        if (!arch_join(p, sizeof(p), rel)) return false;
        if (!arch.overwrite && fs_stat(p, &fi)) {
            snprintf(arch.exists, sizeof(arch.exists), "%s", rel);
            return false;
        }
    }
    arch.files++;
    px_printf(arch.sh, "%s\n", rel);
    if (arch.list) { out->write = null_write; return true; }
    int h = fs_open_write(p);
    if (h < 0) return false;
    out->h = (void *)(intptr_t)h;
    out->write = file_write;
    return true;
}

static void d_close(zf_dir_t *d, zf_io_t *out) {
    (void)d;
    if (out->write == file_write) fs_close_handle((int)(intptr_t)out->h);
}

static bool d_mkdir(zf_dir_t *d, const char *rel) {
    char p[PX_PATH_MAX];
    z_fs_info_t fi;
    (void)d;
    if (arch.list) return true;
    if (!arch_join(p, sizeof(p), rel)) return false;
    if (fs_stat(p, &fi)) return fi.type == Z_FS_TYPE_DIR;
    return fs_mkdir(p) != 0;
}

static void d_remove(zf_dir_t *d, const char *rel) {
    char p[PX_PATH_MAX];
    (void)d;
    if (!arch.list && arch_join(p, sizeof(p), rel)) fs_unlink(p);
}

/* The archive, read through fs_* (zip seeks; tar reads straight on). */
static int32_t in_read(zf_io_t *io, void *buf, uint32_t n) {
    int r = fs_read_chunk((int)(intptr_t)io->h, buf, (int)n);
    if (r < 0) return ZF_E_IO;
    io->pos += (uint32_t)r;
    return r;
}

static bool in_seek(zf_io_t *io, uint32_t pos) {
    if (!fs_seek((int)(intptr_t)io->h, pos)) return false;
    io->pos = pos;
    return true;
}

static bool arch_cancel(void) { return px_intr(arch.sh); }

/* Opens the archive and the target directory (made if it is not
 * there). -1 on trouble, having said what. */
static int arch_open(const char *path, const char *dest, zf_io_t *io, bool list) {

    char abs[PX_PATH_MAX];
    z_fs_info_t fi;
    px_shell_t *sh = arch.sh;

    arch.file = path;
    if (!px_resolve(path, abs, sizeof(abs))) {
        px_printf(sh, "%s: %s: bad path\n", arch.cmd, path);
        return -1;
    }
    memset(io, 0, sizeof(*io));
    int h = fs_open_read(abs);
    if (h < 0) {
        px_printf(sh, "%s: %s: cannot open\n", arch.cmd, path);
        return -1;
    }
    io->h = (void *)(intptr_t)h;
    io->read = in_read;
    io->seek = in_seek;
    io->size = (uint32_t)fs_size(abs);

    if (list) return 0;

    if (!px_resolve(dest ? dest : ".", arch.root, sizeof(arch.root))) {
        px_printf(sh, "%s: %s: bad path\n", arch.cmd, dest);
        fs_close_handle(h);
        return -1;
    }
    if (strcmp(arch.root, "/")) {
        if (!fs_stat(arch.root, &fi)) {
            if (!fs_mkdir(arch.root)) {
                px_printf(sh, "%s: %s: cannot make the directory\n", arch.cmd, dest);
                fs_close_handle(h);
                return -1;
            }
        } else if (fi.type != Z_FS_TYPE_DIR) {
            px_printf(sh, "%s: %s: not a directory\n", arch.cmd, dest);
            fs_close_handle(h);
            return -1;
        }
    }
    return 0;
}

/* Reports how it went, and sets the status. */
static void arch_done(int r, zf_io_t *io) {

    px_shell_t *sh = arch.sh;

    fs_close_handle((int)(intptr_t)io->h);
    zf_cancel_poll = NULL;

    if (r == ZF_E_CANCEL || px_intr(sh)) {
        sh->status = 1;     /* the shell prints ^C and sets 130 */
        return;
    }
    if (arch.exists[0]) {
        px_printf(sh, "%s: %s exists (-o to overwrite)\n", arch.cmd, arch.exists);
        sh->status = 1;
        return;
    }
    if (r < 0) {
        px_printf(sh, "%s: %s: %s\n", arch.cmd, arch.file, zf_strerror(r));
        sh->status = 1;
        return;
    }
    px_printf(sh, "%lu file%s, %lu bytes\n", (unsigned long)arch.files,
              arch.files == 1 ? "" : "s", (unsigned long)arch.bytes);
}

static void arch_begin(px_shell_t *sh, const char *cmd) {
    memset(&arch, 0, sizeof(arch));
    arch.sh = sh;
    arch.cmd = cmd;
}

/* unzip -l: the central directory alone, nothing inflated. */
static void unzip_list(zf_io_t *io) {
    static zz_t z;
    static zz_entry_t e;
    px_shell_t *sh = arch.sh;
    int r = zz_open(&z, io);
    while (r >= 0 && (r = zz_next(&z, &e)) == 1) {
        if (px_intr(sh)) break;
        size_t n = strlen(e.name);
        px_printf(sh, "%10lu  %s\n", (unsigned long)e.usize, e.name);
        if (n && e.name[n - 1] != '/' && e.name[n - 1] != '\\') arch.files++;
        arch.bytes += e.usize;
    }
    arch_done(r < 0 ? r : ZF_OK, io);
}

/* Before writing anything: is any file in the archive there already?
 * The name as zf_arch.c will make it, near enough -- backslashes are
 * separators and "./" means nothing; anything this misses, d_open()
 * still refuses. */
static bool unzip_clash(zf_io_t *io) {
    static zz_t z;
    static zz_entry_t e;
    char rel[ZF_PATH_MAX], p[PX_PATH_MAX];
    z_fs_info_t fi;
    if (zz_open(&z, io) < 0) return false;          /* zf_extract_zip() says why */
    while (zz_next(&z, &e) == 1) {
        int n = (int)strlen(e.name);
        if (!n || e.name[n - 1] == '/' || e.name[n - 1] == '\\') continue;
        const char *s = e.name;
        while (s[0] == '.' && (s[1] == '/' || s[1] == '\\')) s += 2;
        snprintf(rel, sizeof(rel), "%s", s);
        for (char *c = rel; *c; c++) if (*c == '\\') *c = '/';
        if (arch_join(p, sizeof(p), rel) && fs_stat(p, &fi) && fi.type != Z_FS_TYPE_DIR) {
            snprintf(arch.exists, sizeof(arch.exists), "%s", rel);
            return true;
        }
    }
    return false;
}

void px_bi_unzip(px_shell_t *sh, int argc, char **argv) {

    const char *file = NULL, *dest = NULL;
    bool list = false, over = false;
    zf_io_t io;

    arch_begin(sh, "unzip");
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-l")) list = true;
        else if (!strcmp(argv[i], "-o")) over = true;
        else if (!strcmp(argv[i], "-d") && i + 1 < argc) dest = argv[++i];
        else if (argv[i][0] != '-' && !file) file = argv[i];
        else { file = NULL; break; }
    }
    if (!file) {
        px_puts(sh, "usage: unzip [-l] [-o] ARCHIVE.zip [-d DIR]\n");
        sh->status = 1;
        return;
    }
    arch.overwrite = over;

    if (arch_open(file, dest, &io, list) < 0) { sh->status = 1; return; }
    zf_cancel_poll = arch_cancel;

    if (list) { unzip_list(&io); return; }
    if (!over && unzip_clash(&io)) { arch_done(ZF_E_IO, &io); return; }

    zf_dir_t d = { NULL, d_open, d_close, d_mkdir, d_remove };
    arch_done(zf_extract_zip(&io, &d), &io);
}

/* tar -xf / -tf, old style (tar xf) too; z and v are accepted and mean
 * nothing extra -- a gzip'ed tar is known by its first two bytes, and
 * every name is printed anyway. */
void px_bi_tar(px_shell_t *sh, int argc, char **argv) {

    const char *file = NULL, *dest = NULL;
    int mode = 0;
    bool over = false, bad = false;
    zf_io_t io;

    arch_begin(sh, "tar");
    for (int i = 1; i < argc && !bad; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "-C") && i + 1 < argc) { dest = argv[++i]; continue; }
        if (!strcmp(a, "-o")) { over = true; continue; }
        if (i == 1 || a[0] == '-') {
            bool want_file = false;
            for (const char *c = a[0] == '-' ? a + 1 : a; *c; c++) {
                if (*c == 'x' || *c == 't') { if (mode && mode != *c) bad = true; mode = *c; }
                else if (*c == 'f') want_file = true;
                else if (*c != 'v' && *c != 'z') bad = true;
            }
            if (want_file) {
                if (i + 1 < argc) file = argv[++i];
                else bad = true;
            }
            continue;
        }
        bad = true;
    }
    if (bad || !mode || !file) {
        px_puts(sh, "usage: tar -xf ARCHIVE.tar|.tgz|.tar.gz [-C DIR] [-o]\n"
                    "       tar -tf ARCHIVE    (list)\n");
        sh->status = 1;
        return;
    }
    arch.overwrite = over;
    arch.list = (mode == 't');

    if (arch_open(file, dest, &io, arch.list) < 0) { sh->status = 1; return; }
    zf_cancel_poll = arch_cancel;

    uint8_t magic[2] = { 0, 0 };
    int r = in_read(&io, magic, 2);
    if (r < 0 || !in_seek(&io, 0)) { arch_done(ZF_E_IO, &io); return; }

    zf_dir_t d = { NULL, d_open, d_close, d_mkdir, d_remove };
    if (r == 2 && magic[0] == 0x1f && magic[1] == 0x8b) {
        arch_done(zf_extract_tgz(&io, &d), &io);
    } else {
        zf_src_t src;
        zf_src_io(&src, &io);
        arch_done(zf_extract_tar(&src, &d), &io);
    }
}

/* A command stopped without unwinding (job.c): no cancel hook left
 * pointing at a shell that is not running. */
void px_tools_abandon(void) {
    zf_cancel_poll = NULL;
}
