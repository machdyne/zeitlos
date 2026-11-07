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
 * Pipes between builtins are here too, as buffers: the stages run one
 * after the other (exec_one()).
 *
 * -- Where it runs --
 *
 * On the machine a line runs in a coroutine (job.c): px_intr() and the
 * output path hand back to main.c's loop between pieces of work, so the
 * other sessions are served while a long builtin runs. Nothing in this
 * file depends on that -- the host transcript test calls
 * px_exec_line() directly -- but the statics below are why only one
 * command runs at a time (job.c's header).
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
#include "../../common/zargs.h"		// quoting -- docs/posix.md, "Quoting"
#include "../../common/zglob.h"		// wildcards -- docs/posix.md, "Wildcards"
#include "posix.h"
#include "zproc.h"           /* ps: z_proc_info_t, Z_PROC_FLAG_* */
#include "zsoc.h"            /* ps: Z_TICK_HZ, for CPU seconds */
#include "zgz.h"            /* zcat, gunzip */
#include "formats/zf_zip.h" /* zz_window_take(): one inflate window, shared with unzip/tar */
#include "zco.h"            /* streaming pipes: a stage in a coroutine of its own */

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
    sh->poll = NULL;
    sh->intr = false;
    sh->job_line[0] = 0;
    sh->job_resume = false;
    sh->job_status = 0;
}

static bool stage_gone(void);

bool px_intr(px_shell_t *sh) {
    px_job_tick();
    if (!sh->intr && sh->poll && sh->poll(sh->ctx)) sh->intr = true;
    return sh->intr || stage_gone();
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

/*
 * -- streaming pipes --
 *
 * The buffer above is the way a pipe ran until November 2026, and is
 * still the way for a pipe this cannot stream (pipe_streams()). Now
 * each stage but the last runs in a coroutine of its own (zco.h), and
 * the stages hand over as the bytes go: the last stage, wanting input
 * (px_in_next()), resumes the stage before it, which runs until it has
 * filled its pipe (PX_PIPE_BUF), ended, or used its slice of time; that
 * one in turn resumes the stage before it when it wants input. So
 * nothing is held whole: `cat big | grep x | wc` has no size limit, and
 * `cat big | head -n 3` stops cat after the first pipe-full.
 *
 * What the stages share is what made this hard (docs/posix.md, "What
 * streams and what does not"). Two kinds:
 *
 *   - the shell's own per-command state -- out_fd, the status, `< file`
 *     input, the line-cut flag -- is SWAPPED as stages hand over
 *     (stage_resume()): each stage sees its own;
 *   - buffers a stage may be in the middle of when it hands over --
 *     its arguments (split, wildcards), px_printf()'s, the line and
 *     chunk buffers of feed_lines() and text.c's feed_bytes() -- are
 *     ONE PER STAGE (st_* below point at the running stage's).
 *
 * A builtin's own statics are its alone, so a pipe streams only if no
 * builtin is in it twice, and at most one of zcat, gunzip, unzip and
 * tar (the one inflate window); see pipe_streams() for the rest.
 */
struct px_stage {
    zco_t       co;             /* a producer's coroutine */
    px_shell_t *sh;
    char       *cmd;
    px_stage_t *prev;           /* the stage this one reads, or NULL */
    int         idx;
    bool        producer;       /* every stage but the last */
    bool        done;           /* its command has returned */
    bool        out_gone;       /* its reader has ended: stop */
    bool        want_main;      /* handed back because its slice is used */
    bool        overflow;       /* ran off its stack */
    bool        in_had;         /* the reader holds a piece of prev's pipe */
    char       *pipe;           /* what it has written, not yet read */
    int         pipe_len;
    /* the shell's state while another stage runs */
    int         v_out_fd, v_status, v_in_len;
    const char *v_in;
    bool        v_in_taken, v_trunc, v_enough;
    /* its buffers (NULL: the shell's own -- the last stage) */
    char       *tok, *glob, *pbuf, *line, *rbuf, *bbuf;
    uint8_t    *stack;
};

static px_stage_t stage_tab[PX_PIPE_STAGES];
static px_stage_t *cur_stage;           /* the stage running, or NULL */
static uint8_t *pipes_mem;              /* px_pipes_init() */
static size_t pipes_bytes;

static char tok_buf[2 * PX_LINE_MAX + 16];
static char glob_buf[4096];
static char pbuf0[PX_LINE_MAX];
static char line0[PX_LINE_MAX];
static char rbuf0[1024];
static char bbuf0[1024];
/* the running stage's (or the shell's own) */
static char *st_tok = tok_buf, *st_glob = glob_buf, *st_pbuf = pbuf0,
            *st_line = line0, *st_rbuf = rbuf0, *st_bbuf = bbuf0;
static bool in_taken;                   /* sh->in has been handed out */
static bool line_truncated;
static bool feed_enough;                /* the line filter has what it wants: stop feeding */

void px_feed_enough(void) { feed_enough = true; }
bool px_feed_is_enough(void) { return feed_enough; }
void px_feed_begin(void) { feed_enough = false; }

char *px_stage_chunk_buf(void) { return st_bbuf; }

void px_pipes_init(void *mem, size_t bytes) {
    pipes_mem = mem;
    pipes_bytes = bytes;
}

static bool stage_gone(void) { return cur_stage && cur_stage->out_gone; }

static void stage_point(px_stage_t *st) {
    cur_stage = st;
    bool own = st && st->tok;
    st_tok = own ? st->tok : tok_buf;
    st_glob = own ? st->glob : glob_buf;
    st_pbuf = own ? st->pbuf : pbuf0;
    st_line = own ? st->line : line0;
    st_rbuf = own ? st->rbuf : rbuf0;
    st_bbuf = own ? st->bbuf : bbuf0;
    px_fd_owner = st ? st->idx + 1 : 0;
}

static void stage_save(px_stage_t *st) {
    if (!st) return;
    st->v_out_fd = out_fd;
    st->v_status = st->sh->status;
    st->v_in = st->sh->in;
    st->v_in_len = st->sh->in_len;
    st->v_in_taken = in_taken;
    st->v_trunc = line_truncated;
    st->v_enough = feed_enough;
}

static void stage_load(px_stage_t *st) {
    stage_point(st);
    if (!st) return;
    out_fd = st->v_out_fd;
    st->sh->status = st->v_status;
    st->sh->in = st->v_in;
    st->sh->in_len = st->v_in_len;
    in_taken = st->v_in_taken;
    line_truncated = st->v_trunc;
    feed_enough = st->v_enough;
}

/* Runs `pr` until it fills its pipe, ends, or hands back for the main
 * loop; the running stage's state put away meanwhile. */
static void stage_resume(px_stage_t *pr) {
    px_stage_t *me = cur_stage;
    stage_save(me);
    stage_load(pr);
    bool more = zco_resume(&pr->co);
    stage_save(pr);
    stage_load(me);
    if (!more) {
        pr->done = true;
        if (pr->co.overflow) pr->overflow = true;
    }
}

/* A stage handed back because its slice is used: hand back further,
 * to the stage reading this one, or from the last stage to the main
 * loop (job.c). */
static void stage_up(void) {
    px_stage_t *me = cur_stage;
    if (me && me->producer) { me->want_main = true; zco_yield(); }
    else px_job_yield();
}

/* From px_job_tick(): a producer's slice is used. */
void px_stage_tick(void) {
    px_stage_t *me = cur_stage;
    if (!me || !me->producer || zco_current() != &me->co) return;
    me->want_main = true;
    zco_yield();
}

static void pipe_write(px_stage_t *st, const char *s, int len) {
    while (len > 0 && !st->out_gone) {
        int room = PX_PIPE_BUF - st->pipe_len;
        if (!room) { zco_yield(); continue; }       /* to the reader, who empties it */
        int k = len < room ? len : room;
        memcpy(st->pipe + st->pipe_len, s, (size_t)k);
        st->pipe_len += k;
        s += k;
        len -= k;
    }
}

/* Its reader has ended (or never read): the stage stops at its next
 * px_intr() and its output goes nowhere. Run to its end, so it closes
 * its files and frees its memory. */
static void stage_drain(px_stage_t *pr) {
    if (!pr) return;
    pr->out_gone = true;
    while (!pr->done) {
        stage_resume(pr);
        if (pr->want_main) { pr->want_main = false; stage_up(); }
    }
}

int px_in_next(px_shell_t *sh, const char **p) {
    px_stage_t *st = cur_stage;
    if (st && st->prev) {
        px_stage_t *pr = st->prev;
        if (st->in_had) pr->pipe_len = 0;           /* the last piece is done with */
        st->in_had = false;
        while (!pr->pipe_len) {
            if (pr->done || sh->intr || st->out_gone) return 0;
            stage_resume(pr);
            if (pr->want_main) { pr->want_main = false; stage_up(); }
        }
        st->in_had = true;
        *p = pr->pipe;
        return pr->pipe_len;
    }
    if (!sh->in || in_taken || !sh->in_len) return 0;
    in_taken = true;
    *p = sh->in;
    return sh->in_len;
}

bool px_has_input(px_shell_t *sh) {
    return (cur_stage && cur_stage->prev) || sh->in;
}

static void sink(px_shell_t *sh, const char *s, int len) {

    if (cur_stage) {
        /* A streaming pipe: Ctrl+C stops every stage at once, as a
         * signal would, so what they would say after it is not said. */
        if (sh->intr) return;
        if (cur_stage->producer) { pipe_write(cur_stage, s, len); return; }
    }

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

void px_write(px_shell_t *sh, const char *s, int len) {
    if (len > 0) sink(sh, s, len);
}

void px_printf(px_shell_t *sh, const char *fmt, ...) {
    /* One buffer, not per-call allocation. A shell that allocated to
     * print would fail exactly when reporting that memory ran out.
     * One per stage of a streaming pipe: a stage can hand over in the
     * middle of writing it (pipe_write()). */
    char *buf = st_pbuf;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, PX_LINE_MAX, fmt, ap);
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
    /* 256 entries, as the file browser (Z_FLIST_MAX): /docs on the
     * card holds 150 and was listed in part at 128. buf takes the
     * listing as full paths, so it is sized for 256 of ~30 bytes. */
    static char buf[8192];
    static z_fs_info_t info[256];
    static const char *names[256];
    static uint16_t order[256];
    uint32_t count = 0, truncated = 0;
    int longfmt = 0, argi = 1;

    /* -l: size and modification time (UTC -- the RTC's time; see
     * docs/filesystem.md, "Timestamps"). */
    if (argi < argc && !strcmp(argv[argi], "-l")) { longfmt = 1; argi++; }

    if (!px_resolve(argi < argc ? argv[argi] : ".", abs, sizeof(abs))) {
        px_printf(sh, "ls: bad path\n");
        sh->status = 1;
        return;
    }

    if (!fs_list_ex(abs, buf, sizeof(buf), info, 256, &count, &truncated)) {
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
     * An index array and an insertion sort: `count` is bounded at 256
     * and the strings and infos stay where they are.
     */
    const char *p = buf;

    for (uint32_t i = 0; i < count; i++) {
        /* Entries come back as full paths (sw/common/zfs.h), and in a
         * listing of one directory only the last component is wanted. */
        const char *slash = strrchr(p, '/');
        names[i] = slash ? slash + 1 : p;
        order[i] = (uint16_t)i;
        while (*p) p++;
        p++;
    }

    for (uint32_t i = 1; i < count; i++) {
        uint16_t k = order[i];
        uint32_t j = i;
        while (j > 0 && strcmp(names[order[j - 1]], names[k]) > 0) {
            order[j] = order[j - 1];
            j--;
        }
        order[j] = k;
    }

    for (uint32_t i = 0; i < count; i++) {
        const z_fs_info_t *fi = &info[order[i]];
        int dir = (fi->type == Z_FS_TYPE_DIR);
        if (longfmt) {
            if (dir) px_puts(sh, "     <dir>");
            else px_printf(sh, "%10lu", (unsigned long)fi->size);
            if (fi->fdate)
                px_printf(sh, "  %04u-%02u-%02u %02u:%02u  ",
                    1980 + (fi->fdate >> 9), (fi->fdate >> 5) & 15,
                    fi->fdate & 31, fi->ftime >> 11, (fi->ftime >> 5) & 63);
            else
                px_puts(sh, "                    ");
        }
        px_printf(sh, "%s%s\n", names[order[i]], dir ? "/" : "");
    }

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
            if (n <= 0 || px_intr(sh)) break;
            sink(sh, chunk, n);
        }
        fs_close_handle(h);
        if (px_intr(sh)) return;
    }
}

/* -- gzip: zcat and gunzip (sw/common/zgz.c, docs/posix.md) --------
 *
 * The 32 KB window zinflate needs is sw/common/formats' static one,
 * borrowed with zz_window_take() for the command and given back after:
 * unzip and tar (tools.c) inflate through the same window, and two
 * copies of 32 KB that are never in use at once would be one too many.
 * zgz checks the trailer -- the CRC-32 and length -- so a corrupt file
 * is reported, never quietly decoded to something else. */
static uint8_t *gz_window;
static z_gz_t gz_file;
static char gz_buf[1024];

static void bi_zcat(px_shell_t *sh, int argc, char **argv) {

    if (argc < 2) {
        px_puts(sh, "usage: zcat <file.gz> ...\n");
        sh->status = 1;
        return;
    }

    gz_window = zz_window_take();
    for (int i = 1; i < argc && gz_window && !px_intr(sh); i++) {
        char abs[PX_PATH_MAX];
        int32_t r;
        if (!px_resolve(argv[i], abs, sizeof(abs))) {
            px_printf(sh, "zcat: %s: bad path\n", argv[i]);
            sh->status = 1;
            continue;
        }
        if (z_gz_open(&gz_file, abs, gz_window) != 0) {
            px_printf(sh, "zcat: %s: cannot open\n", argv[i]);
            sh->status = 1;
            continue;
        }
        while ((r = z_gz_read(&gz_file, gz_buf, sizeof(gz_buf))) > 0 && !px_intr(sh))
            sink(sh, gz_buf, (int)r);
        if (r < 0) {
            px_printf(sh, "zcat: %s: %s\n", argv[i], z_gz_strerror(r));
            sh->status = 1;
        }
        z_gz_close(&gz_file);
    }
    if (gz_window) zz_window_give();
}

/*
 * gunzip [-k] [-f] FILE.gz ... writes FILE, as gzip does: only a name
 * ending .gz, no overwriting without -f, the .gz removed only once the
 * whole file is out and its CRC checked (kept with -k), and a partial
 * output removed if anything fails.
 */
static void bi_gunzip(px_shell_t *sh, int argc, char **argv) {

    int keep = 0, force = 0, files = 0;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-k")) { keep = 1; continue; }
        if (!strcmp(argv[i], "-f")) { force = 1; continue; }
        if (argv[i][0] == '-') {
            px_printf(sh, "gunzip: unknown option %s\n", argv[i]);
            sh->status = 1;
            return;
        }
    }

    gz_window = zz_window_take();
    for (int i = 1; i < argc && gz_window && !px_intr(sh); i++) {
        char abs[PX_PATH_MAX], out[PX_PATH_MAX];
        size_t n;
        int h, ok = 1;
        int32_t r;

        if (argv[i][0] == '-') continue;
        files++;
        if (!px_resolve(argv[i], abs, sizeof(abs))) {
            px_printf(sh, "gunzip: %s: bad path\n", argv[i]);
            sh->status = 1;
            continue;
        }
        n = strlen(abs);
        if (n < 4 || abs[n - 3] != '.' || (abs[n - 2] | 0x20) != 'g' || (abs[n - 1] | 0x20) != 'z' ||
                abs[n - 4] == '/') {
            px_printf(sh, "gunzip: %s: not a .gz name (zcat reads any)\n", argv[i]);
            sh->status = 1;
            continue;
        }
        memcpy(out, abs, n - 3);
        out[n - 3] = 0;

        if (!force && (h = fs_open_read(out)) >= 0) {
            fs_close_handle(h);
            px_printf(sh, "gunzip: %s exists (-f to overwrite)\n", out);
            sh->status = 1;
            continue;
        }
        if (z_gz_open(&gz_file, abs, gz_window) != 0) {
            px_printf(sh, "gunzip: %s: cannot open\n", argv[i]);
            sh->status = 1;
            continue;
        }
        if ((h = fs_open_write(out)) < 0) {
            z_gz_close(&gz_file);
            px_printf(sh, "gunzip: %s: cannot create\n", out);
            sh->status = 1;
            continue;
        }
        while ((r = z_gz_read(&gz_file, gz_buf, sizeof(gz_buf))) > 0) {
            if (px_intr(sh)) { ok = 0; break; }     /* Ctrl+C: removed below */
            if (fs_write_chunk(h, gz_buf, (int)r) != (int)r) {
                px_printf(sh, "gunzip: %s: write failed (card full?)\n", out);
                ok = 0;
                break;
            }
        }
        if (ok && r < 0) {
            px_printf(sh, "gunzip: %s: %s\n", argv[i], z_gz_strerror(r));
            ok = 0;
        }
        z_gz_close(&gz_file);
        fs_close_handle(h);
        if (!ok) {
            fs_unlink(out);                 /* nothing half-written left behind */
            sh->status = 1;
            continue;
        }
        if (!keep && !fs_unlink(abs)) {
            px_printf(sh, "gunzip: %s: decompressed, but not removed\n", argv[i]);
            sh->status = 1;
        }
    }

    if (gz_window) zz_window_give();
    if (!files && !px_intr(sh)) {
        px_puts(sh, "usage: gunzip [-k] [-f] <file.gz> ...\n");
        sh->status = 1;
    }
}

static void bi_rm(px_shell_t *sh, int argc, char **argv) {
    /* rm -r: a tree, through zwalk (tools.c) */
    if (argc > 2 && !strcmp(argv[1], "-r")) {
        for (int i = 2; i < argc && !px_intr(sh); i++) px_tree_op(sh, "rm", argv[i], NULL);
        return;
    }
    if (argc < 2) { px_puts(sh, "usage: rm [-r] <file> ...\n"); sh->status = 1; return; }
    for (int i = 1; i < argc; i++) {
        char abs[PX_PATH_MAX];
        if (!px_resolve(argv[i], abs, sizeof(abs)) || !fs_unlink(abs)) {
            px_printf(sh, "rm: %s: failed\n", argv[i]);
            sh->status = 1;
        }
    }
}

static void bi_mkdir(px_shell_t *sh, int argc, char **argv) {
    if (argc < 2) { px_puts(sh, "usage: mkdir <dir> ...\n"); sh->status = 1; return; }
    for (int i = 1; i < argc; i++) {
        char abs[PX_PATH_MAX];
        if (!px_resolve(argv[i], abs, sizeof(abs)) || !fs_mkdir(abs)) {
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
    /* cp -r: a tree, through zwalk (tools.c) */
    if (argc == 4 && !strcmp(argv[1], "-r")) { px_tree_op(sh, "cp", argv[2], argv[3]); return; }
    if (argc != 3) {
        px_puts(sh, "usage: cp [-r] <from> <to>\n");
        sh->status = 1;
        return;
    }
    if (!copy_file(sh, argv[1], argv[2])) sh->status = 1;
}

/*
 * Renames or moves: one rename in the filesystem, instant whatever the
 * size (FS_RENAME, docs/filesystem.md). Between volumes -- the card
 * and /ram, say -- a rename is impossible, and fs_move() copies and
 * then removes the source, only once the copy has succeeded, so an
 * interrupted `mv` loses nothing.
 *
 * As in Unix, a destination that is an existing directory means "into
 * it": `mv notes.txt docs` gives docs/notes.txt.
 */
static void bi_mv(px_shell_t *sh, int argc, char **argv) {

    char src[PX_PATH_MAX], dst[PX_PATH_MAX];
    z_fs_info_t fi;
    int err;

    if (argc != 3) {
        px_puts(sh, "usage: mv <from> <to>\n");
        sh->status = 1;
        return;
    }

    if (!px_resolve(argv[1], src, sizeof(src)) ||
        !px_resolve(argv[2], dst, sizeof(dst))) {
        px_puts(sh, "mv: bad path\n");
        sh->status = 1;
        return;
    }

    if (fs_stat(dst, &fi) && fi.type == Z_FS_TYPE_DIR) {
        const char *base = strrchr(src, '/');
        size_t n = strlen(dst);
        base = base ? base + 1 : src;
        if (n + 1 + strlen(base) + 1 > sizeof(dst)) {
            px_puts(sh, "mv: path too long\n");
            sh->status = 1;
            return;
        }
        if (n == 0 || dst[n - 1] != '/') dst[n++] = '/';
        strcpy(dst + n, base);
    }

    if (!fs_move(src, dst, &err)) {
        px_printf(sh, "mv: %s: %s\n", argv[1], fs_strerror(err));
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

/* Reconfigure the FPGA: the kernel syncs every open file first
 * (k_reboot, docs/zboot.md). Returns only if it could not. */
static void boot_failed(px_shell_t *sh, const char *what, int rc) {
    const char *why =
        rc == -1 ? "this board's gateware cannot reconfigure the FPGA (no PROGRAMN pin)" :
        rc == -2 ? "no jumploader at 0x200000 -- power-cycle instead" :
        rc == -3 ? "the jumploader could not be re-pointed" :
        rc == -4 ? "this gateware does not reload through a jumploader" :
        "the FPGA did not reconfigure";
    px_puts(sh, what);
    px_puts(sh, ": ");
    px_puts(sh, why);
    px_puts(sh, " (docs/zboot.md)\n");
}

static void bi_reboot(px_shell_t *sh, int argc, char **argv) {
    (void)argc; (void)argv;
    px_puts(sh, "rebooting...\n");
    boot_failed(sh, "reboot", z_jump(0));
}

/* Boot the gateware at a flash address, through the jumploader. */
static void bi_jump(px_shell_t *sh, int argc, char **argv) {
    char *end;
    unsigned long a;
    if (argc != 2) { px_puts(sh, "usage: jump ADDR (hex, 64 KB aligned)\n"); return; }
    a = strtoul(argv[1], &end, 16);
    if (*end || (a & 0xFFFF) || a > 0xFF0000) { px_puts(sh, "jump: ADDR is a hex flash address, 64 KB aligned\n"); return; }
    px_puts(sh, "jumping...\n");
    boot_failed(sh, "jump", z_jump((uint32_t)a));
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
/* line_truncated: with the stage state, above */

static bool feed_lines(px_shell_t *sh, const char *path,
                       line_fn fn, void *ctx) {

    line_truncated = false;
    feed_enough = false;

    /* the running stage's: a line can be handed over half-read */
    char *buf = st_rbuf;
    char *line = st_line;
    int n = 0;

    if (!path) {
        /* From the pipeline: a piece at a time as it comes. */
        const char *p;
        int left;
        while (!feed_enough && !px_intr(sh) && (left = px_in_next(sh, &p)) > 0) {
            for (; left > 0 && !feed_enough; p++, left--) {
                if (*p == '\n') {
                    line[n] = 0;
                    fn(sh, line, n, ctx);
                    n = 0;
                } else if (n < PX_LINE_MAX - 1) {
                    line[n++] = *p;
                } else {
                    line_truncated = true;
                }
            }
        }
        if (n && !feed_enough) { line[n] = 0; fn(sh, line, n, ctx); }
        return true;
    }

    char abs[PX_PATH_MAX];
    if (!px_resolve(path, abs, sizeof(abs))) return false;

    int h = fs_open_read(abs);
    if (h < 0) return false;

    for (;;) {
        int got = fs_read_chunk(h, buf, (int)sizeof(rbuf0));
        if (got <= 0 || feed_enough || px_intr(sh)) break;
        for (int i = 0; i < got && !feed_enough; i++) {
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
    if (n && !feed_enough) { line[n] = 0; fn(sh, line, n, ctx); }
    return true;
}

bool px_feed_lines(px_shell_t *sh, const char *path, px_line_fn fn, void *ctx, bool *cut) {
    bool ok = feed_lines(sh, path, fn, ctx);
    if (cut) *cut = line_truncated;
    return ok;
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
        const char *p;
        int k;
        while (!px_intr(sh) && (k = px_in_next(sh, &p)) > 0) wc_bytes(a, p, k);
        return true;
    }

    char abs[PX_PATH_MAX];
    if (!px_resolve(path, abs, sizeof(abs))) return false;

    int h = fs_open_read(abs);
    if (h < 0) return false;

    for (;;) {
        int got = fs_read_chunk(h, buf, (int)sizeof(buf));
        if (got <= 0 || px_intr(sh)) break;
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
    if (h->seen >= h->want) px_feed_enough();   /* the rest is not read: a pipe's writer stops */
}

/*
 * head and tail share their argument parsing: an optional -N before
 * the file. Not the full POSIX -n N form, because a shell with no
 * getopt is better with one spelling than with two that disagree.
 */
static int count_arg(int *argc, char **argv, int def) {
    /* -n N and -nN, POSIX's spelling, as well as -N */
    if (*argc > 2 && !strcmp(argv[1], "-n") && argv[2][0] >= '0' && argv[2][0] <= '9') {
        int v = atoi(argv[2]);
        for (int i = 1; i + 2 < *argc; i++) argv[i] = argv[i + 2];
        *argc -= 2;
        return v;
    }
    if (*argc > 1 && argv[1][0] == '-' && argv[1][1] == 'n' && argv[1][2] >= '0' && argv[1][2] <= '9') {
        int v = atoi(argv[1] + 2);
        for (int i = 1; i + 1 < *argc; i++) argv[i] = argv[i + 1];
        (*argc)--;
        return v;
    }
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

/* grep is text.c's: regular expressions, -E -F -i -v -n -c -l -q. */

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

/* sort's options: -n, -r, -u, -f, -k N (a field, from 1), -t C (its
 * separator; blanks if not given). */
static struct { bool num, rev, uniq, fold; int key; char sep; } so;

/* The key of a line: from field so.key on, or the whole line. */
static const char *sort_key(const char *l) {
    if (so.key <= 1) return l;
    int f = 1;
    const char *p = l;
    while (*p && f < so.key) {
        if (so.sep) {
            if (*p++ == so.sep) f++;
        } else {
            while (*p == ' ' || *p == '\t') p++;           /* blanks, then a field */
            while (*p && *p != ' ' && *p != '\t') p++;
            f++;
        }
    }
    if (!so.sep) while (*p == ' ' || *p == '\t') p++;
    return p;
}

/* A leading number, as -n reads it: blanks, a sign, digits, a fraction. */
static int num_cmp(const char *a, const char *b) {
    while (*a == ' ' || *a == '\t') a++;
    while (*b == ' ' || *b == '\t') b++;
    int sa = *a == '-' ? -1 : 1, sb = *b == '-' ? -1 : 1;
    if (*a == '-') a++;
    if (*b == '-') b++;
    const char *ia = a, *ib = b;
    while (*ia >= '0' && *ia <= '9') ia++;
    while (*ib >= '0' && *ib <= '9') ib++;
    bool za = true, zb = true;                      /* is it zero? -0 is 0 */
    for (const char *q = a; q < ia; q++) if (*q != '0') za = false;
    for (const char *q = b; q < ib; q++) if (*q != '0') zb = false;
    const char *fa = *ia == '.' ? ia + 1 : ia, *fb = *ib == '.' ? ib + 1 : ib;
    for (const char *q = fa; *q >= '0' && *q <= '9'; q++) if (*q != '0') za = false;
    for (const char *q = fb; *q >= '0' && *q <= '9'; q++) if (*q != '0') zb = false;
    if (za) sa = 1;
    if (zb) sb = 1;
    if (sa != sb) return sa < sb ? -1 : 1;
    /* the same sign: compare magnitudes, then flip for negatives */
    while (*a == '0' && a < ia) a++;
    while (*b == '0' && b < ib) b++;
    int la = (int)(ia - a), lb = (int)(ib - b), c = 0;
    if (la != lb) c = la < lb ? -1 : 1;
    else {
        c = strncmp(a, b, (size_t)la);
        if (!c) {                                   /* the fractions, digit by digit */
            while (!c && ((*fa >= '0' && *fa <= '9') || (*fb >= '0' && *fb <= '9'))) {
                char x = (*fa >= '0' && *fa <= '9') ? *fa++ : '0';
                char y = (*fb >= '0' && *fb <= '9') ? *fb++ : '0';
                c = (x > y) - (x < y);
            }
        }
    }
    return sa * c;
}

static int str_cmp(const char *a, const char *b) {
    if (!so.fold) return strcmp(a, b);
    for (;; a++, b++) {
        int x = (unsigned char)*a, y = (unsigned char)*b;
        if (x >= 'a' && x <= 'z') x -= 32;
        if (y >= 'a' && y <= 'z') y -= 32;
        if (x != y || !x) return x - y;
    }
}

static int line_cmp(const char *a, const char *b) {
    const char *ka = sort_key(a), *kb = sort_key(b);
    int c = so.num ? num_cmp(ka, kb) : str_cmp(ka, kb);
    return so.rev ? -c : c;
}

static void bi_sort(px_shell_t *sh, int argc, char **argv) {

    static sort_t s;
    s.n = 0; s.used = 0; s.full = false;
    memset(&so, 0, sizeof(so));

    const char *path = NULL;
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (a[0] == '-' && a[1]) {
            if (a[1] == 'k' || a[1] == 't') {
                const char *v = a[2] ? a + 2 : (i + 1 < argc ? argv[++i] : NULL);
                if (!v) { px_printf(sh, "sort: -%c needs a value\n", a[1]); sh->status = 1; return; }
                if (a[1] == 'k') so.key = atoi(v);
                else so.sep = v[0];
                continue;
            }
            for (const char *o = a + 1; *o; o++) {
                if (*o == 'n') so.num = true;
                else if (*o == 'r') so.rev = true;
                else if (*o == 'u') so.uniq = true;
                else if (*o == 'f') so.fold = true;
                else { px_printf(sh, "sort: unknown option -%c\n", *o); sh->status = 1; return; }
            }
            continue;
        }
        path = a;
    }

    if (!feed_lines(sh, path, sort_line, &s)) {
        px_printf(sh, "sort: %s: cannot open\n", path);
        sh->status = 1;
        return;
    }

    if (s.full) {
        px_printf(sh, "sort: input larger than %d lines or %d bytes\n",
                  SORT_MAX, SORT_TEXT);
        sh->status = 1;
        return;
    }

    /* Insertion sort on the offset array -- stable, so equal keys keep
     * their order. O(n^2) on 512 lines is a few hundred thousand
     * comparisons -- well under a frame on this machine, and it is
     * thirty lines less code than anything faster. If sorting a big
     * file ever matters, the LIMIT will hurt first. */
    for (int i = 1; i < s.n; i++) {
        int v = s.off[i];
        int j = i;
        while (j > 0 && line_cmp(s.text + s.off[j - 1], s.text + v) > 0) {
            s.off[j] = s.off[j - 1];
            j--;
        }
        s.off[j] = v;
    }

    for (int i = 0; i < s.n; i++) {
        /* -u: a line whose key equals the one before is left out */
        if (so.uniq && i && !line_cmp(s.text + s.off[i - 1], s.text + s.off[i])) continue;
        px_printf(sh, "%s\n", s.text + s.off[i]);
    }
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
        if (!px_resolve(argv[i], abs, sizeof(abs)) || !fs_unlink(abs)) {
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
 * That is precisely what zcc's /tmp/zcc.args stopgap was waiting for:
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
 * WHICH PROGRAM. The kernel resolves a bare name in /apps and then in
 * flash, and knows nothing about this shell's working directory -- so
 * the shell looks there itself, first. `zcc -o test test.c` then `test`
 * runs ./test, not an installed /apps/test (docs/layout.md, "Finding a
 * program"). Unlike Unix there is no need to type `./`: this is a
 * single-user machine, so the reason Unix keeps the working directory
 * out of the search does not apply, and the rule people expect is
 * "the program I just made is the one that runs". Only a real
 * executable counts, and the shell says so when it hides an installed
 * program of the same name. A name with a '/' in it is a path,
 * resolved against the working directory here because the kernel
 * would take it from the root.
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

    // The program gets its arguments as one string. Each is quoted if
    // it needs to be -- a space, a quote -- so a program that splits
    // the string with z_args_split() (zcc, zfpga) gets them back
    // exactly; one that takes the whole string as a path gets it back
    // too (z_launch_path_take(), zwin.h). docs/posix.md, "Quoting".
    char args[PX_LINE_MAX];
    if (!z_args_join(argc - 2, argv + 2, args, sizeof(args))) {
        px_puts(sh, "run: arguments too long\n");
        sh->status = 1;
        return;
    }

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

    const char *prog = argv[1];
    char here[PX_PATH_MAX];
    if (strchr(prog, '/')) {
        if (px_resolve(prog, here, sizeof(here))) prog = here;
    } else if (px_resolve(prog, here, sizeof(here)) && z_exec_exists(here)) {
        if (z_exec_exists(argv[1]))
            px_printf(sh, "%s: running %s, not the installed one\n", argv[1], here);
        prog = here;
    }

    uint32_t pid = z_proc_run(prog);
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

/* -- processes -- */

#define PX_PS_MAX 32            /* the kernel's Z_PROCS_MAX (sw/os/kernel.h) */

/* One line per process: its pid (what `kill` takes), the name it
 * registered, whether it is running, waiting or being killed, its
 * memory, and the CPU it has used since it started. repl's (ps) is the
 * same snapshot as data (sw/common/zapi/zapi.c). */
static void bi_ps(px_shell_t *sh, int argc, char **argv) {
    static z_proc_info_t procs[PX_PS_MAX];
    uint32_t truncated = 0, n;
    (void)argc; (void)argv;

    n = z_proc_list(procs, PX_PS_MAX, &truncated);
    px_puts(sh, "  PID NAME                     STATE    MEM KB    CPU s\n");
    for (uint32_t i = 0; i < n; i++) {
        const z_proc_info_t *p = &procs[i];
        const char *state = (p->flags & Z_PROC_FLAG_DIE) ? "dying" :
            (p->flags & Z_PROC_FLAG_LOADING) ? "load" :
            (p->flags & Z_PROC_FLAG_BLOCKED) ? "wait" : "run";
        px_printf(sh, "%5lu %-24s %-5s %9lu %8lu\n", (unsigned long)p->pid,
            p->name[0] ? p->name : "-", state, (unsigned long)(p->size / 1024),
            (unsigned long)(p->cpu_ticks / Z_TICK_HZ));
    }
    if (truncated) px_puts(sh, "(more processes than this list holds)\n");
    sh->status = 0;
}

/* `free`: the memory every program is loaded into -- the kernel's pool
 * -- and this shell's own share of it. "largest" is the one to watch:
 * a program needs one block that big, so a pool with plenty free can
 * still refuse it if the free space is in pieces. repl's (free) is the
 * same numbers as data. */
static void bi_free(px_shell_t *sh, int argc, char **argv) {
    z_mem_stats_args_t m;
    (void)argc; (void)argv;
    memset(&m, 0, sizeof(m));
    if (!z_mem_stats(&m)) {
        px_puts(sh, "free: unavailable\n");
        sh->status = 1;
        return;
    }
    px_puts(sh, "          total     used     free  largest\n");
    px_printf(sh, "KB     %8lu %8lu %8lu %8lu\n",
        (unsigned long)(m.total / 1024), (unsigned long)(m.used / 1024),
        (unsigned long)(m.free / 1024), (unsigned long)(m.largest_free / 1024));
    px_printf(sh, "blocks: %lu used, %lu free (%lu of %lu descriptors)\n",
        (unsigned long)m.used_blocks, (unsigned long)m.free_blocks,
        (unsigned long)m.blocks_used, (unsigned long)m.blocks_max);
#ifndef PX_HOST_TEST
    {
        /* This process: its static footprint (_start.._end, the linker's)
         * and what malloc() has added past it -- as repl's (free). */
        extern char _end, _start;
        void *sbrk(intptr_t);
        uint32_t stat = (uint32_t)&_end - (uint32_t)&_start;
        uint32_t heap = (uint32_t)sbrk(0) - (uint32_t)&_end;
        px_printf(sh, "this shell: %lu KB static, %lu KB heap\n",
            (unsigned long)(stat / 1024), (unsigned long)(heap / 1024));
        /* The commands' stack (job.c): its size, and the most any
         * command has used of it since posix started -- the number to
         * look at if PX_JOB_STACK is ever in doubt. */
        px_printf(sh, "command stack: %lu KB, at most %lu bytes used so far\n",
            (unsigned long)(px_job_stack_size() / 1024), (unsigned long)px_job_stack_peak());
    }
#endif
    sh->status = 0;
}

/* `kill PID|NAME ...`: a pid from ps, or a registered name (net0,
 * netserve0). No ownership check -- any process may kill any other,
 * the kernel's own trust model (z_proc_kill(), zeitlos.h). */
static void bi_kill(px_shell_t *sh, int argc, char **argv) {
    sh->status = 0;
    if (argc < 2) {
        px_puts(sh, "usage: kill PID|NAME ...   (see ps)\n");
        sh->status = 2;
        return;
    }
    for (int i = 1; i < argc; i++) {
        char *end;
        uint32_t pid = (uint32_t)strtoul(argv[i], &end, 10);
        if (*end || end == argv[i]) {
            if (!z_pid_lookup(argv[i], &pid)) {
                px_printf(sh, "kill: %s: no such process\n", argv[i]);
                sh->status = 1;
                continue;
            }
        }
        if (!pid || z_proc_kill(pid) != Z_OK) {
            px_printf(sh, "kill: %s: could not kill it\n", argv[i]);
            sh->status = 1;
        }
    }
}

/* `port NAME`: this terminal, to another port -- repl0, console0,
 * serial0, or anything registered. */
static void bi_port(px_shell_t *sh, int argc, char **argv) {
    char err[128];
    if (argc != 2) {
        px_puts(sh, "usage: port NAME   (repl0, console0, serial0 ...; F12 comes back)\n");
        sh->status = 2;
        return;
    }
    if (!px_switch_port(px_current_conn, argv[1], err, sizeof(err))) {
        px_printf(sh, "port: %s\n", err);
        sh->status = 1;
        return;
    }
    px_printf(sh, "connecting to %s -- disconnecting now\n", argv[1]);
    sh->status = 0;
}

static void bi_exit(px_shell_t *sh, int argc, char **argv) {
    sh->want_exit = true;
    sh->status = (argc > 1) ? atoi(argv[1]) : 0;
}

typedef struct {
    const char *name;
    void (*fn)(px_shell_t *, int, char **);
    const char *help;
} builtin_t;

static void bi_which(px_shell_t *sh, int argc, char **argv);

static const builtin_t builtins[] = {
    { "cd",    bi_cd,    "change directory" },
    { "pwd",   bi_pwd,   "print the working directory" },
    { "ls",    bi_ls,    "list a directory (-l: size and date)" },
    { "cat",   bi_cat,   "print files" },
    { "zcat",  bi_zcat,  "print .gz files, decompressed" },
    { "gunzip", bi_gunzip, "FILE.gz to FILE (-k keep it, -f overwrite)" },
    { "echo",  bi_echo,  "print arguments" },
    { "rm",    bi_rm,    "delete files (-r: directories and all in them)" },
    { "cp",    bi_cp,    "copy a file (-r: a directory and all in it)" },
    { "mv",    bi_mv,    "rename or move a file or directory" },
    { "touch", bi_touch, "create a file if it does not exist" },
    { "clear", bi_clear, "clear the screen" },
    { "reboot", bi_reboot, "reconfigure the FPGA, after syncing open files" },
    { "jump", bi_jump, "boot the gateware at a flash address (hex)" },
    { "mkdir", bi_mkdir, "make directories" },
    { "find",  px_bi_find,  "list a tree  (find [DIR] -name '*.c' -type f|d [-exec CMD {} \\;])" },
    { "diff",  px_bi_diff,  "differences between two files, as diff -u" },
    { "unzip", px_bi_unzip, "unzip [-l] [-o] A.zip [-d DIR]" },
    { "tar",   px_bi_tar,   "tar -xf|-tf A.tar|.tgz|.tar.gz [-C DIR] [-o]" },
    { "wc",    bi_wc,    "count lines, words and characters" },
    { "head",  bi_head,  "first lines of a file  (head -5 f, head -n 5 f)" },
    { "tail",  bi_tail,  "last lines of a file   (tail -5 f, tail -n 5 f)" },
    { "grep",  px_bi_grep, "lines matching a pattern  (grep -i -n 'a.*b' f; -E -F -v -c -l -q)" },
    { "sort",  bi_sort,  "sort lines  (-n numbers, -r reverse, -u once, -f any case, -k N, -t C)" },
    { "uniq",  bi_uniq,  "drop adjacent duplicate lines" },
    { "sed",   px_bi_sed,  "edit lines  (sed 's/old/new/g' f, -n, -E, -i)" },
    { "tr",    px_bi_tr,   "translate or delete characters  (tr a-z A-Z, -d, -s)" },
    { "cut",   px_bi_cut,  "fields or characters of lines  (cut -d: -f1,3 f)" },
    { "tee",   px_bi_tee,  "copy input to files and onward  (tee [-a] f)" },
    { "cmp",   px_bi_cmp,  "first difference between two files (-s: status only)" },
    { "xxd",   px_bi_xxd,  "hex dump  (xxd [-s OFF] [-l LEN] f)" },
    { "date",  px_bi_date, "date and time  (date, date -u, date +%F)" },
    { "which", bi_which, "what a name runs: a builtin or a program" },
    { "seq",   px_bi_seq,  "numbers in a row  (seq 5, seq 2 2 10, -s SEP)" },
    { "rev",   px_bi_rev,  "each line backwards" },
    { "basename", px_bi_basename, "the last part of a path  (basename /a/b.txt .txt)" },
    { "dirname", px_bi_dirname, "a path less its last part" },
    { "rmdir", bi_rmdir, "remove an empty directory" },
    { "df",    bi_df,    "filesystem usage" },
    { "run",   bi_run,   "run a Zeitlos program, passing arguments" },
    { "ps",    bi_ps,    "list processes" },
    { "free",  bi_free,  "memory: the pool programs load into, and this shell" },
    { "kill",  bi_kill,  "stop a process, by pid or name (kill netserve0)" },
    { "port",  bi_port,  "connect this terminal to another port (port repl0)" },
    { "help",  bi_help,  "this list" },
    { "exit",  bi_exit,  "close this session" },
    { 0, 0, 0 }
};

/*
 * What a name runs, found the way exec_simple() and bi_run() find it:
 * a builtin first, then a program in the working directory, then an
 * installed one (the card's /apps, or the flash archive).
 */
static void bi_which(px_shell_t *sh, int argc, char **argv) {
    if (argc < 2) { px_puts(sh, "usage: which NAME ...\n"); sh->status = 1; return; }
    for (int a = 1; a < argc; a++) {
        const char *name = argv[a];
        bool found = false;
        for (int i = 0; builtins[i].name && !found; i++)
            if (!strcmp(builtins[i].name, name)) {
                px_printf(sh, "%s: shell builtin\n", name);
                found = true;
            }
        char here[PX_PATH_MAX], apps[PX_PATH_MAX];
        if (!found && !strchr(name, '/') && px_resolve(name, here, sizeof(here)) && z_exec_exists(here)) {
            px_printf(sh, "%s\n", here);
            found = true;
        }
        if (!found && strchr(name, '/') && px_resolve(name, here, sizeof(here)) && z_exec_exists(here)) {
            px_printf(sh, "%s\n", here);
            found = true;
        }
        if (!found && !strchr(name, '/') && z_exec_exists(name)) {
            snprintf(apps, sizeof(apps), "/apps/%s", name);
            if (fs_size(apps) > 0) px_printf(sh, "%s\n", apps);
            else px_printf(sh, "%s: in flash\n", name);
            found = true;
        }
        if (!found) { px_printf(sh, "which: %s: not found\n", name); sh->status = 1; }
    }
}

/*
 * One builtin, for find -exec: the words already split, no redirection,
 * no wildcards, no programs, and where find's own output goes. A
 * program cannot be one: the shell starts it and goes on, so there is
 * nothing to wait on per file. Nor can find itself -- its walk's state
 * is static.
 */
bool px_run_builtin(px_shell_t *sh, int argc, char **argv) {
    if (!strcmp(argv[0], "find")) {
        px_puts(sh, "find: -exec cannot run find\n");
        return false;
    }
    for (int i = 0; builtins[i].name; i++) {
        if (strcmp(argv[0], builtins[i].name)) continue;
        int st = sh->status;
        builtins[i].fn(sh, argc, argv);
        int r = sh->status;
        sh->status = st;
        return r == 0;
    }
    px_printf(sh, "find: -exec %s: not a builtin -- -exec runs the shell's own commands\n", argv[0]);
    return false;
}

static void bi_help(px_shell_t *sh, int argc, char **argv) {
    (void)argc; (void)argv;
    px_puts(sh, "posix -- see docs/posix.md\n");
    for (int i = 0; builtins[i].name; i++)
        px_printf(sh, "  %-8s %s\n", builtins[i].name, builtins[i].help);
    px_puts(sh, "\nanything else is run as a program: 'zcc hello.c -o hello'\n"
                "redirection: > >> <   chaining: && and ||\n"
                "quoting: 'a b' \"a b\" a\\ b   wildcards: * ? [abc]\n"
                "Ctrl+C stops a command that is running or waiting its turn\n");
}

/* ------------------------------------------------------------------ */
/* the line                                                            */

/*
 * Splits a command into arguments, with quoting: '...', "..." and \x
 * (sw/common/zargs.h, docs/posix.md "Quoting").
 *
 * This used to split on spaces and nothing else, deliberately: a Zeitlos
 * path could not contain a space, and every argument here is a path or a
 * flag. Long file names changed the first half of that -- `My
 * Notes.txt` is a real file now -- so an argument has to be able to hold
 * one.
 *
 * The arguments are written into a buffer of their own rather than cut
 * out of the line in place: a quoted wildcard is marked in them for the
 * globbing below, and marking can make an argument longer than the text
 * it came from. Each argument's Z_ARG_* flags say whether it was quoted
 * (so it can never be an operator like `>`) and whether it has a
 * wildcard to expand.
 */
/* tok_buf: with the stage state, above (st_tok) */

static int split(const char *line, char **argv, uint8_t *fl, int max) {
    return z_args_split(line, st_tok, sizeof(tok_buf), argv, fl, max);
}

/*
 * Pulls `> file` or `>> file` out of the argument vector.
 *
 * Done after splitting rather than during, so that `>file` and `> file`
 * both work without the splitter needing to know about redirection --
 * the splitter's job is whitespace and nothing else.
 */
static bool take_redirect(px_shell_t *sh, int *argc, char **argv, uint8_t *fl) {

    for (int i = 0; i < *argc; i++) {

        int append = 0;
        char *target = 0;

        // A quoted '>' is an argument: `echo '>'` prints it.
        if (fl[i] & Z_ARG_QUOTED) continue;

        if (!strcmp(argv[i], ">") || !strcmp(argv[i], ">>")) {
            append = (argv[i][1] == '>');
            if (i + 1 >= *argc) {
                px_puts(sh, "syntax error: expected a filename after >\n");
                return false;
            }
            target = argv[i + 1];
            for (int j = i; j + 2 <= *argc; j++) { argv[j] = argv[j + 2]; fl[j] = fl[j + 2]; }
            *argc -= 2;
        } else if (argv[i][0] == '>') {
            append = (argv[i][1] == '>');
            target = argv[i] + (append ? 2 : 1);
            if (!*target) {
                px_puts(sh, "syntax error: expected a filename after >\n");
                return false;
            }
            for (int j = i; j + 1 <= *argc; j++) { argv[j] = argv[j + 1]; fl[j] = fl[j + 1]; }
            *argc -= 1;
        } else {
            continue;
        }

        // The target is a name, never a pattern: `> *.txt` writes a
        // file called that, as other shells do when it would match
        // nothing, rather than guessing which file was meant.
        z_args_unmark(target);

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

/* The first word of a stage: its builtin's name, or "" if it is quoted. */
static int stage_name(const char *c, char *out, int cap) {
    int k = 0;
    while (*c == ' ') c++;
    if (*c == '\'' || *c == '"' || *c == '\\') { out[0] = 0; return 0; }
    while (*c && *c != ' ' && k < cap - 1) out[k++] = *c++;
    out[k] = 0;
    return k;
}

/* An unquoted `ch` in the stage's text. */
static bool stage_has(const char *c, char ch) {
    while (*c) {
        if (*c == '\'' || *c == '"' || *c == '\\') { c = z_args_skip(c); continue; }
        if (*c == ch) return true;
        c++;
    }
    return false;
}

/*
 * Can these stages stream? Every one a builtin (a program writes to its
 * own stdout relay, which the old path reports), none in it twice, at
 * most one of the inflate window's users, no find -exec (it runs other
 * builtins, whose statics it would share), `<` only on the first stage
 * and `>` only on the last -- and the memory for it given (main.c).
 */
static bool pipe_streams(px_shell_t *sh, char **stages, int n) {
    (void)sh;
    if (!pipes_mem || n > PX_PIPE_STAGES ||
        pipes_bytes < (size_t)(n - 1) * PX_STAGE_MEM) return false;
    char names[PX_PIPE_STAGES][24];
    int inflaters = 0;
    for (int i = 0; i < n; i++) {
        char *nm = names[i];
        if (!stage_name(stages[i], nm, sizeof(names[i]))) return false;
        bool found = false;
        for (int b = 0; builtins[b].name; b++) if (!strcmp(builtins[b].name, nm)) found = true;
        if (!found || !strcmp(nm, "run") || !strcmp(nm, "exit")) return false;
        for (int j = 0; j < i; j++) if (!strcmp(names[j], nm)) return false;
        if (!strcmp(nm, "zcat") || !strcmp(nm, "gunzip") || !strcmp(nm, "unzip") || !strcmp(nm, "tar"))
            if (++inflaters > 1) return false;
        if (strstr(stages[i], "-exec")) return false;
        if (i > 0 && stage_has(stages[i], '<')) return false;
        if (i < n - 1 && stage_has(stages[i], '>')) return false;
    }
    return true;
}

static size_t pipe_stack_last;          /* the deepest stage of the last pipe */
size_t px_pipe_stack_last(void) { return pipe_stack_last; }

static void stage_body(void *arg) {
    px_stage_t *st = arg;
    exec_simple(st->sh, st->cmd);
    stage_drain(st->prev);      /* it read what it wanted: the rest stops */
}

/* Runs a pipe that streams: the stages before the last in coroutines of
 * their own, on stacks and buffers from px_pipes_init()'s memory; the
 * last here, in the command's own context, pulling. */
static void pipe_run(px_shell_t *sh, char **cmd, int n) {
    uint8_t *m = pipes_mem;
    for (int i = 0; i < n; i++) {
        px_stage_t *st = &stage_tab[i];
        memset(st, 0, sizeof(*st));
        st->sh = sh;
        st->cmd = cmd[i];
        st->idx = i;
        st->prev = i ? &stage_tab[i - 1] : NULL;
        st->producer = i < n - 1;
        st->v_out_fd = 1;
        if (!st->producer) continue;
        st->stack = m;                       m += PX_STAGE_STACK;
        st->pipe = (char *)m;                m += PX_PIPE_BUF;
        st->tok = (char *)m;                 m += sizeof(tok_buf);
        st->glob = (char *)m;                m += sizeof(glob_buf);
        st->pbuf = (char *)m;                m += PX_LINE_MAX;
        st->line = (char *)m;                m += PX_LINE_MAX;
        st->rbuf = (char *)m;                m += sizeof(rbuf0);
        st->bbuf = (char *)m;                m += sizeof(bbuf0);
        zco_init(&st->co, stage_body, st, st->stack, PX_STAGE_STACK);
    }

    px_stage_t *last = &stage_tab[n - 1];
    const char *in0 = sh->in;
    int in0_len = sh->in_len;
    stage_point(last);
    exec_simple(sh, last->cmd);
    stage_drain(last->prev);
    int status = sh->status;

    bool over = false;
    pipe_stack_last = 0;
    for (int i = 0; i < n - 1; i++) {
        if (stage_tab[i].overflow) over = true;
        size_t u = zco_stack_used(&stage_tab[i].co);
        if (u > pipe_stack_last) pipe_stack_last = u;
    }
    stage_point(NULL);
    sh->in = in0;
    sh->in_len = in0_len;
    if (over) {
        /* A stage ran off its stack and was stopped where it stood:
         * what it held is let go of, as job.c does for a command. */
        px_job_release();
        px_close_all_files();
        px_printf(sh, "posix: a stage of the pipe ran out of stack (%d bytes) and was stopped\n",
            PX_STAGE_STACK);
        status = 1;
    }
    sh->status = status;
}

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
        // A quoted '|' is an argument, not a pipe.
        if (*p == '\'' || *p == '"' || *p == '\\') { p = (char *)z_args_skip(p); continue; }
        if (p[0] == '|' && p[1] != '|') {
            *p++ = 0;
            while (*p == ' ') p++;
            stages[n++] = p;
            continue;
        }
        p++;
    }

    if (n == 1) { exec_simple(sh, cmd); return; }

    if (pipe_streams(sh, stages, n)) { pipe_run(sh, stages, n); return; }

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

        if (sh->intr) { cap_active = false; break; }

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

/*
 * Expands the arguments with unquoted wildcards (docs/posix.md,
 * "Wildcards"). `*.c` becomes every matching name in the current
 * directory, and `src/` followed by `*.c` every match in src, in the order `ls` would list
 * them. A pattern that matches nothing is kept as it was typed, as
 * other shells do, so `rm *.bak` with nothing to remove says so rather
 * than running `rm` with no arguments.
 *
 * Wildcards are matched in the LAST part of a path only: one in a
 * directory part (`src*` followed by a slash) is taken literally. Names starting with '.' are matched only by
 * a pattern that starts with one, the Unix convention.
 *
 * The expanded names live in glob_buf, which lasts until the next
 * command; argv points into it.
 */
/* glob_buf: with the stage state, above (st_glob) */
static char glob_list[4096];
static uint8_t glob_types[128];

static int glob_cmp(const char *a, const char *b) {
    // As ls sorts (bi_ls), so a wildcard lists files in the same order.
    return strcmp(a, b);
}

static bool expand_globs(px_shell_t *sh, int *argc, char **argv, uint8_t *fl) {

    char *out[PX_MAX_ARGS];
    uint8_t ofl[PX_MAX_ARGS];
    int n = 0;
    size_t used = 0;

    /* find's -name PATTERN is find's to match, in every directory it
     * walks -- not the shell's, in this one. Passed on as typed, its
     * quoted characters still marked, which z_glob_match() honours. */
    bool find_cmd = *argc > 0 && !strcmp(argv[0], "find");

    for (int i = 0; i < *argc; i++) {

        if (!(fl[i] & Z_ARG_WILD) ||
            (find_cmd && i > 0 && !strcmp(argv[i - 1], "-name"))) {
            if (n >= PX_MAX_ARGS) goto too_many;
            ofl[n] = fl[i];
            out[n++] = argv[i];
            continue;
        }

        // Split at the last '/': the directory, as typed, and the
        // pattern for the names in it.
        char *pat = argv[i];
        char *slash = strrchr(pat, '/');
        char dir[PX_PATH_MAX];
        const char *base = pat;
        if (slash) {
            size_t dl = (size_t)(slash - pat);
            if (dl >= sizeof(dir)) dl = sizeof(dir) - 1;
            memcpy(dir, pat, dl);
            dir[dl] = 0;
            z_args_unmark(dir);
            base = slash + 1;
        } else {
            dir[0] = 0;
        }

        int first = n;
        char abs[PX_PATH_MAX];
        uint32_t count = 0, truncated = 0;
        const char *listdir = slash ? (dir[0] ? dir : "/") : ".";

        if (px_resolve(listdir, abs, sizeof(abs)) &&
            fs_list_into(abs, glob_list, sizeof(glob_list), glob_types,
                sizeof(glob_types), &count, &truncated)) {

            const char *p = glob_list;
            for (uint32_t k = 0; k < count; k++) {
                const char *name = strrchr(p, '/');
                name = name ? name + 1 : p;
                size_t pl = strlen(p);
                bool dot_ok = (base[0] == '.');
                if ((name[0] != '.' || dot_ok) && z_glob_match(base, name)) {
                    if (n >= PX_MAX_ARGS) goto too_many;
                    size_t need = (slash ? strlen(dir) + 1 : 0) + strlen(name) + 1;
                    if (used + need > sizeof(glob_buf)) goto too_many;
                    char *w = st_glob + used;
                    if (slash) {
                        strcpy(w, dir);
                        strcat(w, "/");
                        strcat(w, name);
                    } else {
                        strcpy(w, name);
                    }
                    used += need;
                    // Sorted into place among this pattern's matches.
                    int j = n;
                    while (j > first && glob_cmp(out[j - 1], w) > 0) {
                        out[j] = out[j - 1];
                        ofl[j] = ofl[j - 1];
                        j--;
                    }
                    out[j] = w;
                    ofl[j] = Z_ARG_QUOTED;  // a name, not an operator
                    n++;
                }
                p += pl + 1;
            }
        }

        if (n == first) {
            // Nothing matched: the pattern itself, as typed.
            z_args_unmark(argv[i]);
            if (n >= PX_MAX_ARGS) goto too_many;
            ofl[n] = fl[i];
            out[n++] = argv[i];
        }

    }

    for (int i = 0; i < n; i++) { argv[i] = out[i]; fl[i] = ofl[i]; }
    *argc = n;
    return true;

too_many:
    px_printf(sh, "posix: too many names from a wildcard (at most %d arguments)\n",
        PX_MAX_ARGS);
    return false;

}

/*
 * `< file`: the file as the command's input, exactly as if it were the
 * previous stage of a pipe -- so `tr a-z A-Z < f` is `cat f | tr a-z
 * A-Z`, held in full like a pipe's, at most CAP_MAX bytes. A larger
 * file is refused rather than cut: a filter that sees half its input
 * gives a wrong answer that looks right.
 */
static char in_buf[CAP_MAX];

static bool take_input(px_shell_t *sh, int *argc, char **argv, uint8_t *fl) {
    for (int i = 0; i < *argc; i++) {
        if (fl[i] & Z_ARG_QUOTED) continue;
        char *target;
        int drop;
        if (!strcmp(argv[i], "<")) {
            if (i + 1 >= *argc) { px_puts(sh, "syntax error: expected a filename after <\n"); return false; }
            target = argv[i + 1];
            drop = 2;
        } else if (argv[i][0] == '<' && argv[i][1]) {
            target = argv[i] + 1;
            drop = 1;
        } else continue;
        z_args_unmark(target);
        char abs[PX_PATH_MAX];
        int h = px_resolve(target, abs, sizeof(abs)) ? fs_open_read(abs) : -1;
        if (h < 0) { px_printf(sh, "posix: %s: cannot open\n", target); return false; }
        int n = 0;
        for (;;) {
            int got = fs_read_chunk(h, in_buf + n, CAP_MAX - n);
            if (got <= 0) break;
            n += got;
            if (n == CAP_MAX) {
                char extra;
                if (fs_read_chunk(h, &extra, 1) > 0) {
                    fs_close_handle(h);
                    px_printf(sh, "posix: < %s: larger than %d bytes -- give the command the file instead\n",
                        target, CAP_MAX);
                    return false;
                }
                break;
            }
        }
        fs_close_handle(h);
        sh->in = in_buf;
        sh->in_len = n;
        for (int j = i; j + drop <= *argc; j++) { argv[j] = argv[j + drop]; fl[j] = fl[j + drop]; }
        *argc -= drop;
        return true;
    }
    return true;
}

static void exec_simple(px_shell_t *sh, char *cmd) {

    char *argv[PX_MAX_ARGS];
    uint8_t fl[PX_MAX_ARGS];
    int argc = split(cmd, argv, fl, PX_MAX_ARGS);
    const char *saved_in = sh->in;
    int saved_len = sh->in_len;

    if (!argc) return;

    out_fd = 1;
    sh->status = 0;
    in_taken = false;

    if (!take_redirect(sh, &argc, argv, fl)) { sh->status = 1; return; }
    if (!take_input(sh, &argc, argv, fl)) { sh->status = 1; goto done; }
    if (!expand_globs(sh, &argc, argv, fl)) { sh->status = 1; goto done; }
    if (!argc) goto done;

    for (int i = 0; builtins[i].name; i++) {
        if (strcmp(argv[0], builtins[i].name)) continue;
        builtins[i].fn(sh, argc, argv);
        px_close_all_files();
        out_fd = 1;
        if (sh->intr) {
            /* To the terminal whatever the redirection: it is the
             * person being told, not the file. Once, by the last stage
             * of a streaming pipe. */
            if (!cur_stage || !cur_stage->producer) out_raw(sh, "^C\r\n", 4);
            sh->status = 130;
        }
        sh->in = saved_in;
        sh->in_len = saved_len;
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

done:
    px_close_all_files();
    out_fd = 1;
    sh->in = saved_in;
    sh->in_len = saved_len;
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
            // Quoted && and || are arguments -- skip over quotes.
            if (*p == '\'' || *p == '"' || *p == '\\') { p = (char *)z_args_skip(p); continue; }
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
            if (sh->want_exit || sh->intr) return;     /* Ctrl+C: the rest of the line too */

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
    sh->intr = false;
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
    sh->intr = false;

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

/*
 * After a command that was stopped without unwinding -- it ran off its
 * stack (job.c) -- the shell's own statics are put back as they are
 * between commands: output to the terminal, no pipe being filled, no
 * descriptor open, the inflate window free for the next zcat or unzip.
 */
void px_shell_abandon(void) {
    for (int i = 0; i < PX_PIPE_STAGES; i++)
        if (stage_tab[i].producer && !stage_tab[i].done) zco_abandon(&stage_tab[i].co);
    memset(stage_tab, 0, sizeof(stage_tab));
    stage_point(NULL);
    in_taken = false;
    cap_active = false;
    cap_len = 0;
    cap_overflow = false;
    out_fd = 1;
    px_close_all_files();
    gz_window = NULL;
    zz_window_give();
    px_tools_abandon();
}
