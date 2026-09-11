/*
 * hello_term.c -- output to the term window, not the serial console.
 *
 *     zcc -I /libz/include -L /libz -o /apps/hello /hello_term.c
 *     hello
 *
 * -- Why this is not just printf --
 *
 * printf() writes to fd 1, which on Zeitlos is the UART: the serial
 * console, not the window the user typed the command in. That is the
 * right default -- a program started from the kernel shell has no
 * terminal -- and it means a program run from `posix` prints somewhere
 * nobody is looking.
 *
 * `posix` will relay output for a program that ASKS. The program opens
 * a second connection tagged "stdout" and writes there; posix passes
 * it on to whichever session started the command. That is what `zcc`
 * itself does (sw/apps/zcc/port_dev.c), and it is about fifteen lines.
 *
 * A program that does not ask keeps printing to the console, which is
 * what every existing app does and what you want when there is no
 * posix to ask.
 */

#include "libz.h"

/* Must match PX_STDOUT_TAG in sw/apps/posix/posix.h. */
#define STDOUT_TAG "stdout"

static z_port_t out;
static int connected;

static void term_open(void) {

    unsigned pid = 0;
    int rv;

    /*
     * These printf()s go to the SERIAL CONSOLE, always -- they are
     * deliberately not routed to the terminal, because they are about
     * whether the routing works. Keep a console attached the first
     * time you run this; a program that prints nowhere looks identical
     * whether it never started, never connected, or connected and had
     * its output dropped.
     */
    if (!z_pid_lookup("posix0", &pid) || !pid) {
        printf("hello: no posix0 -- using the console\n");
        return;
    }

    printf("hello: posix0 is pid %u\n", pid);

    /* z_port_connect_str(), not z_port_connect_arg().
     *
     * The real one takes a z_obj_t BY VALUE, and zcc cannot pass or
     * return a struct that way (docs/zcc.md) -- it now refuses the
     * call rather than miscompiling it, which is how this was found.
     * z_port_connect_str() is the same call with the string passed as
     * a string. See sw/apps/zcc/libz/glue.c. */
    rv = z_port_connect_str(&out, pid, STDOUT_TAG);

    if (rv != Z_OK) {
        printf("hello: posix0 refused the stdout connection (rv %d)\n", rv);
        return;
    }

    connected = 1;
    printf("hello: connected, conn %u -- output goes to the term window\n",
        out.conn_id);
}

static void term_puts(const char *s) {

    if (!connected) {
        /* Falls back to the console, so the program still works when
         * run without posix. */
        printf("%s", s);
        return;
    }

    /* One send per call is fine for a few lines. A program producing
     * a lot of output should batch: z_port_send() refuses once eight
     * messages are unacked, and the excess is DROPPED -- see
     * docs/ports.md, which is the trap everything here has hit once.
     *
     * The return value is CHECKED. A dropped send is otherwise silent,
     * and silence is indistinguishable from every other way this can
     * fail. */
    if (z_port_send(&out, s, strlen(s)) != Z_OK)
        printf("hello: send refused (%d bytes) -- unacked window full?\n",
            (int)strlen(s));
}

static void term_close(void) {
    if (connected) z_port_close(&out);
}

int main(void) {

    int i;

    term_open();

    term_puts("hello from a program built on the machine\r\n");

    for (i = 1; i <= 5; i++) {
        char line[64];
        snprintf(line, sizeof(line), "  %d squared is %d\r\n", i, i * i);
        term_puts(line);
    }

    /* CRLF, not LF. This goes straight to a VT100 (docs/terminal.md)
     * with nothing in between to expand it -- posix relays the bytes
     * as they arrive. printf() to the console does the expansion in
     * _write(), which is why the fallback path above looks the same
     * but behaves differently. */
    term_puts("\r\nthat is all\r\n");

    term_close();
    printf("hello: done\n");
    return 0;
}
