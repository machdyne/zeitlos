/*
 * posix -- running a command: one at a time, in a coroutine
 * (docs/posix.md, "How a command runs"; docs/responsive.md).
 *
 * A builtin is one long call -- `find /`, `diff`, `unzip` are loops
 * with their state deep in their locals -- so it runs as a coroutine
 * (sw/common/zco.h) on a stack of its own, and the main loop resumes it
 * whenever it has nothing else to do. It hands back:
 *
 *  - from px_intr(), which every long builtin already calls between
 *    pieces of work, once its slice (about 10 ms) is used -- so the
 *    checks stay as cheap as they were, a clock read each;
 *  - from the output path when the terminal has not taken what was
 *    sent (px_job_wait()), until its acks come back.
 *
 * Meanwhile the main loop reads every message and serves every
 * session: typing, echo, acks, connects, children's output.
 *
 * -- one at a time --
 *
 * The builtins keep their working state in statics: sh.c's argument
 * buffer (argv points into it), redirection (out_fd and the descriptor
 * table, which px_close_all_files() empties after every command), the
 * pipe buffers, px_printf()'s buffer, find's walker, diff's buffers,
 * the archives' state, the one 32 KB inflate window, zgz's stream, cp
 * -r's walker. Two commands interleaved would each trample the other's,
 * and so would even `echo` run between two slices of a `find` (it
 * re-splits into the same argument buffer). So ONE command runs at a
 * time, and a command typed in another session waits in a queue here,
 * in the order it was typed; Ctrl+C takes it out. The process-wide
 * working directory is shared anyway (vfs.c).
 *
 * -- stopping --
 *
 * Never by abandoning the coroutine: a builtin holds open files,
 * malloc()ed memory, the inflate window. Ctrl+C sets the session's flag;
 * the builtin sees it at its next px_intr() and unwinds through its own
 * cleanup, exactly as before.
 *
 * -- running off the stack --
 *
 * zco notices at the next hand-back that the guard at the stack's
 * bottom was written (zco.h): the command is finished on the spot,
 * never resumed. What it held is then let go here: the descriptor
 * table and every file this process has open (one command at a time,
 * and the main loop keeps none open, so every open handle is the
 * command's), the memory it took through px_malloc(), the inflate
 * window, the shell's pipe and redirection state. Below the stack lies
 * PX_JOB_SLACK bytes of nothing, filled with a pattern; if that is
 * intact the overrun touched nothing else. If it is not, something
 * further down may have been written, and px_job_step() says so.
 */

#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdlib.h>

#include "zeitlos.h"
#include "zfsapp.h"
#include "zfs.h"            /* Z_FS_MAX_OPEN */
#include "../../common/zco.h"
#include "posix.h"

#define QUEUE_MAX   8       /* sessions with a command waiting; more than PX_MAX_CONNS */
#define ALLOCS_MAX  16      /* blocks a command holds at once (diff: 10) */
#define SLACK_FILL  0x5D5D5D5Du

static zco_t        co;
static uint8_t     *stack;          /* above the slack */
static size_t       stack_bytes;
static uint8_t     *slack;
static uint32_t     slice;          /* ticks a resume runs before px_intr() hands back */
static uint32_t     slice_start;
static bool         blocked;        /* handed back to wait for the terminal */

static px_shell_t  *cur;            /* the command in the coroutine */
static px_shell_t  *queue[QUEUE_MAX];
static int          queued;

static size_t       last_used, peak_used;

static void        *allocs[ALLOCS_MAX];

/* ------------------------------------------------------------------ */
/* memory a command holds                                              */

void *px_malloc(size_t n) {
    for (int i = 0; i < ALLOCS_MAX; i++) {
        if (allocs[i]) continue;
        allocs[i] = malloc(n ? n : 1);
        return allocs[i];
    }
    return NULL;            /* reported by the caller as out of memory */
}

void *px_calloc(size_t n, size_t size) {
    void *p = px_malloc(n * size);
    if (p) memset(p, 0, n * size);
    return p;
}

void px_free(void *p) {
    if (!p) return;
    for (int i = 0; i < ALLOCS_MAX; i++)
        if (allocs[i] == p) { allocs[i] = NULL; free(p); return; }
}

int px_alloc_count(void) {
    int n = 0;
    for (int i = 0; i < ALLOCS_MAX; i++) if (allocs[i]) n++;
    return n;
}

static void free_all(void) {
    for (int i = 0; i < ALLOCS_MAX; i++) { free(allocs[i]); allocs[i] = NULL; }
}

/* ------------------------------------------------------------------ */

void px_job_init(void *mem, size_t bytes, uint32_t slice_ticks) {
    slack = mem;
    stack = slack + PX_JOB_SLACK;
    stack_bytes = bytes - PX_JOB_SLACK;
    slice = slice_ticks;
    for (size_t i = 0; i + 4 <= PX_JOB_SLACK; i += 4)
        memcpy(slack + i, &(uint32_t){ SLACK_FILL }, 4);
}

static bool slack_ok(void) {
    for (size_t i = 0; i + 4 <= PX_JOB_SLACK; i += 4) {
        uint32_t w;
        memcpy(&w, slack + i, 4);
        if (w != SLACK_FILL) return false;
    }
    return true;
}

static void body(void *arg) {
    px_shell_t *sh = arg;
    if (sh->job_resume) px_resume(sh, sh->job_status);
    else px_exec_line(sh, sh->job_line);
}

static bool enqueue(px_shell_t *sh) {
    if (sh == cur || queued >= QUEUE_MAX) return false;
    for (int i = 0; i < queued; i++) if (queue[i] == sh) return false;
    queue[queued++] = sh;
    return true;
}

bool px_job_submit(px_shell_t *sh, const char *line) {
    int n = 0;
    while (line[n] && n < PX_LINE_MAX - 1) { sh->job_line[n] = line[n]; n++; }
    sh->job_line[n] = 0;
    sh->job_resume = false;
    return enqueue(sh);
}

bool px_job_submit_resume(px_shell_t *sh, int status) {
    sh->job_resume = true;
    sh->job_status = status;
    return enqueue(sh);
}

int px_job_state(const px_shell_t *sh) {
    if (sh == cur) return PX_JOB_RUNNING;
    for (int i = 0; i < queued; i++) if (queue[i] == sh) return PX_JOB_QUEUED;
    return PX_JOB_NONE;
}

int px_job_ahead(const px_shell_t *sh) {
    int n = cur ? 1 : 0;
    for (int i = 0; i < queued && queue[i] != sh; i++) n++;
    return n;
}

bool px_job_cancel(px_shell_t *sh) {
    for (int i = 0; i < queued; i++) {
        if (queue[i] != sh) continue;
        for (; i + 1 < queued; i++) queue[i] = queue[i + 1];
        queued--;
        return true;
    }
    return false;
}

px_shell_t *px_job_current(void) { return cur; }
bool px_job_pending(void) { return cur || queued; }

/* Inside the command: hand back if the slice is used. Outside (the host
 * transcript test runs commands directly) nothing happens. */
void px_job_tick(void) {
    if (!cur || zco_current() != &co) return;
    if (z_uptime_ticks() - slice_start >= slice) zco_yield();
}

bool px_job_wait(void) {
    if (!cur || zco_current() != &co) return false;
    blocked = true;
    zco_yield();
    return true;
}

/* Everything a command that will never be resumed was holding. */
static void let_go(void) {
    px_shell_abandon();
    for (int h = 0; h < Z_FS_MAX_OPEN; h++) fs_close_handle(h);
    free_all();
}

int px_job_step(px_shell_t **ended) {

    *ended = NULL;
    if (!cur) {
        if (!queued) return PX_STEP_IDLE;
        cur = queue[0];
        for (int i = 0; i + 1 < queued; i++) queue[i] = queue[i + 1];
        queued--;
        px_current_conn = cur->ctx;
        zco_init(&co, body, cur, stack, stack_bytes);
    }

    blocked = false;
    slice_start = z_uptime_ticks();
    bool more = zco_resume(&co);

    /* The slack is a second, wider guard: zco's 128 bytes can be
     * stepped over by a frame with a big array it writes only part of
     * (zco.h), 2 KB much less easily -- and a host test did exactly
     * that, through exec_simple()'s argv. Written means overrun,
     * whatever zco saw: the command is finished here and now. A
     * coroutine stopped this way is never resumed, which is what
     * zco_abandon() records; everything it held is let go below. */
    bool below = !slack_ok();
    if (more && !below) return blocked ? PX_STEP_BLOCKED : PX_STEP_RAN;
    if (more) zco_abandon(&co);

    last_used = zco_stack_used(&co);
    if (last_used > peak_used) peak_used = last_used;
    *ended = cur;
    cur = NULL;
    if (!co.overflow && !below) return PX_STEP_DONE;

    let_go();
    (*ended)->status = 1;
    return below ? PX_STEP_OVERRUN : PX_STEP_OVERFLOW;
}

size_t px_job_stack_size(void) { return stack_bytes; }
size_t px_job_stack_last(void) { return last_used; }
size_t px_job_stack_peak(void) { return peak_used; }
