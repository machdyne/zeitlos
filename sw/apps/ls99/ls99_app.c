/*
 * ls99 -- a virtual Zwölf module, started by bench for each `module` in
 * its netlist (docs/ls99.md). The module itself is ls99.c; this is its
 * board: bench, through the module messages of zbench.h.
 *
 * Messages from bench arrive at any moment -- while a send waits for
 * room, or inside a local-bus transaction (zi2cx waits for its reply) --
 * and are queued then, acted on only at the board's service(): a Sechs
 * transaction replayed in the middle of another would be re-entrant.
 *
 * Time: bench broadcasts its virtual clock. This module's own clock is
 * advanced only by its sleeps, and a sleep waits until bench's clock has
 * passed its end: computing takes no time, waiting takes exactly its
 * time, so the program's schedule is exact at any speed. While no
 * program runs, the module's clock follows bench's, so a program starts
 * at "now".
 *
 * Catching up must be cheap. SLEEP waits in 100ms pieces with a break
 * check after each (basic.c, wait_ms()): at x3600 that is 36,000 pieces
 * a second, and reading messages is a system call (so is the clock). A
 * piece bench's clock has already passed returns at once, sending only
 * changed pins; messages are read on every 256th such piece, and on
 * every 32nd break check -- and on every turn of a sleep that really
 * waits. Reading them on every piece left the module hours behind
 * bench's clock at x3600.
 *
 * Files are the module's own storage: /bench/NAME/.
 */

#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdlib.h>

#include "../../common/zeitlos.h"
#include "../../common/zsoc.h"
#include "../../common/zport.h"
#include "../../common/zwin.h"          /* z_launch_arg_take() */
#include "../../common/zfsapp.h"
#include "../../common/zi2cx.h"
#include "../../common/zbench.h"
#include "../../ext/basic/basic.h"
#include "../../ext/basic/sechs/sechs.h"
#include "ls99.h"

static z_port_t bench;
static char name[16], local_bus[ZB_NAME], program[64], dir[40];
static uint8_t profile, addr;
static bool hello, quit;
static uint32_t bench_time, my_clock;
static uint8_t levels[ZB_PINS] = { 2, 2, 2, 2, 2, 2, 2, 2 };
static uint8_t drives[ZB_PINS], drives_sent[ZB_PINS] = { 9, 9, 9, 9, 9, 9, 9, 9 };
static z_i2cx_t local;
static bool local_open;

/* ---- messages from bench: queued, then acted on ---- */

#define INBOX 16
#define MSG_MAX (ZB_MAX + 8)
static uint8_t inbox[INBOX][MSG_MAX];
static uint8_t inbox_len[INBOX];
static int in_head, in_tail;

static void take(z_msg_t *m) {
    if (!bench.connected || m->from != bench.peer_pid) return;
    if (m->subject == Z_PORT_DATA) {
        int n = (int)z_blob_len(&m->obj), next = (in_tail + 1) % INBOX;
        if (n > MSG_MAX) n = MSG_MAX;
        if (next != in_head) {
            memcpy(inbox[in_tail], z_blob_data(&m->obj), (size_t)n);
            inbox_len[in_tail] = (uint8_t)n;
            in_tail = next;
        }
        z_port_send_ack(m);
    } else if (m->subject == Z_PORT_DATA_ACK) {
        z_port_handle_ack(&bench, m);
    } else if (m->subject == Z_PORT_CLOSE) {
        z_port_close(&bench);
        quit = true;
    }
}

static void pump(void) {
    z_msg_t m;
    while (z_msg_read(&m) == Z_OK) take(&m);
}

/* a message to bench; a port takes eight unacked, so wait for acks */
static void send(const void *d, int n) {
    for (int i = 0; i < 200 && bench.connected; i++) {
        if (z_port_send(&bench, d, (uint32_t)n) == Z_OK) return;
        pump();
        z_proc_wait(1);
    }
}

static void act(const uint8_t *d, int n) {
    switch (d[0]) {
    case ZB_HELLO: {
        const char *s = (const char *)d + 3;
        profile = d[1];
        addr = d[2];
        strncpy(local_bus, s, sizeof(local_bus) - 1);
        s += strlen(s) + 1;
        if (s < (const char *)d + n) strncpy(program, s, sizeof(program) - 1);
        hello = true;
        break;
    }
    case ZB_TIME:
        if (n >= 5) bench_time = (uint32_t)d[1] | (uint32_t)d[2] << 8 |
                                 (uint32_t)d[3] << 16 | (uint32_t)d[4] << 24;
        break;
    case ZB_LEVELS:
        if (n >= 1 + ZB_PINS) memcpy(levels, d + 1, ZB_PINS);
        break;
    case ZB_SLAVE: {
        uint8_t r[2 + 3 + ZB_MAX];
        int wn = d[2], rn = n > 3 + wn ? d[3 + wn] : 0;
        if (rn > ZB_MAX) rn = ZB_MAX;
        ls99_slave(d + 3, wn, r + 3, rn);
        r[0] = ZB_DONE;
        r[1] = d[1];
        r[2] = ZB_ACK;
        send(r, 3 + rn);
        break;
    }
    case ZB_QUIT:
        quit = true;
        sechs.cmd = CMD_HALT;                   /* a running program stops */
        break;
    }
}

static void flush_drives(void) {
    if (!memcmp(drives, drives_sent, ZB_PINS)) return;
    uint8_t m[1 + ZB_PINS];
    m[0] = ZB_DRIVES;
    memcpy(m + 1, drives, ZB_PINS);
    memcpy(drives_sent, drives, ZB_PINS);
    send(m, sizeof(m));
}

static char con[64];
static int con_len;

static void flush_console(void) {
    if (!con_len) return;
    uint8_t m[1 + sizeof(con)];
    m[0] = ZB_CONSOLE;
    memcpy(m + 1, con, (size_t)con_len);
    send(m, 1 + con_len);
    con_len = 0;
}

static void serve(void) {
    pump();
    while (in_head != in_tail) {
        int i = in_head;
        in_head = (in_head + 1) % INBOX;
        act(inbox[i], inbox_len[i]);
    }
    flush_drives();
    flush_console();
}

/* the board's turn, from the module: messages on one call in 32 (it
 * comes after every program line and every 100ms piece of a wait) */
static void b_service(void) {
    static unsigned calls;
    if (++calls % 32 == 0 || in_head != in_tail) serve();
    else {
        flush_drives();
        flush_console();
    }
}

/* ---- the board (ls99.h) ---- */

static void b_drive(int i, int d) {
    if (i >= 0 && i < ZB_PINS) drives[i] = (uint8_t)d;
}

static int b_level(int i) {
    return i >= 0 && i < ZB_PINS ? levels[i] : LS99_FLOAT;
}

static void other(z_msg_t *m) {
    take(m);                                    /* bench's, while zi2cx waits */
}

static int b_i2c(uint8_t a, const uint8_t *w, int wn, uint8_t *r, int rn) {
    int st;
    if (!local_bus[0]) return -1;               /* no bus on C/D: nobody answers */
    if (!local_open) {
        if (z_i2cx_open(&local, local_bus) != Z_I2C_OK) return -1;
        local.other = other;
        local_open = true;
    }
    if (wn && rn) st = z_i2cx_write_read(&local, a, w, (uint32_t)wn, r, (uint32_t)rn);
    else if (rn) st = z_i2cx_read(&local, a, r, (uint32_t)rn);
    else st = z_i2cx_write(&local, a, w, (uint32_t)wn);
    return st == Z_I2C_OK ? 0 : -1;
}

static void b_sleep(uint32_t ms) {
    static unsigned caught_up;
    my_clock += ms;
    flush_drives();                             /* a pin changed: at once */
    if ((int32_t)(bench_time - my_clock) >= 0) {
        /* already past: catching up, cheaply; now and then a new time */
        if (++caught_up % 256 == 0) serve();
        return;
    }
    flush_console();
    while (!quit && (int32_t)(bench_time - my_clock) < 0) {
        serve();
        if (sechs.cmd == CMD_HALT || sechs.con_break) return;   /* hw_break acts */
        z_proc_wait(1);
    }
}

static void b_console(char c) {
    if (con_len >= (int)sizeof(con)) flush_console();
    con[con_len++] = c;
    if (c == '\n') flush_console();
}

static void b_set_addr(uint8_t a) {
    uint8_t m[2] = { ZB_ADDR, a };
    send(m, 2);
}

/* about 2ms of real time, the master's turn included */
static void b_pause(void) {
    serve();
    z_proc_wait(2);
}

static const ls99_board_t board = {
    b_drive, b_level, b_i2c, b_sleep, b_service, b_console, b_set_addr, b_pause,
};

/* ---- files: /bench/NAME/, the module's own storage ---- */

static int file = -1;
static uint8_t file_mode;
static char file_path[64], tmp_path[64];

int hw_fopen(const char *n, uint8_t mode) {
    snprintf(file_path, sizeof(file_path), "%s/%s", dir, n);
    file_mode = mode;
    if (mode == FS_READ) {
        file = fs_open_read(file_path);
        return file < 0 ? FS_ERR_NOT_FOUND : FS_OK;
    }
    if (mode == FS_APPEND) {
        z_fs_info_t fi;
        if (fs_stat(file_path, &fi) != 1) fs_touch(file_path);
        file = fs_open_rw(file_path);
        if (file >= 0 && fs_stat(file_path, &fi) == 1) fs_seek(file, fi.size);
        return file < 0 ? FS_ERR_IO : FS_OK;
    }
    snprintf(tmp_path, sizeof(tmp_path), "%s/_SAVING.TMP", dir);
    file = fs_open_write(tmp_path);
    return file < 0 ? FS_ERR_IO : FS_OK;
}

int hw_fread(uint8_t *buf, uint16_t len) {
    int n = fs_read_chunk(file, buf, len);
    return n < 0 ? FS_ERR_IO : n;
}

int hw_fwrite(const uint8_t *buf, uint16_t len) {
    return fs_write_chunk(file, buf, len) == len ? FS_OK : FS_ERR_FULL;
}

int hw_fclose(void) {
    int ok = file >= 0 && fs_close_handle(file);
    file = -1;
    if (file_mode == FS_WRITE && ok) {
        fs_unlink(file_path);
        ok = fs_rename(tmp_path, file_path, 0);
    }
    return ok ? FS_OK : FS_ERR_IO;
}

void hw_fabort(void) {
    if (file >= 0) fs_close_handle(file);
    file = -1;
    if (file_mode == FS_WRITE) fs_unlink(tmp_path);
}

int hw_fdelete(const char *n) {
    char p[64];
    snprintf(p, sizeof(p), "%s/%s", dir, n);
    return fs_unlink(p) ? FS_OK : FS_ERR_NOT_FOUND;
}

int hw_fdir(fs_dir_cb cb) {
    static char buf[1024];
    static uint8_t types[32];
    uint32_t n = 0;
    if (!fs_list_into(dir, buf, sizeof(buf), types, 32, &n, 0)) return FS_OK;
    char *p = buf;
    for (uint32_t i = 0; i < n; i++, p += strlen(p) + 1) {
        const char *f = strrchr(p, '/') ? strrchr(p, '/') + 1 : p;
        if (types[i] != Z_FS_TYPE_DIR && f[0] != '_') cb(f, 0);
    }
    return FS_OK;
}

int hw_fformat(void) {
    return HW_ERR_UNSUPPORTED;          /* a virtual module's files are /bench/NAME */
}

/* ---- the module ---- */

/* the program in the netlist, typed in as at its console, then run */
static void start(void) {
    ls99_init(&board, profile, addr);
    my_clock = bench_time;
    if (!program[0]) return;
    char *text = fs_mallocfile(program);
    if (!text) {
        for (const char *s = "program not found\r\n"; *s; s++) b_console(*s);
        return;
    }
    for (char *p = text; *p; ) {
        char line[BASIC_LINE];
        int n = 0;
        while (*p && *p != '\n' && *p != '\r') {
            if (n < BASIC_LINE - 1) line[n++] = *p;
            p++;
        }
        while (*p == '\r' || *p == '\n') p++;
        line[n] = 0;
        if (n) ls99_line(line);
    }
    free(text);
    if (!quit) ls99_line("RUN");
}

int main(void) {
    char tag[32];
    uint32_t pid = 0;
    if (!z_launch_arg_take(name, sizeof(name)) || !name[0]) return 1;
    snprintf(dir, sizeof(dir), "/bench/%s", name);
    fs_mkdir("/bench");
    fs_mkdir(dir);
    snprintf(tag, sizeof(tag), "%s%s", ZB_MODULE_TAG, name);
    if (!z_pid_lookup(ZB_PROVIDER, &pid) || !pid ||
        z_port_connect_arg(&bench, pid, z_obj_str(tag)) != Z_OK)
        return 1;
    for (int i = 0; i < 400 && !hello && !quit; i++) {
        serve();
        z_proc_wait(1);
    }
    if (!hello) return 1;
    start();
    while (!quit) {
        if (ls99_poll()) start();               /* RESET: the module starts again */
        if (!basic_running) my_clock = bench_time;
        serve();
        z_proc_wait(1);
    }
    if (local_open) z_i2cx_close(&local);
    if (bench.connected) z_port_close(&bench);
    return 0;
}
