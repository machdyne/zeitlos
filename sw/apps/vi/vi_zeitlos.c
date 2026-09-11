/*
 * vi -- nextvi, running on Zeitlos.
 *
 * The editor is sw/ext/nextvi, vendored and ISC-licensed, and NONE of
 * its logic is touched by this file. What is here is the four things
 * it needs from an operating system that Zeitlos does not have in the
 * shape POSIX describes:
 *
 *   1. a terminal to draw on      -- a port connection, asked for from
 *                                    `posix` and handed over by `term`
 *   2. stdin and stdout           -- z_stdin_hook / z_stdout_hook
 *   3. raw mode, window size      -- not needed, and one constant
 *   4. fork/exec, signals, poll   -- stubbed
 *
 * -- Why hooks rather than a replacement term.c --
 *
 * nextvi's term.c reaches the outside world through exactly two
 * primitives: `write(1, ...)` behind the term_write macro, and
 * `read(fd, ..., 1)` in term_read(). Both are newlib calls, and
 * sw/common/zeitlos.c already routes newlib's _write() through
 * z_stdout_hook. Adding the input half (z_stdin_hook, added for this)
 * means term.c runs UNMODIFIED.
 *
 * The alternative -- a Zeitlos term.c replacing upstream's -- would
 * have meant re-porting 352 lines every time nextvi is re-vendored,
 * and those lines contain the input-buffer and recording machinery
 * that the rest of the editor depends on. See sw/ext/nextvi/ZEITLOS.md.
 *
 * -- The terminal handoff --
 *
 * Same sequence as sw/apps/ttytest, which was built to prove it:
 * register a name, ask posix0 for the terminal, accept term's
 * connection, run, exit. posix takes the terminal back when this
 * process ends (docs/posix.md, 5.1 and 5.2).
 */

#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdlib.h>

#include "../../common/zeitlos.h"
#include "../../common/zsoc.h"
#include "../../common/zport.h"
#include "../../common/zobj.h"
#include "../../common/zwin.h"     /* z_launch_arg_take() */
#include "../../common/zfsapp.h"   /* fs_mallocfile() -- the /vi.args stopgap */

/* Must match PX_TTY_TAG in sw/apps/posix/posix.h. Duplicated rather
 * than included because that header pulls in the whole shell; four
 * characters, said out loud here. */
#define PX_TTY_TAG  "tty:"

/*
 * Diagnostics that bypass the hooks.
 *
 * printf() goes through z_stdout_hook once that is installed, which
 * means straight into the term window and on top of whatever the
 * editor has drawn -- and, worse, through the very path being
 * diagnosed. These go to the UART directly, so they are visible on the
 * serial console and invisible to the editor's screen.
 */
void uart_putc(char c);         /* sw/common/zeitlos.c */

static void trace(const char *s) {
    for (; *s; s++) {
        if (*s == '\n') uart_putc('\r');
        uart_putc(*s);
    }
}

static void trace_num(const char *label, long v) {
    char b[16];
    int n = 0, neg = v < 0;
    if (neg) v = -v;
    if (!v) b[n++] = '0';
    while (v) { b[n++] = (char)('0' + v % 10); v /= 10; }
    trace(label);
    if (neg) uart_putc('-');
    while (n) uart_putc(b[--n]);
    trace("\n");
}

/* nextvi: nonzero while the editor is reading typed text rather than
 * commands. See the wrappers at the end of sw/ext/nextvi/led.c and
 * vi_stdin() below. */
extern int vi_typing;

static z_port_t conn;           /* term, once it arrives */
static z_port_t ask;            /* the request channel to posix */
static char name[24] = "vi";
static bool term_gone;

/* -- output ------------------------------------------------------- */

/*
 * Batched, and this is not an optimisation.
 *
 * z_port_send() refuses past Z_PORT_MAX_PENDING_SENDS (8) unacked
 * messages. A full-screen redraw is thousands of bytes and nextvi
 * emits it through many small write() calls, so unbatched output would
 * lose most of a screen. `repl` found this with paste echo and `posix`
 * found it again with a compiler's output -- see docs/ports.md.
 */
#define VO_BUF 1024

static char vo_buf[VO_BUF];
static uint32_t vo_len;

static void vo_pump(void);

static void vo_flush(void) {

    if (!vo_len || !conn.connected) { vo_len = 0; return; }

    /* Bounded retry. Giving up loses part of a redraw, which nextvi
     * will repaint; spinning forever would hang the editor on a
     * terminal that has gone away. */
    for (int try = 0; try < 64; try++) {
        if (z_port_send(&conn, vo_buf, vo_len) == Z_OK) break;
        vo_pump();
        z_proc_wait(1);
    }

    vo_len = 0;
}

static void vi_stdout(const char *data, uint32_t len) {

    while (len) {
        uint32_t room = VO_BUF - vo_len;
        uint32_t take = (len < room) ? len : room;
        memcpy(vo_buf + vo_len, data, take);
        vo_len += take;
        data += take;
        len -= take;
        if (vo_len == VO_BUF) vo_flush();
    }
}

/* -- input -------------------------------------------------------- */

/*
 * A one-byte-at-a-time reader, because that is how term_read() asks.
 *
 * Keystrokes arrive as Z_PORT_DATA carrying several bytes (an escape
 * sequence is three), so they are buffered here and handed out singly.
 */
#define VI_BUF 256

static uint8_t vi_buf[VI_BUF];
static uint32_t vi_head, vi_tail;

/*
 * Reads messages without blocking. Handles acks, keystrokes, and the
 * far end going away.
 *
 * Called from BOTH the input path and the output retry, so it must not
 * write to the port -- vo_flush() calls it, and a pump that flushed
 * would recurse. It only queues.
 */
static void vo_pump(void) {

    z_msg_t msg;

    while (z_msg_read(&msg) == Z_OK) {

        if (msg.subject == Z_PORT_CONNECT) {
            /* term, arriving because posix pointed it here. A second
             * connection while one is up is refused: this editor has
             * one screen. */
            if (conn.connected) {
                z_port_refuse(&msg, "vi: already in use");
                continue;
            }
            z_port_accept(&conn, &msg, 1);
            continue;
        }

        /* Filtered by SENDER, not by tag.
         *
         * Two connections exist -- the request channel to posix and
         * the session with term -- and their conn_ids can collide,
         * because posix assigns slot+1 and this side assigns its own.
         * ttytest found that the hard way; see docs/posix.md, 5.2. */
        if (!conn.connected || msg.from != conn.peer_pid) {
            trace_num("vi: ignored subject ", (long)msg.subject);
            trace_num("      from pid ", (long)msg.from);
            continue;
        }

        if (msg.subject == Z_PORT_DATA) {

            uint32_t len = z_blob_len(&msg.obj);
            const uint8_t *data = (const uint8_t *)z_blob_data(&msg.obj);

            z_port_send_ack(&msg);

            for (uint32_t i = 0; i < len; i++) {
                uint32_t next = (vi_tail + 1) % VI_BUF;
                if (next == vi_head) break;     /* full: drop, do not wrap */
                vi_buf[vi_tail] = data[i];
                vi_tail = next;
            }

        } else if (msg.subject == Z_PORT_DATA_ACK) {

            z_port_handle_ack(&conn, &msg);

        } else if (msg.subject == Z_PORT_CLOSE) {

            /* term left -- F12, or the window closed. Reported as end
             * of input, which is how nextvi learns to stop. */
            trace_num("vi: CLOSE from pid ", (long)msg.from);
            z_port_close(&conn);
            term_gone = true;

        }
    }
}

static uint32_t vi_stdin(char *buf, uint32_t len) {

    if (!len) return 0;

    while (vi_head == vi_tail) {

        if (term_gone) {
            trace("vi: stdin EOF (term gone)\n");
            return 0;
        }

        /* Anything the editor has drawn but not yet sent must go out
         * BEFORE waiting for a key, or the screen the user is being
         * asked to respond to is still sitting in a buffer. */
        vo_flush();

        vo_pump();
        if (vi_head != vi_tail) break;

        /* Blocks rather than spins. An editor waits for a keystroke
         * essentially all of the time, and a process that spins is a
         * tax on every other one (docs/app_runtime.md). */
        z_proc_wait(1);
    }

    /*
     * Arrow keys, translated to h/j/k/l -- but only when the editor is
     * reading COMMANDS.
     *
     * `term` sends ESC [ A for Up (sw/apps/term/term.c), and it must
     * keep doing so: `repl`, telnet and ssh sessions all depend on it,
     * and the remote end of those is a real terminal that expects it.
     * So the translation has to happen here, in the one program that
     * cannot use the sequences.
     *
     * nextvi binds no escape sequences -- neither does real vi, where
     * movement is h/j/k/l. Left untranslated, Up reads as ESC (leave
     * insert mode), [ (a motion prefix), then A (append, ENTERING
     * insert mode): the cursor does not move and typing starts
     * inserting.
     *
     * `vi_typing` is nonzero while the editor is reading typed text --
     * insert mode, the `:` line, search prompts. Translating there
     * would type the letter h, j, k or l into the document instead of
     * moving, which is worse than the arrow doing nothing. So arrows
     * are passed through unchanged in those contexts, exactly as
     * upstream would see them.
     *
     * Only translated when the WHOLE sequence is already in the ring.
     * A lone ESC -- the user pressing Escape -- arrives in its own
     * message and is passed through untouched, which is what makes
     * this safe rather than a guess about timing.
     */
    if (!vi_typing && vi_buf[vi_head] == 0x1b) {

        uint32_t second = (vi_head + 1) % VI_BUF;
        uint32_t third  = (vi_head + 2) % VI_BUF;

        bool have3 = (vi_head != vi_tail) && (second != vi_tail) &&
                     (third != vi_tail);

        if (have3 && vi_buf[second] == '[') {

            char mapped = 0;
            switch (vi_buf[third]) {
            case 'A': mapped = 'k'; break;      /* up */
            case 'B': mapped = 'j'; break;      /* down */
            case 'C': mapped = 'l'; break;      /* right */
            case 'D': mapped = 'h'; break;      /* left */
            default:  break;
            }

            if (mapped) {
                vi_head = (vi_head + 3) % VI_BUF;
                buf[0] = mapped;
                return 1;
            }
        }
    }

    uint32_t n = 0;
    while (n < len && vi_head != vi_tail) {

        char c = (char)vi_buf[vi_head];
        vi_head = (vi_head + 1) % VI_BUF;

        /*
         * CR becomes LF -- what a tty driver's ICRNL would have done.
         *
         * `term` sends 0x0d for Enter (sw/common/zkbd.c maps the HID
         * usage to CR, and term.c passes it through). On a POSIX
         * system the terminal driver translates that to 0x0a before
         * any application sees it, because ICRNL is an INPUT flag and
         * is on by default.
         *
         * nextvi's term_init() clears ICANON, ISIG and ECHO -- all
         * c_lflag bits -- and never touches c_iflag. So it does not
         * disable the translation; it RELIES on it. There is no tty
         * driver here to do it, so this is where it has to happen.
         *
         * The symptom without it: Enter inserts a literal CR, which
         * the editor renders as ^M, and no line is ever broken.
         */
        if (c == '\r') c = '\n';

        buf[n++] = c;
    }
    return n;
}

/* -- entry -------------------------------------------------------- */

int nextvi_main(int argc, char **argv);     /* vi.c, renamed by the Makefile */
void uc_bytemode(void);                     /* sw/ext/nextvi/uc.c */
extern int xshape, xorder;

int main(void) {

    uint32_t posix_pid = 0;
    char arg[32];
    static char argline[256];
    static char *argv[16];
    int argc = 1;

    /* How much heap this process actually has, reported once.
     *
     * nextvi's emalloc()/erealloc() print "out of memory" and exit,
     * which is correct and says nothing about how much there was --
     * and the answer is usually that the process is in the wrong
     * stack/heap tier (z_proc_stack_size_for(), sw/os/kernel.h), which
     * is a KERNEL setting rather than anything in this binary. Printing
     * it here turns "out of memory" into a number that can be compared
     * against what the tier should give.
     */
    {
        extern char _end;
        char probe;
        printf("vi: heap about %ld KB\n",
            (long)((&probe - &_end) / 1024));
    }

    /* Byte mode BEFORE anything reads a file. See ZEITLOS.md: with
     * the UTF-8 table left as upstream has it, a multi-byte sequence
     * is one character to the editor and several blank cells to a
     * terminal whose font stops at 0x7f, and the cursor and the screen
     * disagree from there on. */
    uc_bytemode();
    xshape = 0;
    xorder = 0;

    if (!z_pid_register("vi", name, sizeof(name))) {
        printf("vi: cannot register a name; term cannot find me\n");
        return 1;
    }

    /* The command line, from posix's launch argument. */
    argv[0] = "vi";

    /* The launch argument first -- what `posix` sets. Failing that,
     * /vi.args, one command line.
     *
     * The file is a stopgap and the same one sw/apps/zcc carries, for
     * the same reason: `run vi` from the KERNEL shell passes no
     * arguments, and neither does sim/, which has no wm to set a
     * launch argument at all. It is what makes the editor testable
     * without hardware. */
    if (!z_launch_arg_take(argline, sizeof(argline)) || !argline[0]) {
        char *s = fs_mallocfile("/vi.args");
        int i = 0;
        if (s) {
            while (s[i] && s[i] != '\n' && i < (int)sizeof(argline) - 1) {
                argline[i] = s[i];
                i++;
            }
            argline[i] = 0;
            free(s);
        }
    }

    if (argline[0]) {
        char *p = argline;
        while (*p && argc < 15) {
            while (*p == ' ') p++;
            if (!*p) break;
            argv[argc++] = p;
            while (*p && *p != ' ') p++;
            if (*p) *p++ = 0;
        }
    }
    argv[argc] = NULL;

    /*
     * No posix means the serial console, not a failure.
     *
     * Started from the kernel shell there is no terminal to be handed
     * over -- but there IS a terminal: the console this message is
     * printing on. Leaving the hooks uninstalled makes newlib's
     * read()/write() do exactly what they already do, which is talk to
     * the UART.
     *
     * That is worth having for its own sake (an editor on the console
     * is the recovery path when `wm` is not running) and it is what
     * makes the editor testable under sim/, which has no posix, no
     * term and no wm. Every bug found that way is one not found on
     * hardware.
     */
    if (!z_pid_lookup("posix0", &posix_pid) || !posix_pid) {
        printf("vi: no posix0 -- using the serial console\n");
        return nextvi_main(argc, argv);
    }

    snprintf(arg, sizeof(arg), "%s%s", PX_TTY_TAG, name);

    if (z_port_connect_arg(&ask, posix_pid, z_obj_str(arg)) != Z_OK) {
        printf("vi: posix0 would not take the terminal request\n");
        return 1;
    }
    z_port_close(&ask);

    /* Wait for term. Bounded: if the handoff did not happen there is
     * no screen, and sitting here forever would look like a hang with
     * no explanation. */
    for (int i = 0; i < 200 && !conn.connected; i++) {
        vo_pump();
        if (conn.connected) break;
        z_proc_wait(Z_TICK_HZ / 100);
    }

    if (!conn.connected) {
        printf("vi: term never connected -- handoff failed\n");
        return 1;
    }

    /* Only now, so that nothing the editor prints can reach the
     * console instead of the screen. */
    z_stdout_hook = vi_stdout;
    z_stdin_hook = vi_stdin;

    trace("vi: entering editor\n");

    int rv = nextvi_main(argc, argv);

    trace_num("vi: editor returned ", (long)rv);
    trace_num("vi: term_gone=", (long)term_gone);
    trace_num("vi: conn.connected=", (long)conn.connected);

    vo_flush();
    z_stdout_hook = NULL;
    z_stdin_hook = NULL;

    /*
     * The connection is NOT closed here.
     *
     * Closing it makes `term` print "port closed by peer" and drop to
     * local echo, and there is then a window -- up to one posix idle
     * pass, 100ms -- where term is connected to nothing at all. If
     * anything goes wrong with the handback in that window the user is
     * simply stranded.
     *
     * Exiting without closing leaves term pointed at a process that is
     * about to disappear, which is untidy for a few milliseconds and
     * recovers cleanly: posix notices the exit and redirects term to
     * itself. F12 remains the escape either way.
     */
    trace("vi: exiting -- posix should take the terminal back\n");
    return rv;
}
