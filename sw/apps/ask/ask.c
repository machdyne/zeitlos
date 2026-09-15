/*
 * ask -- answers a question from the datasets shipped with the machine
 *
 *   > run wm
 *   > run ask
 *
 * See docs/ask_app.md for what this is and, more importantly, what it
 * is not: it FINDS text, it does not write text. Every character it
 * displays is a byte range of a file on the card, and Enter opens that
 * file in `read` at that offset.
 *
 * -- the window is deliberately narrow --
 *
 * WIN_W is 208, against `read`'s 320. The two are meant to be open at
 * the same time -- you ask here and read there -- and on a 640x480
 * screen there is no room for two wide windows plus the dock. So this
 * one is sized to what a result LIST needs (a title, a heading, a
 * score) rather than to what a passage needs, and the passage itself
 * is `read`'s job.
 *
 * That has a real cost: at 5px per glyph and a 3px margin, a line is
 * 40 characters. Titles are truncated with an ellipsis rather than
 * wrapped, because a wrapped title in a list of eight makes the list
 * unreadable at a glance.
 *
 * -- no printf --
 *
 * Same reasoning as sw/apps/view: printf drags in ~100KB of newlib
 * stdio (docs/app_runtime.md), which is more than this app plus its
 * whole index reader. Numbers are formatted by putu() below, which
 * costs about forty bytes. Diagnostics go out UART0 through dbg().
 *
 * -- staying responsive --
 *
 * A query over 250,000 chunks is tens of millions of MACs and cannot
 * be made instant on a 48MHz core with no data cache. What it CAN be
 * is interruptible, which is what matters: ai_query_step() does
 * AI_SCAN_SLICE vectors and returns, this loop drains the mailbox
 * between slices, the window redraws, the bar moves and Escape is
 * answered. Nothing here ever holds the CPU across a whole query.
 */

#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>     /* exit() only -- no stdio, see the note above */
#include <string.h>

#include "../../common/zeitlos.h"
#include "../../common/zsoc.h"
#include "../../common/zwm.h"
#include "../../common/zwin.h"
#include "../../common/zgfx.h"
#include "../../common/zfont.h"
#include "../../common/zkbd.h"
#include "../../common/zask.h"
#include "../../common/zobj.h"
#include "../../common/zmsg.h"

#include "aidx.h"

/* Narrow on purpose -- see the header comment. 208 wide leaves room
 * for `read` at 320 plus the dock on a 640-wide screen.
 *
 * The height is COMPUTED from what it has to hold, not picked: the
 * query line, the rule, the status line, eight two-line results and
 * the pack buttons. It was 300 and half of that was empty, which on a
 * 480-tall screen is a third of the vertical space taken for nothing
 * while `read` sits next to it wanting all it can get. */
#define WIN_W       208
#define WIN_H       (Y_LIST_C + Z_ASK_HITS_DEFAULT * ROW_H_C + BTN_H_C + 4)

#define MARGIN      3
#define FONT        (&z_font_5x8)
#define LINE_H      8

static z_win_t win;

/* -- query state -- */
static char     query[AI_QUERY_MAX];
static int      qlen;
static bool     query_dirty = true;     /* Enter searches vs opens */

static ai_query_t Q;
static bool     running;
static int      sel;
static bool     loaded;

static char     status[48] = "loading...";

/* ------------------------------------------------------------------ *
 * Tiny formatting -- see the no-printf note above
 * ------------------------------------------------------------------ */

static int scat(char *d, int cap, int at, const char *s)
{
    while (*s && at < cap - 1) d[at++] = *s++;
    d[at] = 0;
    return at;
}

static int putu(char *d, int cap, int at, uint32_t v)
{
    char t[12];
    int n = 0;
    if (!v) t[n++] = '0';
    while (v) { t[n++] = (char)('0' + (v % 10)); v /= 10; }
    while (n && at < cap - 1) d[at++] = t[--n];
    d[at] = 0;
    return at;
}

/* Straight out UART0. sw/apps/view/view.c does the same and for the
 * same reason: this is about eighty bytes against printf's hundred
 * kilobytes. */
static void dbg_putc(char c)
{
    while (!(reg_uart0_lsr & 0x20));
    reg_uart0_data = (uint8_t)c;
}

static void dbg(const char *s)
{
    while (*s) {
        if (*s == '\n') dbg_putc('\r');
        dbg_putc(*s++);
    }
}

/* Copies at most `cols` glyphs, marking truncation. A list of eight
 * truncated titles reads; a list of eight wrapped ones does not. */
static void fit(char *dst, int cap, const char *src, int cols)
{
    int i = 0;
    if (cols > cap - 1) cols = cap - 1;
    while (src[i] && i < cols) { dst[i] = src[i]; i++; }
    if (src[i] && cols >= 2) { dst[cols - 1] = '~'; }
    dst[i] = 0;
    if (src[i] && cols >= 2) { dst[cols - 1] = '~'; dst[cols] = 0; }
}

/* ------------------------------------------------------------------ *
 * Drawing
 * ------------------------------------------------------------------ */

static int cols(void)
{
    int w = z_win_content_w(&win) - 2 * MARGIN;
    return w / FONT->w;
}

/* The same layout numbers as constants, so WIN_H can be computed from
 * them at compile time. LINE_H and MARGIN are already constants; these
 * exist because the Y_* macros below read z_win_content_h(). */
#define ROW_H_C   (2 * LINE_H + 2)
#define BTN_H_C   (LINE_H + 7)   /* 8px font + 1px border top and bottom + clearance */
#define Y_LIST_C  (MARGIN + LINE_H + 2 + 3 + LINE_H + 2)

#define Y_QUERY   MARGIN
#define Y_RULE    (Y_QUERY + LINE_H + 2)
#define Y_STATUS  (Y_RULE + 3)
#define Y_LIST    (Y_STATUS + LINE_H + 2)

/* A row of pack buttons along the bottom, one per installed pack,
 * each opening that pack's /ark/<pack>/index.md in `read`.
 *
 * Worth the pixels because there is otherwise NO convenient way to
 * reach those indexes. The corpus is numbered files (8.3, FF_USE_LFN
 * 0), so `files` shows a wall of digits, and the index.md that makes
 * them browsable is itself just another file in a directory nobody
 * has a reason to open. `ask` is the app that knows the packs exist. */
#define BTN_H     BTN_H_C
#define Y_BUTTONS (z_win_content_h(&win) - BTN_H)

static void draw_query(void)
{
    char line[64];
    int n;
    z_win_fill_rect(&win, 0, Y_QUERY, win.w, LINE_H, 0);
    n = scat(line, sizeof(line), 0, "> ");
    n = scat(line, sizeof(line), n, query);
    /* a block cursor, so it is obvious where typing goes */
    if (n < (int)sizeof(line) - 1) { line[n++] = '_'; line[n] = 0; }
    {
        char shown[64];
        int c = cols();
        int len = (int)strlen(line);
        /* scroll the query left once it runs past the window rather
         * than truncating the END -- the end is where the cursor is,
         * and a cursor you cannot see is worse than a start you
         * cannot see */
        fit(shown, sizeof(shown), len > c ? line + (len - c) : line, c);
        z_win_draw_text(&win, MARGIN, Y_QUERY, shown, 1, FONT);
    }
}

static void draw_status(void)
{
    char shown[64];
    z_win_fill_rect(&win, 0, Y_STATUS, win.w, LINE_H, 0);
    fit(shown, sizeof(shown), status, cols());
    z_win_draw_text(&win, MARGIN, Y_STATUS, shown, 1, FONT);
}

/* A result is two lines: title, then heading and score. Two rather
 * than one because at 40 columns a title alone already fills the row,
 * and the heading is what distinguishes two hits from the same
 * document -- which is the common case in a long manual. */
#define ROW_H  ROW_H_C

static void draw_list(void)
{
    int y = Y_LIST;
    int h = z_win_content_h(&win) - BTN_H - 2;
    int i;
    int c = cols();

    z_win_fill_rect(&win, 0, Y_LIST, win.w, h - Y_LIST, 0);

    for (i = 0; i < Q.nhits && y + ROW_H <= h; i++) {
        const ai_hit_t *hit = &Q.hits[i];
        char line[72], shown[72];
        int n;
        bool on = (i == sel);

        if (on) z_win_fill_rect(&win, 0, y - 1, win.w, ROW_H, 1);

        /* z_win_draw_text() paints a SOLID CELL and sets the
         * background to 0 unconditionally (zwin.h). Drawing the
         * selected row with colour 0 therefore gave ink of 0 on a cell
         * of 0 -- not invisible text, but a cell that ERASED the
         * highlight it was drawn over. That is the white and black
         * bars with no letters.
         *
         * z_win_draw_text2() takes both colours and is what to use
         * whenever the background under the text is not the window
         * background. */
        n = putu(line, sizeof(line), 0, (uint32_t)(i + 1));
        n = scat(line, sizeof(line), n, ". ");
        n = scat(line, sizeof(line), n, hit->title);
        fit(shown, sizeof(shown), line, c);
        if (on) z_win_draw_text2(&win, MARGIN, y, shown, 0, 1, FONT);
        else    z_win_draw_text(&win, MARGIN, y, shown, 1, FONT);

        n = scat(line, sizeof(line), 0, "   ");
        n = scat(line, sizeof(line), n, hit->head[0] ? hit->head : "-");
        fit(shown, sizeof(shown), line, c - 5);
        if (on) z_win_draw_text2(&win, MARGIN, y + LINE_H, shown, 0, 1, FONT);
        else    z_win_draw_text(&win, MARGIN, y + LINE_H, shown, 1, FONT);

        /* the score, right-aligned on the second line.
         *
         * Shown ALWAYS, and that is the point. This program's worst
         * failure is not "no result", it is a confident wrong result
         * that looks exactly like a right one. A reader who can see
         * that the best hit scored 310 is being told the truth. */
        n = putu(line, sizeof(line), 0, (uint32_t)hit->score);
        {
            int sx = MARGIN + (c - (int)strlen(line)) * FONT->w;
            if (on) z_win_draw_text2(&win, sx, y + LINE_H, line, 0, 1, FONT);
            else    z_win_draw_text(&win, sx, y + LINE_H, line, 1, FONT);
        }

        y += ROW_H;
    }

    if (!Q.nhits && !running && !query_dirty) {
        z_win_draw_text(&win, MARGIN, Y_LIST,
                        "No passage here answers", 1, FONT);
        z_win_draw_text(&win, MARGIN, Y_LIST + LINE_H,
                        "that.", 1, FONT);
    }
}

/* Left edge and width of pack button `i`, splitting the width evenly.
 * Returns false if it would be too narrow to label. */
static bool btn_rect(int i, int *x, int *w)
{
    int n = ai_pack_count();
    int avail = z_win_content_w(&win) - 2 * MARGIN;
    if (n <= 0) return false;
    *w = avail / n;
    if (*w < 4 * FONT->w) return false;
    *x = MARGIN + i * (*w);
    return true;
}

static void draw_buttons(void)
{
    int i, x, w;
    int y = Y_BUTTONS;

    z_win_fill_rect(&win, 0, y - 1, win.w, BTN_H + 1, 0);
    if (!ai_pack_count()) return;
    z_win_fill_rect(&win, 0, y - 1, win.w, 1, 1);

    for (i = 0; i < ai_pack_count(); i++) {
        char shown[20];
        int bw;
        if (!btn_rect(i, &x, &w)) break;
        bw = w - 2;
        /* Four fills rather than z_win_hw_box() -- window-relative,
         * see draw_all(). zwidget.c draws its own icons in software
         * for the same reason and says so. */
        z_win_fill_rect(&win, x, y + 1, bw, 1, 1);
        z_win_fill_rect(&win, x, y + BTN_H - 2, bw, 1, 1);
        z_win_fill_rect(&win, x, y + 1, 1, BTN_H - 2, 1);
        z_win_fill_rect(&win, x + bw - 1, y + 1, 1, BTN_H - 2, 1);
        fit(shown, sizeof(shown), ai_pack(i)->name, (bw - 6) / FONT->w);
        z_win_draw_text(&win, x + 3, y + 3, shown, 1, FONT);
    }
}

static void open_pack_index(int i)
{
    const ai_pack_t *p = ai_pack(i);
    char arg[AI_PATH_MAX];
    int n;
    if (!p) return;
    /* /ark/<pack>/index.md -- written by tools/ask at build time, and
     * `read` follows its relative links into the corpus. */
    n = scat(arg, sizeof(arg), 0, "/ark/");
    n = scat(arg, sizeof(arg), n, p->name);
    scat(arg, sizeof(arg), n, "/index.md");
    z_launch_arg_set(arg);
    if (!z_proc_run("read")) {
        scat(status, sizeof(status), 0, "could not start read");
        draw_status();
    }
}

static void draw_all(void)
{
    z_win_clear(&win);
    draw_query();
    /* z_win_fill_rect(), NOT z_win_hw_line().
     *
     * zwin.h is explicit and it cost a shipped bug: the hardware
     * rasterizer entry points take ABSOLUTE SCREEN COORDINATES, while
     * z_win_fill_rect() and z_win_draw_text() are window-relative.
     * Passing window-relative coordinates to them draws everything
     * offset by the window's position on screen -- which for the pack
     * buttons put every label exactly the content inset below its own
     * box. sw/common/tests/zrender.h lists this as the third of three
     * bugs it was written to catch, and it caught this one in one
     * look. */
    z_win_fill_rect(&win, 0, Y_RULE, win.w, 1, 1);
    draw_status();
    draw_list();
    draw_buttons();
}

/* ------------------------------------------------------------------ *
 * Status lines
 * ------------------------------------------------------------------ */

static void status_idle(void)
{
    int n, i;
    uint32_t chunks = 0;
    for (i = 0; i < ai_pack_count(); i++) chunks += ai_pack(i)->nchunks;
    n = putu(status, sizeof(status), 0, chunks);
    n = scat(status, sizeof(status), n, " passages, ");
    n = putu(status, sizeof(status), n, (uint32_t)ai_pack_count());
    n = scat(status, sizeof(status), n, " pack");
    if (ai_pack_count() != 1) n = scat(status, sizeof(status), n, "s");
    /* The line-editing keys are not discoverable otherwise, and
     * backspacing through a whole question to ask the next one is
     * the first thing anybody hits. */
    scat(status, sizeof(status), n, "  ^U clears");
}

static void status_progress(void)
{
    int n;
    if (!Q.total) {
        /* No coarse scan to measure -- a lexical-only pack does a
         * bounded amount of card I/O per query term instead. A
         * percentage of zero work is worse than no percentage. */
        scat(status, sizeof(status), 0, "searching...  ESC cancels");
        return;
    }
    n = scat(status, sizeof(status), 0, "searching ");
    n = putu(status, sizeof(status), n, (100u * Q.scanned) / Q.total);
    scat(status, sizeof(status), n, "%  ESC cancels");
}

static void load_progress(uint32_t done, uint32_t total, const char *what)
{
    static uint32_t last_pct = 101;
    uint32_t pct = total ? (100u * done) / total : 0;
    int n;
    if (pct == last_pct) return;
    last_pct = pct;
    n = scat(status, sizeof(status), 0, "loading ");
    n = scat(status, sizeof(status), n, what);
    n = scat(status, sizeof(status), n, " ");
    n = putu(status, sizeof(status), n, pct);
    scat(status, sizeof(status), n, "%");
    draw_status();
}

/* ------------------------------------------------------------------ *
 * Actions
 * ------------------------------------------------------------------ */

static void start_query(void)
{
    if (!loaded || !qlen) return;
    if (!ai_query_begin(&Q, query, 8)) return;
    running = true;
    sel = 0;
    query_dirty = false;
    status_progress();
    draw_status();
    draw_list();
}

static void open_selected(void)
{
    char arg[AI_PATH_MAX];
    if (sel < 0 || sel >= Q.nhits) return;
    if (!ai_launch_arg(&Q.hits[sel], arg, sizeof(arg))) {
        scat(status, sizeof(status), 0, "path too long to open");
        draw_status();
        return;
    }
    /* Set the argument BEFORE running: the new process can reach its
     * own startup before this one runs again, and it claims the
     * argument there (zwm.h, Z_WM_SET_ARG). */
    z_launch_arg_set(arg);
    if (!z_proc_run("read")) {
        scat(status, sizeof(status), 0, "could not start read");
        draw_status();
    }
}

/* ------------------------------------------------------------------ *
 * Input
 * ------------------------------------------------------------------ */

static void handle_key(uint32_t packed)
{
    uint32_t k = Z_WM_UNPACK_KEY_KEYSYM(packed);
    uint8_t mods = (uint8_t)Z_WM_UNPACK_KEY_MODIFIERS(packed);

    if (!Z_WM_UNPACK_KEY_PRESSED(packed)) return;

    if (mods & Z_KBD_MOD_CTRL) {
        if (k == 0x11) {                        /* Ctrl+Q */
            /* Release the query's file handle before exiting. The
             * kernel sweeps abandoned handles now (fsapi.c's
             * k_fs_release_all), but an app should not rely on that
             * to tidy up after itself on a path it controls. */
            ai_query_cancel(&Q);
            ai_free_all();
            z_win_destroy(&win);
            exit(0);
        }
        if (k == 0x15) {                        /* Ctrl+U -- clear line */
            qlen = 0;
            query[0] = 0;
            query_dirty = true;
            draw_query();
        } else if (k == 0x17) {                 /* Ctrl+W -- delete word */
            while (qlen && query[qlen - 1] == ' ') query[--qlen] = 0;
            while (qlen && query[qlen - 1] != ' ') query[--qlen] = 0;
            query_dirty = true;
            draw_query();
        }
        return;
    }

    switch (k) {

    case 0x1b:                                  /* Escape */
        if (running) {
            /* Cancellation is immediate: the scan holds no lock and
             * allocates nothing per slice, so this is one flag. */
            ai_query_cancel(&Q);
            running = false;
            scat(status, sizeof(status), 0, "cancelled");
            draw_status();
        } else {
            qlen = 0;
            query[0] = 0;
            query_dirty = true;
            Q.nhits = 0;
            status_idle();
            draw_all();
        }
        return;

    case 0x0d:                                  /* Enter */
        /* Search if the query changed since the last one, otherwise
         * open what is selected. That is what makes one key do the
         * obvious thing at both ends of the loop: type, Enter, arrow,
         * Enter. */
        if (query_dirty) start_query();
        else open_selected();
        return;

    case 0x7f:                                  /* Backspace */
        if (qlen) { query[--qlen] = 0; query_dirty = true; draw_query(); }
        return;

    case Z_KEY_UP:
        if (sel > 0) { sel--; draw_list(); }
        return;

    case Z_KEY_DOWN:
        if (sel + 1 < Q.nhits) { sel++; draw_list(); }
        return;

    default: break;
    }

    /* 1-9 jump straight to a result, which is faster than arrowing and
     * is what the numbers in the list are for. */
    if (k >= '1' && k <= '9' && !query_dirty && Q.nhits) {
        int i = (int)(k - '1');
        if (i < Q.nhits) { sel = i; draw_list(); open_selected(); return; }
    }

    if (k >= 0x20 && k < 0x7f && qlen < AI_QUERY_MAX - 1) {
        query[qlen++] = (char)k;
        query[qlen] = 0;
        query_dirty = true;
        draw_query();
    }
}

/* ------------------------------------------------------------------ *
 * Mouse
 *
 * A click on a result selects it AND opens it, which is what a click
 * on a search result means everywhere else. Keyboard selection stays
 * two steps (arrow, then Enter) because arrowing through a list is
 * browsing; clicking one is choosing it.
 *
 * Acted on RELEASE inside the same row the press started in, so
 * dragging off a result cancels -- the same rule z_widget.h's buttons
 * follow.
 * ------------------------------------------------------------------ */

static uint8_t last_buttons;
static int press_row = -1;
static int press_btn = -1;

static int row_at(int cy)
{
    int y = Y_LIST;
    int i;
    int h = z_win_content_h(&win) - BTN_H - 2;
    for (i = 0; i < Q.nhits && y + ROW_H <= h; i++) {
        if (cy >= y - 1 && cy < y - 1 + ROW_H) return i;
        y += ROW_H;
    }
    return -1;
}

static int button_at(int cx, int cy)
{
    int i, x, w;
    if (cy < Y_BUTTONS) return -1;
    for (i = 0; i < ai_pack_count(); i++) {
        if (!btn_rect(i, &x, &w)) break;
        if (cx >= x && cx < x + w) return i;
    }
    return -1;
}

static void handle_mouse(uint32_t packed)
{
    int cx, cy;
    bool inside = z_win_mouse_content_xy(&win, packed, &cx, &cy);
    uint8_t buttons = (uint8_t)Z_WM_UNPACK_MOUSE_BUTTONS(packed);
    bool down = (buttons & Z_MOUSE_BTN_LEFT) != 0;
    bool was = (last_buttons & Z_MOUSE_BTN_LEFT) != 0;

    last_buttons = buttons;

    if (!inside) {
        if (!down) { press_row = -1; press_btn = -1; }
        return;
    }

    if (down && !was) {
        press_row = row_at(cy);
        press_btn = button_at(cx, cy);
        if (press_row >= 0 && press_row != sel) {
            sel = press_row;
            draw_list();
        }
        return;
    }

    if (!down && was) {
        int r = row_at(cy);
        int b = button_at(cx, cy);
        if (press_btn >= 0 && b == press_btn) {
            open_pack_index(b);
        } else if (press_row >= 0 && r == press_row) {
            sel = r;
            draw_list();
            open_selected();
        }
        press_row = -1;
        press_btn = -1;
    }
}

/* ------------------------------------------------------------------ *
 * The ask service (zask.h)
 *
 * Only Z_ASK_INFO is answered in this build. Z_ASK_QUERY needs the
 * query state machine to be per-requester rather than the single
 * global one the window uses, and doing that before the window path
 * is proven on hardware would mean debugging two things at once.
 * The protocol in zask.h is the contract; this is the first half of
 * implementing it.
 * ------------------------------------------------------------------ */

static void handle_service(z_msg_t *msg)
{
    if (msg->subject != Z_ASK_INFO) return;

    {
        z_obj_t reply = z_obj_map(8);
        const ai_pack_t *p = ai_pack(0);
        uint32_t chunks = 0;
        int i;
        for (i = 0; i < ai_pack_count(); i++) chunks += ai_pack(i)->nchunks;
        z_map_set(&reply, "ok", z_obj_uint32(loaded ? 1u : 0u));
        z_map_set(&reply, "name", z_obj_str(p ? p->name : ""));
        z_map_set(&reply, "dsid", z_obj_uint32(p ? p->dsid : 0u));
        z_map_set(&reply, "encoder_id", z_obj_uint32(p ? p->encoder_id : 0u));
        z_map_set(&reply, "packs", z_obj_uint32((uint32_t)ai_pack_count()));
        z_map_set(&reply, "nchunks", z_obj_uint32(chunks));
        z_map_set(&reply, "resident", z_obj_uint32(ai_resident()));
        /* Reports what is actually happening, not what the bitstream
         * has. There is no accelerator yet, so this is honestly 0. */
        z_map_set(&reply, "accel", z_obj_uint32(0u));
        z_msg_new_send(msg->from, Z_ASK_INFO_REPLY, msg->tag, reply);
    }
}

/* ------------------------------------------------------------------ *
 * Main
 * ------------------------------------------------------------------ */

static bool drain(void)
{
    z_msg_t msg;
    bool redraw = false;

    while (z_msg_read(&msg) == Z_OK) {

        if (msg.subject == Z_WM_SET_CLIP) {
            z_win_apply_clip(&win, &msg.obj);
            continue;
        }
        if (msg.subject == Z_WM_REDRAW) {
            z_win_apply_redraw(&win, msg.obj.val.uint32);
            redraw = true;
        } else if (msg.subject == Z_WM_WINDOW_MOVED) {
            z_win_parse_rect(&win, &msg.obj);
        } else if (msg.subject == Z_WM_KEY) {
            handle_key(msg.obj.val.uint32);
        } else if (msg.subject == Z_WM_MOUSE) {
            handle_mouse(msg.obj.val.uint32);
        } else {
            handle_service(&msg);
        }
    }

    if (redraw) {
        draw_all();
        z_win_redraw_done(&win);
    }
    return redraw;
}

int main(void)
{
    char regname[32];

    if (z_win_create_flags(&win, "ask", WIN_W, WIN_H, -1, -1,
        Z_WIN_FLAG_CLOSE_ICON | Z_WIN_FLAG_CLOSE_KILLS_OWNER) != Z_OK) {
        dbg("ask: no window\n");
        return 1;
    }

    /* Registered by name, not by a fixed pid -- zweb.h's reasoning. */
    z_pid_register("ask", regname, sizeof(regname));

    draw_all();

    /* Loading is 1-2.5MB off the card at a realistic 300-600 KB/s, so
     * it is seconds and the bar is not decoration. load_progress()
     * redraws the status line as it goes; the window is already up, so
     * the app looks alive from the first frame rather than after. */
    loaded = ai_load_all("/ask", load_progress) > 0;

    if (!loaded) {
        /* Say WHY. The four realistic causes -- nothing in /ask, an
         * index this build cannot read, files from two distributions
         * mixed, and malloc refusing the coarse array because
         * sw/os/kernel.h never got the HUGE tier entry -- are not
         * distinguishable from "no packs", and the last one is by far
         * the most likely on a first run. */
        scat(status, sizeof(status), 0, ai_error());
        dbg("ask: no packs loaded: ");
        dbg(ai_error());
        dbg("\n");
        draw_all();
    } else {
        status_idle();
        if (ai_failed_count()) {
            /* Loaded something, but not everything. Say so on the
             * status line rather than showing a healthy-looking
             * passage count over a corpus that quietly shrank. */
            int n = scat(status, sizeof(status), 0, ai_error());
            n = scat(status, sizeof(status), n, " (");
            n = putu(status, sizeof(status), n,
                     (uint32_t)ai_failed_count());
            scat(status, sizeof(status), n, " pack failed)");
        }
        dbg("ask: loaded ");
        dbg(status);
        dbg("\n");
        draw_all();
    }

    for (;;) {

        drain();

        if (running) {
            /* One slice, then straight back to the mailbox. This is
             * the whole responsiveness contract: the window redraws,
             * the bar moves and Escape is answered WHILE a query over
             * a quarter of a million passages is in flight. */
            if (ai_query_step(&Q)) {
                running = false;
                if (Q.phase == AI_Q_DONE) {
                    sel = 0;
                    status_idle();
                }
                draw_status();
                draw_list();
            } else {
                status_progress();
                draw_status();
            }
            continue;       /* no wait while working */
        }

        /* Idle: wait rather than spin. A process that spins takes a
         * full scheduler share from whatever is in the foreground --
         * docs/app_runtime.md, "Every app now yields". */
        z_proc_wait(Z_TICK_HZ / 20);
    }
}
