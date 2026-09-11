/*
 * posix -- the shell.
 *
 * Takes a line, produces output. No ports, no messages, no windows
 * anywhere in this file, which is the point: everything in it can be
 * driven from a test harness on a development machine (tests/), and
 * the port plumbing in main.c is thin enough to read in one sitting.
 *
 * -- What is here, and what is Phase 5 --
 *
 * Here: argument splitting, `>` and `>>` redirection, exit status,
 * `&&` and `||`, and the builtins that make an edit-compile-run loop
 * work.
 *
 * Phase 5: pipes. They need two commands running at once, and every
 * command here runs to completion before the next starts. The honest
 * way to add them is a small coroutine discipline inside this process
 * -- a builtin that yields when its output buffer fills -- and that is
 * a bigger change than it looks, so it is not being smuggled in early.
 */

#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>

#include "zeitlos.h"
#include "zfsapp.h"
#include "zwin.h"           /* z_launch_arg_set() */
#include "posix.h"

void *px_current_conn;

void px_shell_init(px_shell_t *sh, px_out_fn out, void *ctx) {
    sh->out = out;
    sh->ctx = ctx;
    sh->status = 0;
    sh->want_exit = false;
    sh->child_pid = 0;
    sh->pending[0] = 0;
    sh->pending_join = 0;
    sh->in = NULL;
    sh->in_len = 0;
}

/*
 * -- Line endings --
 *
 * A terminal wants CRLF and a file wants LF, and a command should not
 * have to know which it is writing to. So commands emit LF and this
 * function expands it when the destination is the terminal.
 *
 * Getting this backwards is the classic way a redirected file ends up
 * with stray CRs in it -- which then breaks the compiler reading it
 * back, since a CR is not whitespace anywhere in C's grammar.
 */
static void out_raw(px_shell_t *sh, const char *s, int len) {
    if (sh->out) sh->out(sh->ctx, s, len);
}

/* Where the current command's stdout is going. Set by redirection
 * before the command runs and restored after; a file descriptor
 * rather than a flag, so a future `2>` costs nothing structural. */
static int out_fd = 1;

/*
 * -- pipes --
 *
 * A pipe between two builtins is a BUFFER, not a second process and
 * not a coroutine: the left side runs to completion into `cap_buf`,
 * then the right side runs with that as its `sh->in`.
 *
 * docs/posix.md section 9e originally planned this as one process per
 * stage, with builtin-to-builtin piping as a later optimisation. That
 * was backwards. Running them in sequence needs no concurrency at all,
 * and the concurrency it avoids is the part that would have been hard.
 *
 * What it costs is that the intermediate is held in full, so a
 * pipeline is bounded by CAP_MAX rather than streaming. On a machine
 * whose largest file is its own source that is a cap, not a design
 * limit -- and exceeding it is reported rather than silently
 * truncating, because a pipeline that quietly drops half its input
 * gives a wrong answer that looks like a right one.
 */
#define CAP_MAX 16384

/*
 * TWO buffers, alternating.
 *
 * One is not enough, and the first version used one with a comment
 * confidently explaining why that was safe: "the stages run strictly
 * one after another, so the single buffer is never read and written at
 * once". Wrong. A middle stage READS the buffer as its input while
 * WRITING its output into the same buffer -- `cat f | grep 1 | wc`
 * had grep overwriting its own source as it scanned, and returned 3
 * matches where there were 4.
 *
 * Caught by the assertion suite on its first run, which is the case
 * for having one: the transcript suite would have recorded 3 as the
 * expected answer and never mentioned it again.
 */
static char cap_buf[2][CAP_MAX];
static int cap_which;
static int cap_len;
static bool cap_active;
static bool cap_overflow;

static void sink(px_shell_t *sh, const char *s, int len) {

    if (cap_active) {
        char *dst = cap_buf[cap_which];
        for (int i = 0; i < len; i++) {
            if (cap_len >= CAP_MAX - 1) { cap_overflow = true; break; }
            dst[cap_len++] = s[i];
        }
        dst[cap_len] = 0;
        return;
    }

    if (out_fd == 1) {
        /* to the terminal: LF becomes CRLF */
        int start = 0;
        for (int i = 0; i < len; i++) {
            if (s[i] != '\n') continue;
            if (i > start) out_raw(sh, s + start, i - start);
            out_raw(sh, "\r\n", 2);
            start = i + 1;
        }
        if (len > start) out_raw(sh, s + start, len - start);
        return;
    }

    px_fd_t *f = px_fd(out_fd);
    if (f && f->kind == PX_FD_FILE)
        fs_write_chunk(f->handle, s, len);
}

void px_puts(px_shell_t *sh, const char *s) {
    sink(sh, s, (int)strlen(s));
}

void px_printf(px_shell_t *sh, const char *fmt, ...) {
    /* One buffer, not per-call allocation. A shell that allocated to
     * print would fail exactly when reporting that memory ran out. */
    static char buf[PX_LINE_MAX];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    px_puts(sh, buf);
}

/* ------------------------------------------------------------------ */
/* builtins                                                            */

static void bi_pwd(px_shell_t *sh, int argc, char **argv) {
    (void)argc; (void)argv;
    px_printf(sh, "%s\n", px_getcwd());
}

static void bi_cd(px_shell_t *sh, int argc, char **argv) {
    char err[128];
    const char *target = (argc > 1) ? argv[1] : "/";
    if (!px_chdir(target, err, sizeof(err))) {
        px_printf(sh, "%s\n", err);
        sh->status = 1;
    }
}

static void bi_echo(px_shell_t *sh, int argc, char **argv) {
    for (int i = 1; i < argc; i++) {
        if (i > 1) px_puts(sh, " ");
        px_puts(sh, argv[i]);
    }
    px_puts(sh, "\n");
}

static void bi_ls(px_shell_t *sh, int argc, char **argv) {

    char abs[PX_PATH_MAX];
    static char buf[4096];
    static uint8_t types[128];
    uint32_t count = 0, truncated = 0;

    if (!px_resolve(argc > 1 ? argv[1] : ".", abs, sizeof(abs))) {
        px_printf(sh, "ls: bad path\n");
        sh->status = 1;
        return;
    }

    if (!fs_list_into(abs, buf, sizeof(buf), types, 128, &count, &truncated)) {
        px_printf(sh, "ls: %s: cannot read\n", abs);
        sh->status = 1;
        return;
    }

    /*
     * Sorted, which the filesystem does not do for us -- FatFs returns
     * directory order, which is creation order, which is arbitrary.
     * An unsorted listing is hard to scan and, more practically here,
     * makes the output of `ls` depend on the history of the card.
     *
     * An index array and an insertion sort: `count` is bounded at 128
     * and the strings stay where they are, so this costs 128 pointers
     * of stack and no copying.
     */
    static const char *names[128];
    static uint8_t ntypes[128];
    const char *p = buf;

    for (uint32_t i = 0; i < count; i++) {
        /* Entries come back "/"-prefixed (sw/common/zfs.h) and the
         * leading slash is noise in a listing of one directory. */
        names[i] = (*p == '/') ? p + 1 : p;
        ntypes[i] = types[i];
        while (*p) p++;
        p++;
    }

    for (uint32_t i = 1; i < count; i++) {
        const char *nk = names[i];
        uint8_t tk = ntypes[i];
        uint32_t j = i;
        while (j > 0 && strcmp(names[j - 1], nk) > 0) {
            names[j] = names[j - 1];
            ntypes[j] = ntypes[j - 1];
            j--;
        }
        names[j] = nk;
        ntypes[j] = tk;
    }

    for (uint32_t i = 0; i < count; i++)
        px_printf(sh, "%s%s\n", names[i], ntypes[i] ? "/" : "");

    if (truncated) px_puts(sh, "... (truncated)\n");
}

static void bi_cat(px_shell_t *sh, int argc, char **argv) {

    if (argc < 2) {
        px_puts(sh, "usage: cat <file> ...\n");
        sh->status = 1;
        return;
    }

    for (int i = 1; i < argc; i++) {
        char abs[PX_PATH_MAX];
        if (!px_resolve(argv[i], abs, sizeof(abs))) {
            px_printf(sh, "cat: %s: bad path\n", argv[i]);
            sh->status = 1;
            continue;
        }

        int h = fs_open_read(abs);
        if (h < 0) {
            px_printf(sh, "cat: %s: cannot open\n", argv[i]);
            sh->status = 1;
            continue;
        }

        /* Chunked, and the chunk is a compromise rather than a
         * default: bigger means fewer trips into FatFs, which the
         * syscall dispatcher will not preempt (docs/filesystem.md),
         * but every byte also has to fit through the port a message
         * at a time. 1KB is one comfortable message and one cheap
         * read. */
        static char chunk[1024];
        for (;;) {
            int n = fs_read_chunk(h, chunk, sizeof(chunk));
            if (n <= 0) break;
            sink(sh, chunk, n);
        }
        fs_close_handle(h);
    }
}

static void bi_rm(px_shell_t *sh, int argc, char **argv) {
    if (argc < 2) { px_puts(sh, "usage: rm <file> ...\n"); sh->status = 1; return; }
    for (int i = 1; i < argc; i++) {
        char abs[PX_PATH_MAX];
        if (!px_resolve(argv[i], abs, sizeof(abs)) || fs_unlink(abs) != 0) {
            px_printf(sh, "rm: %s: failed\n", argv[i]);
            sh->status = 1;
        }
    }
}

static void bi_mkdir(px_shell_t *sh, int argc, char **argv) {
    if (argc < 2) { px_puts(sh, "usage: mkdir <dir> ...\n"); sh->status = 1; return; }
    for (int i = 1; i < argc; i++) {
        char abs[PX_PATH_MAX];
        if (!px_resolve(argv[i], abs, sizeof(abs)) || fs_mkdir(abs) != 0) {
            px_printf(sh, "mkdir: %s: failed\n", argv[i]);
            sh->status = 1;
        }
    }
}

/*
 * Copies a file. The first gap a user actually hits: without it there
 * is no way to keep a copy of something before editing it.
 *
 * Chunked through a fixed buffer rather than read-whole-file, because
 * the files this machine edits are its own sources and some of them
 * are larger than a comfortable allocation -- and because a copy that
 * fails halfway on a big file is worse than one that is merely slow.
 */
static bool copy_file(px_shell_t *sh, const char *from, const char *to) {

    char src[PX_PATH_MAX], dst[PX_PATH_MAX];
    static char buf[2048];

    if (!px_resolve(from, src, sizeof(src)) ||
        !px_resolve(to, dst, sizeof(dst))) {
        px_printf(sh, "cp: bad path\n");
        return false;
    }

    if (!strcmp(src, dst)) {
        px_printf(sh, "cp: '%s' and '%s' are the same file\n", from, to);
        return false;
    }

    int in = fs_open_read(src);
    if (in < 0) { px_printf(sh, "cp: %s: cannot open\n", from); return false; }

    int out = fs_open_write(dst);
    if (out < 0) {
        fs_close_handle(in);
        px_printf(sh, "cp: %s: cannot create\n", to);
        return false;
    }

    bool ok = true;
    for (;;) {
        int n = fs_read_chunk(in, buf, (int)sizeof(buf));
        if (n < 0) { ok = false; break; }
        if (n == 0) break;
        if (fs_write_chunk(out, buf, n) != n) { ok = false; break; }
    }

    fs_close_handle(in);
    fs_close_handle(out);

    if (!ok) px_printf(sh, "cp: %s: write failed\n", to);
    return ok;
}

static void bi_cp(px_shell_t *sh, int argc, char **argv) {
    if (argc != 3) {
        px_puts(sh, "usage: cp <from> <to>\n");
        sh->status = 1;
        return;
    }
    if (!copy_file(sh, argv[1], argv[2])) sh->status = 1;
}

/*
 * Renames, by copying and then removing.
 *
 * FatFs has f_rename and sw/common/zfsapp.h does not expose it, so
 * this is copy-then-unlink. That is slower and it is also SAFER in one
 * way worth stating: the source is only removed once the copy has
 * succeeded, so an interrupted `mv` loses nothing.
 *
 * Exposing f_rename would make this atomic and one syscall; it is a
 * small addition to zfsapp if `mv` of a large file ever becomes
 * annoying.
 */
static void bi_mv(px_shell_t *sh, int argc, char **argv) {

    char src[PX_PATH_MAX];

    if (argc != 3) {
        px_puts(sh, "usage: mv <from> <to>\n");
        sh->status = 1;
        return;
    }

    if (!copy_file(sh, argv[1], argv[2])) { sh->status = 1; return; }

    if (!px_resolve(argv[1], src, sizeof(src)) || fs_unlink(src) != 0) {
        px_printf(sh, "mv: %s: copied but not removed\n", argv[1]);
        sh->status = 1;
    }
}

/*
 * Creates a file if it does not exist, and does nothing if it does.
 *
 * NOT implemented by opening for write: fs_open_write() truncates, so
 * touching an existing file would empty it -- the same trap that made
 * `>>` destroy files (see take_redirect()). fs_size() cannot tell an
 * empty file from a missing one (sw/os/fsapi.h), so existence is
 * probed with fs_open_read(), which can.
 */
static void bi_touch(px_shell_t *sh, int argc, char **argv) {

    if (argc < 2) {
        px_puts(sh, "usage: touch <file> ...\n");
        sh->status = 1;
        return;
    }

    for (int i = 1; i < argc; i++) {

        char abs[PX_PATH_MAX];
        if (!px_resolve(argv[i], abs, sizeof(abs))) {
            px_printf(sh, "touch: %s: bad path\n", argv[i]);
            sh->status = 1;
            continue;
        }

        int h = fs_open_read(abs);
        if (h >= 0) { fs_close_handle(h); continue; }   /* exists */

        h = fs_open_write(abs);
        if (h < 0) {
            px_printf(sh, "touch: %s: cannot create\n", argv[i]);
            sh->status = 1;
            continue;
        }
        fs_close_handle(h);
    }
}

static void bi_clear(px_shell_t *sh, int argc, char **argv) {
    (void)argc; (void)argv;
    /* The terminal is a VT100 (docs/terminal.md): erase display, home
     * the cursor. */
    px_puts(sh, "\033[2J\033[H");
}

/*
 * -- filters --
 *
 * Each takes a file argument OR reads sh->in, which is the previous
 * stage of a pipeline. One code path serves `wc x.c` and
 * `cat x.c | wc`, which is the point of writing them this way rather
 * than as file-only commands that get retrofitted later.
 *
 * Files are streamed through a fixed buffer; a pipe's input is already
 * in memory. feed_lines() hides the difference, calling `fn` once per
 * line with the newline stripped.
 */
typedef void (*line_fn)(px_shell_t *sh, const char *line, int len, void *ctx);

/*
 * Set when a line was longer than the buffer and got cut.
 *
 * It used to be dropped silently, which is the worst of both: `wc`
 * undercounted a file with one long line and reported the wrong number
 * with no hint that anything had happened. A filter that cannot see
 * all of its input should say so.
 *
 * `wc` no longer uses this path at all -- it counts raw bytes, so it
 * is exact on any line length. head/tail/grep still work a line at a
 * time and report when one was cut.
 */
static bool line_truncated;

static bool feed_lines(px_shell_t *sh, const char *path,
                       line_fn fn, void *ctx) {

    line_truncated = false;

    static char buf[1024];
    static char line[PX_LINE_MAX];
    int n = 0;

    if (!path) {
        /* From the pipeline. */
        const char *p = sh->in;
        int left = sh->in_len;
        if (!p) return true;
        while (left > 0) {
            if (*p == '\n') {
                line[n] = 0;
                fn(sh, line, n, ctx);
                n = 0;
            } else if (n < PX_LINE_MAX - 1) {
                line[n++] = *p;
            } else {
                line_truncated = true;
            }
            p++;
            left--;
        }
        if (n) { line[n] = 0; fn(sh, line, n, ctx); }
        return true;
    }

    char abs[PX_PATH_MAX];
    if (!px_resolve(path, abs, sizeof(abs))) return false;

    int h = fs_open_read(abs);
    if (h < 0) return false;

    for (;;) {
        int got = fs_read_chunk(h, buf, (int)sizeof(buf));
        if (got <= 0) break;
        for (int i = 0; i < got; i++) {
            if (buf[i] == '\n') {
                line[n] = 0;
                fn(sh, line, n, ctx);
                n = 0;
            } else if (n < PX_LINE_MAX - 1) {
                line[n++] = buf[i];
            } else {
                line_truncated = true;
            }
        }
    }
    fs_close_handle(h);

    /* A final line with no newline is still a line. */
    if (n) { line[n] = 0; fn(sh, line, n, ctx); }
    return true;
}

typedef struct { long lines, words, chars; bool in_word; } wc_acc_t;

static void wc_bytes(wc_acc_t *a, const char *p, int n) {
    for (int i = 0; i < n; i++) {
        a->chars++;
        if (p[i] == '\n') a->lines++;
        bool sp = p[i] == ' ' || p[i] == '\t' || p[i] == '\n' || p[i] == '\r';
        if (!sp && !a->in_word) { a->words++; a->in_word = true; }
        else if (sp) a->in_word = false;
    }
}

/*
 * Counts the raw byte stream rather than going through feed_lines().
 *
 * That makes it EXACT on any line length -- the line path truncates at
 * PX_LINE_MAX and a file with one very long line would otherwise be
 * undercounted, silently. It is also simpler: lines are just newlines.
 *
 * Streaming, so `wc` has no file-size limit. Through a pipe it is
 * bounded by CAP_MAX like every other stage.
 */
static bool wc_count(px_shell_t *sh, const char *path, wc_acc_t *a) {

    static char buf[1024];

    if (!path) {
        if (sh->in) wc_bytes(a, sh->in, sh->in_len);
        return true;
    }

    char abs[PX_PATH_MAX];
    if (!px_resolve(path, abs, sizeof(abs))) return false;

    int h = fs_open_read(abs);
    if (h < 0) return false;

    for (;;) {
        int got = fs_read_chunk(h, buf, (int)sizeof(buf));
        if (got <= 0) break;
        wc_bytes(a, buf, got);
    }
    fs_close_handle(h);
    return true;
}

static void bi_wc(px_shell_t *sh, int argc, char **argv) {

    if (argc < 2) {
        wc_acc_t a = {0, 0, 0, false};
        wc_count(sh, NULL, &a);
        px_printf(sh, "%ld %ld %ld\n", a.lines, a.words, a.chars);
        return;
    }

    for (int i = 1; i < argc; i++) {
        wc_acc_t a = {0, 0, 0, false};
        if (!wc_count(sh, argv[i], &a)) {
            px_printf(sh, "wc: %s: cannot open\n", argv[i]);
            sh->status = 1;
            continue;
        }
        px_printf(sh, "%ld %ld %ld %s\n", a.lines, a.words, a.chars, argv[i]);
    }
}

typedef struct { int want, seen; } head_t;

static void head_line(px_shell_t *sh, const char *line, int len, void *ctx) {
    (void)len;
    head_t *h = ctx;
    if (h->seen++ < h->want) px_printf(sh, "%s\n", line);
}

/*
 * head and tail share their argument parsing: an optional -N before
 * the file. Not the full POSIX -n N form, because a shell with no
 * getopt is better with one spelling than with two that disagree.
 */
static int count_arg(int *argc, char **argv, int def) {
    if (*argc > 1 && argv[1][0] == '-' && argv[1][1] >= '0' && argv[1][1] <= '9') {
        int v = 0;
        for (const char *p = argv[1] + 1; *p >= '0' && *p <= '9'; p++)
            v = v * 10 + (*p - '0');
        for (int i = 1; i + 1 < *argc; i++) argv[i] = argv[i + 1];
        (*argc)--;
        return v;
    }
    return def;
}

static void bi_head(px_shell_t *sh, int argc, char **argv) {
    head_t h = { count_arg(&argc, argv, 10), 0 };
    const char *path = argc > 1 ? argv[1] : NULL;
    if (!feed_lines(sh, path, head_line, &h)) {
        px_printf(sh, "head: %s: cannot open\n", argv[1]);
        sh->status = 1;
        return;
    }
    if (line_truncated)
        px_printf(sh, "head: a line was longer than %d and was cut\n",
                  PX_LINE_MAX - 1);
}

/*
 * tail keeps a ring of the last N lines.
 *
 * A ring rather than reading the file backwards, because zfsapp can
 * seek but a line-oriented backward read means guessing at buffer
 * boundaries -- and because this has to work on a pipe, where there is
 * nothing to seek.
 */
#define TAIL_MAX 64

typedef struct {
    char lines[TAIL_MAX][128];
    int n, head, want;
} tail_t;

static void tail_line(px_shell_t *sh, const char *line, int len, void *ctx) {
    (void)sh; (void)len;
    tail_t *t = ctx;
    snprintf(t->lines[t->head], sizeof(t->lines[0]), "%s", line);
    t->head = (t->head + 1) % TAIL_MAX;
    if (t->n < TAIL_MAX) t->n++;
}

static void bi_tail(px_shell_t *sh, int argc, char **argv) {

    static tail_t t;
    t.n = 0; t.head = 0;
    t.want = count_arg(&argc, argv, 10);
    if (t.want > TAIL_MAX) t.want = TAIL_MAX;

    const char *path = argc > 1 ? argv[1] : NULL;
    if (!feed_lines(sh, path, tail_line, &t)) {
        px_printf(sh, "tail: %s: cannot open\n", argv[1]);
        sh->status = 1;
        return;
    }

    if (line_truncated)
        px_printf(sh, "tail: a line was longer than %d and was cut\n",
                  PX_LINE_MAX - 1);

    int show = t.n < t.want ? t.n : t.want;
    int start = (t.head - show + TAIL_MAX) % TAIL_MAX;
    for (int i = 0; i < show; i++)
        px_printf(sh, "%s\n", t.lines[(start + i) % TAIL_MAX]);
}

typedef struct { const char *pat; int matched; } grep_t;

static void grep_line(px_shell_t *sh, const char *line, int len, void *ctx) {
    (void)len;
    grep_t *g = ctx;
    if (strstr(line, g->pat)) { px_printf(sh, "%s\n", line); g->matched++; }
}

/*
 * Fixed-string matching, not regular expressions.
 *
 * There IS a regex engine on this machine -- sw/ext/nextvi's, reached
 * through `vi`'s `:g` and `:s` -- and pulling it into the shell would
 * cost far more than it returns for a command whose common use is a
 * literal word. Said plainly here so the limit is a decision rather
 * than a surprise: `grep 'a.*b'` looks for those five characters.
 */
static void bi_grep(px_shell_t *sh, int argc, char **argv) {

    if (argc < 2) {
        px_puts(sh, "usage: grep <text> [file]\n");
        sh->status = 1;
        return;
    }

    grep_t g = { argv[1], 0 };
    const char *path = argc > 2 ? argv[2] : NULL;

    if (!feed_lines(sh, path, grep_line, &g)) {
        px_printf(sh, "grep: %s: cannot open\n", argv[2]);
        sh->status = 1;
        return;
    }

    if (line_truncated)
        px_printf(sh, "grep: a line was longer than %d and was cut\n",
                  PX_LINE_MAX - 1);

    /* Nothing matched is status 1, as grep has always reported it --
     * which is what makes `grep x f && ...` useful. */
    if (!g.matched) sh->status = 1;
}

/*
 * -- sort and uniq --
 *
 * sort is the one command here that CANNOT stream: it has to see every
 * line before it can emit the first. So it is bounded where the others
 * are not -- SORT_MAX lines, and the text has to fit SORT_TEXT.
 *
 * That limit is reported rather than silently dropping the tail,
 * because a sort that quietly loses input is worse than one that
 * refuses: the output still looks sorted.
 */
#define SORT_MAX   512
#define SORT_TEXT  16384

typedef struct {
    char text[SORT_TEXT];
    int  off[SORT_MAX];
    int  n, used;
    bool full;
} sort_t;

static void sort_line(px_shell_t *sh, const char *line, int len, void *ctx) {
    (void)sh;
    sort_t *s = ctx;
    if (s->n >= SORT_MAX || s->used + len + 1 > SORT_TEXT) {
        s->full = true;
        return;
    }
    s->off[s->n++] = s->used;
    memcpy(s->text + s->used, line, (size_t)len);
    s->used += len;
    s->text[s->used++] = 0;
}

static void bi_sort(px_shell_t *sh, int argc, char **argv) {

    static sort_t s;
    s.n = 0; s.used = 0; s.full = false;

    const char *path = argc > 1 ? argv[1] : NULL;

    if (!feed_lines(sh, path, sort_line, &s)) {
        px_printf(sh, "sort: %s: cannot open\n", argv[1]);
        sh->status = 1;
        return;
    }

    if (s.full) {
        px_printf(sh, "sort: input larger than %d lines or %d bytes\n",
                  SORT_MAX, SORT_TEXT);
        sh->status = 1;
        return;
    }

    /* Insertion sort on the offset array. O(n^2) on 512 lines is a few
     * hundred thousand comparisons -- well under a frame on this
     * machine, and it is thirty lines less code than anything faster.
     * If sorting a big file ever matters, the LIMIT will hurt first. */
    for (int i = 1; i < s.n; i++) {
        int v = s.off[i];
        int j = i;
        while (j > 0 && strcmp(s.text + s.off[j - 1], s.text + v) > 0) {
            s.off[j] = s.off[j - 1];
            j--;
        }
        s.off[j] = v;
    }

    for (int i = 0; i < s.n; i++)
        px_printf(sh, "%s\n", s.text + s.off[i]);
}

typedef struct { char prev[PX_LINE_MAX]; bool have; } uniq_t;

static void uniq_line(px_shell_t *sh, const char *line, int len, void *ctx) {
    (void)len;
    uniq_t *u = ctx;
    if (u->have && !strcmp(u->prev, line)) return;
    px_printf(sh, "%s\n", line);
    snprintf(u->prev, sizeof(u->prev), "%s", line);
    u->have = true;
}

/*
 * Removes ADJACENT duplicates, which is what uniq has always done --
 * it is meant to follow sort. Streaming, so no size limit.
 */
static void bi_uniq(px_shell_t *sh, int argc, char **argv) {
    static uniq_t u;
    u.have = false;
    const char *path = argc > 1 ? argv[1] : NULL;
    if (!feed_lines(sh, path, uniq_line, &u)) {
        px_printf(sh, "uniq: %s: cannot open\n", argv[1]);
        sh->status = 1;
    }
}

static void bi_rmdir(px_shell_t *sh, int argc, char **argv) {

    if (argc < 2) {
        px_puts(sh, "usage: rmdir <dir> ...\n");
        sh->status = 1;
        return;
    }

    for (int i = 1; i < argc; i++) {
        char abs[PX_PATH_MAX];
        /* FatFs's f_unlink removes an empty directory as well as a
         * file, and refuses a non-empty one -- which is rmdir's
         * contract exactly, so there is nothing to add. */
        if (!px_resolve(argv[i], abs, sizeof(abs)) || fs_unlink(abs) != 0) {
            px_printf(sh, "rmdir: %s: failed (not empty?)\n", argv[i]);
            sh->status = 1;
        }
    }
}

static void bi_df(px_shell_t *sh, int argc, char **argv) {
    (void)argc; (void)argv;
    uint32_t total = 0, freek = 0;
    if (!fs_df(&total, &freek)) {
        px_puts(sh, "df: unavailable\n");
        sh->status = 1;
        return;
    }
    px_printf(sh, "%lu KB total, %lu KB free\n",
              (unsigned long)total, (unsigned long)freek);
}

/*
 * Runs a real Zeitlos program.
 *
 * This is the escape hatch and, for now, the only way to run anything
 * that is not a builtin. The argument string is handed over through
 * the LAUNCH ARGUMENT (z_launch_arg_set, sw/common/zwin.h) -- the
 * mechanism wm already uses to give a newly launched app a filename --
 * because Z_SYS_PROC_RUN itself carries a name and nothing else.
 *
 * That is precisely what zcc's /zcc.args stopgap was waiting for:
 * `zcc hello.c -o hello` here becomes a real argv on the other side,
 * with no file involved. See docs/zcc.md, "Getting arguments to it".
 *
 * The child's OUTPUT comes back if the child asks for it: it looks up
 * this shell by name, opens a second connection with PX_STDOUT_TAG as
 * the connect argument, and everything it writes there is relayed to
 * whichever terminal ran the command. `zcc` does this; a program that
 * does not still writes to the kernel console, which is the old
 * behaviour and the right default for something that knows nothing
 * about us.
 *
 * What this still does NOT do is WAIT for the child or report its exit
 * status. `run` returns as soon as the process starts, so `zcc x.c &&
 * run x` does not mean what it looks like -- the `&&` tests whether
 * the compiler STARTED. Fixing that needs the shell to have a notion
 * of a running child rather than a started one, which is the same
 * thing pipes need. Phase 5.
 */
static void bi_run(px_shell_t *sh, int argc, char **argv) {

    if (argc < 2) {
        px_puts(sh, "usage: run <program> [args...]\n");
        sh->status = 1;
        return;
    }

    char args[PX_LINE_MAX];
    int n = 0;
    for (int i = 2; i < argc; i++) {
        int len = (int)strlen(argv[i]);
        if (n + len + 2 >= (int)sizeof(args)) break;
        if (n) args[n++] = ' ';
        memcpy(args + n, argv[i], (size_t)len);
        n += len;
    }
    args[n] = 0;

    /* ALWAYS set, even to the empty string.
     *
     * The launch argument is claimed by the child, not consumed by the
     * parent -- so a program that never calls z_launch_arg_take()
     * leaves the previous command's arguments pending, and the NEXT
     * child claims them. `zcc a.c -o a` followed by `run hello` would
     * hand hello a compiler command line.
     *
     * Caught by the test transcript, where a bare `zcc` showed
     * arg='src/a.c -o a -v' inherited from three commands earlier. */
    z_launch_arg_set(args);

    uint32_t pid = z_proc_run(argv[1]);
    if (!pid) {
        px_printf(sh, "run: %s: cannot start\n", argv[1]);
        sh->status = 1;
        return;
    }

    /* Recorded, not waited on. main.c watches for the exit; see
     * px_shell_t's own comment for why this shell must never block. */
    sh->child_pid = pid;
}

static void bi_help(px_shell_t *sh, int argc, char **argv);

static void bi_exit(px_shell_t *sh, int argc, char **argv) {
    sh->want_exit = true;
    sh->status = (argc > 1) ? atoi(argv[1]) : 0;
}

typedef struct {
    const char *name;
    void (*fn)(px_shell_t *, int, char **);
    const char *help;
} builtin_t;

static const builtin_t builtins[] = {
    { "cd",    bi_cd,    "change directory" },
    { "pwd",   bi_pwd,   "print the working directory" },
    { "ls",    bi_ls,    "list a directory" },
    { "cat",   bi_cat,   "print files" },
    { "echo",  bi_echo,  "print arguments" },
    { "rm",    bi_rm,    "delete files" },
    { "cp",    bi_cp,    "copy a file" },
    { "mv",    bi_mv,    "rename a file" },
    { "touch", bi_touch, "create a file if it does not exist" },
    { "clear", bi_clear, "clear the screen" },
    { "mkdir", bi_mkdir, "make directories" },
    { "wc",    bi_wc,    "count lines, words and characters" },
    { "head",  bi_head,  "first lines of a file  (head -5 f)" },
    { "tail",  bi_tail,  "last lines of a file   (tail -5 f)" },
    { "grep",  bi_grep,  "lines containing text  (fixed string)" },
    { "sort",  bi_sort,  "sort lines" },
    { "uniq",  bi_uniq,  "drop adjacent duplicate lines" },
    { "rmdir", bi_rmdir, "remove an empty directory" },
    { "df",    bi_df,    "filesystem usage" },
    { "run",   bi_run,   "run a Zeitlos program, passing arguments" },
    { "help",  bi_help,  "this list" },
    { "exit",  bi_exit,  "close this session" },
    { 0, 0, 0 }
};

static void bi_help(px_shell_t *sh, int argc, char **argv) {
    (void)argc; (void)argv;
    px_puts(sh, "posix -- see docs/posix.md\n");
    for (int i = 0; builtins[i].name; i++)
        px_printf(sh, "  %-8s %s\n", builtins[i].name, builtins[i].help);
    px_puts(sh, "\nanything else is run as a program: 'zcc hello.c -o hello'\n"
                "redirection: > and >>   chaining: && and ||\n");
}

/* ------------------------------------------------------------------ */
/* the line                                                            */

/*
 * Splits in place. No quoting and no escapes.
 *
 * Deliberate, and worth stating rather than leaving as an omission: a
 * Zeitlos path cannot contain a space (FAT long names can, but nothing
 * in this tree creates one), and every argument a command here takes
 * is a path or a flag. Adding quoting would mean adding escaping, and
 * then a user has to know which is which -- for a capability nothing
 * needs yet.
 */
static int split(char *line, char **argv, int max) {
    int argc = 0;
    char *p = line;
    while (*p && argc < max) {
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) break;
        argv[argc++] = p;
        while (*p && *p != ' ' && *p != '\t') p++;
        if (*p) *p++ = 0;
    }
    return argc;
}

/*
 * Pulls `> file` or `>> file` out of the argument vector.
 *
 * Done after splitting rather than during, so that `>file` and `> file`
 * both work without the splitter needing to know about redirection --
 * the splitter's job is whitespace and nothing else.
 */
static bool take_redirect(px_shell_t *sh, int *argc, char **argv) {

    for (int i = 0; i < *argc; i++) {

        int append = 0;
        const char *target = 0;

        if (!strcmp(argv[i], ">") || !strcmp(argv[i], ">>")) {
            append = (argv[i][1] == '>');
            if (i + 1 >= *argc) {
                px_puts(sh, "syntax error: expected a filename after >\n");
                return false;
            }
            target = argv[i + 1];
            for (int j = i; j + 2 <= *argc; j++) argv[j] = argv[j + 2];
            *argc -= 2;
        } else if (argv[i][0] == '>') {
            append = (argv[i][1] == '>');
            target = argv[i] + (append ? 2 : 1);
            if (!*target) {
                px_puts(sh, "syntax error: expected a filename after >\n");
                return false;
            }
            for (int j = i; j + 1 <= *argc; j++) argv[j] = argv[j + 1];
            *argc -= 1;
        } else {
            continue;
        }

        char err[128];

        /*
         * `>>` opens READ-WRITE and seeks to the end.
         *
         * The first version opened for write and then seeked, which
         * looks equivalent and is not: fs_open_write() TRUNCATES, so
         * the seek moved past a file that was now empty and the gap
         * was filled with zero bytes. `echo a > f; echo b >> f` gave
         * eight NULs and then "b" -- the original contents silently
         * replaced by padding, which is the worst possible outcome for
         * an append.
         */
        int fd = px_open_append(target, append, err, sizeof(err));
        if (fd < 0) {
            px_printf(sh, "%s\n", err);
            return false;
        }

        out_fd = fd;
        return true;
    }
    return true;
}

/* Runs one command with no pipe in it. */
static void exec_simple(px_shell_t *sh, char *cmd);

/*
 * Splits on `|` and runs the stages in sequence, each reading what the
 * last one wrote.
 *
 * A stage that starts a CHILD ends the pipeline: a process writes to
 * its own stdout relay, not into this buffer, and pretending otherwise
 * would silently drop its output. Reported rather than ignored.
 */
static void exec_one(px_shell_t *sh, char *cmd) {

    char *stages[8];
    int n = 0;
    char *p = cmd;

    stages[n++] = p;
    while (*p && n < 8) {
        if (p[0] == '|' && p[1] != '|') {
            *p++ = 0;
            while (*p == ' ') p++;
            stages[n++] = p;
            continue;
        }
        p++;
    }

    if (n == 1) { exec_simple(sh, cmd); return; }

    const char *feed = NULL;
    int feed_len = 0;

    for (int i = 0; i < n; i++) {

        bool last = (i == n - 1);

        sh->in = feed;
        sh->in_len = feed_len;

        if (!last) {
            /* Write into the buffer this stage is NOT reading. */
            cap_which = (feed == cap_buf[0]) ? 1 : 0;
            cap_active = true;
            cap_len = 0;
            cap_overflow = false;
            cap_buf[cap_which][0] = 0;
        }

        exec_simple(sh, stages[i]);

        if (sh->child_pid) {
            cap_active = false;
            px_printf(sh, "posix: '%s' is a program, not a builtin -- "
                          "its output cannot be piped\n", stages[i]);
            sh->status = 1;
            break;
        }

        if (!last) {
            cap_active = false;
            if (cap_overflow)
                px_printf(sh, "posix: pipe truncated at %d bytes\n", CAP_MAX);
            feed = cap_buf[cap_which];
            feed_len = cap_len;
        }
    }

    sh->in = NULL;
    sh->in_len = 0;
}

static void exec_simple(px_shell_t *sh, char *cmd) {

    char *argv[PX_MAX_ARGS];
    int argc = split(cmd, argv, PX_MAX_ARGS);

    if (!argc) return;

    out_fd = 1;
    sh->status = 0;

    if (!take_redirect(sh, &argc, argv)) { sh->status = 1; return; }
    if (!argc) { px_close_all_files(); out_fd = 1; return; }

    for (int i = 0; builtins[i].name; i++) {
        if (strcmp(argv[0], builtins[i].name)) continue;
        builtins[i].fn(sh, argc, argv);
        px_close_all_files();
        out_fd = 1;
        return;
    }

    /*
     * Not a builtin, so it is a program. `zcc hello.c` and
     * `run zcc hello.c` are the same thing -- typing `run` is not
     * something anyone should have to remember, and a name that is
     * neither a builtin nor a program gets a clean error either way.
     */
    {
        char *runv[PX_MAX_ARGS + 1];
        runv[0] = "run";
        for (int i = 0; i < argc && i < PX_MAX_ARGS; i++) runv[i + 1] = argv[i];
        bi_run(sh, argc + 1, runv);
    }

    px_close_all_files();
    out_fd = 1;
}

/*
 * Runs a line, one `&&`/`||`-separated command at a time.
 *
 * Returns as soon as a command starts a CHILD, with `child_pid` set
 * and the rest of the line saved in `pending`. The caller resumes it
 * with px_resume() once the child exits. Everything else runs to
 * completion as before.
 *
 * Splitting it this way, rather than looping until the line is done,
 * is what lets the shell keep serving its other connections while a
 * compile runs.
 *
 * -- On skipping, which is the part that was wrong --
 *
 * A failed `&&` skips the NEXT COMMAND. It does not abandon the line.
 * `false && a || b` runs `b`, because the `||` sees the failure that
 * `&&` left in place -- shells evaluate these strictly left to right
 * with no precedence between them, which reads as though `||` binds
 * looser and does not.
 *
 * The first version returned on a failed join and printed nothing for
 * that line at all. Caught by the test transcript, which is the case
 * for having one: the behaviour is easy to state, easy to get wrong,
 * and invisible until somebody writes the third clause.
 *
 * `first_join` is how the segment at `p` joins to whatever ran before
 * it -- 0 for the start of a line, or the operator a resumed child was
 * waiting on.
 */
static void exec_rest(px_shell_t *sh, char *p, int first_join) {

    int join = first_join;

    for (;;) {
        char *seg = p;
        int next_join = 0;          /* 0 none, 1 &&, 2 || */

        while (*p) {
            if (p[0] == '&' && p[1] == '&') { next_join = 1; break; }
            if (p[0] == '|' && p[1] == '|') { next_join = 2; break; }
            p++;
        }

        int more = (*p != 0);
        if (more) { *p = 0; p += 2; }

        /* Skip, but keep going: the status stays as it was, so the
         * next operator in the chain tests the same thing this one
         * did. */
        int skip = (join == 1 && sh->status != 0) ||
                   (join == 2 && sh->status == 0);

        if (!skip) {
            exec_one(sh, seg);
            if (sh->want_exit) return;

            if (sh->child_pid) {
                /* Suspend. The rest of the line, and how it joins to
                 * the command now running, wait for the exit. */
                sh->pending_join = more ? next_join : 0;
                if (more) {
                    int n = 0;
                    while (p[n] && n < PX_LINE_MAX - 1) {
                        sh->pending[n] = p[n];
                        n++;
                    }
                    sh->pending[n] = 0;
                } else {
                    sh->pending[0] = 0;
                }
                return;
            }
        }

        if (!more) return;
        join = next_join;
    }
}

void px_exec_line(px_shell_t *sh, char *line) {
    sh->child_pid = 0;
    sh->pending[0] = 0;
    sh->pending_join = 0;
    exec_rest(sh, line, 0);
}

/*
 * Continues a line after the child it was waiting on has exited.
 * `status` is the child's, and becomes the shell's -- which is the
 * whole point: `&&` now tests what the child DID, not that it started.
 */
void px_resume(px_shell_t *sh, int status) {

    sh->child_pid = 0;
    sh->status = status;

    if (!sh->pending[0]) return;

    /* Copied out first: exec_rest() splits its argument in place, and
     * a command in the tail can itself start a child and write back
     * into sh->pending. */
    char rest[PX_LINE_MAX];
    int n = 0;
    while (sh->pending[n] && n < PX_LINE_MAX - 1) { rest[n] = sh->pending[n]; n++; }
    rest[n] = 0;

    int join = sh->pending_join;
    sh->pending[0] = 0;
    sh->pending_join = 0;

    exec_rest(sh, rest, join);
}
