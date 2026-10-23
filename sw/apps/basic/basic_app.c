/*
 * basic -- the BASIC computer: Machdyne BASIC (sw/ext/basic) on a
 * 320x240 screen of its own, in a window or full screen. docs/basic_app.md.
 *
 * The screen (bscreen.c) is an image in this process's memory, shown
 * through the arcade runtime (sw/common/games/zarcade.h, docs/arcade.md):
 * the runtime keeps the window and full screen, in black and white or in
 * colour, and its colour window on a board that has one (docs/color.md);
 * paint() draws the rows bscreen says changed, a plane at a time in
 * colour. F1 is the runtime's: the help on BASIC's own screen (the
 * screen toolkit, ui/sui.h), in the window and full screen alike.
 *
 * A program runs inside basic_yield(). The interpreter asks hw_break() at
 * every program line and hw_delay_ms() while it waits; both keep this app
 * alive: they read the runtime's events (keys, Escape to stop, the window
 * closed) and draw a frame when one is due (ar_frame_due()), never
 * waiting for one in between.
 *
 * BASIC is kept apart from the rest of the system: its files are in
 * /data/basic and nowhere else, FORMAT is not available, and it has no pins.
 *
 * It is also the port provider basic0: a terminal (term: F11, port
 * basic0) can connect and type into the same BASIC, text only. Keys from
 * there are decoded (bterm.c) into the keysyms wm sends, so both go the
 * same way; everything printed goes to the screen and the terminal. The
 * port's messages come through the runtime (ar_msg_hook()).
 *
 * Reading events never prints: keys from either side wait in an inbox
 * and are acted on after the events are read. Printing to the port may
 * have to read messages (acks) to make room, and a handler that printed
 * would be called from inside its own printing.
 *
 * On a build host (-DAR_HOST_TESTS, tests/render.c) the same file runs
 * on the runtime's software framebuffer, without the port, the pins or
 * files.
 */

#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#include "../../common/zpaths.h"
#include "../../common/zkbd.h"
#include "../../common/games/zarcade.h"
#include "../../ext/basic/basic.h"

#ifndef AR_HOST_TESTS
#include "../../common/zeitlos.h"
#include "../../common/zsoc.h"
#include "../../common/zwm.h"
#include "../../common/zgfx.h"
#include "../../common/zfsapp.h"
#include "../../common/zport.h"
#include "../../common/zi2cx.h"
#endif

#include "bscreen.h"
#include "bedit.h"
#include "bplat.h"
#include "bterm.h"

#define DIR         Z_DIR_BASIC_PROGRAMS
#define TICK_HZ     732             /* z_uptime_ticks() */
#define FRAME_TICKS 12              /* about one video frame */

static bscreen_t scr;
static bedit_t ed;

static uint32_t grey[BS_PLANE_WORDS];   /* bs_mono(): colour in black and white */
static bool quit;
static bool stop;                   /* Escape or Ctrl-C while running */
static bool line_ready;
static uint8_t keys[32];            /* KEY: pressed while running */
static int nkeys;

static uint32_t inbox[64];          /* keysyms, from wm and the terminal */
static int in_head, in_tail;

#ifndef AR_HOST_TESTS
static z_port_t tport;              /* a terminal on basic0 */
static bterm_t tdec;
static bool greet;                  /* a terminal has just connected */
static char name[24] = "basic";
#endif

static void pump(void);

bscreen_t *bp_screen(void) {
    return &scr;
}

/* ---- showing the screen ---- */

/* The runtime asks for the picture inside r (logical, 8-pixel tiles).
 *
 * In black and white (the window, mono full screen): plane 0 itself, or
 * -- once a program has used colour -- bs_mono()'s picture, paper black
 * and the rest white.
 *
 * In colour (full screen, or the colour window): the four planes as
 * they are, through BASIC's palette; planes 1..3 are blank until a
 * program uses colour, and any plane past them (the colour window's
 * fifth) is blank. */
static void paint(void *user, const ar_rect_t *r) {
    int k, n;
    (void)user;
    if (!ar_color()) {
        const uint32_t *img = scr.px;
        if (scr.colour) {
            bs_mono(&scr, r->y, r->y + r->h, grey);
            img = grey;
        }
        ar_bitmap_opaque(img, BS_STRIDE, r->x, r->y, r->x, r->y, r->w, r->h);
        return;
    }
    for (n = 1; (1 << n) < ar_colours(); n++) ;
    ar_plane(0, scr.px, BS_STRIDE, r->x, r->y, r->x, r->y, r->w, r->h);
    for (k = 1; k < n; k++) {
        if (k <= 3 && scr.colour)
            ar_plane(k, scr.pl[k - 1], BS_STRIDE, r->x, r->y, r->x, r->y, r->w, r->h);
        else
            ar_plane_fill(k, r->x, r->y, r->w, r->h, 0);
    }
}

static const char help_text[] =
    "A small home computer: type BASIC, PRINT and draw on a "
    "320x240 screen of its own.\n\n"
    "Enter\tRun the line, or store a numbered one\n"
    "Backspace\tErase the last character\n"
    "Up\tRecall the last line entered\n"
    "Esc\tErase the line; stop a running program\n"
    "Ctrl+C\tStop a running program\n"
    "F10\tColour on or off\n"
    "F11\tFull screen, or back to the window\n\n"
    "Number each line (10 PRINT \"HELLO\"), then RUN. LIST, "
    "SAVE name, LOAD name and DIR work on " Z_DIR_BASIC_PROGRAMS "; HELP "
    "lists every command.";

static ar_desc_t desc = {
    .name = "BASIC",
    .help_title = "BASIC help",
    .help_text = help_text,
    .help_doc = "basic_app",
    .paint = paint,
};

/* What changed since the last frame, to the runtime: the palette, and
 * the rows bscreen marked. */
static void mark(void) {
    int y0, y1;
    if (scr.pal_dirty) {
        ar_pal_load(scr.pal, 16);
        bs_pal_clean(&scr);
    }
    if (bs_dirty(&scr, &y0, &y1)) {
        ar_dirty(0, y0, BS_W, y1 - y0);
        bs_clean(&scr);
    }
}

/* A frame: what changed is drawn at the next boundary (at once when one
 * is due). */
static void frame(void) {
    mark();
    ar_frame();
}

/* ---- full screen ---- */

bool bp_full_screen(bool on) {
    return ar_set_fullscreen(on);
}

/* ---- keys ---- */

static void inbox_add(uint32_t k) {
    int next = (in_tail + 1) % (int)(sizeof(inbox) / sizeof(inbox[0]));
    if (next != in_head) {
        inbox[in_tail] = k;
        in_tail = next;
    }
}

/* F1 and F11 are the runtime's (help on this screen; full screen, or
 * back); Escape is BASIC's in full screen too (ar_escape_leaves()). */
static void key(uint32_t k) {
    if (basic_running) {
        if (k == 0x1B || k == 0x03) {           /* Escape, Ctrl-C */
            stop = true;
            return;
        }
        uint8_t c = k == Z_KEY_UP ? BP_KEY_UP : k == Z_KEY_DOWN ? BP_KEY_DOWN :
            k == Z_KEY_LEFT ? BP_KEY_LEFT : k == Z_KEY_RIGHT ? BP_KEY_RIGHT :
            k == '\r' ? 13 : k == 0x7F ? 8 : be_latin9(k);
        if (c && nkeys < (int)sizeof(keys)) keys[nkeys++] = c;
        return;
    }
    if (!line_ready && be_key(&ed, k)) line_ready = true;
}

uint8_t bp_key(void) {
    if (!nkeys) return 0;
    uint8_t c = keys[0];
    memmove(keys, keys + 1, (size_t)--nkeys);
    return c;
}

/* ---- the terminal on basic0: output batched, as UTF-8 ---- */

#ifndef AR_HOST_TESTS
#define TOUT 512
static char tout[TOUT];
static int tout_len;

static void tflush(void) {
    if (!tout_len) return;
    if (tport.connected)
        for (int i = 0; i < 64; i++) {      /* bounded: a terminal that has
                                               stopped reading loses text */
            if (z_port_send(&tport, tout, (uint32_t)tout_len) == Z_OK) break;
            pump();                         /* the acks that make room */
            if (!tport.connected) break;    /* or it has gone */
            z_proc_wait(1);
        }
    tout_len = 0;
}

static void tput(uint8_t c) {
    char u[3];
    int n;
    if (!tport.connected) return;
    if (tout_len > TOUT - 4) tflush();
    if (c == '\b') {                        /* erase on the terminal too */
        memcpy(tout + tout_len, "\b \b", 3);
        tout_len += 3;
        return;
    }
    n = bt_out(c, u);
    memcpy(tout + tout_len, u, (size_t)n);
    tout_len += n;
    if (c == '\n') tflush();
}

/* The port's messages, as the runtime reads them (ar_msg_hook()) */
static void port_msg(void *mp) {
    z_msg_t *msg = (z_msg_t *)mp;
    switch (msg->subject) {
    case Z_PORT_CONNECT:
        if (tport.connected) {
            z_port_refuse(msg, "basic: a terminal is already connected");
        } else {
            z_port_accept(&tport, msg, 1);
            memset(&tdec, 0, sizeof(tdec));
            greet = true;
        }
        break;
    case Z_PORT_DATA:
        if (tport.connected && msg->from == tport.peer_pid) {
            uint32_t len = z_blob_len(&msg->obj), k;
            const uint8_t *d = (const uint8_t *)z_blob_data(&msg->obj);
            z_port_send_ack(msg);
            for (uint32_t i = 0; i < len; i++)
                if (bt_in(&tdec, d[i], &k)) inbox_add(k);
            if (bt_end(&tdec, &k)) inbox_add(k);
        }
        break;
    case Z_PORT_DATA_ACK:
        if (tport.connected && msg->from == tport.peer_pid)
            z_port_handle_ack(&tport, msg);
        break;
    case Z_PORT_CLOSE:
        if (tport.connected && msg->from == tport.peer_pid) z_port_close(&tport);
        break;
    default:
        break;
    }
}
#else
static void tflush(void) { }
static void tput(uint8_t c) { (void)c; }
#endif

/* everything BASIC and the editor print: the screen and the terminal */
static void emit(uint8_t c) {
    bs_putc(&scr, c);
    tput(c);
}

/* ---- events ---- */

/* read the events; nothing is printed */
static void pump(void) {
    ar_event_t e;
    while (ar_poll(&e)) {
        if (e.type == AR_EV_KEY && e.pressed) inbox_add(e.key);
        else if (e.type == AR_EV_QUIT) quit = stop = true;
    }
}

/* read the events, then act on the keys and a new terminal */
static void messages(void) {
    pump();
    while (in_head != in_tail) {
        uint32_t k = inbox[in_head];
        in_head = (in_head + 1) % (int)(sizeof(inbox) / sizeof(inbox[0]));
        key(k);
    }
#ifndef AR_HOST_TESTS
    if (greet) {
        static const char hello[] =
            "MACHDYNE BASIC 1 -- text here; graphics in the BASIC window\r\n";
        greet = false;
        for (const char *p = hello; *p; p++) tput((uint8_t)*p);
        if (!basic_running && !basic_input) {
            for (const char *p = "READY.\r\n"; *p; p++) tput((uint8_t)*p);
            for (int i = 0; i < ed.len; i++) tput((uint8_t)ed.line[i]);
        }
    }
#endif
    tflush();
}

/* ---- what the interpreter asks of the system (basic.h) ---- */

void hw_putc(char c) {
    emit((uint8_t)c);
}

/* Every program line: the events, and a frame if one is due -- never a
 * wait. */
int hw_break(void) {
    messages();
    if (ar_frame_due()) frame();
    return stop;
}

/* The time, in ticks of TICK_HZ: on a build host it moves a frame at a
 * time, with ar_frame(). */
#ifndef AR_HOST_TESTS
static uint32_t now(void) { return z_uptime_ticks(); }
#else
static uint32_t now(void) { return ar_ticks() * TICK_HZ / 60u; }
#endif

/* Frames while there is a frame or more to wait, then the rest asleep. */
void hw_delay_ms(uint16_t ms) {
    uint32_t start = now(), span = (uint32_t)ms * TICK_HZ / 1000;
    while (!stop && now() - start < span) {
        uint32_t left = span - (now() - start);
        messages();
#ifndef AR_HOST_TESTS
        if (left < FRAME_TICKS) {
            if (ar_frame_due()) frame();
            z_proc_wait(left);
            continue;
        }
#else
        (void)left;
#endif
        frame();
    }
}

/* SYNC: what was drawn, shown at the next frame boundary. */
void bp_sync(void) {
    messages();
    frame();
}

#ifndef AR_HOST_TESTS
/* Pins 3 and 4 (C, D) as I2C, on the bench bus a netlist gives the
 * BASIC computer (`basic BUS`, docs/bench.md), through zi2cx: opened
 * again at every PINS, so a netlist reloaded in bench is used by the
 * next RUN. NET on pins 1 and 2 is "left to the system": nothing to do,
 * and a module's program (PINS NET, NET, I2C, I2C) runs here unchanged.
 * Anything else: there are no pins. */
static z_i2cx_t i2c_bus;
static bool i2c_open;

/* while zi2cx waits for bench: wm's messages and the terminal's, to the
 * runtime as ar_poll() would have read them (nothing is printed: keys
 * wait in the runtime's queue and the inbox) */
static void i2c_other(z_msg_t *m) {
    ar_msg(m);
}

int hw_pin_mode(uint8_t pin, uint8_t mode) {
    if (mode == PM_NONE) return 0;
    if (mode == PM_NET && (pin == 1 || pin == 2)) return 0;
    if (mode == PM_I2C && (pin == 3 || pin == 4)) {
        if (pin == 3 || !i2c_open) {
            if (i2c_open) z_i2cx_close(&i2c_bus);
            i2c_open = false;
            i2c_bus.other = i2c_other;
            if (z_i2cx_open_role(&i2c_bus, "basic") != Z_I2C_OK) return HW_ERR_UNSUPPORTED;
            i2c_open = true;
        }
        return 0;
    }
    return HW_ERR_UNSUPPORTED;
}
void hw_pin_write(uint8_t pin, uint8_t level) { (void)pin; (void)level; }
uint8_t hw_pin_read(uint8_t pin) { (void)pin; return 0; }
int16_t hw_adc(uint8_t pin) { (void)pin; return -1; }
void hw_led(uint8_t on) { (void)on; }
int hw_i2c(uint8_t addr, const uint8_t *w, uint8_t wn, uint8_t *r, uint8_t rn) {
    int st;
    if (!i2c_open) return -1;
    if (wn && rn) st = z_i2cx_write_read(&i2c_bus, addr, w, wn, r, rn);
    else if (rn) st = z_i2cx_read(&i2c_bus, addr, r, rn);
    else st = z_i2cx_write(&i2c_bus, addr, w, wn);
    return st == Z_I2C_OK ? 0 : -1;
}

/* ---- files: its disk, /data/basic, only (docs/layout.md) ---- */

static int file = -1;
static uint8_t file_mode;
static char file_path[64];

#define TMP DIR "/_SAVING.TMP"

int hw_fopen(const char *name, uint8_t mode) {
    snprintf(file_path, sizeof(file_path), DIR "/%s", name);
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
    /* FS_WRITE: a new file, put in place only when it is closed, so a
     * failed SAVE leaves the old one */
    file = fs_open_write(TMP);
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
        ok = fs_rename(TMP, file_path, 0);
    }
    return ok ? FS_OK : FS_ERR_IO;
}

void hw_fabort(void) {
    if (file >= 0) fs_close_handle(file);
    file = -1;
    if (file_mode == FS_WRITE) fs_unlink(TMP);
}

int hw_fdelete(const char *name) {
    char p[64];
    snprintf(p, sizeof(p), DIR "/%s", name);
    return fs_unlink(p) ? FS_OK : FS_ERR_NOT_FOUND;
}

int hw_fdir(fs_dir_cb cb) {
    static char buf[2048];
    static uint8_t types[64];
    uint32_t n = 0;
    if (!fs_list_into(DIR, buf, sizeof(buf), types, 64, &n, 0)) return FS_OK;
    char *p = buf;
    for (uint32_t i = 0; i < n; i++, p += strlen(p) + 1) {
        const char *name = strrchr(p, '/') ? strrchr(p, '/') + 1 : p;
        if (types[i] == Z_FS_TYPE_DIR || name[0] == '_') continue;
        cb(name, 0);
    }
    return FS_OK;
}

int hw_fformat(void) {
    return HW_ERR_UNSUPPORTED;      /* a learning machine: no FORMAT */
}

#endif /* !AR_HOST_TESTS: the pins and the files */

/* ---- the BASIC computer ---- */

static void say(const char *s) {
    while (*s) emit((uint8_t)*s++);
}

static void ready(void) {
    if (!basic_input) say("READY.\r\n");
    be_start(&ed);
    bs_cursor(&scr, true);
}

static void enter(void) {
    char line[BE_MAX + 1];
    memcpy(line, ed.line, sizeof(line));
    line_ready = false;
    bs_cursor(&scr, false);
    stop = false;
    nkeys = 0;
    basic_yield((uint8_t *)line);
    if (stop && !basic_running) stop = false;
    if (!quit) ready();
    tflush();
}

int main(void) {
#ifndef AR_HOST_TESTS
    if (!z_fb_hw_blit_mem_available()) {
        printf("basic: this bitstream's blitter has no memory copy mode\n");
        return 1;
    }
    fs_mkdir(DIR);                  /* (already there is fine) */
#endif
    bs_init(&scr);
    be_init(&ed, &scr);
    ed.out = emit;
    memcpy(desc.palette, scr.pal, sizeof(desc.palette));
    desc.ink[1] = ar_ink_white;
#ifndef AR_HOST_TESTS
    z_pid_register("basic", name, sizeof(name));    /* basic0, for term */
    /* colour in the window where it is all sixteen colours: 32 on an
     * eight-plane board (8, on six, would show half of them wrong) */
    ar_window_colour(z_color_window_planes() >= 4);
    ar_msg_hook(port_msg);
#else
    ar_window_colour(true);
#endif
    if (!ar_init(&desc)) {
        printf("basic: no window (is wm running?)\n");
        return 1;
    }
    ar_text_input(true);            /* C and M are letters */
    ar_escape_leaves(false);        /* Escape stops a program, full screen too */
    say("MACHDYNE BASIC 1\r\n\r\n");
    if (basic_boot()) {             /* /data/basic/BOOT.BAS */
        static char run[] = "RUN";
        basic_yield((uint8_t *)run);
    }
    ready();
    while (!quit) {
        messages();
        if (line_ready) enter();
        if (!quit) frame();
    }
#ifndef AR_HOST_TESTS
    if (tport.connected) {
        static const char bye[] = "\r\nBASIC has closed\r\n";
        tout_len = 0;
        for (const char *p = bye; *p; p++) tput((uint8_t)*p);
        tflush();
        z_port_close(&tport);
    }
#endif
    ar_shutdown();
    return 0;
}
