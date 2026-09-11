/*
 * Assertion-based tests for posix's builtins.
 *
 * The other harness (run_sh.c) records a transcript and diffs it
 * against a golden copy. That catches REGRESSIONS and nothing else: it
 * would record `wc` counting wrong just as happily as counting right,
 * and the only signal would be a human reading the diff.
 *
 * These cases say what the answer should BE. A command with defined
 * behaviour -- wc, head, grep, cp -- deserves that; a transcript is
 * the right tool for the shell's overall shape and the wrong one for
 * arithmetic.
 *
 * Format (tests/cases.txt):
 *
 *   # comment
 *   $ command to run
 *   expected output, line by line
 *   ? 1                 <- expect this exit status (default 0)
 *
 * A case ends at the next `$` or `#`. Expected output is compared
 * exactly, with CRLF normalised to LF -- the shell emits CRLF to a
 * terminal and that is not what a test should be asserting about.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../posix.h"

void host_fs_set_root(const char *dir);
int host_child_status(void);

#define OUT_MAX 8192

static char out[OUT_MAX];
static int out_len;

static void capture(void *ctx, const char *buf, int len) {
    (void)ctx;
    for (int i = 0; i < len && out_len < OUT_MAX - 1; i++) {
        if (buf[i] == '\r') continue;       /* CRLF -> LF */
        out[out_len++] = buf[i];
    }
    out[out_len] = 0;
}

static int passed, failed;

static void report(const char *cmd, const char *want, int want_status,
                   int got_status) {

    if (!strcmp(out, want) && got_status == want_status) {
        passed++;
        return;
    }

    failed++;
    printf("FAIL  %s\n", cmd);
    if (got_status != want_status)
        printf("      status: got %d, want %d\n", got_status, want_status);
    if (strcmp(out, want)) {
        printf("      --- want ---\n");
        for (const char *p = want; *p; ) {
            const char *e = strchr(p, '\n');
            int n = e ? (int)(e - p) : (int)strlen(p);
            printf("      |%.*s\n", n, p);
            if (!e) break;
            p = e + 1;
        }
        printf("      --- got ----\n");
        for (const char *p = out; *p; ) {
            const char *e = strchr(p, '\n');
            int n = e ? (int)(e - p) : (int)strlen(p);
            printf("      |%.*s\n", n, p);
            if (!e) break;
            p = e + 1;
        }
    }
}

int main(int argc, char **argv) {

    char line[PX_LINE_MAX];
    char cmd[PX_LINE_MAX] = "";
    char want[OUT_MAX] = "";
    int want_status = 0;
    int have_cmd = 0;

    FILE *f;

    if (argc < 3) { fprintf(stderr, "usage: %s <root> <cases>\n", argv[0]); return 2; }

    host_fs_set_root(argv[1]);
    f = fopen(argv[2], "r");
    if (!f) { perror(argv[2]); return 2; }

    px_fd_init();

    while (1) {

        char *got = fgets(line, sizeof(line), f);
        char *nl;

        if (got) { nl = strchr(line, '\n'); if (nl) *nl = 0; }

        /* A new command, a comment, a BLANK LINE, or end of file
         * closes the case being collected.
         *
         * Blank lines terminate rather than being collected: they are
         * what separates cases in the file, and the first version
         * appended them to the expected output, so every case that was
         * followed by one expected a trailing empty line. Thirteen
         * false failures before the shell was even involved.
         *
         * The cost is that a command whose output legitimately
         * contains a blank line cannot be expressed. None does yet; a
         * marker can be added when one does. */
        if (!got || line[0] == '$' || line[0] == '#' || line[0] == 0) {

            if (have_cmd) {
                px_shell_t sh;
                px_shell_init(&sh, capture, NULL);
                out_len = 0; out[0] = 0;

                char run[PX_LINE_MAX];
                snprintf(run, sizeof(run), "%s", cmd);
                px_exec_line(&sh, run);

                while (sh.child_pid) px_resume(&sh, host_child_status());

                report(cmd, want, want_status, sh.status);
                have_cmd = 0;
                want[0] = 0;
                want_status = 0;
            }

            if (!got) break;
            if (line[0] == 0) continue;

            if (line[0] == '$') {
                const char *p = line + 1;
                while (*p == ' ') p++;
                snprintf(cmd, sizeof(cmd), "%s", p);
                have_cmd = 1;
            }
            continue;
        }

        if (line[0] == '?') {
            want_status = atoi(line + 1);
            continue;
        }

        if (have_cmd) {
            strncat(want, line, sizeof(want) - strlen(want) - 2);
            strncat(want, "\n", sizeof(want) - strlen(want) - 1);
        }
    }

    fclose(f);
    printf("\n%d passed, %d failed\n", passed, failed);
    return failed ? 1 : 0;
}
