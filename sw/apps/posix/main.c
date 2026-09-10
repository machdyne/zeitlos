/*
 * posix -- the port provider.
 *
 * Everything this file does is plumbing: accept connections, turn
 * incoming bytes into lines, hand each line to px_exec_line() and send
 * whatever comes back. The shell itself (sh.c) has no idea any of this
 * exists, which is what lets it be tested without a machine.
 *
 * Modelled on sw/apps/repl/repl.c, which is the reference
 * implementation of this protocol, and deliberately simpler than it:
 * no Scheme, no editor bridge, no pager. See docs/ports.md.
 */

#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#include "../../common/zeitlos.h"
#include "../../common/zport.h"
#include "../../common/zline.h"
#include "../../common/zsoc.h"      /* Z_TICK_HZ, for the idle wait */
#include "../../common/zobj.h"      /* Z_STR, for the connect argument */
#include "../../common/zproc.h"     /* Z_PROC_STATE_*, for the child watcher */
#include "posix.h"

/* Same ceiling and the same reasoning as repl's: small on purpose, and
 * a refused fifth connection is a clean visible failure rather than a
 * crash. Two terminals into one shell is the realistic case; four is
 * already generous. */
#define PX_MAX_CONNS 4

typedef struct {
    z_port_t    port;
    z_line_t    line;

    /* Set while a command from this connection has a child running.
     * The shell state is kept per connection because two terminals can
     * each be waiting on their own child. */
    px_shell_t  sh;
    bool        waiting;

    /* A SINK is a child process's stdout, not a terminal session.
     * Bytes arriving on it are relayed to `owner` and never fed to the
     * line editor -- a compiler's diagnostics are not shell commands,
     * and treating them as such would be both wrong and funny. */
    bool        is_sink;
    void       *owner;      /* px_conn_t *, of the session that spawned it */
} px_conn_t;

static px_conn_t conns[PX_MAX_CONNS];

/* One history across every connection, like repl's: recalling in one
 * terminal a command typed in another is the behaviour people expect,
 * not a compromise. */
static z_line_hist_t line_history;

static const char *BANNER =
    "posix -- a Unix-shaped shell for Zeitlos\r\n"
    "type 'help' for the builtins; anything else runs as a program\r\n"
    "F12 returns to repl\r\n";

static const char *PROMPT = "$ ";

/*
 * -- Output batching --
 *
 * Bytes are accumulated and sent as few messages as possible rather
 * than one per write.
 *
 * Not an optimisation. z_port_send() refuses once
 * Z_PORT_MAX_PENDING_SENDS (8) messages are unacked (zport.h), and a
 * command like `cat` produces output far faster than this loop returns
 * to read acks -- so an unbatched `cat` of anything longer than eight
 * writes would have its TAIL SILENTLY DROPPED. repl hit exactly this
 * with paste echo, nine characters in and one missing; the note is in
 * repl.c and it is the reason this was written batched from the start.
 */
#define OUT_BATCH 1024

static char out_batch[OUT_BATCH];
static uint32_t out_len;
static px_conn_t *out_conn;

static void out_flush(void) {
    if (!out_len || !out_conn) return;
    if (z_port_send(&out_conn->port, out_batch, out_len) != Z_OK)
        printf("posix: z_port_send failed (%lu bytes)\n",
               (unsigned long)out_len);
    out_len = 0;
}

static void conn_out(void *ctx, const char *buf, int len) {

    px_conn_t *c = (px_conn_t *)ctx;

    if (out_conn != c) { out_flush(); out_conn = c; }

    while (len > 0) {
        uint32_t room = OUT_BATCH - out_len;
        uint32_t take = ((uint32_t)len < room) ? (uint32_t)len : room;
        memcpy(out_batch + out_len, buf, take);
        out_len += take;
        buf += take;
        len -= (int)take;
        if (out_len == OUT_BATCH) out_flush();
    }
}

/* ------------------------------------------------------------------ */

static px_conn_t *find_conn_by_tag(uint32_t tag) {
    if (tag < 1 || tag > PX_MAX_CONNS) return 0;
    px_conn_t *c = &conns[tag - 1];
    return c->port.connected ? c : 0;
}

static void prompt(px_conn_t *c) {
    conn_out(c, PROMPT, (int)strlen(PROMPT));
    out_flush();
}

static void handle_connect(const z_msg_t *msg) {

    int slot;
    for (slot = 0; slot < PX_MAX_CONNS; slot++)
        if (!conns[slot].port.connected) break;

    if (slot == PX_MAX_CONNS) {
        z_port_refuse(msg, "posix: too many connections");
        return;
    }

    /* A child asking to route its stdout here, rather than a terminal
     * asking for a session. See PX_STDOUT_TAG in posix.h. */
    bool sink = false;
    if (msg->obj.type == Z_STR && msg->obj.val.str &&
        !strcmp(msg->obj.val.str, PX_STDOUT_TAG))
        sink = true;

    z_port_accept(&conns[slot].port, msg, (uint32_t)(slot + 1));

    conns[slot].is_sink = sink;
    conns[slot].owner = NULL;

    if (sink) {
        /* Bound to whichever session is running a command right now.
         * If none is -- a program started from somewhere else that
         * happens to know our name -- its output goes nowhere rather
         * than to an arbitrary terminal. */
        conns[slot].owner = px_current_conn;
        return;                 /* no banner, no prompt: not a session */
    }

    z_line_reset(&conns[slot].line);
    z_line_set_history(&conns[slot].line, &line_history);

    conn_out(&conns[slot], BANNER, (int)strlen(BANNER));
    prompt(&conns[slot]);
}

static void handle_close(const z_msg_t *msg) {
    px_conn_t *c = find_conn_by_tag(msg->tag);
    if (!c) return;
    if (out_conn == c) { out_flush(); out_conn = 0; }

    /* A sink closing used to re-prompt the session that ran the child,
     * as a proxy for "the child finished". That proxy is gone: the
     * main loop now watches the actual process (Z_SYS_PROC_STATUS) and
     * prompts from there, so a program that closes its output early --
     * or never opens one at all -- is handled the same way as one that
     * does. Closing a sink now closes a sink and nothing else. */
    if (c->is_sink) {
        c->is_sink = false;
        c->owner = NULL;
        z_port_close(&c->port);
        return;
    }

    z_port_close(&c->port);
}

/* Called when a command finishes, whether it ran a child or not. */
static void finish_line(px_conn_t *c) {

    out_flush();

    if (c->sh.want_exit) {
        conn_out(c, "bye\r\n", 5);
        out_flush();
        z_port_close(&c->port);
        return;
    }

    prompt(c);
}

static void run_line(px_conn_t *c, char *line) {

    px_shell_init(&c->sh, conn_out, c);

    /* So that a child spawned by this command knows where to send its
     * output back to. */
    px_current_conn = c;

    if (*line) z_line_history_add(&line_history, line);

    px_exec_line(&c->sh, line);
    out_flush();

    if (c->sh.child_pid) {
        /* Do NOT prompt, and do not block. The main loop watches for
         * the exit; until then this connection simply has no prompt,
         * which is what a shell running a command looks like. */
        c->waiting = true;
        return;
    }

    finish_line(c);
    return;
}

static void handle_data(const z_msg_t *msg) {

    px_conn_t *c = find_conn_by_tag(msg->tag);

    /* The ack goes out FIRST, before any of the work below.
     * z_port_send() on the other side is refusing to send more until
     * this arrives, so acking after a long command means the terminal
     * cannot type during it -- and a command that never returns would
     * wedge the connection rather than merely being slow. */
    z_port_send_ack(msg);

    if (!c) return;

    const uint8_t *data = (const uint8_t *)z_blob_data(&msg->obj);
    uint32_t len = z_blob_len(&msg->obj);
    if (!data) return;

    /* A child's stdout: relay it verbatim to the session that started
     * the command, and do not let the line editor near it. */
    if (c->is_sink) {
        px_conn_t *dest = (px_conn_t *)c->owner;
        if (dest && dest->port.connected)
            conn_out(dest, (const char *)data, (int)len);
        out_flush();
        return;
    }

    /* Input arriving while a child is running is DROPPED, not queued.
     *
     * Queuing it would mean typing ahead into a shell that is about to
     * print a compiler's output over the top of what was typed, and
     * executing it afterwards without the user having seen the prompt
     * it belongs to. A real terminal would give this to the child;
     * this one cannot, because the child's input is not routed here.
     * Dropping is the honest version of that until 5.2 hands the
     * terminal over properly (docs/posix.md). */
    if (c->waiting) return;

    for (uint32_t i = 0; i < len; i++) {

        /* z_line_feed() owns the editing AND the echo: it hands back
         * the bytes to send so that backspace, history recall and
         * mid-line edits all look right on the far end. Sending them
         * through conn_out() means they are batched with everything
         * else, which is what stops a paste losing its tail -- see
         * out_flush()'s comment. */
        char echo[Z_LINE_ECHO_MAX];
        uint32_t echo_len = 0;

        int r = z_line_feed(&c->line, data[i], echo, &echo_len, sizeof(echo));

        if (echo_len) conn_out(c, echo, (int)echo_len);

        if (r > 0) {
            /* The completed line lives in the line buffer, and
             * z_line_reset() must happen before the next byte -- but
             * px_exec_line() rewrites its argument in place while
             * splitting, so it gets a copy rather than the buffer
             * itself. */
            char linebuf[PX_LINE_MAX];
            int n = 0;
            while (c->line.buf[n] && n < PX_LINE_MAX - 1) {
                linebuf[n] = c->line.buf[n];
                n++;
            }
            linebuf[n] = 0;

            z_line_reset(&c->line);
            run_line(c, linebuf);
        }
    }

    out_flush();
}

int main(void) {

    char instance_name[24] = "posix";

    if (z_pid_register("posix", instance_name, sizeof(instance_name)))
        printf("posix: starting as pid %ld, registered as '%s'.\n",
               (long)z_getpid(), instance_name);
    else
        printf("posix: starting as pid %ld (name registration failed -- "
               "term will not be able to find this instance by name).\n",
               (long)z_getpid());

    px_fd_init();

    for (int i = 0; i < PX_MAX_CONNS; i++)
        conns[i].port.connected = false;

    while (1) {

        z_msg_t msg;
        while (z_msg_read(&msg) == Z_OK) {
            if (msg.subject == Z_PORT_CONNECT) {
                handle_connect(&msg);
            } else if (msg.subject == Z_PORT_DATA) {
                handle_data(&msg);
            } else if (msg.subject == Z_PORT_DATA_ACK) {
                px_conn_t *c = find_conn_by_tag(msg.tag);
                if (c) z_port_handle_ack(&c->port, &msg);
            } else if (msg.subject == Z_PORT_CLOSE) {
                handle_close(&msg);
            }
        }

        /* Any child that has finished since the last pass.
         *
         * Polled rather than pushed: the kernel has no "process
         * exited" message, and adding one is a bigger change than this
         * needed (docs/kernel.md). At Z_TICK_HZ/10 the cost is one
         * syscall per waiting connection per 100ms, and the latency
         * between a compile finishing and the prompt returning is the
         * same 100ms -- which is below what anyone notices and well
         * above what it costs. */
        for (int i = 0; i < PX_MAX_CONNS; i++) {

            px_conn_t *c = &conns[i];
            uint32_t state = 0;
            int32_t status = 0;

            if (!c->waiting || !c->port.connected) continue;

            z_proc_status(c->sh.child_pid, &state, &status);
            if (state == Z_PROC_STATE_RUNNING) continue;

            /* UNKNOWN as well as EXITED: a child that finished long
             * enough ago to have fallen out of the kernel's exit ring
             * is still finished, and leaving the terminal without a
             * prompt forever would be the worse failure. Its status is
             * 0, which is the same thing this shell assumed about
             * every child before there was a status at all. */
            c->waiting = false;
            px_current_conn = c;
            px_resume(&c->sh, (int)status);
            out_flush();

            if (c->sh.child_pid) { c->waiting = true; continue; }

            finish_line(c);
        }

        /* Blocks rather than spins. docs/app_runtime.md records what
         * the difference is worth on this machine: a `view` JPEG
         * decode went from 7.7s to 3.1s once the other processes
         * stopped busy-waiting. A shell is idle almost all the time,
         * so this is the single most important line in the loop. */
        z_proc_wait(Z_TICK_HZ / 10);
    }

    return 0;
}
