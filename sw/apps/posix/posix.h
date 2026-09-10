/*
 * posix -- a Unix-shaped userland in one Zeitlos process.
 *
 * See docs/posix.md for what this is a phase of, and vfs.c's header
 * for what "POSIX" is and is not being claimed here.
 *
 * `term` reaches this the same way it reaches `repl`: it is a port
 * provider registered as `posix0`, so `port posix0` at a term prompt
 * connects to it. That needed no change to sw/common/zconnect.c --
 * the `port` target already takes any registered name -- and none to
 * `repl`, which keeps working exactly as before. The two coexist and
 * term's F11 bar picks between them.
 */

#ifndef POSIX_H
#define POSIX_H

#include <stdint.h>
#include <stdbool.h>

#define PX_PATH_MAX   256
#define PX_MAX_FD     12
#define PX_MAX_ARGS   24
#define PX_LINE_MAX   512

/* -- descriptors -- */

typedef enum {
    PX_FD_FREE = 0,
    PX_FD_TTY,          /* the port this shell is talking over */
    PX_FD_FILE
} px_fd_kind_t;

typedef struct {
    px_fd_kind_t kind;
    int handle;         /* zfsapp handle, for PX_FD_FILE */
} px_fd_t;

void px_fd_init(void);
px_fd_t *px_fd(int fd);
int px_open(const char *path, int write, char *err, int errlen);

/* Opens for writing, positioned at the end when `append` is set. A
 * separate entry point rather than a flag on px_open() because the two
 * use different underlying calls -- see its definition in vfs.c. */
int px_open_append(const char *path, int append, char *err, int errlen);
void px_close(int fd);
void px_close_all_files(void);

/* -- paths -- */

const char *px_getcwd(void);
bool px_resolve(const char *path, char *out, int cap);
bool px_chdir(const char *path, char *err, int errlen);

/*
 * -- output --
 *
 * Everything a command prints goes through this, and nothing calls
 * printf directly.
 *
 * The reason is that a command's output has three possible
 * destinations -- the port, a file, or nowhere -- and only the shell
 * knows which. A builtin that called printf would be writing to the
 * process's own UART, which is the kernel console and not the user's
 * terminal at all. That mistake is invisible on a desk with a serial
 * cable attached and total on a machine without one.
 */
typedef void (*px_out_fn)(void *ctx, const char *buf, int len);

typedef struct {
    px_out_fn   out;
    void       *ctx;
    int         status;         /* exit status of the last command */
    bool        want_exit;

    /* A child started by this command and not yet finished.
     *
     * The shell does NOT block on it -- this process serves up to four
     * terminal connections and blocking would stop serving all of
     * them. `px_exec_line()` returns with this set, main.c watches for
     * the exit as an ordinary event, and the rest of the line resumes
     * from `pending`.
     *
     * That is what makes `a && b` mean what it says: without it, `&&`
     * tested whether `a` STARTED. */
    uint32_t    child_pid;
    char        pending[PX_LINE_MAX];   /* rest of the line, after the child */
    int         pending_join;           /* 0 none, 1 &&, 2 || */
} px_shell_t;

void px_puts(px_shell_t *sh, const char *s);
void px_printf(px_shell_t *sh, const char *fmt, ...);

/* Runs one command line. Everything else in this app exists to call
 * this: it takes a line and produces output, with no reference to
 * ports, messages or windows -- which is what makes it testable off
 * the device. See tests/. */
void px_exec_line(px_shell_t *sh, char *line);

/* Continues a line whose command started a child, once that child has
 * exited. `status` becomes the shell's, so `&&` and `||` test what the
 * child actually did. */
void px_resume(px_shell_t *sh, int status);

void px_shell_init(px_shell_t *sh, px_out_fn out, void *ctx);

/*
 * The connection whose command is currently running.
 *
 * Set by main.c before px_exec_line() and read by main.c when a child
 * opens an output connection back -- see PX_STDOUT_TAG below. It lives
 * here rather than in main.c's statics because the shell is what
 * defines "currently running", even though only main.c uses the value.
 */
extern void *px_current_conn;

/*
 * The connect argument a child sends to say "this connection is my
 * stdout, not a terminal session".
 *
 * A spawned program's output would otherwise go to the kernel console
 * -- it is a separate process, its printf goes through _write() to the
 * UART, and the shell writes to a zport connection. Nothing joins them
 * unless the child asks.
 *
 * A distinct connect argument rather than a separate message subject:
 * z_port_connect_arg() already carries one (sw/common/zport.h, added
 * for net's telnet provider, which likewise has to know something
 * before it can decide whether to accept). Reusing it means no new
 * protocol and no change to zport.
 */
#define PX_STDOUT_TAG "stdout"

#endif
