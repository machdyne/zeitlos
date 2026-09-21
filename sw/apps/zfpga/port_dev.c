/*
 * zfpga -- the Zeitlos side of the seam. See zfpga_port.h.
 *
 * An ORDINARY Zeitlos app: sw/common's runtime, newlib,
 * sw/common/riscv-app.ld. docs/zcc.md records what a special-purpose
 * build shape cost that project on real hardware, and zfpga does not
 * repeat it.
 *
 * The output relay below is zcc's (sw/apps/zcc/port_dev.c), for the same
 * reasons, which are recorded there at length. In short: when a posix
 * shell is running, output goes to it on a port connection tagged
 * PX_STDOUT_TAG, so `zfpga pack x.cfg` typed in a term window prints in
 * that window; otherwise to the UART.
 */

#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>

#include "zeitlos.h"
#include "zfsapp.h"
#include "zwin.h"               /* z_launch_arg_take() */
#include "zport.h"
#include "zobj.h"
#include "../posix/posix.h"     /* PX_STDOUT_TAG */
#include "zfpga_port.h"

struct zio_file {
    int h;
};

static zio_file_t *wrap(int h) {
    zio_file_t *f;
    if (h < 0) return NULL;
    f = malloc(sizeof(*f));
    if (!f) { fs_close_handle(h); return NULL; }
    f->h = h;
    return f;
}

/* fs_open_read() is -1 for a missing file and a handle for an empty
 * one. That is the distinction fs_size() cannot make, and the one that
 * cost zcc a silent miscompile. */
zio_file_t *zio_open_read(const char *path) {
    return wrap(fs_open_read(path));
}

zio_file_t *zio_open_write(const char *path) {
    return wrap(fs_open_write(path));
}

/* Chunks are capped so that one trip into FatFs stays short: the
 * syscall dispatcher will not preempt a process inside it
 * (docs/filesystem.md), and wm cannot redraw meanwhile. Callers already
 * loop, so a cap here costs nothing. */
#define ZIO_CHUNK (16 * 1024)

int zio_read(zio_file_t *f, void *buf, int n) {
    if (n > ZIO_CHUNK) n = ZIO_CHUNK;
    return fs_read_chunk(f->h, buf, n);
}

int zio_write(zio_file_t *f, const void *buf, int n) {
    const uint8_t *p = buf;
    int done = 0;
    while (done < n) {
        int want = n - done;
        int w;
        if (want > ZIO_CHUNK) want = ZIO_CHUNK;
        w = fs_write_chunk(f->h, p + done, want);
        if (w <= 0) return -1;
        done += w;
    }
    return done;
}

int zio_close(zio_file_t *f) {
    int ok = fs_close_handle(f->h);
    free(f);
    return ok ? 0 : -1;
}

/* -- output ---------------------------------------------------------- */

void uart_putc(char c);         /* sw/common/zeitlos.c */

static z_port_t out_port;
static bool out_connected;

/* Batched: z_port_send() refuses past Z_PORT_MAX_PENDING_SENDS unacked
 * messages and the excess is dropped. See zcc's port_dev.c. */
#define OUT_BUF 512

static char out_buf[OUT_BUF];
static int out_len;

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
    int i, t;
    if (!out_len) return;
    if (out_connected) {
        for (t = 0; t < 32; t++) {
            if (z_port_send(&out_port, out_buf, (uint32_t)out_len) == Z_OK)
                break;
            out_pump();
            z_proc_wait(1);
        }
    } else {
        for (i = 0; i < out_len; i++) {
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

/*
 * Close only once every send has been acknowledged.
 *
 * A port message's payload is a blob in the SENDER's heap, which the
 * receiver reads in place (zport.h, z_port_t's pending table), and which
 * is freed on the acknowledgement. Exiting with sends outstanding frees
 * this process's whole memory under messages posix has not read yet.
 * The first on-board run of `zfpga build` did exactly that: it failed at
 * once, wrote its error, exited while posix was still accepting the
 * connection -- and the message was never seen, and the machine froze
 * (docs/zfpga.md sec. 21). zcc's port layer, which this one copied, has
 * the same exit path; it has simply never failed that fast.
 *
 * Bounded, like everything here: ~2 s, then give up and close anyway.
 */
void zio_out_close(void) {
    out_flush();
    if (out_connected) {
        uint32_t start = z_uptime_ticks();
        while (out_port.pending_count > 0 && (z_uptime_ticks() - start) < 732u * 2u) {
            out_pump();
            if (out_port.pending_count > 0) z_proc_wait(1);
        }
        z_port_close(&out_port);
        out_connected = false;
    }
}

void zio_console(const char *s) {
    /* Unconnected, zio_out() is already writing to the UART; a second
     * copy there would only repeat the message. */
    if (!out_connected) return;
    for (; *s; s++) {
        if (*s == '\n') uart_putc('\r');
        uart_putc(*s);
    }
}

uint32_t zio_ms(void) {
    return (uint32_t)((uint64_t)z_uptime_ticks() * 1000u / 732u);
}

void zio_out(const char *s) {
    for (; *s; s++) {
        if (out_len >= OUT_BUF - 2) out_flush();
        if (*s == '\n') out_buf[out_len++] = '\r';
        out_buf[out_len++] = *s;
        if (*s == '\n') out_flush();
    }
}

void zio_exit(int status) {
    zio_out_close();
    exit(status);
}

/* The launch argument, which posix sets; failing that /zfpga.args, for
 * driving it from the kernel shell. Z_WM_ARG_MAX is 96 bytes, which is
 * why board profiles exist (docs/zfpga.md sec. 4). */
int zio_get_args(char *buf, int cap) {
    if (z_launch_arg_take(buf, cap) && buf[0]) return 1;
    {
        /* Read through the same chunked path as everything else, not
         * fs_mallocfile(): one less sw/common dependency, and one less
         * caller of newlib's printf (see docs/zfpga.md sec. 11). */
        zio_file_t *f = zio_open_read("/zfpga.args");
        int n, i = 0;
        if (!f) return 0;
        n = zio_read(f, buf, cap - 1);
        zio_close(f);
        if (n <= 0) return 0;
        while (i < n && buf[i] != '\n' && buf[i] != '\r') i++;
        buf[i] = 0;
        return i > 0;
    }
}

void *zio_block(size_t n) {
    return malloc(n);
}

void zio_free(void *p) {
    free(p);
}

int zio_fat83(void) {
    return 1;
}
