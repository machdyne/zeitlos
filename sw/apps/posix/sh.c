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

static void sink(px_shell_t *sh, const char *s, int len) {

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
    { "mkdir", bi_mkdir, "make directories" },
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

static void exec_one(px_shell_t *sh, char *cmd) {

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
