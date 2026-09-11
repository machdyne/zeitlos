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
#include "../../common/zconnect.h"   /* z_conn_handoff() -- the terminal handoff */
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

    /* The terminal has been handed to a child (5.2). While this is set
     * the term window on the other end is talking to somebody else, so
     * nothing may be written here and no prompt may be printed -- the
     * bytes would arrive at a screen this shell no longer owns.
     *
     * The port is CLOSED while this is set. term disconnects from us
     * before connecting to the child -- that is what handing over
     * means -- so `port.connected` is false and `port.peer_pid` is
     * gone. Everything needed to get the terminal back has to be kept
     * here instead, and the slot has to survive a close that would
     * otherwise free it. */
    bool        tty_away;
    uint32_t    tty_term_pid;   /* who to call back; port.peer_pid is gone */

    /* The child exited while the terminal was away, so its status is
     * waiting for term to come back before the line can continue. */
    bool        resume_pending;
    int         resume_status;
} px_conn_t;

static px_conn_t conns[PX_MAX_CONNS];

/* One history across every connection, like repl's: recalling in one
 * terminal a command typed in another is the behaviour people expect,
 * not a compromise. */
static z_line_hist_t line_history;

/* The name this shell registered, so it can tell a term window to come
 * back to it. Module scope because px_tty_return() needs it and main()
 * is not the only place it is read. */
static char instance_name[24] = "posix";

/*
 * Tells the term window on the far end of `conn` to go and connect to
 * `provider` instead -- Z_TERM_SET_PORT, through zconnect's helper.
 *
 * `provider` NULL means "come back to me", which is how a session is
 * recovered when the child exits.
 *
 * Fire and forget: there is no reply. If the provider name does not
 * resolve, term reports that itself and F12 disconnects it to the panel,
 * which is the escape hatch that already exists for a telnet session
 * to a host that stops answering.
 */
static void tty_tell(uint32_t term_pid, const char *provider) {

    z_conn_target_t t;

    if (!term_pid || !provider) return;

    memset(&t, 0, sizeof(t));
    t.kind = Z_CONN_PORT;
    snprintf(t.provider, sizeof(t.provider), "%s", provider);
    t.arg = z_obj_none();
    snprintf(t.detail, sizeof(t.detail), "%s", provider);

    z_conn_handoff(term_pid, &t);

}

void px_tty_handoff(void *conn, const char *provider) {

    px_conn_t *c = (px_conn_t *)conn;

    if (!c || !c->port.connected || !c->port.peer_pid) return;

    /* Remembered BEFORE the handoff, because term closes its
     * connection to us as soon as it acts on the message and
     * port.peer_pid goes with it. */
    c->tty_term_pid = c->port.peer_pid;
    c->tty_away = true;

    tty_tell(c->tty_term_pid, provider);

}

/*
 * Asks term to come back.
 *
 * This does NOT clear tty_away -- term has to actually arrive first,
 * and it arrives as a fresh Z_PORT_CONNECT. handle_connect() matches
 * it back to this slot by pid and clears the flag there.
 */
void px_tty_return(void *conn) {

    px_conn_t *c = (px_conn_t *)conn;

    if (!c || !c->tty_away || !c->tty_term_pid) {
        printf("posix: cannot return the terminal (away=%d pid=%lu)\n",
               c ? (int)c->tty_away : -1,
               c ? (unsigned long)c->tty_term_pid : 0);
        return;
    }

    tty_tell(c->tty_term_pid, instance_name);

}

static const char *BANNER =
    "posix -- a Unix-shaped shell for Zeitlos\r\n"
    "type 'help' for the builtins; anything else runs as a program\r\n"
    "F12 disconnects\r\n";

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
/* 4KB, not 1KB.
 *
 * A child's output arrives as a burst -- `zcc` with no arguments
 * prints its whole usage text in one go -- and this buffer is what
 * absorbs it while the far end acks. 1KB did not, and the excess was
 * dropped outright. */
#define OUT_BATCH 4096

static char out_batch[OUT_BATCH];
static uint32_t out_len;
static px_conn_t *out_conn;
static uint32_t out_dropped;

/*
 * Tries to send the batch. Returns false and KEEPS the bytes if the
 * far end is not ready.
 *
 * The old version dropped on the first refusal, and that is what
 * `posix: z_port_send failed (44 bytes)` was: z_port_send() refuses
 * past Z_PORT_MAX_PENDING_SENDS (8) unacked messages, and this loop
 * only drains its mailbox every Z_TICK_HZ/10 -- so a child producing
 * fifteen lines in a burst overran it long before any ack was read,
 * and the tail of its output simply vanished.
 *
 * Retrying here instead of pumping acks inline is deliberate. Pumping
 * would mean calling z_msg_read() from inside the send path, which
 * re-enters handle_data(), which calls conn_out(), which calls this --
 * unbounded recursion on a 16KB stack, reached only under exactly the
 * load that triggers it. The main loop retries instead, where there is
 * no call stack to grow.
 */
static bool out_flush(void) {

    if (!out_len) return true;
    if (!out_conn || !out_conn->port.connected) { out_len = 0; return true; }

    if (z_port_send(&out_conn->port, out_batch, out_len) != Z_OK)
        return false;

    out_len = 0;
    return true;

}

static void conn_out(void *ctx, const char *buf, int len) {

    px_conn_t *c = (px_conn_t *)ctx;

    /* Dropped, not queued. The term window is connected to somebody
     * else; these bytes have nowhere to go, and holding them would
     * mean replaying a compiler's output over an editor's screen the
     * moment the editor exits. */
    if (c->tty_away) return;

    if (out_conn != c) { out_flush(); out_conn = c; }

    while (len > 0) {

        uint32_t room = OUT_BATCH - out_len;

        if (!room) {
            /* Full and the far end will not take it. Dropping the NEW
             * bytes rather than the buffered ones keeps the output in
             * order -- a truncated tail is readable, an interleaved
             * middle is not. Counted so it can be reported once rather
             * than per chunk. */
            if (!out_flush()) { out_dropped += (uint32_t)len; return; }
            room = OUT_BATCH - out_len;
        }

        uint32_t take = ((uint32_t)len < room) ? (uint32_t)len : room;
        memcpy(out_batch + out_len, buf, take);
        out_len += take;
        buf += take;
        len -= (int)take;
    }
}

/* ------------------------------------------------------------------ */

/* Forward: handle_connect() needs it to finish a line that was waiting
 * on a child which had the terminal, and it is defined with the rest of
 * the line machinery below. */
static void finish_line(px_conn_t *c);

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

    /* A free slot is one with no port AND no session behind it.
     *
     * `port.connected` alone is not enough: a session whose terminal
     * has been handed to a child (5.2) has closed its port -- term
     * disconnects before connecting to the child -- and is still very
     * much alive, holding the shell state, the pending line, the
     * child it is waiting on and the pid to call the terminal back
     * from. Handing that slot to the next connection destroys all of
     * it, and the terminal has nowhere to return to. */
    int slot;
    for (slot = 0; slot < PX_MAX_CONNS; slot++)
        if (!conns[slot].port.connected && !conns[slot].tty_away) break;

    if (slot == PX_MAX_CONNS) {
        z_port_refuse(msg, "posix: too many connections");
        return;
    }

    /* A child asking for something, rather than a terminal asking for
     * a session. Two forms, both carried in the connect argument:
     * PX_STDOUT_TAG to route its output here, PX_TTY_TAG to ask for
     * the terminal itself. See posix.h for why the child asks. */
    bool sink = false;
    const char *tty_name = NULL;

    if (msg->obj.type == Z_STR && msg->obj.val.str) {
        const char *a = msg->obj.val.str;
        if (!strcmp(a, PX_STDOUT_TAG)) sink = true;
        else if (!strncmp(a, PX_TTY_TAG, strlen(PX_TTY_TAG)))
            tty_name = a + strlen(PX_TTY_TAG);
    }

    /* Term coming BACK from a handoff, rather than a new session.
     *
     * It arrives as an ordinary CONNECT because that is what it is --
     * term was told to connect to us and did. Matched by pid to the
     * slot that gave its terminal away, so the session continues with
     * its shell state, its pending line and the exit status of the
     * child that had it. A fresh slot would mean a banner and a lost
     * command line. */
    for (int i = 0; i < PX_MAX_CONNS; i++) {

        px_conn_t *c = &conns[i];

        if (!c->tty_away || c->tty_term_pid != msg->from) continue;

        printf("posix: term %lu came back to slot %d\n",
               (unsigned long)msg->from, i);

        z_port_accept(&c->port, msg, (uint32_t)(i + 1));
        c->tty_away = false;
        c->tty_term_pid = 0;

        z_line_reset(&c->line);

        if (c->resume_pending) {
            c->resume_pending = false;
            px_current_conn = c;
            px_resume(&c->sh, c->resume_status);
            out_flush();
            if (c->sh.child_pid) { c->waiting = true; return; }
        }

        finish_line(c);
        return;
    }

    if (tty_name && *tty_name) {

        /* ACCEPTED and then closed, not refused.
         *
         * Refusing was the first version and it says the wrong thing:
         * the request succeeded. A child that reads REFUSED has no way
         * to tell "posix is not there" from "posix did what you asked",
         * and the two want opposite responses -- fall back to the
         * console, or start drawing.
         *
         * Accept-then-close gives it CONNECTED followed by CLOSE, which
         * reads as "acknowledged, and this channel is done". The child's
         * real conversation is with the term window, which is about to
         * be pointed at it. */
        px_conn_t *owner = (px_conn_t *)px_current_conn;

        z_port_accept(&conns[slot].port, msg, (uint32_t)(slot + 1));
        conns[slot].is_sink = false;
        conns[slot].owner = NULL;

        if (owner && owner->port.connected && !owner->tty_away)
            px_tty_handoff(owner, tty_name);

        z_port_close(&conns[slot].port);
        return;
    }

    z_port_accept(&conns[slot].port, msg, (uint32_t)(slot + 1));

    conns[slot].is_sink = sink;
    conns[slot].owner = NULL;

    if (sink) {
        /* Bound to whichever session is running a command right now.
         * If none is -- a program started from somewhere else that
         * happens to know our name -- its output goes nowhere rather
         * than to an arbitrary terminal. */
        conns[slot].owner = px_current_conn;
        /* To the console, not to the terminal: this is about whether
         * the terminal routing works, so sending it through the thing
         * being diagnosed would tell us nothing. */
        printf("posix: stdout sink from pid %lu as conn %d, owner %s\n",
               (unsigned long)msg->from, slot + 1,
               px_current_conn ? "set" : "NONE -- output will be dropped");
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

    /* A session whose terminal has been handed away closes as a matter
     * of course -- term disconnects from us before connecting to the
     * child. That is the handoff working, not the session ending, so
     * the slot keeps its shell state and waits for term to come back.
     *
     * Without this the session is torn down at the moment of handoff
     * and there is nothing left to return the terminal TO: the child
     * exits, the watcher finds no connected session, and the terminal
     * is left in local echo. Which is exactly what happened. */
    if (c->tty_away) {

        /*
         * Closed LOCALLY -- z_port_close() is not called, because it
         * SENDS a CLOSE to the peer and the peer has already left.
         *
         * This is the bug that made `vi` drop the terminal. term
         * disconnects from us to go to the child, we call
         * z_port_close(), and the CLOSE that generates arrives at term
         * AFTER it is connected to the child -- carrying our conn_id,
         * which is very likely the same number the child assigned.
         * Both are slot+1 of their own tables, and both are usually 1.
         * term sees a CLOSE with its current connection's tag and
         * drops to local echo.
         *
         * The same conn_id collision bit sw/apps/ttytest and
         * sw/apps/vi from the other side (docs/posix.md, 5.2), where
         * the fix was to filter incoming messages by SENDER. Here the
         * fix is not to send the message at all: there is nothing to
         * tell. term initiated this close and knows about it.
         */
        c->port.connected = false;
        return;
    }
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

    if (!c) { z_port_send_ack(msg); return; }

    const uint8_t *data = (const uint8_t *)z_blob_data(&msg->obj);
    uint32_t len = z_blob_len(&msg->obj);

    /* A child's stdout: relay it verbatim to the session that started
     * the command, and do not let the line editor near it.
     *
     * -- Why this one acks LAST --
     *
     * A terminal is acked first, below, so that typing is never held
     * up by a slow command. A sink is the opposite case: it is a
     * PRODUCER, and the ack is the only backpressure there is.
     * z_port_send() on the child's side refuses past
     * Z_PORT_MAX_PENDING_SENDS (8) unacked messages, so withholding
     * the ack until the bytes are safely buffered makes the child wait
     * instead of overrunning us.
     *
     * The numbers line up deliberately: a child can have at most 8
     * messages in flight, `zcc` batches its output at 512 bytes a
     * message (OUT_BUF in sw/apps/zcc/port_dev.c), so 4KB is the most
     * that can arrive before it must wait -- and OUT_BATCH is 4KB. So
     * nothing is dropped, where acking first meant a burst of fifteen
     * lines lost its tail.
     *
     * A child that sends larger messages than that can still overrun
     * it, which is what out_dropped counts and reports. It is a
     * deliberate ceiling rather than an assumption: the alternative,
     * buffering without limit, turns a misbehaving child into an
     * out-of-memory failure in the shell.
     */
    if (c->is_sink) {

        px_conn_t *dest = (px_conn_t *)c->owner;

        if (!dest)
            printf("posix: %lu bytes from a sink with no owner, dropped\n",
                   (unsigned long)len);
        else if (!dest->port.connected)
            printf("posix: %lu bytes for a session that has gone, dropped\n",
                   (unsigned long)len);

        if (data && len && dest && dest->port.connected)
            conn_out(dest, (const char *)data, (int)len);

        out_flush();

        /* Acked even when the bytes had to be dropped. Withholding
         * here would make the child wait forever on a connection that
         * is never going to drain -- turning a truncated line of
         * output into a hung compiler, which is the worse failure. */
        z_port_send_ack(msg);
        return;
    }

    /* The ack goes out FIRST for a terminal session. z_port_send() on
     * the other side is refusing to send more until this arrives, so
     * acking after a long command means the terminal cannot type
     * during it -- and a command that never returns would wedge the
     * connection rather than merely being slow. */
    z_port_send_ack(msg);

    if (!data) return;

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

        /* Anything that could not be delivered last time. Retried
         * here, not in out_flush(), so that draining the mailbox for
         * acks cannot re-enter the send path. */
        out_flush();

        if (out_dropped) {
            printf("posix: dropped %lu bytes of output (far end not "
                   "reading)\n", (unsigned long)out_dropped);
            out_dropped = 0;
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

            /* NOT `!c->port.connected`: a session whose terminal is
             * away has no port, and it is precisely the one that needs
             * watching -- the child holding its terminal is the child
             * it is waiting for. */
            if (!c->waiting) continue;
            if (!c->port.connected && !c->tty_away) continue;

            z_proc_status(c->sh.child_pid, &state, &status);
            if (state == Z_PROC_STATE_RUNNING) continue;

            /* UNKNOWN as well as EXITED: a child that finished long
             * enough ago to have fallen out of the kernel's exit ring
             * is still finished, and leaving the terminal without a
             * prompt forever would be the worse failure. Its status is
             * 0, which is the same thing this shell assumed about
             * every child before there was a status at all. */
            c->waiting = false;

            /* The child had the terminal. Ask for it back and stop
             * here: term has to actually arrive, as a fresh CONNECT,
             * before anything can be printed. The status waits with
             * the slot and handle_connect() resumes the line. */
            if (c->tty_away) {
                printf("posix: child %lu gone (state %lu); asking term %lu "
                       "to come back to '%s'\n",
                       (unsigned long)c->sh.child_pid, (unsigned long)state,
                       (unsigned long)c->tty_term_pid, instance_name);
                c->resume_pending = true;
                c->resume_status = (int)status;
                px_tty_return(c);
                continue;
            }

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
