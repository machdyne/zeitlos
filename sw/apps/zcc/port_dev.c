/*
 * zcc -- the Zeitlos port.
 *
 * See zcc_port.h for the seam and port_host.c for the other side.
 *
 * -- zcc is an ORDINARY Zeitlos app --
 *
 * It links sw/common's runtime and newlib through
 * sw/common/riscv-app.ld, with newlib's crt0 as the entry point,
 * exactly like `repl`, `term` and everything else in sw/apps.
 *
 * It was not always. The first version was freestanding, with its own
 * linker script and a hand-written `_start`, linking libz instead of
 * the C library -- and that was not a design decision, it was an
 * artefact of the machine it was developed on, whose RISC-V toolchain
 * had no newlib and so could not link a normal app at all. Every one
 * of the assumptions that entry stub made was untested, and two of
 * them crashed a real system: `main()` was called with a0/a1 holding
 * whatever the kernel left there, and the compiler read them as
 * argc/argv.
 *
 * There is nothing zcc needs that an ordinary app does not have. The
 * lesson is narrow and worth keeping: a build shape adopted to work
 * around the development environment is a build shape nobody is
 * testing.
 *
 * libz is still involved, but only as DATA -- libz.bin is the runtime
 * zcc embeds into the programs it produces, read from the card at run
 * time. It is not linked into zcc. See docs/zcc.md, "Where libz comes
 * from".
 *
 * -- Few trips into FatFs --
 *
 * The syscall dispatcher refuses to preempt a process that is inside
 * FatFs (docs/filesystem.md), so what matters is not how much is read
 * at once but how OFTEN the region is entered. Reads use a 32KB chunk
 * and writes go out in a single call.
 */

#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

#include "zeitlos.h"
#include "zfsapp.h"
#include "zwin.h"           /* z_launch_arg_take() */
#include "zport.h"
#include "zobj.h"
#include "../posix/posix.h"   /* PX_STDOUT_TAG -- the one thing shared */
#include "zcc_port.h"

/*
 * Reads a whole file, and distinguishes "not there" from "empty".
 *
 * This is the one place where the device and the host genuinely
 * disagree, and it cost a real bug. fs_size() returns 0 BOTH for a
 * missing file and for an empty one -- sw/os/fsapi.h says so outright
 * and calls it deliberate, because te's README wants opening a
 * nonexistent file to be how you create one.
 *
 * A first version treated that 0 as success and returned an empty
 * buffer. The preprocessor probes for a header by trying to read each
 * candidate in turn, so the FIRST candidate that did not exist
 * "succeeded" with no content -- and `#include "libz.h"` expanded to
 * nothing, silently, with no error anywhere. The program then failed
 * with `'printf' is not declared` on a line that had nothing wrong
 * with it.
 *
 * fs_open_read() is the unambiguous test: -1 for a missing file, a
 * handle for an empty one. So the file is opened first, and only then
 * is its size trusted.
 */
#define ZIO_CHUNK (32 * 1024)

char *zio_read_file(const char *path, int *len) {

    int h = fs_open_read(path);
    if (h < 0) return NULL;

    int n = fs_size((char *)path);
    if (n < 0) { fs_close_handle(h); return NULL; }

    /* Two spare bytes past the end, not one: the lexer looks one
     * character ahead in several places (`/` then `*`, `.` then a
     * digit) and doing that at the last byte must not read past the
     * allocation. */
    char *buf = malloc((size_t)n + 2);
    if (!buf) { fs_close_handle(h); return NULL; }

    int got = 0;
    while (got < n) {
        int want = n - got;
        if (want > ZIO_CHUNK) want = ZIO_CHUNK;
        int r = fs_read_chunk(h, buf + got, want);
        if (r <= 0) break;
        got += r;
    }
    fs_close_handle(h);

    if (got != n) { free(buf); return NULL; }

    buf[n] = 0;
    buf[n + 1] = 0;
    if (len) *len = n;
    return buf;
}

int zio_write_file(const char *path, const void *buf, int len) {
    int n = fs_write_file((char *)path, (char *)buf, len);
    return (n == len) ? 0 : -1;
}

/*
 * -- Where zcc's output goes --
 *
 * Two possible destinations, decided once at startup.
 *
 * If a `posix` shell is running, zcc opens a second port connection to
 * it marked PX_STDOUT_TAG and writes there, so `zcc hello.c` typed in
 * a `term` window prints into that window. Without this, output goes
 * to the UART -- which is the kernel console, not the terminal the
 * command was typed in, and looks like the compiler silently did
 * nothing.
 *
 * Otherwise (started from the kernel shell, or no posix running) it
 * goes to the UART, which is exactly right there.
 *
 * The provider is found by NAME, not by an argument: z_pid_lookup()
 * on "posix0", the same way `term` finds `repl0`. That means no
 * protocol change, nothing new for `posix` to pass, and zcc works
 * unchanged when started by hand. The cost is that a SECOND posix
 * instance would register as "posix1" and not be found -- acceptable
 * while there is one shell, and the fix when there is not is for
 * posix to pass its own name in the launch argument.
 */

void uart_putc(char c);         /* sw/common/zeitlos.c */

static z_port_t out_port;
static bool out_connected;

/* Batched, and this is not an optimisation.
 *
 * z_port_send() refuses once Z_PORT_MAX_PENDING_SENDS (8) messages are
 * unacked (zport.h). A compiler emitting a diagnostic per line, or
 * -v output, would blow through eight sends long before returning
 * anywhere that could read the acks -- and the excess is DROPPED, so
 * the tail of the output would silently vanish. repl found this the
 * same way, losing the end of a pasted line. */
#define OUT_BUF 512

static char out_buf[OUT_BUF];
static int out_len;

/* Reads pending messages so that acks are consumed. Without this the
 * pending table fills after eight sends and stays full forever,
 * because nothing else in this program ever reads its mailbox. */
static void out_pump(void) {
    z_msg_t msg;
    while (z_msg_read(&msg) == Z_OK) {
        if (msg.subject == Z_PORT_DATA_ACK)
            z_port_handle_ack(&out_port, &msg);
        else if (msg.subject == Z_PORT_CLOSE)
            out_connected = false;
    }
}

static void out_flush(void) {

    if (!out_len) return;

    if (out_connected) {
        /* A bounded retry: pump acks and try again, then give up and
         * drop the chunk rather than spin forever if the shell has
         * stopped reading. A compiler that hangs because its output
         * could not be delivered is worse than one whose output is
         * truncated. */
        for (int try = 0; try < 32; try++) {
            if (z_port_send(&out_port, out_buf, (uint32_t)out_len) == Z_OK)
                break;
            out_pump();
            z_proc_wait(1);
        }
    } else {
        for (int i = 0; i < out_len; i++) {
            if (out_buf[i] == '\n') uart_putc('\r');
            uart_putc(out_buf[i]);
        }
    }

    out_len = 0;
}

void zio_out_open(void) {

    uint32_t pid = 0;

    if (!z_pid_lookup("posix0", &pid) || !pid) return;

    if (z_port_connect_arg(&out_port, pid, z_obj_str(PX_STDOUT_TAG)) == Z_OK)
        out_connected = true;
}

void zio_out_close(void) {
    out_flush();
    if (out_connected) {
        z_port_close(&out_port);
        out_connected = false;
    }
}

/*
 * Deliberately not printf() or fputs().
 *
 * docs/app_runtime.md's standing warning: one printf links newlib's
 * formatter and costs on the order of 100KB, and anything touching a
 * FILE another 40KB. zcc formats its own messages (util.c) precisely
 * so that the only thing left here is moving bytes.
 *
 * LF becomes CRLF on the way out, because a diagnostic is written with
 * plain \n and both destinations want both -- the same expansion
 * _write() does when no stdout hook is installed.
 */
void zio_out(const char *s) {
    for (; *s; s++) {
        if (out_len >= OUT_BUF - 2) out_flush();
        if (*s == '\n') out_buf[out_len++] = '\r';
        out_buf[out_len++] = *s;
        if (*s == '\n') out_flush();
    }
}

void zio_exit(int status) {
    /* Every exit path goes through here, including zcc_error()'s, so
     * this is the one place that has to remember to flush. A compiler
     * whose last message is lost because it exited is a compiler
     * nobody can debug. */
    zio_out_close();
    exit(status);
}

/*
 * Where a command line comes from on a machine that has no argv.
 *
 * A Zeitlos process is started by name and nothing else: `run zcc` at
 * the shell, or Z_SYS_PROC_RUN from `posix`, and k_proc_run() carries
 * a name and no arguments. Two places are checked, in order:
 *
 *   1. the launch argument (z_launch_arg_take, sw/common/zwin.h),
 *      which is what `posix`'s `run` builtin sets;
 *   2. the file /zcc.args, one command line.
 *
 * The second is a stopgap for driving zcc from the kernel shell, where
 * `run` passes nothing.
 */
int zio_get_args(char *buf, int cap) {

    if (z_launch_arg_take(buf, cap) && buf[0]) return 1;

    {
        char *s = fs_mallocfile("/zcc.args");
        int i = 0;
        if (!s) return 0;
        while (s[i] && s[i] != '\n' && i < cap - 1) { buf[i] = s[i]; i++; }
        buf[i] = 0;
        free(s);
        return i > 0;
    }
}
