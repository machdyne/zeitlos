/*
 * sechs -- Sechs modules on a PMOD, from the posix shell (docs/sechs.md):
 *
 *   sechs [-p PORT | -b BUS] scan | info ADDR | reg ADDR N [VALUE] |
 *                   halt|run|reset|program ADDR | addr ADDR NEW |
 *                   console ADDR | send ADDR FILE
 *
 * The bus is zi2cx's (zi2cx.h): -b pmodN, or a bench bus with virtual
 * modules on it (docs/bench.md, docs/ls99.md); -p N is -b pmodN. On a
 * PMOD port the module's pins 1 (A, SCL) and 2 (B, SDA) are its pins 0
 * and 1. The commands are sechs_cmd.c; this file is the bus and the
 * terminal.
 *
 * Output goes to the shell's terminal through posix's "stdout" relay,
 * as zcc's does (sw/apps/zcc/port_dev.c), or to the serial console when
 * there is no posix. `console` needs keys too, so it takes the terminal
 * over the way vi does (sw/apps/vi/vi_zeitlos.c): posix hands term to
 * this process, which edits a line locally and types it into the
 * module.
 */

#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdlib.h>

#include "../../common/zeitlos.h"
#include "../../common/zport.h"
#include "../../common/zwin.h"          /* z_launch_arg_take() */
#include "../../common/zargs.h"
#include "../../common/zgpio.h"
#include "../../common/zi2c.h"
#include "../../common/zi2cx.h"
#include "../../common/zfsapp.h"
#include "../posix/posix.h"             /* PX_STDOUT_TAG, PX_TTY_TAG */

#include "sechs_cmd.h"

void uart_putc(char c);                 /* sw/common/zeitlos.c */

static z_i2cx_t bus;
static z_port_t out_port, tty, ask;
static bool out_on;
static bool term_gone;
static char name[24] = "sechs";

/* ---- messages: acks, keys, term arriving and leaving ---- */

#define KEY_BUF 64
static uint8_t keys[KEY_BUF];
static int key_head, key_tail;

static void take(z_msg_t *m);

static void pump(void) {
    z_msg_t msg;
    while (z_msg_read(&msg) == Z_OK) take(&msg);
}

/* one message: also zi2cx's `other`, while it waits for bench */
static void take(z_msg_t *mp) {
    z_msg_t msg = *mp;
    {
        if (msg.subject == Z_PORT_CONNECT) {
            if (tty.connected) z_port_refuse(&msg, "sechs: already in use");
            else z_port_accept(&tty, &msg, 1);
        } else if (msg.subject == Z_PORT_DATA_ACK) {
            if (tty.connected && msg.from == tty.peer_pid) z_port_handle_ack(&tty, &msg);
            else if (out_on) z_port_handle_ack(&out_port, &msg);
        } else if (msg.subject == Z_PORT_DATA && tty.connected && msg.from == tty.peer_pid) {
            uint32_t len = z_blob_len(&msg.obj);
            const uint8_t *d = (const uint8_t *)z_blob_data(&msg.obj);
            z_port_send_ack(&msg);
            for (uint32_t i = 0; i < len; i++) {
                int next = (key_tail + 1) % KEY_BUF;
                if (next == key_head) break;
                keys[key_tail] = d[i];
                key_tail = next;
            }
        } else if (msg.subject == Z_PORT_CLOSE) {
            if (tty.connected && msg.from == tty.peer_pid) {
                z_port_close(&tty);
                term_gone = true;
            } else {
                out_on = false;
            }
        }
    }
}

/* ---- output: batched (a port refuses past eight unacked sends) ---- */

#define OUT_BUF 512
static char out_buf[OUT_BUF];
static int out_len;

static void flush(void) {
    if (!out_len) return;
    z_port_t *p = tty.connected ? &tty : out_on ? &out_port : 0;
    if (p) {
        for (int i = 0; i < 64; i++) {
            if (z_port_send(p, out_buf, (uint32_t)out_len) == Z_OK) break;
            pump();
            z_proc_wait(1);
        }
    } else {
        for (int i = 0; i < out_len; i++) uart_putc(out_buf[i]);
    }
    out_len = 0;
}

static void print(const char *s) {
    for (; *s; s++) {
        if (out_len >= OUT_BUF - 2) flush();
        if (*s == '\n') out_buf[out_len++] = '\r';
        out_buf[out_len++] = *s;
        if (*s == '\n') flush();
    }
}

/* ---- the bus ---- */

static int b_write(void *c, uint8_t addr, const uint8_t *d, int n) {
    (void)c;
    return z_i2cx_write(&bus, addr, d, (uint32_t)n) == Z_I2C_OK ? 0 : -1;
}

static int b_read(void *c, uint8_t addr, uint8_t reg, uint8_t *d, int n) {
    (void)c;
    return z_i2cx_write_read(&bus, addr, &reg, 1, d, (uint32_t)n) == Z_I2C_OK ? 0 : -1;
}

static void b_idle(void *c) {
    (void)c;
    flush();
    pump();
    z_proc_wait(1);
}

static long b_ms(void *c) {
    (void)c;
    return (long)((uint64_t)z_uptime_ticks() * 1000 / Z_TICK_HZ);
}

static void b_out(void *c, char ch) {
    char s[2] = { ch, 0 };
    (void)c;
    print(s);
    if (ch != '\n' && out_len) flush();     /* a prompt shows at once */
}

static int key(void) {
    flush();
    pump();
    if (key_head != key_tail) {
        int k = keys[key_head];
        key_head = (key_head + 1) % KEY_BUF;
        return k;
    }
    return term_gone ? -2 : -1;
}

static char *read_file(const char *path) {
    return fs_mallocfile((char *)path);
}

static void free_file(char *d) {
    free(d);
}

static sx_io_t io = {
    { b_write, b_read, b_idle, b_ms, b_out, 0 }, print, key, read_file, free_file
};

/* the terminal, from posix, for console (as vi does); false if there is
 * no posix, which leaves output on the serial console and no keys */
static bool take_terminal(void) {
    uint32_t pid = 0;
    char arg[32];
    if (!z_pid_lookup("posix0", &pid) || !pid) return false;
    if (!z_pid_register("sechs", name, sizeof(name))) return false;
    strcpy(arg, PX_TTY_TAG);
    strcat(arg, name);
    if (z_port_connect_arg(&ask, pid, z_obj_str(arg)) != Z_OK) return false;
    z_port_close(&ask);
    for (int i = 0; i < 200 && !tty.connected; i++) {
        pump();
        z_proc_wait(Z_TICK_HZ / 100);
    }
    return tty.connected;
}

int main(void) {
    static char line[256], buf[256];
    char *argv[16];
    uint8_t flags[16];
    int argc = 0, a = 0, r;
    char busname[24] = "pmod0";

    if (z_launch_arg_take(line, sizeof(line)) && line[0])
        argc = z_args_split(line, buf, sizeof(buf), argv, flags, 16);

    if (argc >= 2 && !strcmp(argv[0], "-p")) {
        int n = atoi(argv[1]), k = 4;             /* "pmodN", without printf */
        strcpy(busname, "pmod");
        if (n >= 10) busname[k++] = (char)('0' + n / 10 % 10);
        busname[k++] = (char)('0' + (n < 0 ? 0 : n) % 10);
        busname[k] = 0;
        a = 2;
    } else if (argc >= 2 && !strcmp(argv[0], "-b")) {
        strncpy(busname, argv[1], sizeof(busname) - 1);
        a = 2;
    }

    if (argc - a >= 1 && !strcmp(argv[a], "console")) {
        if (!take_terminal()) {
            print("sechs: console needs the posix shell's terminal\n");
            return 1;
        }
    } else {
        uint32_t pid = 0;
        if (z_pid_lookup("posix0", &pid) && pid &&
            z_port_connect_arg(&out_port, pid, z_obj_str(PX_STDOUT_TAG)) == Z_OK)
            out_on = true;
    }

    int o = z_i2cx_open(&bus, busname);
    if (o == Z_I2CX_NO_BENCH || o == Z_I2CX_NO_BUS) {
        print("sechs: ");
        print(busname);
        print(o == Z_I2CX_NO_BUS ? ": no such bus\n" : ": no bench running\n");
        r = 2;
    } else {
        bus.other = take;                   /* keys and acks, while zi2cx waits */
        r = sx_run(&io, argc - a, argv + a);
        z_i2cx_close(&bus);
    }

    flush();
    if (out_on) z_port_close(&out_port);
    /* the terminal is left to posix, which takes it back when this
     * process has gone (as vi does) */
    return r;
}
