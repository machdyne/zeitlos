/*
 * posix's main loop, on the build machine: the REAL main.c (included
 * below), its sessions and its coroutine, with this file playing the
 * kernel's messages and the term windows at the other end.
 *
 *   make -C sw/apps/posix test       (run.sh runs it after the others)
 *
 * The other harnesses drive sh.c directly; this one drives what a
 * person at two terminals does, and checks that a long command in one
 * session does not hold up the other (docs/posix.md, "How a command
 * runs"):
 *
 *   1. two sessions: one runs a long `find`, the other types and runs
 *      `echo` -- its typing is echoed at once, its command waits its
 *      turn (with a note) and then runs; both outputs are complete
 *   2. Ctrl+C in the middle of each long builtin: ^C, status 130,
 *      nothing left open or allocated, no half-written file left
 *   3. Ctrl+C on a command still waiting its turn: taken out
 *   4. a session that closes (or whose term dies) during its command:
 *      the command unwinds, nothing more is sent, its slot is not
 *      reused until it has, and a new session works
 *   5. a slow terminal: all of a big `cat` arrives, in order, nothing
 *      dropped -- and the other session is served meanwhile
 *   6. typeahead during a command runs at the next prompt
 *   7. a child: the session waits for it without holding up the other
 *   8. a command that runs off its stack: stopped, told, let go of
 *
 * The clock moves one tick each time it is read (and by the wait in
 * z_proc_wait()), so a slice -- Z_TICK_HZ/100 ticks -- is a few
 * px_intr() calls: a long command takes many passes of the loop, which
 * is what lets the other session get in between.
 *
 * Exit 0 on success.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <sys/stat.h>

/* main.c's diagnostics (to the console on the machine) go to a log
 * here, shown only with PX_VERBOSE. */
static int test_log(const char *fmt, ...);
#define printf test_log
#include "../main.c"
#undef printf

void host_fs_set_root(const char *dir);
int host_open_handles(void);

static const char *root;
static int failures;
static bool verbose;

static int test_log(const char *fmt, ...) {
    va_list ap;
    if (!verbose) return 0;
    va_start(ap, fmt);
    fputs("    [posix] ", stdout);
    int n = vprintf(fmt, ap);
    va_end(ap);
    return n;
}

#define CHECK(cond, ...) do { if (!(cond)) { failures++; \
    fprintf(stdout, "FAIL  %s:%d: ", __FILE__, __LINE__); \
    fprintf(stdout, __VA_ARGS__); fputc('\n', stdout); } } while (0)

/* ------------------------------------------------------------------ */
/* the kernel                                                          */

static uint32_t now;
static uint32_t waits;              /* z_proc_wait() calls */

uint32_t z_uptime_ticks(void) { return now++; }
void z_proc_wait(uint32_t t) { now += t; waits++; }

#define INBOX 256
static z_msg_t inbox[INBOX];
static int in_head, in_n;

static void post(uint32_t from, uint32_t subject, uint32_t tag, z_obj_t obj) {
    if (in_n == INBOX) { fprintf(stdout, "FATAL inbox full\n"); exit(1); }
    z_msg_t *m = &inbox[(in_head + in_n++) % INBOX];
    memset(m, 0, sizeof(*m));
    m->to = 1;
    m->from = from;
    m->subject = subject;
    m->tag = tag;
    m->obj = obj;
}

z_rv z_msg_read(z_msg_t *m) {
    if (!in_n) return Z_FAIL;
    *m = inbox[in_head];
    in_head = (in_head + 1) % INBOX;
    in_n--;
    return Z_OK;
}

z_obj_t z_obj_none(void) { z_obj_t o; memset(&o, 0, sizeof(o)); o.type = Z_NONE; return o; }
uint32_t z_blob_len(const z_obj_t *o) { return o->type == Z_BLOB ? ((z_blob_t *)o->val.ptr)->len : 0; }
void *z_blob_data(const z_obj_t *o) { return o->type == Z_BLOB ? ((z_blob_t *)o->val.ptr)->data : NULL; }

bool z_pid_register(const char *base, char *out, uint32_t len) { snprintf(out, len, "%s0", base); return true; }
uint32_t z_getpid(void) { return 1; }

/* Children: host_fs.c's z_proc_run() starts pid 7; it runs until a
 * scenario says it has exited. */
static bool child_running;
z_rv z_proc_status(uint32_t pid, uint32_t *state, int32_t *status) {
    *state = (pid == 7 && child_running) ? Z_PROC_STATE_RUNNING : Z_PROC_STATE_EXITED;
    *status = 0;
    return Z_OK;
}

void z_conn_handoff(uint32_t term_pid, const z_conn_target_t *t) { (void)term_pid; (void)t; }
bool z_conn_prepare(z_conn_kind_t kind, const char *text, z_conn_target_t *out, char *err, size_t errlen) {
    (void)kind; (void)text; (void)out; snprintf(err, errlen, "no ports here"); return false;
}

/* ------------------------------------------------------------------ */
/* the terminals                                                       */

typedef struct {
    uint32_t pid;
    bool     alive;
    bool     connected;
    uint32_t conn_id;
    char    *screen;            /* everything posix sent it */
    size_t   n, cap;
    int      unacked;           /* messages posix has sent and it has not acked */
    int      ack_every;         /* 0: acks everything each pass; N: one every N passes */
    int      tick;
    bool     closed_by_posix;
} term_t;

#define NTERMS 4
static term_t terms[NTERMS];

static term_t *term_of(uint32_t pid) {
    for (int i = 0; i < NTERMS; i++) if (terms[i].pid == pid) return &terms[i];
    return NULL;
}

z_rv z_port_send(z_port_t *p, const void *d, uint32_t n) {
    term_t *t = term_of(p->peer_pid);
    if (!p->connected || !t) return Z_FAIL;
    if (p->pending_count >= Z_PORT_MAX_PENDING_SENDS) return Z_FAIL;
    if (t->n + n + 1 > t->cap) {
        t->cap = (t->n + n + 1) * 2;
        t->screen = realloc(t->screen, t->cap);
    }
    memcpy(t->screen + t->n, d, n);
    t->n += n;
    t->screen[t->n] = 0;
    t->unacked++;
    p->pending_count++;
    return Z_OK;
}

void z_port_handle_ack(z_port_t *p, const z_msg_t *m) {
    if (!p->connected || m->tag != p->conn_id || !p->pending_count) return;
    p->pending_count--;
}

void z_port_send_ack(const z_msg_t *m) { (void)m; }

void z_port_accept(z_port_t *p, const z_msg_t *m, uint32_t id) {
    term_t *t = term_of(m->from);
    p->peer_pid = m->from;
    p->conn_id = id;
    p->connected = true;
    p->pending_count = 0;
    p->pending_head = 0;
    if (t) { t->connected = true; t->conn_id = id; }
}

void z_port_close(z_port_t *p) {
    term_t *t = term_of(p->peer_pid);
    if (!p->connected) return;
    p->connected = false;
    if (t) { t->connected = false; t->closed_by_posix = true; }
}

void z_port_forget(z_port_t *p) { p->pending_count = 0; p->connected = false; }
void z_port_refuse(const z_msg_t *m, const char *why) { (void)m; (void)why; }
void z_port_reject_stranger(const z_msg_t *m) { (void)m; }
bool z_port_refuse_unauthenticated(const z_msg_t *m, const char *who) { (void)m; (void)who; return false; }
bool z_port_pid_running(uint32_t pid) { term_t *t = term_of(pid); return t ? t->alive : child_running; }
bool z_port_peer_gone(const z_port_t *p) { return p->connected && !z_port_pid_running(p->peer_pid); }

static void term_connect(term_t *t, uint32_t pid) {
    free(t->screen);
    memset(t, 0, sizeof(*t));
    t->pid = pid;
    t->alive = true;
    post(pid, Z_PORT_CONNECT, 0, z_obj_none());
}

static void term_type(term_t *t, const char *s) {
    z_blob_t *b = malloc(sizeof(*b));        /* kept: the test is short */
    z_obj_t o = z_obj_none();
    b->len = (uint32_t)strlen(s);
    b->data = (uint8_t *)strdup(s);
    o.type = Z_BLOB;
    o.val.ptr = b;
    post(t->pid, Z_PORT_DATA, t->conn_id, o);
}

static void term_close(term_t *t) {
    post(t->pid, Z_PORT_CLOSE, t->conn_id, z_obj_none());
    t->connected = false;
}

/* What a terminal does between two passes: acks what it has read. */
static void terms_ack(void) {
    for (int i = 0; i < NTERMS; i++) {
        term_t *t = &terms[i];
        if (!t->alive || !t->connected) { t->unacked = 0; continue; }
        while (t->unacked > 0) {
            if (t->ack_every && ++t->tick % t->ack_every) break;
            post(t->pid, Z_PORT_DATA_ACK, t->conn_id, z_obj_none());
            t->unacked--;
            if (t->ack_every) break;
        }
    }
}

static void pass(void) { terms_ack(); serve_pass(); }

static px_conn_t *conn_of(const term_t *t) {
    for (int i = 0; i < PX_MAX_CONNS; i++)
        if (conns[i].port.peer_pid == t->pid) return &conns[i];
    return NULL;
}

/* Passes until nothing runs or waits, and every terminal has its
 * output. */
static int settle(void) {
    int n = 0;
    for (; n < 2000000; n++) {
        pass();
        bool unsent = false;
        for (int i = 0; i < PX_MAX_CONNS; i++) if (conns[i].out_len && conns[i].port.connected) unsent = true;
        if (!px_job_pending() && !unsent) { pass(); return n; }
    }
    CHECK(0, "never settled");
    return n;
}

static bool ends_with(const term_t *t, const char *s) {
    size_t k = strlen(s);
    return t->n >= k && !memcmp(t->screen + t->n - k, s, k);
}

/* What `line` prints when run directly, as the terminal shows it. */
static char expect_buf[4 << 20];
static size_t expect_n;
static void expect_out(void *ctx, const char *b, int n) {
    (void)ctx;
    if (expect_n + (size_t)n < sizeof(expect_buf) - 1) { memcpy(expect_buf + expect_n, b, (size_t)n); expect_n += (size_t)n; }
    expect_buf[expect_n] = 0;
}
static const char *expected(const char *line) {
    px_shell_t sh;
    char l[PX_LINE_MAX];
    snprintf(l, sizeof(l), "%s", line);
    expect_n = 0;
    expect_buf[0] = 0;
    px_shell_init(&sh, expect_out, NULL);
    px_exec_line(&sh, l);
    return expect_buf;
}

/* A fresh pair of sessions: A and B, banners and prompts read. */
static term_t *A, *B;
static void two_sessions(void) {
    for (int i = 0; i < NTERMS; i++) if (terms[i].connected) term_close(&terms[i]);
    settle();
    term_connect(&terms[0], 100 + now % 1000);
    term_connect(&terms[1], 1100 + now % 1000);
    A = &terms[0];
    B = &terms[1];
    settle();
    CHECK(ends_with(A, "$ ") && ends_with(B, "$ "), "no prompts");
}

static bool exists(const char *rel) {
    char p[1024];
    struct stat st;
    snprintf(p, sizeof(p), "%s/%s", root, rel);
    return stat(p, &st) == 0;
}

static void nothing_held(const char *what) {
    CHECK(host_open_handles() == 0, "%s: %d file(s) left open", what, host_open_handles());
    CHECK(px_alloc_count() == 0, "%s: %d block(s) left allocated", what, px_alloc_count());
}

/* ------------------------------------------------------------------ */

static void s1_two_sessions(const char *cmd) {
    printf("1. a long %s in one session, echo in the other\n", cmd);
    two_sessions();
    char *want = strdup(expected(cmd));
    char line[PX_LINE_MAX];
    snprintf(line, sizeof(line), "%s\r", cmd);

    term_type(A, line);
    pass();
    for (int k = 0; k < 30; k++) pass();
    CHECK(px_job_current() == &conn_of(A)->sh, "%s is not running after 30 passes", cmd);
    size_t a_before = A->n;

    term_type(B, "echo hi\r");
    pass();
    CHECK(strstr(B->screen, "echo hi\r\n") != NULL, "B's typing was not echoed at once");
    CHECK(strstr(B->screen, "(waiting for a command in another session") != NULL, "no waiting note");
    CHECK(!strstr(B->screen, "hi\r\n$ "), "B's echo ran before A's %s ended", cmd);
    CHECK(px_job_current() == &conn_of(A)->sh, "%s has ended already -- not a long command", cmd);

    int passes = settle();
    CHECK(A->n > a_before, "A's output stopped");
    CHECK(strstr(A->screen, want) != NULL, "A's %s output is not complete and in order", cmd);
    CHECK(ends_with(A, "\r\n$ "), "no prompt after %s", cmd);
    CHECK(ends_with(B, "hi\r\n$ "), "B's echo did not run after A's command");
    printf("   %zu bytes; B's typing echoed in the same pass, its echo ran %d passes later\n",
           strlen(want), passes);
    free(want);
    nothing_held("two sessions");
}

/* Ctrl+C after `slices` slices of the command's run. */
static bool quiet;
static void interrupt(const char *cmd, int slices, const char *gone_file) {
    two_sessions();
    char line[PX_LINE_MAX];
    snprintf(line, sizeof(line), "%s\r", cmd);
    term_type(A, line);
    pass();
    int k = 1;
    while (k < slices && px_job_pending()) { pass(); k++; }
    bool running = px_job_current() == &conn_of(A)->sh;
    CHECK(running, "%s: ended before Ctrl+C (make its input bigger)", cmd);
    term_type(A, "\x03");
    settle();
    CHECK(ends_with(A, "^C\r\n$ "), "%s: no ^C and prompt at the end", cmd);
    CHECK(conn_of(A)->sh.status == 130, "%s: status %d, not 130", cmd, conn_of(A)->sh.status);
    if (gone_file) CHECK(!exists(gone_file), "%s: left %s behind", cmd, gone_file);
    nothing_held(cmd);
    if (!quiet) printf("   %-38s stopped after %d passes\n", cmd, k);
}

static void s2_ctrl_c(void) {
    printf("2. Ctrl+C in the middle of each long builtin\n");
    interrupt("cat big.txt", 20, NULL);
    interrupt("find /", 20, NULL);
    interrupt("grep 7 big.txt", 20, NULL);
    interrupt("wc big.txt", 3, NULL);
    interrupt("head -100000 big.txt", 20, NULL);
    interrupt("tail -5 big.txt", 3, NULL);
    interrupt("uniq big.txt", 20, NULL);
    interrupt("sort big.txt", 2, NULL);
    interrupt("zcat big.gz", 20, NULL);
    interrupt("gunzip -k -f big.gz", 5, "big");
    interrupt("diff dbig.txt dbig2.txt", 3, NULL);
    interrupt("unzip -o arch/huge.zip -d cc_unzip", 5, "cc_unzip/big.bin");
    interrupt("tar -xf arch/huge.tgz -C cc_tar -o", 5, "cc_tar/big.bin");
    interrupt("cp -r bigdir cc_copy", 5, NULL);
    interrupt("cat big.txt | grep 7 | wc", 20, NULL);

    /* And at every one of the first dozen slices, for the ones that
     * hold the most: files being written, memory, the inflate window. */
    static const char *const many[] = {
        "unzip -o arch/huge.zip -d cc_unzip", "tar -xf arch/huge.tgz -C cc_tar -o",
        "gunzip -k -f big.gz", "diff dbig.txt dbig2.txt", "cp -r bigdir cc_copy_",
        "zcat big.gz",
    };
    quiet = true;
    for (unsigned i = 0; i < sizeof(many) / sizeof(many[0]); i++)
        for (int k = 1; k <= 12; k++) {
            char cmd[96];
            /* a copy goes somewhere new each time: never into what the
             * last one left (cp -r refuses an existing name) */
            snprintf(cmd, sizeof(cmd), "%s%s", many[i], many[i][strlen(many[i]) - 1] == '_' ?
                     (char[3]){ (char)('a' + k), 0, 0 } : "");
            interrupt(cmd, k, NULL);
        }
    quiet = false;
    printf("   and each of %u at every one of its first 12 slices\n",
           (unsigned)(sizeof(many) / sizeof(many[0])));
}

static void s3_cancel_queued(void) {
    printf("3. Ctrl+C on a command waiting its turn\n");
    two_sessions();
    const char *want = expected("find /");
    char *want_find = strdup(want);
    term_type(A, "find /\r");
    for (int k = 0; k < 10; k++) pass();
    term_type(B, "echo queued\r");
    pass();
    CHECK(px_job_state(&conn_of(B)->sh) == PX_JOB_QUEUED, "B's command is not waiting");
    term_type(B, "\x03");
    pass();
    CHECK(px_job_state(&conn_of(B)->sh) == PX_JOB_NONE, "B's command was not taken out");
    CHECK(ends_with(B, "^C\r\n$ "), "B: no ^C and prompt");
    term_type(B, "echo now\r");
    settle();
    CHECK(!strstr(B->screen, "queued\r\n$"), "the cancelled command ran");
    CHECK(ends_with(B, "now\r\n$ "), "B's next command did not run");
    CHECK(strstr(A->screen, want_find) != NULL, "A's find was disturbed");
    free(want_find);
    nothing_held("cancel queued");
}

static void s4_close(bool dies) {
    printf("4. a session that %s during its command\n", dies ? "dies" : "closes");
    two_sessions();
    term_type(A, "cat big.txt\r");
    for (int k = 0; k < 20; k++) pass();
    px_conn_t *ca = conn_of(A);
    CHECK(px_job_current() == &ca->sh, "cat is not running");
    if (dies) { A->alive = false; A->connected = false; }
    else term_close(A);
    size_t at_close = A->n;
    pass();
    /* A new session now must not get the slot the command still uses. */
    term_t *C = &terms[2];
    term_connect(C, 2100 + now % 1000);
    pass();
    if (px_job_current() == &ca->sh)
        CHECK(conn_of(C) != ca, "a new session got the slot of a command still unwinding");
    settle();
    CHECK(A->n == at_close || dies, "output went to a closed session");
    CHECK(!A->closed_by_posix || !dies, "posix closed a dead peer");
    nothing_held(dies ? "dies" : "closes");
    term_type(C, "echo ok\r");
    settle();
    CHECK(ends_with(C, "ok\r\n$ "), "a new session does not work after it");
    term_close(C);
    settle();
}

static void s5_slow_terminal(void) {
    printf("5. a slow terminal: all of a big cat, and the other session served\n");
    two_sessions();
    char *want = strdup(expected("cat big.txt"));
    A->ack_every = 4;
    size_t start = A->n;
    term_type(A, "cat big.txt\r");
    for (int k = 0; k < 100; k++) pass();
    CHECK(px_job_current() == &conn_of(A)->sh, "cat is not running");
    uint32_t w0 = waits;
    size_t b0 = B->n;
    term_type(B, "ec");
    pass();
    CHECK(B->n == b0 + 2, "B's typing waited for A's terminal");
    for (int k = 0; k < 20; k++) pass();
    CHECK(waits > w0, "the loop spun while the command waited for its terminal");
    term_type(B, "ho b\r");
    settle();
    CHECK(strstr(A->screen + start, want) != NULL, "A's cat output is not all there, in order");
    CHECK(ends_with(B, "b\r\n$ "), "B's command did not run");
    printf("   %zu bytes through a terminal acking one message in four passes\n", strlen(want));
    A->ack_every = 0;
    free(want);
    nothing_held("slow terminal");
}

static void s6_typeahead(void) {
    printf("6. typeahead during a command\n");
    two_sessions();
    term_type(A, "find /\r");
    for (int k = 0; k < 10; k++) pass();
    term_type(A, "echo ahead\r");
    settle();
    settle();
    CHECK(ends_with(A, "$ echo ahead\r\nahead\r\n$ "), "the typeahead did not run at the next prompt");
}

static void s7_child(void) {
    printf("7. a child: its session waits, the other does not\n");
    two_sessions();
    child_running = true;
    term_type(A, "zcc x.c && echo after\r");
    settle();
    CHECK(conn_of(A)->waiting, "A is not waiting for its child");
    term_type(B, "echo b\r");
    settle();
    CHECK(ends_with(B, "b\r\n$ "), "B was held up by A's child");
    child_running = false;
    settle();
    CHECK(ends_with(A, "after\r\n$ "), "the rest of A's line did not run after the child");
}

static void s8_overflow(uint8_t *normal) {
    printf("8. a command that runs off its stack\n");
    two_sessions();
    /* A tiny stack, with room below it that the test owns: whatever
     * the overrun writes lands in pad. */
    static uint8_t pad[64 * 1024 + PX_JOB_SLACK + 1024];
    px_job_init(pad + 64 * 1024, PX_JOB_SLACK + 1024, Z_TICK_HZ / 100);
    term_type(A, "diff dbig.txt dbig2.txt\r");
    settle();
    CHECK(strstr(A->screen, "ran out of its 1 KB stack") != NULL, "no overflow message: %s", A->screen);
    CHECK(ends_with(A, "$ "), "no prompt after the overflow");
    nothing_held("overflow");
    printf("   %s\n", strstr(A->screen, "memory below") ? "it wrote below the stack (reported)" : "contained");
    px_job_init(normal, 4 * PX_JOB_STACK + PX_JOB_SLACK, Z_TICK_HZ / 100);
    term_type(A, "zcat notes.gz\r");
    settle();
    CHECK(ends_with(A, "second line\r\n$ "), "the next command (zcat: the inflate window) does not work");
    nothing_held("after overflow");
}

int main(int argc, char **argv) {

    if (argc < 2) { fprintf(stderr, "usage: %s ROOT\n", argv[0]); return 2; }
    root = argv[1];
    verbose = getenv("PX_VERBOSE") != NULL;
    host_fs_set_root(root);

    /* Four times the machine's: x86-64 frames are wider (run.sh's
     * table), and twice that again under AddressSanitizer. */
    static uint8_t job_mem[4 * PX_JOB_STACK + PX_JOB_SLACK];
    serve_init(job_mem, sizeof(job_mem));
    {
        char err[64];
        px_chdir("/", err, sizeof(err));
    }

    s1_two_sessions("find /");
    s1_two_sessions("cat big.txt");
    s2_ctrl_c();
    s3_cancel_queued();
    s4_close(false);
    s4_close(true);
    s5_slow_terminal();
    s6_typeahead();
    s7_child();
    s8_overflow(job_mem);

    printf("\nmain loop: %s (%d failure%s); command stack peak %zu bytes of %zu\n",
           failures ? "FAILED" : "ok", failures, failures == 1 ? "" : "s",
           px_job_stack_peak(), px_job_stack_size());
    return failures ? 1 : 0;
}
