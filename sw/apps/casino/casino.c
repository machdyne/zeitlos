/*
 * Zeitlos Casino -- the front desk.
 *
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * One window that shows what you are worth, lends you money when you
 * are not worth anything, and starts the four games. One dock icon
 * instead of four.
 *
 * -- there is no free money any more --
 *
 * sw/common/games/zbank.h used to hand out a fresh stack when the bank
 * ran dry. That makes an exactly-computed house edge meaningless: a
 * 5.359% edge costs nothing if losing is undone by asking.
 *
 * Running out means BORROWING now, and the loan is recorded. Borrow
 * 1000 and you owe 1200 -- the vig is charged once, at the counter.
 * Nothing forces repayment; what the debt does is sit in the net-worth
 * figure, which is the number this window leads with. `buyin` in any of
 * the games is now exactly "borrow a starting stack", so none of them
 * changed.
 *
 * -- launching --
 *
 * z_proc_run() takes a name with no path, the same way the dock and
 * sh's `run` do. A game that is already open is not started again --
 * z_proc_list() is asked first, because four copies of blackjack all
 * writing /user/casino.dat is exactly the concurrency zbank_adjust() is
 * careful about, and there is no reason to invite it.
 */

#include <stdio.h>

#include "../../common/zeitlos.h"
#include "../../common/zwin.h"
#include "../../common/zwm.h"
#include "../../common/zgfx.h"
#include "../../common/zsoc.h"
#include "../../common/zkbd.h"
#include "../../common/zfont.h"
#include "../../common/games/zbank.h"

#include "cs_banner.h"

#define WIN_W 320
#define WIN_H 240

#define FONT (&z_font_5x8)
#define FW 5
#define FH 8
#define LINE_H 10

/* One row per game. Tight, because the list grows and the banner does
 * not shrink -- at the old height a fifth game did not fit. */
#define CS_ROW_H 12

#define MSG_LEN 48
#define CMD_LEN 40

/* The games, in the order a person would try them: the one with the
 * best odds first. */
typedef struct {
    const char *proc;      /* what z_proc_run() takes -- no path */
    const char *name;
    const char *blurb;
} cs_game_t;

static const cs_game_t games[] = {
    { "blkjack", "Blackjack", "6 decks, S17, 3:2 -- 0.4% with basic strategy" },
    { "poker",     "Poker",     "hold'em, draw, stud -- against 1 to 7 opponents" },
    { "roulette",  "Roulette",  "american wheel, 5.3% -- `wheel euro` for 2.7%" },
    { "slots",     "Slots",     "3 reels, 5 lines -- 5.4%, and a 1 in 819 jackpot" },
    { "craps",     "Craps",     "the odds behind the line are the only fair bet" }
};

#define NGAMES ((int)(sizeof games / sizeof games[0]))

static z_win_t win;
static bool running = true;

static zbank_t bank;
static int bank_state;
static int sel;

static char message[MSG_LEN];
static char cmd[CMD_LEN];
static int cmd_len;

static z_clip_t content;
static int banner_y, net_y, list_y, help_y, msg_y, cmd_y;
static bool fits;

/* -- text ------------------------------------------------------------------ */

static int slen(const char *s)
{
    int n = 0;
    while (s && s[n]) n++;
    return n;
}

/* Hand-rolled, for the reason docs/app_runtime.md gives: one conversion
 * specifier links picolibc's formatter at a cost of around 100KB. */
static char *num(int32_t n, char *buf)
{
    char tmp[12];
    int i = 0, j = 0;
    bool neg = false;

    if (n < 0) { neg = true; n = -n; }
    if (n == 0) tmp[i++] = '0';
    while (n > 0 && i < 11) { tmp[i++] = (char)('0' + n % 10); n /= 10; }
    if (neg) buf[j++] = '-';
    while (i > 0) buf[j++] = tmp[--i];
    buf[j] = '\0';

    return buf;
}

static void cat(char *b, int len, const char *s)
{
    int n = slen(b);
    while (s && *s && n < len - 1) b[n++] = *s++;
    b[n] = '\0';
}

static void cat_num(char *b, int len, int32_t v)
{
    char t[12];
    cat(b, len, num(v, t));
}

static void say(const char *s)
{
    int i = 0;
    while (s[i] && i < MSG_LEN - 1) { message[i] = s[i]; i++; }
    message[i] = '\0';
}

static void say_num(const char *a, int32_t n, const char *b)
{
    message[0] = '\0';
    cat(message, MSG_LEN, a);
    cat_num(message, MSG_LEN, n);
    cat(message, MSG_LEN, b);
}

/* -- drawing --------------------------------------------------------------- */

static void fill(int x, int y, int w, int h, int c)
{
    int x1 = x + w - 1, y1 = y + h - 1;

    if (x < content.x0) x = content.x0;
    if (y < content.y0) y = content.y0;
    if (x1 > content.x1) x1 = content.x1;
    if (y1 > content.y1) y1 = content.y1;
    if (x1 < x || y1 < y) return;

    z_fb_hw_fill_rect(x, y, x1 - x + 1, y1 - y + 1, c);
}

static void frame(int x, int y, int w, int h, int c)
{
    fill(x, y, w, 1, c); fill(x, y + h - 1, w, 1, c);
    fill(x, y, 1, h, c); fill(x + w - 1, y, 1, h, c);
}

static void text(int x, int y, const char *s, int c)
{
    z_fb_draw_text(x, y, s, c, FONT, &content);
}

static void draw_banner(void)
{
    int bw = CS_BANNER_W, bh = CS_BANNER_H;
    int dx = content.x0 + ((content.x1 - content.x0 + 1) - bw) / 2;
    int sx = 0, sy = 0, dy = banner_y;
    int i, passes;

    if (dx < content.x0) { sx = content.x0 - dx; bw -= sx; dx = content.x0; }
    if (dx + bw - 1 > content.x1) bw = content.x1 - dx + 1;
    if (dy + bh - 1 > content.y1) bh = content.y1 - dy + 1;
    if (bw <= 0 || bh <= 0) return;

    if (!z_fb_hw_blit_mem_available()) {
        int px, py;
        for (py = 0; py < bh; py++)
            for (px = 0; px < bw; px++) {
                int b = (int)((cs_banner[(sy + py) * CS_BANNER_STRIDE_W +
                    ((sx + px) >> 5)] >> ((sx + px) & 31)) & 1u);
                z_fb_set_pixel(dx + px, dy + py, b, &content);
            }
        return;
    }

    /* The blitter's scissor is persistent hardware state and
     * z_fb_hw_blit_mem() sets CTRL_CLIP without programming it, so a
     * blit issued without this inherits whatever rectangle the last
     * unrelated operation left behind. */
    passes = z_gfx_visible_count();
    if (passes < 1) passes = 1;

    for (i = 0; i < passes; i++) {
        z_clip_t c;
        c.x0 = dx; c.y0 = dy; c.x1 = dx + bw - 1; c.y1 = dy + bh - 1;
        if (!z_gfx_blit_scissor(i, &c)) continue;
        z_fb_hw_blit_mem(cs_banner + sy * CS_BANNER_STRIDE_W,
            CS_BANNER_STRIDE, sx, 0, dx, dy, bw, bh);
    }

    z_gfx_blit_scissor_reset();
}

static void layout(void)
{
    int w;

    z_win_content_rect(&win, &content);
    w = content.x1 - content.x0 + 1;

    banner_y = content.y0 + 2;
    net_y = banner_y + CS_BANNER_H + 4;
    list_y = net_y + LINE_H * 2 + 2;

    cmd_y = content.y1 - FH + 1;
    msg_y = cmd_y - LINE_H;
    help_y = msg_y - LINE_H;

    /* NGAMES, not a constant. The banner grew to 100 rows and the list
     * grew by a game, and the two together are most of the window --
     * so whether it fits is arithmetic rather than something to assume. */
    fits = (w >= CS_BANNER_W + 4) &&
        (list_y + NGAMES * CS_ROW_H <= help_y);
}

static void draw_net(void)
{
    char line[64];
    int32_t net = zbank_net(&bank);

    fill(content.x0, net_y, content.x1 - content.x0 + 1, LINE_H * 2, 0);

    line[0] = '\0';
    cat(line, sizeof line, "chips ");
    cat_num(line, sizeof line, bank.chips);
    if (bank.debt > 0) {
        cat(line, sizeof line, "   owed ");
        cat_num(line, sizeof line, bank.debt);
    }
    text(content.x0 + 4, net_y, line, 1);

    /* NET WORTH IS THE HEADLINE. Chips alone flatter anyone who has
     * borrowed, which is everyone who has been here a while -- and it
     * is the only number a loan actually changes. */
    line[0] = '\0';
    cat(line, sizeof line, "worth ");
    cat_num(line, sizeof line, net);
    text(content.x1 - 3 - slen(line) * FW, net_y, line, 1);

    line[0] = '\0';
    if (bank_state == ZBANK_CORRUPT)
        cat(line, sizeof line, "bank file damaged -- `reset` to start over");
    else if (bank.chips <= 0)
        cat(line, sizeof line, "out of chips -- `borrow 1000`");
    else if (bank.debt > 0)
        cat(line, sizeof line, "`repay 500` when you are ahead");
    text(content.x0 + 4, net_y + LINE_H, line, 1);
}

static void draw_list(void)
{
    int i;

    for (i = 0; i < NGAMES; i++) {
        int y = list_y + i * CS_ROW_H;
        bool on = (i == sel);

        fill(content.x0 + 2, y, content.x1 - content.x0 - 3, CS_ROW_H, 0);

        if (on) frame(content.x0 + 2, y, content.x1 - content.x0 - 3,
            CS_ROW_H - 1, 1);

        text(content.x0 + 8, y + 1, games[i].name, 1);
        text(content.x0 + 8 + 11 * FW, y + 1, games[i].blurb, 1);
    }
}

static void draw_status(void)
{
    char line[CMD_LEN + 4];

    fill(content.x0, help_y, content.x1 - content.x0 + 1, FH, 0);
    text(content.x0 + 4, help_y,
        "up/down and Return to play, or type a name", 1);

    fill(content.x0, msg_y, content.x1 - content.x0 + 1, FH, 0);
    text(content.x0 + 4, msg_y, message, 1);

    fill(content.x0, cmd_y, content.x1 - content.x0 + 1, FH, 0);
    line[0] = '\0';
    cat(line, sizeof line, "> ");
    cat(line, sizeof line, cmd);
    cat(line, sizeof line, "_");
    text(content.x0 + 4, cmd_y, line, 1);
}

static void repaint(void)
{
    layout();

    fill(content.x0, content.y0, content.x1 - content.x0 + 1,
        content.y1 - content.y0 + 1, 0);

    if (!fits) {
        text(content.x0 + 2, content.y0 + 2, "window too small", 1);
        return;
    }

    draw_banner();
    draw_net();
    draw_list();
    draw_status();
}

/* -- the bank -------------------------------------------------------------- */

static void reload_bank(void)
{
    bank_state = zbank_load(&bank);

    if (bank_state == ZBANK_CORRUPT)
        puts("casino: /user/casino.dat is damaged; not writing to it");
}

static void do_borrow(int32_t amount)
{
    int32_t chips;
    int rv;

    if (amount <= 0) { say("borrow takes an amount"); return; }

    rv = zbank_borrow(amount, &chips);

    if (rv == ZBANK_REFUSED) {
        /* Says the limit rather than just refusing: "no" without a
         * number is a door with no handle. */
        say_num("no more credit -- the house stops at ", ZBANK_MAX_DEBT, "");
        return;
    }
    if (rv != ZBANK_OK) { say(zbank_strerror(rv)); return; }

    reload_bank();
    say_num("borrowed, and you now owe ", bank.debt, "");
}

static void do_repay(int32_t amount)
{
    int32_t chips, paid;
    int rv = zbank_repay(amount, &chips, &paid);

    if (rv == ZBANK_REFUSED) {
        say(bank.debt > 0 ? "no chips to repay with" : "you owe nothing");
        return;
    }
    if (rv != ZBANK_OK) { say(zbank_strerror(rv)); return; }

    reload_bank();
    if (bank.debt > 0) say_num("paid ", paid, " -- still owing");
    else say("paid off. You owe nothing.");
}

/* -- launching ------------------------------------------------------------- */

static bool already_running(const char *proc)
{
    static z_proc_info_t list[24];
    uint32_t n, trunc = 0, i;

    n = z_proc_list(list, 24, &trunc);

    for (i = 0; i < n; i++) {
        const char *a = list[i].name, *b = proc;
        while (*a && *a == *b) { a++; b++; }
        if (*a == '\0' && *b == '\0') return true;
    }

    return false;
}

static void launch(int i)
{
    uint32_t pid;

    if (i < 0 || i >= NGAMES) return;

    /* A game that is already open is raised by clicking its window, not
     * by starting a second copy. Four blackjacks all writing
     * /user/casino.dat is exactly the concurrency zbank_adjust() is careful
     * about, and there is no reason to invite it. */
    if (already_running(games[i].proc)) {
        message[0] = '\0';
        cat(message, MSG_LEN, games[i].name);
        cat(message, MSG_LEN, " is already open");
        return;
    }

    pid = z_proc_run(games[i].proc);

    if (!pid) {
        /* The likely cause is that the binary is not on the card, which
         * is a build step rather than a bug -- so name it. */
        say("could not start it -- is it on the card?");
        return;
    }

    message[0] = '\0';
    cat(message, MSG_LEN, games[i].name);
    cat(message, MSG_LEN, " -- good luck");
}

/* -- input ----------------------------------------------------------------- */

static bool is_digit(char c) { return c >= '0' && c <= '9'; }

static bool word_is(const char *s, int n, const char *w)
{
    int i;
    for (i = 0; i < n; i++) {
        char c = s[i];
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        if (!w[i] || c != w[i]) return false;
    }
    return w[n] == '\0';
}

static void run_command(const char *line)
{
    const char *p = line;
    int n = 0, i;
    int32_t amount = 0;

    while (*p == ' ') p++;
    while (p[n] && p[n] != ' ') n++;
    if (n == 0) return;

    {
        const char *q = p + n;
        while (*q == ' ') q++;
        if (is_digit(*q)) {
            while (is_digit(*q)) { amount = amount * 10 + (*q - '0'); q++; }
        }
    }

    for (i = 0; i < NGAMES; i++)
        if (word_is(p, n, games[i].proc)) { sel = i; launch(i); return; }

    if (word_is(p, n, "borrow")) { do_borrow(amount ? amount : ZBANK_START); return; }
    if (word_is(p, n, "repay")) { do_repay(amount ? amount : bank.debt); return; }
    if (word_is(p, n, "bank")) {
        say_num("worth ", zbank_net(&bank), " all in");
        return;
    }
    if (word_is(p, n, "reset")) {
        if (zbank_reset() == ZBANK_OK) { reload_bank(); say("started over"); }
        else say("could not write the bank");
        return;
    }
    if (word_is(p, n, "quit") || word_is(p, n, "q")) { running = false; return; }

    say("try a game name, borrow, repay, bank or quit");
}

static void handle_key(uint32_t packed)
{
    uint32_t sym = Z_WM_UNPACK_KEY_KEYSYM(packed);

    if (!Z_WM_UNPACK_KEY_PRESSED(packed)) return;

    if (sym == Z_KEY_UP && cmd_len == 0) {
        if (sel > 0) sel--;
        repaint();
        return;
    }
    if (sym == Z_KEY_DOWN && cmd_len == 0) {
        if (sel < NGAMES - 1) sel++;
        repaint();
        return;
    }

    if (sym == '\r' || sym == '\n') {
        if (cmd_len == 0) { launch(sel); reload_bank(); repaint(); return; }
        {
            char l[CMD_LEN];
            int i;
            for (i = 0; i < cmd_len && i < CMD_LEN - 1; i++) l[i] = cmd[i];
            l[i] = '\0';
            cmd_len = 0;
            cmd[0] = '\0';
            run_command(l);
        }
        repaint();
        return;
    }

    if (sym == 0x08 || sym == 0x7f) {
        if (cmd_len > 0) cmd[--cmd_len] = '\0';
        draw_status();
        return;
    }

    if (sym == 0x1b) { cmd_len = 0; cmd[0] = '\0'; draw_status(); return; }

    if (sym >= 0x20 && sym < 0x7f && cmd_len < CMD_LEN - 1) {
        cmd[cmd_len++] = (char)sym;
        cmd[cmd_len] = '\0';
        /* Two rows of text, not the whole desk. */
        draw_status();
    }
}

static void handle_mouse(uint32_t packed)
{
    static bool was_down;
    bool down = (Z_WM_UNPACK_MOUSE_BUTTONS(packed) & Z_MOUSE_BTN_LEFT) != 0;
    int y = (int)Z_WM_UNPACK_MOUSE_Y(packed);
    int i;

    /* On the press edge only: mouse messages repeat while a button is
     * held, and launching on the level would start a game a dozen
     * times. */
    if (down && !was_down && fits) {
        for (i = 0; i < NGAMES; i++) {
            int y0 = list_y + i * CS_ROW_H;
            if (y >= y0 && y < y0 + CS_ROW_H) {
                sel = i;
                launch(i);
                reload_bank();
                repaint();
                break;
            }
        }
    }

    was_down = down;
}

static void pump(void)
{
    z_msg_t msg;

    while (z_msg_read(&msg) == Z_OK) {

        switch (msg.subject) {

        case Z_WM_KEY:
            if (msg.obj.type == Z_UINT32) handle_key(msg.obj.val.uint32);
            break;

        case Z_WM_MOUSE:
            if (msg.obj.type == Z_UINT32) handle_mouse(msg.obj.val.uint32);
            break;

        case Z_WM_SET_CLIP:
            if (!z_win_apply_clip(&win, &msg.obj))
                puts("casino: bad clip region message");
            break;

        case Z_WM_REDRAW:
            if (msg.obj.type != Z_UINT32) break;
            if (z_win_redraw_id(msg.obj.val.uint32) != win.id) break;
            z_win_apply_redraw(&win, msg.obj.val.uint32);
            repaint();
            z_win_redraw_done(&win);
            break;

        case Z_WM_WINDOW_MOVED:
            z_win_parse_rect(&win, &msg.obj);
            layout();
            break;

        case Z_WM_WINDOW_RESIZED:
            if (z_win_apply_resized(&win, &msg.obj)) repaint();
            break;

        case Z_WM_CLOSE:
            running = false;
            break;

        default:
            break;
        }
    }
}

int main(void)
{
    char arg[Z_WM_ARG_MAX];
    uint32_t last = 0;

    puts("casino: starting");

    /* Claimed whether or not it is used: leaving one pending would hand
     * it to whatever the person opens next -- which here would be one
     * of the games this app launches. */
    if (z_launch_arg_take(arg, sizeof arg) && arg[0])
        puts("casino: ignoring an argument it did not recognise");

    if (z_win_create_flags(&win, "Zeitlos Casino", WIN_W, WIN_H, -1, -1,
        Z_WIN_FLAG_CLOSE_ICON | Z_WIN_FLAG_CLOSE_KILLS_OWNER |
        Z_WIN_FLAG_RESIZABLE) != Z_OK) {
        puts("casino: failed to create window -- is wm running?");
        return 1;
    }

    reload_bank();
    if (message[0] == '\0') say("welcome");

    repaint();

    while (running) {
        pump();
        if (!running) break;

        /* The balance changes while a GAME is running, not while this
         * window is, so it is re-read on a timer rather than only on a
         * keypress. Once a second: often enough to feel live, rare
         * enough that it is not a poll loop. */
        {
            uint32_t now = z_uptime_ticks();
            if (now - last >= Z_TICK_HZ) {
                int32_t was_chips = bank.chips, was_debt = bank.debt;
                last = now;
                reload_bank();
                if (bank.chips != was_chips || bank.debt != was_debt) {
                    layout();
                    if (fits) draw_net();
                }
            }
        }

        z_proc_wait(Z_TICK_HZ / 2);
    }

    z_win_destroy(&win);
    puts("casino: bye");

    return 0;
}
