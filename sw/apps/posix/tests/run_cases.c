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
 *   ! 5                 <- press Ctrl+C at the 5th px_intr() poll
 *
 * A case ends at the next `$` or `#`. Expected output is compared
 * exactly, with CRLF normalised to LF -- the shell emits CRLF to a
 * terminal and that is not what a test should be asserting about.
 *
 * Every case runs as it does on the machine: in job.c's coroutine,
 * handing back at EVERY px_intr() (a slice of 0), so each builtin is
 * cut at every point it can be cut. After each case nothing may be left
 * open or allocated -- Ctrl+C cases included. The coroutine's stack is
 * large here (256 KB) and its use is recorded per command, into
 * $PX_STACK_FILE; run.sh prints the table PX_JOB_STACK is sized from
 * (docs/posix.md, "The command's stack").
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

#include "../posix.h"
#include "zco.h"

void host_fs_set_root(const char *dir);
int host_child_status(void);
int host_open_handles(void);

#define CO_STACK (256 * 1024)

/* Stack used, per command name (the first word, or the one after a
 * leading `cd DIR &&`). */
typedef struct { char name[16]; size_t used; char cmd[64]; } use_t;
static use_t uses[64];
static int nuses;

static void record_use(const char *cmd, size_t used) {
    char name[16];
    const char *p = cmd;
    if (!strncmp(p, "cd ", 3) && strstr(p, "&& ")) p = strstr(p, "&& ") + 3;
    int n = 0;
    while (p[n] && p[n] != ' ' && n < 15) { name[n] = p[n]; n++; }
    name[n] = 0;
    for (int i = 0; i < nuses; i++) {
        if (strcmp(uses[i].name, name)) continue;
        if (used > uses[i].used) { uses[i].used = used; snprintf(uses[i].cmd, sizeof(uses[i].cmd), "%.63s", cmd); }
        return;
    }
    if (nuses < 64) {
        snprintf(uses[nuses].name, sizeof(uses[nuses].name), "%s", name);
        uses[nuses].used = used;
        snprintf(uses[nuses].cmd, sizeof(uses[nuses].cmd), "%.63s", cmd);
        nuses++;
    }
}

static int cmp_use(const void *a, const void *b) {
    const use_t *x = a, *y = b;
    return x->used < y->used ? 1 : x->used > y->used ? -1 : strcmp(x->name, y->name);
}

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

/* Ctrl+C, simulated: sh->poll answers true from the Nth call on --
 * main.c's conn_poll() on the device. -1: never. */
static long intr_after = -1, polls;

static bool test_poll(void *ctx) {
    (void)ctx;
    polls++;
    return intr_after >= 0 && polls >= intr_after;
}

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

    static uint8_t co_mem[CO_STACK];
    px_job_init(co_mem, sizeof(co_mem), 0);

    /* The fixture tree stands in for a whole card and these tests are
     * written from its root. The shell itself starts in /home
     * (Z_DIR_HOME, docs/layout.md), which the fixture does not have. */
    {
        char err[64];
        px_chdir("/", err, sizeof err);
    }

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
                sh.poll = test_poll;
                polls = 0;
                out_len = 0; out[0] = 0;

                /* In the coroutine, as on the machine: started, then
                 * resumed until it ends -- and again after each child. */
                px_shell_t *ended;
                int r, slices = 0;
                size_t used = 0;
                px_job_submit(&sh, cmd);
                for (;;) {
                    while ((r = px_job_step(&ended)) == PX_STEP_RAN || r == PX_STEP_BLOCKED) slices++;
                    if (px_job_stack_last() > used) used = px_job_stack_last();
                    if (r != PX_STEP_DONE) { printf("FAIL  %s: step %d\n", cmd, r); failed++; break; }
                    if (!sh.child_pid) break;
                    px_job_submit_resume(&sh, host_child_status());
                }
                record_use(cmd, used);

                report(cmd, want, want_status, sh.status);

                /* Nothing left behind, however it ended. */
                if (host_open_handles() || px_alloc_count()) {
                    printf("FAIL  %s: left %d file(s) open, %d block(s) allocated\n",
                           cmd, host_open_handles(), px_alloc_count());
                    failed++;
                }
                have_cmd = 0;
                want[0] = 0;
                want_status = 0;
                intr_after = -1;
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

        if (line[0] == '!') {
            intr_after = atol(line + 1);
            continue;
        }

        if (have_cmd) {
            strncat(want, line, sizeof(want) - strlen(want) - 2);
            strncat(want, "\n", sizeof(want) - strlen(want) - 1);
        }
    }

    fclose(f);

    /* The stack table: deepest first. Host (x86-64) frames, so wider
     * than the machine's -- see docs/posix.md for both. */
    if (getenv("PX_STACK_FILE")) {
        FILE *sf = fopen(getenv("PX_STACK_FILE"), "a");
        qsort(uses, (size_t)nuses, sizeof(uses[0]), cmp_use);
        for (int i = 0; sf && i < nuses; i++)
            fprintf(sf, "%s %zu %s\n", uses[i].name, uses[i].used, uses[i].cmd);
        if (sf) fclose(sf);
    }

    printf("\n%d passed, %d failed\n", passed, failed);
    return failed ? 1 : 0;
}
