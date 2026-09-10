/*
 * Drives posix's shell from a script on stdin, printing what a
 * terminal would have seen.
 *
 * The output is compared against a recorded transcript by run.sh. A
 * golden file rather than a differential comparison, unlike zcc's
 * suite -- there is no second shell to compare against, so the trade
 * is the other way round: this catches regressions well and cannot
 * catch a behaviour that was wrong from the start. Which is why the
 * transcript is meant to be READ when it changes, not just accepted.
 */

#include <stdio.h>
#include <string.h>
#include "../posix.h"

void host_fs_set_root(const char *dir);
int host_child_status(void);

static void emit(void *ctx, const char *buf, int len) {
    (void)ctx;
    fwrite(buf, 1, (size_t)len, stdout);
}

int main(int argc, char **argv) {

    char line[PX_LINE_MAX];

    if (argc > 1) host_fs_set_root(argv[1]);

    px_fd_init();

    while (fgets(line, sizeof(line), stdin)) {
        char *nl = strchr(line, '\n');
        if (nl) *nl = 0;
        if (line[0] == '#' || !line[0]) continue;

        px_shell_t sh;
        px_shell_init(&sh, emit, NULL);

        printf("$ %s\n", line);
        px_exec_line(&sh, line);

        /* The machine does this from its main loop, asynchronously,
         * when Z_SYS_PROC_STATUS says the child has gone. Here it is
         * immediate -- the point of the test is that the shell RESUMES
         * with the child's status, not how long it waited. */
        while (sh.child_pid) {
            int st = host_child_status();
            printf("[child %u exited %d]\n", (unsigned)sh.child_pid, st);
            px_resume(&sh, st);
        }

        if (sh.status) printf("[status %d]\n", sh.status);
        if (sh.want_exit) { printf("[exit]\n"); break; }
    }
    return 0;
}
