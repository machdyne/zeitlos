/*
 * bench -- virtual parts, nets and I2C buses from a netlist, shown live
 * (docs/bench.md). Started with a netlist: `bench /bench/panel.net`.
 *
 * The cards are drawn into a canvas the size of the view (draw.c), from
 * the scroll position, and copied to the window. The canvas is a .bss
 * array, as sw/apps/view's document is: an app's malloc heap shares one
 * 16KB allowance with its stack (docs/executables.md), and a canvas from
 * it failed for any useful window, leaving only the status line. And the
 * blitter reads physical memory (zgfx.h, z_fb_hw_blit_mem()): .bss stays
 * put. Resizing reflows the cards; scrolling draws them again.
 *
 * bench is also the port provider bench0: zi2cx (sw/common/zi2cx.c)
 * sends it transactions for its buses (zbench.h), one reply each.
 *
 *   the open icon   another netlist (the file dialog, from /bench)
 *   click      press a button, flip a switch; on a pin: its net, below
 *   F5         read the netlist again (after editing it)
 *   F6         the cards, the bus log, the modules' consoles
 *   1 2 3      virtual time at x1, x60, x3600; space: pause
 *   wheel      scroll
 *
 * Real hardware (docs/bench.md): bench owns the PMOD ports its netlist's
 * gpio parts name, and is their only user while it runs (as zgpio.h
 * recommends: "a server process that owns the pins"). Every loop it
 * reads their in= pins onto their nets and writes their out= nets onto
 * the pins, open drain. A bus with segment= sends what no virtual part
 * answers out through zi2c on that port's pins 1 and 2. Its ports are
 * listed as buses ("pmod1"), so zi2cx goes through bench for them. With
 * real hardware, virtual time stays at x1.
 *
 * Modules (docs/ls99.md): one ls99 process each, started after the
 * netlist is read, one at a time (a launch argument is claimed by the
 * next process started). bench keeps the virtual clock and broadcasts
 * it; it sends each module the levels of its nets when they change;
 * and a client's transaction to a module goes to the module's process,
 * its reply back to that client when it comes -- bench serves everything
 * else meanwhile.
 */

#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdlib.h>

#include "../../common/zeitlos.h"
#include "../../common/zsoc.h"
#include "../../common/zwm.h"
#include "../../common/zwin.h"
#include "../../common/zgfx.h"
#include "../../common/zkbd.h"
#include "../../common/zwidget.h"
#include "../../common/zdialog.h"
#include "../../common/zgpio.h"
#include "../../common/zi2c.h"
#include "../../common/zfsapp.h"
#include "../../common/zport.h"
#include "../../common/zbench.h"
#include "../../common/zfont.h"

#include "core.h"
#include "netlist.h"
#include "draw.h"

#define WIN_W       480
#define WIN_H       360
#define STATUS_H    12
#define SCROLL_STEP 16
#define CLIENTS     8

static z_win_t win;
static z_scrollbar_t vsb, hsb;
static int view_x, view_y, view_w, view_h, scroll_x, scroll_y;

/* the view: at most the screen */
#define VIEW_WPL    (BD_MAX_W / 32)
#define VIEW_H_MAX  480
static uint32_t view_px[VIEW_H_MAX * VIEW_WPL] __attribute__((section(".bss")));
static canvas_t view;
static int doc_w, doc_h;            /* the whole document, for the scrollbars */
static bool dirty = true;
static char path[128], status[160];
static uint32_t start_ticks;

static z_port_t clients[CLIENTS];
static char name[24] = "bench";

/* virtual time */
static uint32_t vclock, last_ticks;
static int speed = 1;
static bool paused;

/* the modules' processes */
#define MODS 8
typedef struct {
    z_port_t port;
    int part;                       /* bn_parts index */
    uint32_t pid;
    uint8_t sent[ZB_PINS];          /* the levels last sent */
    uint32_t time_sent;
} mod_t;
static mod_t mods[MODS];
static int nmods;
static char consoles[2048];         /* what the modules print, newest last */
static int consoles_len;

/* a client's transaction waiting for a module */
#define PENDING 16
static struct { z_port_t *client; uint8_t rn; bool used; } pending[PENDING];

static int view_mode;               /* 0 cards, 1 bus log, 2 consoles */

/* the real ports: one per gpio part, with zi2c if it is a segment */
typedef struct {
    part_t *part;
    int port;
    bool segment;
    z_i2c_t i2c;
    uint8_t in_last;
} real_t;
static real_t reals[8];
static int nreals;
static void real_release(void);
static void real_start(void);
static void on_msg(z_msg_t *m);
static void stop_modules(void);
static void start_modules(void);

/* ---- the netlist ---- */

static void load(void) {
    char err[128];
    stop_modules();
    real_release();
    char *text = path[0] ? fs_mallocfile(path) : 0;
    if (!path[0]) {
        bn_clear();
        snprintf(status, sizeof(status), "no netlist: bench /bench/NAME.net");
    } else if (!text) {
        bn_clear();
        snprintf(status, sizeof(status), "%s: cannot read it", path);
    } else if (bn_load(text, err, sizeof(err))) {
        snprintf(status, sizeof(status), "%.40s %.110s", strrchr(path, '/') ? strrchr(path, '/') + 1 : path, err);
    } else {
        status[0] = 0;
    }
    free(text);
    start_ticks = z_uptime_ticks();
    vclock = 0;
    bn_now = 0;
    consoles_len = 0;
    consoles[0] = 0;
    dirty = true;
    real_start();
    if (bn_real()) speed = 1;
    start_modules();
}

/* ---- layout and drawing ---- */

static void layout(void) {
    int cw = z_win_content_w(&win), ch = z_win_content_h(&win);
    view_x = 0;
    view_y = STATUS_H;
    view_w = cw - Z_SB_THICK;
    view_h = ch - STATUS_H - Z_SB_THICK;
    if (view_w < 0) view_w = 0;
    if (view_h < 0) view_h = 0;
    z_scrollbar_set_geom(&vsb, cw - Z_SB_THICK, view_y, view_h);
    z_scrollbar_set_geom(&hsb, view_x, ch - Z_SB_THICK, view_w);
    dirty = true;
}

/* the document's size, the scrollbars, then the view drawn from the
 * scroll position */
static bool log_follow;             /* the log scrolls to its end */

static void render(void) {
    doc_w = view_w > 3 * BD_CELL_W ? view_w : 3 * BD_CELL_W;
    if (doc_w > BD_MAX_W) doc_w = BD_MAX_W;
    doc_h = view_mode == 1 ? bd_draw_log(0, 0) : view_mode == 2 ? bd_draw_text(0, consoles, 0) :
        bd_layout(doc_w);
    if (z_scrollbar_set_range(&vsb, doc_h, view_h)) scroll_y = vsb.value;
    if (z_scrollbar_set_range(&hsb, doc_w, view_w)) scroll_x = hsb.value;
    if ((view_mode != 0) && log_follow && z_scrollbar_set_value(&vsb, doc_h - view_h))
        scroll_y = vsb.value;
    view.px = view_px;
    view.wpl = VIEW_WPL;
    view.w = view_w < BD_MAX_W ? view_w : BD_MAX_W;
    view.h = view_h < VIEW_H_MAX ? view_h : VIEW_H_MAX;
    if (view_mode == 1) bd_draw_log(&view, scroll_y);
    else if (view_mode == 2) bd_draw_text(&view, consoles, scroll_y);
    else bd_draw(&view, scroll_x, scroll_y);
}

static void draw_status(void) {
    char s[200];
    int cw = z_win_content_w(&win);
    if (status[0]) snprintf(s, sizeof(s), "%s", status);
    else {
        unsigned long t = bn_now / 1000;
        snprintf(s, sizeof(s), "%s  %lu:%02lu:%02lu  x%d%s  parts %d  conflicts %d%s",
                 path[0] ? (strrchr(path, '/') ? strrchr(path, '/') + 1 : path) : "-",
                 t / 3600, t / 60 % 60, t % 60, speed, paused ? " paused" : "",
                 bn_nparts, bn_conflicts,
                 view_mode == 1 ? "  [log]" : view_mode == 2 ? "  [consoles]" : "");
    }
    /* dark text on the light bar: z_win_draw_text() paints its cell's
     * background 0 whatever the ink, so ink 0 would be black on black
     * (zwin.h, z_win_draw_text2()) */
    z_win_fill_rect(&win, 0, 0, cw, STATUS_H, 1);
    z_win_draw_text2(&win, 3, 2, s, 0, 1, &z_font_5x8);
}

static void show(void) {
    z_clip_t cc;
    int w = view.w, h = view.h, n;
    z_win_content_rect(&win, &cc);
    n = z_gfx_visible_count();
    if (n == 0) n = 1;
    if (w > 0 && h > 0)
        for (int i = 0; i < n; i++) {
            if (!z_gfx_blit_scissor(i, &cc)) continue;
            z_fb_hw_blit_mem(view.px, VIEW_WPL * 4, 0, 0,
                             cc.x0 + view_x, cc.y0 + view_y, w, h);
        }
    z_gfx_blit_scissor_reset();
    if (w < view_w) z_win_fill_rect(&win, view_x + (w > 0 ? w : 0), view_y, view_w - (w > 0 ? w : 0), view_h, 0);
    draw_status();
}

static void repaint(void) {
    if (dirty) {
        render();
        dirty = false;
    }
    show();
    z_scrollbar_draw(&vsb, true);
    z_scrollbar_draw(&hsb, true);
}

/* Compared with where the view was drawn, not with what the scrollbar
 * says: after a drag, z_scrollbar_mouse() has already moved the bar, so
 * asking it to move there again changes nothing -- and the view, drawn
 * at the old position, would never follow the bar. */
static void scroll_to(int x, int y) {
    z_scrollbar_set_value(&hsb, x);
    z_scrollbar_set_value(&vsb, y);
    bool c = hsb.value != scroll_x || vsb.value != scroll_y;
    scroll_x = hsb.value;
    scroll_y = vsb.value;
    if (c) {
        log_follow = (view_mode != 0) && scroll_y >= doc_h - view_h;
        dirty = true;                       /* drawn again from there */
    }
}

/* ---- the mouse ---- */

static int held = -1;               /* a button being held down */

static void mouse(uint32_t packed) {
    int cx, cy, pin;
    uint8_t buttons = (uint8_t)Z_WM_UNPACK_MOUSE_BUTTONS(packed);
    static uint8_t last;
    z_win_mouse_content_xy(&win, packed, &cx, &cy);
    if (z_scrollbar_has_pointer(&vsb, cx, cy)) {
        if (z_scrollbar_mouse(&vsb, cx, cy, buttons)) scroll_to(scroll_x, vsb.value);
        last = buttons;
        return;
    }
    if (z_scrollbar_has_pointer(&hsb, cx, cy)) {
        if (z_scrollbar_mouse(&hsb, cx, cy, buttons)) scroll_to(hsb.value, scroll_y);
        last = buttons;
        return;
    }
    z_scrollbar_mouse(&vsb, cx, cy, buttons);
    z_scrollbar_mouse(&hsb, cx, cy, buttons);

    bool down = (buttons & 1) && !(last & 1), up = !(buttons & 1) && (last & 1);
    last = buttons;
    if (up && held >= 0) {
        part_t *p = &bn_parts[held];
        p->type->click(p, false);
        held = -1;
        dirty = true;
        return;
    }
    if (!down || (view_mode != 0) || cy < view_y) return;
    int i = bd_hit(cx - view_x + scroll_x, cy - view_y + scroll_y, &pin);
    if (i < 0) return;
    part_t *p = &bn_parts[i];
    if (p->type->click && p->type->npins == 1) {
        p->type->click(p, true);
        held = i;
    }
    if (pin >= 0) bd_net_info(p, pin, status, sizeof(status));
    else snprintf(status, sizeof(status), "%s: a %s -- %s", p->name, p->type->name, p->type->about);
    dirty = true;
}

/* ---- real hardware ---- */

static real_t *real_of(part_t *p) {
    for (int i = 0; i < nreals; i++)
        if (reals[i].part == p) return &reals[i];
    return 0;
}

/* bn_segment_xfer: what no virtual part answered, on the real pins */
static int real_xfer(part_t *g, uint8_t addr, const uint8_t *w, int wn, uint8_t *r, int rn) {
    real_t *rl = real_of(g);
    int st;
    if (!rl || !rl->segment) return BN_NACK_ADDR;
    if (!wn && !rn) st = z_i2c_probe(&rl->i2c, addr) ? Z_I2C_OK : Z_I2C_NACK;
    else if (wn && rn) st = z_i2c_write_read(&rl->i2c, addr, w, (uint32_t)wn, r, (uint32_t)rn);
    else if (rn) st = z_i2c_read(&rl->i2c, addr, r, (uint32_t)rn, true);
    else st = z_i2c_write(&rl->i2c, addr, w, (uint32_t)wn, true);
    return st == Z_I2C_OK ? BN_ACK : BN_NACK_ADDR;
}

static void real_release(void) {
    for (int i = 0; i < nreals; i++)
        for (int pin = 0; pin < 8; pin++) {
            uint8_t mine = bn_gpio_in_mask(reals[i].part) | bn_gpio_out_mask(reals[i].part) |
                           (reals[i].segment ? 3 : 0);
            if (mine >> pin & 1) z_gpio_mode((uint32_t)reals[i].port, (uint32_t)pin, Z_GPIO_IN);
        }
    nreals = 0;
    bn_segment_xfer = 0;
}

/* the netlist's ports: pins set up, segments' buses opened, and a
 * warning if a real device answers at a virtual part's address */
static void real_start(void) {
    for (int i = 0; i < bn_nparts && nreals < 8; i++) {
        part_t *p = &bn_parts[i];
        if (!p->type->mark || strcmp(p->type->name, "gpio")) continue;
        int port = bn_gpio_port(p);
        if (!z_gpio_present() || port >= (int)z_gpio_port_count()) {
            snprintf(status, sizeof(status), "%s: this board has no PMOD port %d", p->name, port);
            continue;
        }
        real_t *rl = &reals[nreals++];
        memset(rl, 0, sizeof(*rl));
        rl->part = p;
        rl->port = port;
        for (int pin = 0; pin < 8; pin++) {
            if (bn_gpio_in_mask(p) >> pin & 1) z_gpio_mode((uint32_t)port, (uint32_t)pin, Z_GPIO_IN);
            if (bn_gpio_out_mask(p) >> pin & 1) {
                z_gpio_mode((uint32_t)port, (uint32_t)pin, Z_GPIO_OD);
                z_gpio_od_write((uint32_t)port, (uint32_t)pin, true);      /* released */
            }
        }
        for (int b = 0; b < bn_nbuses; b++)
            if (bn_buses[b].segment == i) rl->segment = true;
        if (rl->segment) {
            rl->i2c.scl_port = rl->i2c.sda_port = (uint8_t)port;
            rl->i2c.scl_pin = 0;                /* PMOD pin 1 */
            rl->i2c.sda_pin = 1;                /* PMOD pin 2 */
            rl->i2c.khz = 100;
            rl->i2c.timeout_us = 1000;
            if (z_i2c_init(&rl->i2c) == Z_I2C_BUSY) z_i2c_recover(&rl->i2c);
        }
        rl->in_last = 0xFF;
    }
    bn_segment_xfer = real_xfer;
    for (int b = 0; b < bn_nbuses; b++) {
        real_t *rl = bn_buses[b].segment >= 0 ? real_of(&bn_parts[bn_buses[b].segment]) : 0;
        if (!rl) continue;
        for (int i = 0; i < bn_nparts; i++)
            if (bn_parts[i].bus == b && z_i2c_probe(&rl->i2c, bn_parts[i].addr))
                snprintf(status, sizeof(status), "%s: 0x%02x answers on the real pins too",
                         bn_parts[i].name, bn_parts[i].addr);
    }
}

/* every loop: real inputs onto their nets, out= nets onto the pins */
static void real_tick(void) {
    for (int i = 0; i < nreals; i++) {
        real_t *rl = &reals[i];
        uint8_t in = z_gpio_in_get((uint32_t)rl->port) & bn_gpio_in_mask(rl->part);
        if (in != rl->in_last) {
            bn_gpio_inputs(rl->part, in);
            rl->in_last = in;
            dirty = true;
        }
        uint8_t low = bn_gpio_outputs(rl->part), out = bn_gpio_out_mask(rl->part);
        for (int pin = 0; pin < 8; pin++)
            if (out >> pin & 1) z_gpio_od_write((uint32_t)rl->port, (uint32_t)pin, !(low >> pin & 1));
    }
}

/* ---- bench0: the clients' transactions ---- */

static z_port_t *client_of(uint32_t pid) {
    for (int i = 0; i < CLIENTS; i++)
        if (clients[i].connected && clients[i].peer_pid == pid) return &clients[i];
    return 0;
}

static void request(z_port_t *c, const uint8_t *d, uint32_t len) {
    uint8_t reply[ZB_MAX + 2], r[ZB_MAX];
    const char *bus;
    const uint8_t *w;
    uint8_t addr;
    int wn, rn, n = 0;
    if (len >= 1 && d[0] == ZB_LIST) {
        uint8_t names[(BN_BUSES + 8) * BN_NAME + 1];
        for (int i = 0; i < bn_nbuses; i++) {
            size_t k = strlen(bn_buses[i].name) + 1;
            memcpy(names + n, bn_buses[i].name, k);
            n += (int)k;
        }
        for (int i = 0; i < nreals; i++)        /* its ports: zi2cx comes here */
            n += snprintf((char *)names + n, BN_NAME, "pmod%d", reals[i].port) + 1;
        names[n++] = 0;
        z_port_send(c, names, (uint32_t)n);
        return;
    }
    if (len >= 2 && d[0] == ZB_ROLE) {
        char role[ZB_NAME];
        uint32_t k = len - 1 < ZB_NAME - 1 ? len - 1 : ZB_NAME - 1;
        memcpy(role, d + 1, k);
        role[k] = 0;
        int b = bn_bus_role(role);
        const char *nm = b >= 0 ? bn_buses[b].name : "";
        z_port_send(c, nm, (uint32_t)strlen(nm) + 1);
        return;
    }
    if (zb_xfer_decode(d, (int)len, &bus, &addr, &w, &wn, &rn)) {
        reply[0] = ZB_BAD;
        reply[1] = 0;
        z_port_send(c, reply, 2);
        return;
    }
    int b = bn_bus_find(bus);
    for (int i = 0; b < 0 && i < nreals; i++) {
        char nm[BN_NAME];
        snprintf(nm, sizeof(nm), "pmod%d", reals[i].port);
        if (strcmp(nm, bus)) continue;
        /* an owned port, raw: its real pins, if they are a bus */
        int st = reals[i].segment ? real_xfer(reals[i].part, addr, w, wn, r, rn) : BN_NACK_ADDR;
        reply[0] = (uint8_t)(st == BN_ACK ? ZB_ACK : ZB_NACK_ADDR);
        reply[1] = (uint8_t)(st == BN_ACK ? wn : 0);
        n = 2;
        if (st == BN_ACK && rn) {
            memcpy(reply + 2, r, (size_t)rn);
            n += rn;
        }
        z_port_send(c, reply, (uint32_t)n);
        return;
    }
    if (b < 0) {
        reply[0] = ZB_NO_BUS;
        reply[1] = 0;
        z_port_send(c, reply, 2);
        return;
    }
    for (int i = 0; i < bn_nparts; i++) {
        part_t *p = &bn_parts[i];
        if (p->bus != b || p->addr != addr || !p->type->module) continue;
        /* a module: its own process answers, later */
        mod_t *md = 0;
        int k;
        for (int j = 0; j < nmods; j++)
            if (mods[j].part == i && mods[j].port.connected) md = &mods[j];
        for (k = 0; k < PENDING && pending[k].used; k++);
        if (!md || k == PENDING) {
            reply[0] = ZB_NACK_ADDR;
            reply[1] = 0;
            z_port_send(c, reply, 2);
            return;
        }
        uint8_t fwd[4 + ZB_MAX];
        fwd[0] = ZB_SLAVE;
        fwd[1] = (uint8_t)k;
        fwd[2] = (uint8_t)wn;
        memcpy(fwd + 3, w, (size_t)wn);
        fwd[3 + wn] = (uint8_t)rn;
        pending[k].client = c;
        pending[k].rn = (uint8_t)rn;
        pending[k].used = true;
        z_port_send(&md->port, fwd, (uint32_t)(4 + wn));
        bn_xfer(b, addr, w, wn, 0, 0, 0);       /* logged (no hook here: NACK) */
        bn_log[(bn_log_next + BN_LOG - 1) % BN_LOG].status = BN_ACK;
        return;
    }
    int written = 0, st = bn_xfer(b, addr, w, wn, r, rn, &written);
    reply[0] = (uint8_t)(st == BN_ACK ? ZB_ACK : st == BN_NACK_ADDR ? ZB_NACK_ADDR : ZB_NACK_DATA);
    reply[1] = (uint8_t)written;
    n = 2;
    if (st == BN_ACK && rn) {
        memcpy(reply + 2, r, (size_t)rn);
        n += rn;
    }
    z_port_send(c, reply, (uint32_t)n);
    dirty = true;
}

/* ---- modules ---- */

static mod_t *mod_of(uint32_t pid) {
    for (int i = 0; i < nmods; i++)
        if (mods[i].port.connected && mods[i].port.peer_pid == pid) return &mods[i];
    return 0;
}

static z_port_t *mod_port_of(uint32_t pid) {
    mod_t *md = mod_of(pid);
    return md ? &md->port : 0;
}

/* ls99's pins A-G and LED to the part's (an LS10 has A-D and LED) */
static int mod_pin(part_t *p, int i) {
    if (i == ZB_PINS - 1) return p->type->npins - 1;
    return i < p->type->npins - 1 ? i : -1;
}

static void module_connect(z_msg_t *m, const char *who) {
    part_t *p = bn_part_find(who);
    mod_t *md = 0;
    for (int i = 0; i < nmods; i++)
        if (p && mods[i].part == p - bn_parts && !mods[i].port.connected) md = &mods[i];
    if (!md) {
        z_port_refuse(m, "bench: no such module");
        return;
    }
    z_port_accept(&md->port, m, 1);
    memset(md->sent, 0xFF, sizeof(md->sent));
    md->time_sent = 0xFFFFFFFF;
    uint8_t h[4 + ZB_NAME + 64];
    int n = 3;
    const char *local = "", *prog = bn_module_program(p);
    for (int b = 0; b < bn_nbuses; b++)
        if (bn_buses[b].master == p - bn_parts) local = bn_buses[b].name;
    h[0] = ZB_HELLO;
    h[1] = (uint8_t)(p->type->module - 1);
    h[2] = p->addr;
    memcpy(h + n, local, strlen(local) + 1);
    n += (int)strlen(local) + 1;
    memcpy(h + n, prog, strlen(prog) + 1);
    n += (int)strlen(prog) + 1;
    z_port_send(&md->port, h, (uint32_t)n);
}

static void console_add(const char *who, const uint8_t *d, int n) {
    static bool line_start = true;
    for (int i = 0; i < n; i++) {
        if (d[i] == '\r') continue;
        if (consoles_len > (int)sizeof(consoles) - 40) {     /* drop the oldest half */
            int half = consoles_len / 2;
            while (half < consoles_len && consoles[half] != '\n') half++;
            memmove(consoles, consoles + half + 1, (size_t)(consoles_len - half));
            consoles_len -= half + 1;
        }
        if (line_start) {
            consoles_len += snprintf(consoles + consoles_len, 20, "%s: ", who);
            line_start = false;
        }
        consoles[consoles_len++] = (char)d[i];
        if (d[i] == '\n') line_start = true;
    }
    consoles[consoles_len] = 0;
}

static bool module_data(z_msg_t *m) {
    mod_t *md = mod_of(m->from);
    if (!md) return false;
    uint8_t d[ZB_MAX + 8];
    int n = (int)z_blob_len(&m->obj);
    if (n > (int)sizeof(d)) n = sizeof(d);
    memcpy(d, z_blob_data(&m->obj), (size_t)n);
    z_port_send_ack(m);
    part_t *p = &bn_parts[md->part];
    if (d[0] == ZB_DRIVES && n >= 1 + ZB_PINS) {
        for (int i = 0; i < ZB_PINS; i++) {
            int k = mod_pin(p, i);
            if (k >= 0) bn_pin_drive(p, k, d[1 + i]);
        }
        dirty = true;
    } else if (d[0] == ZB_DONE && n >= 3 && d[1] < PENDING && pending[d[1]].used) {
        uint8_t rep[2 + ZB_MAX];
        int rn = pending[d[1]].rn;
        rep[0] = d[2];
        rep[1] = 0;
        if (rn > n - 3) rn = n - 3;
        memcpy(rep + 2, d + 3, (size_t)rn);
        pending[d[1]].used = false;
        if (pending[d[1]].client->connected) z_port_send(pending[d[1]].client, rep, (uint32_t)(2 + rn));
    } else if (d[0] == ZB_CONSOLE) {
        console_add(p->name, d + 1, n - 1);
        if (view_mode == 2) dirty = true;
    } else if (d[0] == ZB_ADDR && n >= 2) {
        p->addr = d[1];
        dirty = true;
    }
    return true;
}

static void stop_modules(void) {
    uint8_t q = ZB_QUIT;
    for (int i = 0; i < nmods; i++)
        if (mods[i].port.connected) {
            z_port_send(&mods[i].port, &q, 1);
            z_port_close(&mods[i].port);
        }
    nmods = 0;
    memset(pending, 0, sizeof(pending));
}

/* one at a time: a launch argument is claimed by the next process */
static void start_modules(void) {
    for (int i = 0; i < bn_nparts && nmods < MODS; i++) {
        if (!bn_parts[i].type->module) continue;
        mod_t *md = &mods[nmods++];
        memset(md, 0, sizeof(*md));
        md->part = i;
        z_launch_arg_set(bn_parts[i].name);
        md->pid = z_proc_run("ls99");
        for (int t = 0; t < 200 && md->pid && !md->port.connected; t++) {
            z_msg_t m;
            while (z_msg_read(&m) == Z_OK) on_msg(&m);
            z_proc_wait(Z_TICK_HZ / 100);
        }
        if (!md->port.connected)
            snprintf(status, sizeof(status), "%s: ls99 did not start", bn_parts[i].name);
    }
}

/* every loop: the clock, and each module's levels when they changed */
static void modules_tick(void) {
    uint32_t t = z_uptime_ticks();
    uint32_t ms = (t - last_ticks) * 1000u / Z_TICK_HZ;
    if (ms) {
        last_ticks += ms * Z_TICK_HZ / 1000u;
        if (!paused) vclock += ms * (uint32_t)speed;
    }
    bn_now = vclock;
    for (int i = 0; i < nmods; i++) {
        mod_t *md = &mods[i];
        if (!md->port.connected) continue;
        part_t *p = &bn_parts[md->part];
        uint8_t lv[1 + ZB_PINS];
        lv[0] = ZB_LEVELS;
        for (int k = 0; k < ZB_PINS; k++) {
            int pin = mod_pin(p, k);
            lv[1 + k] = (uint8_t)(pin < 0 ? BN_FLOAT : bn_pin_get(p, pin));
        }
        if (memcmp(lv + 1, md->sent, ZB_PINS) && z_port_send(&md->port, lv, sizeof(lv)) == Z_OK)
            memcpy(md->sent, lv + 1, ZB_PINS);
        if (md->time_sent != vclock) {
            uint8_t tm[5] = { ZB_TIME, (uint8_t)vclock, (uint8_t)(vclock >> 8),
                              (uint8_t)(vclock >> 16), (uint8_t)(vclock >> 24) };
            if (z_port_send(&md->port, tm, 5) == Z_OK) md->time_sent = vclock;
        }
    }
}

/* ---- messages ---- */

static bool quit, open_wanted;

static void on_msg(z_msg_t *m) {
    z_port_t *c;
    switch (m->subject) {
    case Z_WM_SET_CLIP:
        z_win_apply_clip(&win, &m->obj);
        break;
    case Z_WM_REDRAW:
        /* only ours: a dialog's are handled inside zdialog.c, but this
         * also arrives while one is being created (as sw/apps/text) */
        if (m->obj.type != Z_UINT32 || z_win_redraw_id(m->obj.val.uint32) != win.id) break;
        z_win_apply_redraw(&win, m->obj.val.uint32);
        repaint();
        z_win_redraw_done(&win);
        break;
    case Z_WM_WINDOW_MOVED:
        z_win_parse_rect(&win, &m->obj);
        break;
    case Z_WM_WINDOW_RESIZED:
        z_win_apply_resized(&win, &m->obj);
        layout();
        break;
    case Z_WM_KEY: {
        uint32_t k = Z_WM_UNPACK_KEY_KEYSYM(m->obj.val.uint32);
        if (!Z_WM_UNPACK_KEY_PRESSED(m->obj.val.uint32)) break;
        if (k == Z_KEY_F5) load();
        else if (k == Z_KEY_F6) {
            view_mode = (view_mode + 1) % 3;
            log_follow = true;
            dirty = true;
        } else if (k == Z_KEY_DOWN) scroll_to(scroll_x, scroll_y + SCROLL_STEP);
        else if (k == Z_KEY_UP) scroll_to(scroll_x, scroll_y - SCROLL_STEP);
        else if (k == '1' || k == '2' || k == '3') {
            if (bn_real() && k != '1')
                snprintf(status, sizeof(status), "x1: real hardware on the bench runs in real time");
            else
                speed = k == '1' ? 1 : k == '2' ? 60 : 3600;
            draw_status();
        } else if (k == ' ') {
            paused = !paused;
            draw_status();
        } else if (k == 0x1B) {
            status[0] = 0;                      /* Escape: the status back */
            dirty = true;
        }
        break;
    }
    case Z_WM_WHEEL:
        scroll_to(scroll_x, scroll_y - 3 * SCROLL_STEP * Z_WM_WHEEL_NOTCHES(m->obj.val.uint32));
        break;
    case Z_WM_MOUSE:
        mouse(m->obj.val.uint32);
        break;
    case Z_WM_CLOSE:
        quit = true;
        break;
    case Z_WM_TITLEBAR_ICON:
        if (m->obj.type == Z_UINT32 && (int)Z_WM_UNPACK_TBICON_ID(m->obj.val.uint32) == win.id &&
            Z_WM_UNPACK_TBICON_KIND(m->obj.val.uint32) == Z_WM_TBICON_OPEN)
            open_wanted = true;             /* not from inside a handler */
        break;
    case Z_PORT_CONNECT:
        if (m->obj.type == Z_STR && m->obj.val.str &&
            !strncmp(m->obj.val.str, ZB_MODULE_TAG, strlen(ZB_MODULE_TAG))) {
            module_connect(m, m->obj.val.str + strlen(ZB_MODULE_TAG));
            break;
        }
        for (int i = 0; i < CLIENTS; i++)
            if (!clients[i].connected) {
                z_port_accept(&clients[i], m, 1);
                return;
            }
        z_port_refuse(m, "bench: too many clients");
        break;
    case Z_PORT_DATA:
        if (module_data(m)) break;
        if ((c = client_of(m->from))) {
            uint32_t len = z_blob_len(&m->obj);
            uint8_t req[ZB_NAME + ZB_MAX + 8];
            if (len > sizeof(req)) len = sizeof(req);
            memcpy(req, z_blob_data(&m->obj), len);
            z_port_send_ack(m);
            request(c, req, len);
        }
        break;
    case Z_PORT_DATA_ACK:
        if ((c = client_of(m->from)) || (c = mod_port_of(m->from))) z_port_handle_ack(c, m);
        break;
    case Z_PORT_CLOSE:
        if ((c = client_of(m->from)) || (c = mod_port_of(m->from))) z_port_close(c);
        break;
    default:
        break;
    }
}

/* While the file dialog is up, wm still asks this window to redraw and
 * waits for the ack, and bench0's clients and the modules still need
 * answers: every message goes through on_msg() as usual (zdialog.h). */
static void dialog_msg(z_msg_t *m, void *user) {
    (void)user;
    on_msg(m);
}

static void open_netlist(void) {
    z_dialog_ctx_t dlg = { 0 };
    char chosen[sizeof(path)];
    dlg.parent = &win;
    dlg.on_msg = dialog_msg;
    if (!z_dialog_open(&dlg, "/bench", chosen, sizeof(chosen))) return;
    strcpy(path, chosen);
    load();
}

int main(void) {
    z_launch_path_take(path, sizeof(path));
    if (!z_fb_hw_blit_mem_available()) {
        printf("bench: this bitstream's blitter has no memory copy mode\n");
        return 1;
    }
    if (z_win_create_flags(&win, "bench", WIN_W, WIN_H, -1, -1,
            Z_WIN_FLAG_CLOSE_ICON | Z_WIN_FLAG_OPEN_ICON | Z_WIN_FLAG_RESIZABLE) != Z_OK) {
        printf("bench: no window (is wm running?)\n");
        return 1;
    }
    z_pid_register("bench", name, sizeof(name));
    z_scrollbar_init(&vsb, &win, Z_SB_VERT);
    z_scrollbar_init(&hsb, &win, Z_SB_HORZ);
    layout();
    last_ticks = z_uptime_ticks();
    load();
    repaint();

    uint32_t last_sec = 0;
    while (!quit) {
        z_msg_t m;
        while (z_msg_read(&m) == Z_OK) on_msg(&m);
        if (open_wanted) {
            open_wanted = false;
            open_netlist();
        }
        real_tick();
        modules_tick();
        if (dirty) {
            render();
            dirty = false;
            show();
            z_scrollbar_draw(&vsb, false);
            z_scrollbar_draw(&hsb, false);
        } else if (bn_now / 1000 != last_sec) {
            draw_status();                      /* the clock */
        }
        last_sec = bn_now / 1000;
        if (!quit) z_proc_wait(nmods ? Z_TICK_HZ / 100 : Z_TICK_HZ / 20);
    }
    stop_modules();
    real_release();
    for (int i = 0; i < CLIENTS; i++)
        if (clients[i].connected) z_port_close(&clients[i]);
    z_win_destroy(&win);
    return 0;
}
