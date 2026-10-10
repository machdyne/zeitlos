/*
 * The POSIX calls nextvi makes that Zeitlos does not have.
 *
 * Every one of these is here because upstream calls it, not because
 * anything needs it to work -- which is why they are stubs rather than
 * implementations, and why each says what happens when it is reached.
 *
 * Keeping them in a separate file from vi_zeitlos.c is deliberate:
 * that file is the port, this one is the list of things the port does
 * NOT do. When one of these grows a real implementation it moves out.
 */

#include <stdint.h>
#include <stddef.h>
#include <errno.h>

#include <poll.h>
#include <dirent.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <sys/stat.h>

#include "../../common/zeitlos.h"

/* -- raw mode --
 *
 * Not needed. A Zeitlos port connection carries bytes exactly as the
 * terminal produced them: no canonical mode, no echo, no signal
 * characters. term_init() calls these to turn all of that off, and it
 * is already off. */
int tcgetattr(int fd, struct termios *t) { (void)fd; (void)t; return 0; }
int tcsetattr(int fd, int actions, const struct termios *t) {
    (void)fd; (void)actions; (void)t; return 0;
}

/*
 * -- window size --
 *
 * Fails, deliberately, so that term_init() falls through to its own
 * defaults of 80x25.
 *
 * `term` windows are resizable and this ought eventually to ask, which
 * means a message to term and a Z_TERM_* subject that does not exist
 * yet. Returning a wrong size confidently would be worse than
 * returning none: the editor would draw off the edge and the cursor
 * arithmetic would disagree with the screen, which is the same class
 * of bug the UTF-8 table avoids (sw/ext/nextvi/ZEITLOS.md).
 */
int ioctl(int fd, unsigned long request, ...) {
    (void)fd; (void)request;
    errno = ENOTTY;
    return -1;
}

/* stdin is a port connection, which is a terminal in every sense the
 * editor cares about. Saying otherwise makes term_read() set xquit and
 * the editor exits immediately. */
int isatty(int fd) { (void)fd; return 1; }

/* -- signals --
 *
 * There are none. SIGWINCH would be the useful one and it arrives as a
 * message here, not a signal; the rest are exit paths the editor does
 * not need on a machine where ^C is just a byte.
 *
 * signal() itself is NOT defined here. newlib has one, and its
 * signal.o gets linked in anyway for other things in that object -- so
 * defining our own is a duplicate-symbol error rather than an
 * override. newlib's implementation records the handler and never
 * calls it, which is exactly the behaviour wanted.
 *
 * kill() IS defined here, because newlib's calls _kill(), which
 * sw/common/zeitlos.c does not provide. */
int kill(int pid, int sig) { (void)pid; (void)sig; return 0; }

/*
 * -- poll --
 *
 * Always ready. term_read() polls before read(), and read() is
 * z_stdin_hook, which does its own blocking and message pumping --
 * so the poll has nothing left to wait for.
 */
int poll(struct pollfd *fds, nfds_t nfds, int timeout) {
    (void)timeout;
    /* Ready, always. Reporting POLLIN on every descriptor is right for
     * the one caller that matters -- term_read(), whose read() is
     * z_stdin_hook and does its own waiting -- and harmless for the
     * `:!` path, which cannot run at all without fork. */
    for (nfds_t i = 0; i < nfds; i++)
        fds[i].revents = fds[i].events;
    return (int)nfds;
}

/*
 * -- directories, stat, the working directory --
 *
 * opendir()/readdir()/closedir(), stat() and getcwd()/chdir() used to
 * be stubs here that failed, which cost `:e <tab>` completion and
 * `:cd`. They are real now: vi is built with POSIX_IO = 1, and
 * sw/common/zposix.c provides them over `fs` (docs/posix_io.md).
 */

/*
 * -- shelling out --
 *
 * `:!cmd`, `:r !cmd` and filters go through fork/execvp. Zeitlos has
 * no fork -- see docs/posix.md section 4.3 for why that is a decision
 * rather than a gap -- so these fail and the editor reports the
 * command did not run.
 *
 * This is the one visible loss of function in the port. It is also the
 * one with an obvious route back: `posix` can already start a program
 * and collect its output (docs/posix.md, "three cases"), so a `:!`
 * that asked posix to run something is a message, not a fork.
 */
/*
 * -- the environment --
 *
 * There is none. newlib's getenv() walks `environ`, which
 * sw/common/zeitlos.c does not define, so linking it would fail --
 * and defining an empty `environ` would work but be a lie that costs a
 * newlib object.
 *
 * nextvi asks for four names and each has a sensible answer without
 * one:
 *
 *   LINES, COLUMNS  the size ioctl already declines to give, so
 *                   term_init() falls through to 80x25 either way
 *   EXINIT          startup ex commands; there is no shell to set it
 *   PWD             only in `:cd`, which cannot work anyway (below)
 *   SHELL           only in the `:!` path, which has no fork
 *
 * Returning NULL for everything is therefore not a degradation -- it
 * is the same behaviour as an environment that does not set them.
 */
char *getenv(const char *name) { (void)name; return NULL; }

/*
 * -- the rest of the `:!` machinery --
 *
 * cmd_pipe() builds a pipeline around fork: two pipes, dup2 onto the
 * child's descriptors, and process-group juggling so the child can own
 * the terminal. None of it can work without fork, and all of it is
 * still LINKED -- the compiler cannot know the function is
 * unreachable, so every symbol it names has to resolve.
 *
 * Hence these. They are not a partial implementation; they are the
 * cost of the one real gap being in a function that is compiled rather
 * than #ifdef'd out.
 */
int pipe(int fds[2]) { (void)fds; errno = ENOSYS; return -1; }
int dup2(int old, int new) { (void)old; (void)new; errno = ENOSYS; return -1; }
int getpgrp(void) { return 1; }
int tcsetpgrp(int fd, int pgrp) { (void)fd; (void)pgrp; errno = ENOSYS; return -1; }

/*
 * setup_signals() installs handlers for SIGWINCH and friends. There
 * are no signals here: a resize arrives as a message and ^C is just a
 * byte. Reporting success is right -- the editor checks, and there is
 * nothing for it to do differently.
 */
struct sigaction;
int sigaction(int sig, const struct sigaction *act, struct sigaction *old) {
    (void)sig; (void)act; (void)old; return 0;
}

int fork(void) { errno = ENOSYS; return -1; }
int execvp(const char *f, char *const argv[]) {
    (void)f; (void)argv; errno = ENOSYS; return -1;
}
int waitpid(int pid, int *st, int opt) {
    (void)pid; (void)st; (void)opt; errno = ECHILD; return -1;
}
