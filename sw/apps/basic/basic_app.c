/*
 * basic -- the BASIC computer: Machdyne BASIC (sw/ext/basic) on a
 * 320x240 screen of its own, in a window or full screen. docs/basic_app.md.
 *
 * The screen (bscreen.c) is an image in this process's memory; this file
 * copies it to the window, or full screen to a game-mode page. Keys come
 * from wm as Z_WM_KEY in both modes: full screen, the game grab sends
 * them here, already through the keyboard layout.
 *
 * A program runs inside basic_yield(). The interpreter asks hw_break() at
 * every program line and hw_delay_ms() while it waits; both keep this app
 * alive: they read wm's messages (redraws, keys, Escape to stop) and show
 * the screen at most once per video frame.
 *
 * BASIC is kept apart from the rest of the system: its files are in
 * /data/basic and nowhere else, FORMAT is not available, and it has no pins.
 *
 * It is also the port provider basic0: a terminal (term: F11, port
 * basic0) can connect and type into the same BASIC, text only. Keys from
 * there are decoded (bterm.c) into the keysyms wm sends, so both go the
 * same way; everything printed goes to the screen and the terminal.
 *
 * The message handler never prints: keys from either side wait in an
 * inbox and are acted on after the messages are read. Printing to the
 * port may have to read messages (acks) to make room, and a handler that
 * printed would be called from inside its own printing.
 */

#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#include "../../common/zpaths.h"
#include "../../common/zeitlos.h"
#include "../../common/zsoc.h"
#include "../../common/zwm.h"
#include "../../common/zwin.h"
#include "../../common/zgfx.h"
#include "../../common/zkbd.h"
#include "../../common/zfsapp.h"
#include "../../common/zcolor.h"
#include "../../common/zport.h"
#include "../../common/zi2cx.h"
#include "../../ext/basic/basic.h"

#include "bscreen.h"
#include "bedit.h"
#include "bplat.h"
#include "bterm.h"

#define DIR         Z_DIR_BASIC_PROGRAMS
#define TICK_HZ     732             /* z_uptime_ticks() */
#define FRAME_TICKS 12              /* about one video frame */

static bscreen_t scr;
static bedit_t ed;
static z_win_t win;
static int chrome_w, chrome_h;
static bool chrome_known;

static bool full;                   /* full screen (game mode) */
static int page;                    /* the game-mode page drawn next */
static bool full_colour;            /* full screen, in hardware colour */
static uint32_t grey[BS_PLANE_WORDS];   /* bs_mono(): colour in black and white */
static bool quit;
static bool stop;                   /* Escape or Ctrl-C while running */
static bool line_ready;
static uint32_t last_shown;
static uint8_t keys[32];            /* KEY: pressed while running */
static int nkeys;

static uint32_t inbox[64];          /* keysyms, from wm and the terminal */
static int in_head, in_tail;

static z_port_t tport;              /* a terminal on basic0 */
static bterm_t tdec;
static bool greet;                  /* a terminal has just connected */
static char name[24] = "basic";

static void pump(void);

static void on_msg(z_msg_t *msg, void *user);

bscreen_t *bp_screen(void) {
    return &scr;
}

/* ---- showing the screen ---- */

/* The picture a monochrome display shows: plane 0 itself, or -- once a
 * program has used colour -- paper black, the rest white (bs_mono()), rows
 * y0..y1-1 brought up to date. */
static const uint32_t *mono_rows(int y0, int y1) {
    if (!scr.colour) return scr.px;
    bs_mono(&scr, y0, y1, grey);
    return grey;
}

static void show_window(bool all) {
    z_clip_t cc;
    int y0 = 0, y1 = BS_H, n;
    const uint32_t *img;
    if (!all && !bs_dirty(&scr, &y0, &y1)) return;
    img = mono_rows(y0, y1);
    z_win_content_rect(&win, &cc);
    n = z_gfx_visible_count();
    if (n == 0) n = 1;
    for (int i = 0; i < n; i++) {
        if (!z_gfx_blit_scissor(i, &cc)) continue;
        z_fb_hw_blit_mem(img, BS_STRIDE, 0, y0, cc.x0, cc.y0 + y0, BS_W, y1 - y0);
    }
    z_gfx_blit_scissor_reset();
}

/* Full screen in colour (docs/color.md): the four planes into the four
 * quadrants, the viewport at (0,0), through the palette.
 *
 * SINGLE-BUFFERED. Sixteen colours use all four quadrants, so there is
 * no page left to draw into while another is shown. The rows that
 * changed are copied straight after a frame boundary, which keeps a
 * small change clear of the picture being scanned; a whole-screen
 * change can show half drawn for one frame. SYNC still paces a
 * program to the display.
 *
 * Entered the first time a picture with colour is shown full screen --
 * which may be in the middle of a program, the moment it first says
 * INK -- so it starts with the whole image. */
static void show_full_colour(bool all) {
    int y0 = 0, y1 = BS_H;
    const void *planes[4] = { scr.px, scr.pl[0], scr.pl[1], scr.pl[2] };
    if (!full_colour) {
        z_color_begin(&z_color_16);
        z_game_set_view(0, 0);
        full_colour = true;
        all = true;
        scr.pal_dirty = true;
    }
    if (!all && !bs_dirty(&scr, &y0, &y1)) y1 = y0;
    z_game_wait_frame();
    if (scr.pal_dirty) {
        z_color_palette_load_now(scr.pal, 16);
        bs_pal_clean(&scr);
    }
    z_gfx_blit_scissor_reset();
    if (y1 > y0)
        z_color_blit_planes(&z_color_16, planes, BS_STRIDE, 0, y0,
            0, y0, BS_W, y1 - y0);
}

/* Full screen: the whole image into the page not on screen, then that
 * page is shown at the next frame boundary. Both pages are drawn in
 * turn, so each gets the whole image. With colour but no colour
 * hardware, the image is bs_mono()'s black and white. */
static void show_full(bool all) {
    int px = page * Z_GAME_VIEW_W;
    if (scr.colour && z_color_available()) {
        show_full_colour(all);
        return;
    }
    z_gfx_blit_scissor_reset();
    z_fb_hw_blit_mem(mono_rows(0, BS_H), BS_STRIDE, 0, 0, px, 0, BS_W, BS_H);
    z_game_set_view((uint32_t)px, 0);
    z_game_wait_frame();
    page ^= 1;
}

static void show(bool all) {
    if (full) show_full(all);
    else show_window(all);
    bs_clean(&scr);
    last_shown = z_uptime_ticks();
}

/* if anything changed, and a frame has passed since the last time */
static void show_now_and_then(void) {
    int y0, y1;
    if (bs_dirty(&scr, &y0, &y1) && z_uptime_ticks() - last_shown >= FRAME_TICKS)
        show(false);
}

/* ---- full screen ---- */

bool bp_full_screen(bool on) {
    uint32_t wm_pid;
    if (on == full) return true;
    if (on) {
        if (!z_game_available()) return false;
        z_gfx_clear_visible();
        z_gfx_blit_scissor_reset();
        z_fb_hw_fill_rect(0, 0, 640, 480, 0);
        z_game_set_enabled(true, false);        /* and the game grab */
        full = true;
        full_colour = false;
        page = 0;
        show(true);
        return true;
    }
    z_game_set_enabled(false, false);       /* colour goes off with it */
    full = false;
    full_colour = false;
    /* every window is where it was, but this app drew over them */
    if (z_pid_lookup("wm0", &wm_pid))
        z_msg_new_send(wm_pid, Z_WM_REPAINT, 0, z_obj_uint32(0));
    return true;
}

/* ---- keys ---- */

static void inbox_add(uint32_t k) {
    int next = (in_tail + 1) % (int)(sizeof(inbox) / sizeof(inbox[0]));
    if (next != in_head) {
        inbox[in_tail] = k;
        in_tail = next;
    }
}

static void key(uint32_t k) {
    if (k == Z_KEY_F6) {                        /* full screen, or back */
        bp_full_screen(!full);
        return;
    }
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

/* everything BASIC and the editor print: the screen and the terminal */
static void emit(uint8_t c) {
    bs_putc(&scr, c);
    tput(c);
}

/* ---- messages ---- */

static void on_msg(z_msg_t *msg, void *user) {
    (void)user;
    switch (msg->subject) {
    case Z_WM_SET_CLIP:
        z_win_apply_clip(&win, &msg->obj);
        break;
    case Z_WM_REDRAW:
        z_win_apply_redraw(&win, msg->obj.val.uint32);
        if (!full) show_window(true);
        /* acked full screen too: wm waits for it before the next
         * window's redraw */
        z_win_redraw_done(&win);
        break;
    case Z_WM_WINDOW_MOVED:
        z_win_parse_rect(&win, &msg->obj);
        break;
    case Z_WM_KEY:
        if (Z_WM_UNPACK_KEY_PRESSED(msg->obj.val.uint32))
            inbox_add(Z_WM_UNPACK_KEY_KEYSYM(msg->obj.val.uint32));
        break;
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
    case Z_WM_GAME_REVOKED:                     /* wm took the screen back */
        full = false;
        full_colour = false;                    /* and colour with it */
        break;
    case Z_WM_CLOSE:
        quit = stop = true;
        break;
    default:
        break;
    }
}

/* read the messages; nothing is printed */
static void pump(void) {
    z_msg_t msg;
    while (z_msg_read(&msg) == Z_OK) on_msg(&msg, NULL);
}

/* read the messages, then act on the keys and a new terminal */
static void messages(void) {
    pump();
    while (in_head != in_tail) {
        uint32_t k = inbox[in_head];
        in_head = (in_head + 1) % (int)(sizeof(inbox) / sizeof(inbox[0]));
        key(k);
    }
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
    tflush();
}

static bool open_window(void) {
    if (z_win_create_cb(&win, "BASIC", (uint32_t)(BS_W + chrome_w),
            (uint32_t)(BS_H + chrome_h), -1, -1, Z_WIN_FLAG_CLOSE_ICON,
            on_msg, NULL) != Z_OK)
        return false;
    if (!chrome_known) {
        /* the frame is wm's business: measure it, then ask for the size
         * that gives the screen its own 320x240 pixels (as chip8 does) */
        chrome_w = (int)win.w - z_win_content_w(&win);
        chrome_h = (int)win.h - z_win_content_h(&win);
        chrome_known = true;
        if (chrome_w || chrome_h) {
            z_win_destroy(&win);
            return open_window();
        }
    }
    return true;
}

/* ---- what the interpreter asks of the system (basic.h) ---- */

void hw_putc(char c) {
    emit((uint8_t)c);
}

int hw_break(void) {
    messages();
    show_now_and_then();
    return stop;
}

void hw_delay_ms(uint16_t ms) {
    uint32_t start = z_uptime_ticks(), span = (uint32_t)ms * TICK_HZ / 1000;
    while (!stop && z_uptime_ticks() - start < span) {
        uint32_t left = span - (z_uptime_ticks() - start);
        messages();
        show_now_and_then();
        z_proc_wait(left < FRAME_TICKS ? left : FRAME_TICKS);
    }
}

void bp_sync(void) {
    uint32_t f;
    show(true);                                 /* full screen: waits a frame */
    if (full) return;
    f = z_game_present() ? z_game_frame() : 0;
    for (int i = 0; i < 4 && !stop; i++) {      /* at most about 4 frames */
        messages();
        if (z_game_present() && z_game_frame() != f) return;
        z_proc_wait(FRAME_TICKS / 4);
        if (!z_game_present()) return;
    }
}

/* Pins 3 and 4 (C, D) as I2C, on the bench bus a netlist gives the
 * BASIC computer (`basic BUS`, docs/bench.md), through zi2cx: opened
 * again at every PINS, so a netlist reloaded in bench is used by the
 * next RUN. NET on pins 1 and 2 is "left to the system": nothing to do,
 * and a module's program (PINS NET, NET, I2C, I2C) runs here unchanged.
 * Anything else: there are no pins. */
static z_i2cx_t i2c_bus;
static bool i2c_open;

/* while zi2cx waits for bench: wm's messages and the terminal's, into
 * the app's own handler (it never prints: keys wait in the inbox) */
static void i2c_other(z_msg_t *m) {
    on_msg(m, NULL);
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
    if (!z_fb_hw_blit_mem_available()) {
        printf("basic: this bitstream's blitter has no memory copy mode\n");
        return 1;
    }
    fs_mkdir(DIR);                  /* (already there is fine) */
    bs_init(&scr);
    be_init(&ed, &scr);
    ed.out = emit;
    z_pid_register("basic", name, sizeof(name));    /* basic0, for term */
    if (!open_window()) {
        printf("basic: no window (is wm running?)\n");
        return 1;
    }
    say("MACHDYNE BASIC 1\r\n\r\n");
    if (basic_boot()) {             /* /data/basic/BOOT.BAS */
        static char run[] = "RUN";
        basic_yield((uint8_t *)run);
    }
    ready();
    show(true);
    while (!quit) {
        messages();
        if (line_ready) enter();
        show_now_and_then();
        if (!line_ready && !quit) z_proc_wait(bs_dirty(&scr, &(int){0}, &(int){0}) ? 1 : 100);
    }
    if (full) bp_full_screen(false);
    if (tport.connected) {
        static const char bye[] = "\r\nBASIC has closed\r\n";
        tout_len = 0;
        for (const char *p = bye; *p; p++) tput((uint8_t)*p);
        tflush();
        z_port_close(&tport);
    }
    z_win_destroy(&win);
    return 0;
}
