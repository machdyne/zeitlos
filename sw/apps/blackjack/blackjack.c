/*
 * Zeitlos blackjack.
 *
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * Six decks by default, dealer stands on all seventeens, blackjack pays
 * 3:2 -- and every one of those is changeable from the command line,
 * because every one of them moves the house edge and the table prints
 * which game you are actually playing.
 *
 * Chips come from /data/casino/bank.dat, shared with every game in the
 * casino. Cards,
 * the shoe and the shuffle come from sw/common/games.
 *
 * -- nothing here has a frame rate --
 *
 * Unlike roulette, this app is entirely event driven: nothing happens
 * until a key or a click arrives. There is no animation to pace and no
 * search to bound, so the whole file is a message pump and a redraw.
 *
 * What it does share with the rest is the rule that matters: an
 * app that stops servicing its queue stalls the WINDOW MANAGER, not
 * just itself, because wm blocks waiting for a redraw acknowledgement.
 * Nothing in this file blocks, which is the easiest way to satisfy
 * that.
 *
 * -- two faces: the desktop and the screen (a dual app) --
 *
 * The desktop face draws the table in one bit in a resizable window.
 * F10, the colour icon or F11 lend the window to the arcade runtime
 * (ar_attach(), sw/common/games/zarcade.h) and the same table is drawn
 * on its 320x240 screen in colour (bj_screen()), full screen with F11.
 * F10, the icon or a resize ask for the desktop face back
 * (AR_EV_DESKTOP). docs/blackjack.md, "In colour";
 * docs/app_kinds.md.
 */

#include <stdio.h>
#include <string.h>

#include "../../common/zeitlos.h"
#include "../../common/zwin.h"
#include "../../common/zwm.h"
#include "../../common/zgfx.h"
#include "../../common/zsoc.h"
#include "../../common/zkbd.h"
#include "../../common/zdialog.h"
#include "../../common/zrng.h"
#include "../../common/games/zrand.h"
#include "../../common/games/zbank.h"
#include "../../common/games/zarcade.h"

#include "bj_board.h"
#include "bj_input.h"

#define WIN_W 320
#define WIN_H 240

static z_win_t     win;
static bj_game_t   game;
static bj_view_t   view;
static bj_layout_t layout_rect;

static bool running = true;
static uint32_t bank_seen;
static bool scr;            /* the screen face: the runtime has the window */
static bool want_desktop;   /* it asked for the desktop face (AR_EV_DESKTOP) */

static void repaint(void);

static void set_str(char *dst, int len, const char *s)
{
    int i = 0;
    while (s[i] && i < len - 1) { dst[i] = s[i]; i++; }
    dst[i] = '\0';
}

static void say(const char *s) { set_str(view.message, BJ_MSG_LEN, s); }

static void say_num(const char *a, int32_t n, const char *b)
{
    char t[12];
    int i = 0, k;
    while (a[i] && i < BJ_MSG_LEN - 1) { view.message[i] = a[i]; i++; }
    bj_num(n, t);
    for (k = 0; t[k] && i < BJ_MSG_LEN - 1; k++) view.message[i++] = t[k];
    for (k = 0; b[k] && i < BJ_MSG_LEN - 1; k++) view.message[i++] = b[k];
    view.message[i] = '\0';
}

/* -- drawing -------------------------------------------------------------- */

/* The screen face's picture, for the runtime: the whole table, cut to
 * what it repaints. */
static void scr_paint(void *user, const ar_rect_t *r)
{
    bj_layout_t t;
    (void)user; (void)r;
    bj_board_layout(&t, &view, 0, 0, AR_W, AR_H);
    bj_board_draw(&t, &view);
}

static char help_text[768];

static ar_desc_t desc = {
    .name = "blackjack",
    .help_title = "Blackjack help",
    .help_text = help_text,
    .help_doc = "blackjack",
    .paint = scr_paint,
    .pointer = true,
};

static void relayout(void)
{
    if (scr) {
        bj_board_layout(&layout_rect, &view, 0, 0, AR_W, AR_H);
    } else {
        z_clip_t c;
        /* Loads this window's visible region as a side effect, which
         * confines every subsequent fill and blit to the part not
         * covered by something in front. */
        z_win_content_rect(&win, &c);
        bj_board_layout(&layout_rect, &view, c.x0, c.y0,
            c.x1 - c.x0 + 1, c.y1 - c.y0 + 1);
    }
}

static void repaint(void)
{
    /* The screen face: the runtime repaints at its next frame. */
    if (scr) {
        ar_dirty_all();
        return;
    }

    relayout();
    bj_board_draw(&layout_rect, &view);
}

/* -- the screen face ------------------------------------------------------- */

static void screen_on(bool full)
{
    if (!scr) {
        scr = true;
        bj_screen(true);
        ar_attach(&win, &desc);
        /* commands are typed: C and M are letters here, not the
         * runtime's colour and music keys */
        ar_text_input(true);
    }
    if (full && !ar_set_fullscreen(true))
        say("this bitstream has no game mode");
    relayout();
    ar_dirty_all();
}

static void screen_off(void)
{
    want_desktop = false;
    if (!scr) return;
    ar_detach();
    bj_screen(false);
    scr = false;
    /* wm's REDRAW, at the desktop size again, repaints the table */
}

/* -- the bank -------------------------------------------------------------- */

static void load_bank(void)
{
    zbank_t b;
    int rv = zbank_load(&b);

    view.chips = b.chips;

    if (rv == ZBANK_CORRUPT) {
        say("bank file damaged -- `buyin` to start over");
        puts("blackjack: " ZBANK_PATH " is damaged; not writing to it");
    }
}

static void settle_bank(void)
{
    int32_t delta = game.returned - game.staked;
    int32_t chips = view.chips;
    int rv;

    /* ONE ADJUSTMENT FOR THE WHOLE ROUND: the net, not a deduction at
     * deal time and a credit at payout.
     *
     * The stake never leaves the bank until the hand is over, so a
     * crash or a window closed mid-hand costs nothing rather than
     * costing the bet. And zbank_adjust() re-reads before it writes, so
     * a win in another game meanwhile is not clobbered. */
    rv = zbank_adjust("blackjack", delta, &chips);

    if (rv == ZBANK_OK) view.chips = chips;
    else { view.chips += delta; say(zbank_strerror(rv)); return; }

    if (delta > 0) say_num("you win ", delta, "");
    else if (delta == 0) say("a push");
    else say_num("the house takes ", -delta, "");
}

static void deal_round(void)
{
    if (view.bet <= 0) view.bet = bj_chip_values[view.chip_sel];

    if (view.bet > view.chips) {
        say("not enough chips -- lower the bet or buy in");
        repaint();
        return;
    }

    if (!bj_round_begin(&game, view.bet)) {
        say("the shoe could not deal");
        repaint();
        return;
    }

    if (game.phase == BJ_PHASE_DONE) settle_bank();
    else if (game.phase == BJ_PHASE_INSURANCE) say("insurance? `i` or `no`");
    else say("hit, stand, double or split");

    repaint();
}

static void do_action(bi_action_t a)
{
    switch (a) {

    case BI_QUIT:
        running = false;
        break;

    case BI_GAME_MODE:
        /* full screen is the screen face's */
        if (!scr) screen_on(true);
        else ar_set_fullscreen(!ar_fullscreen());
        break;

    case BI_NEWGAME: {
        bj_rules_t r = game.rules;
        bj_game_init(&game, &r);
        say_num("new shoe, ", r.ndecks, " decks");
        repaint();
        break;
    }

    case BI_BUYIN: {
        int32_t chips = view.chips;
        int rv = zbank_buyin(&chips);
        if (rv == ZBANK_OK && chips > view.chips) {
            view.chips = chips;
            say_num("bought in for ", chips, " chips");
        } else if (rv != ZBANK_OK) say(zbank_strerror(rv));
        else say("you still have chips -- spend them first");
        repaint();
        break;
    }

    case BI_DEAL:
        deal_round();
        break;

    case BI_ACTED:
        if (game.phase == BJ_PHASE_DONE) settle_bank();
        repaint();
        break;

    case BI_STATUS:
        relayout();
        if (layout_rect.ok) bj_board_draw_status(&layout_rect, &view);
        break;

    case BI_REDRAW:
        repaint();
        break;

    default:
        break;
    }
}

/* -- messages -------------------------------------------------------------- */

/* While the help box is up: keep the table painted (zdialog.h). */
static void help_on_msg(z_msg_t *msg, void *user)
{
    (void)user;
    if (msg->subject == Z_WM_SET_CLIP) {
        z_win_apply_clip(&win, &msg->obj);
    } else if (msg->subject == Z_WM_REDRAW && msg->obj.type == Z_UINT32 &&
               z_win_redraw_id(msg->obj.val.uint32) == win.id) {
        z_win_apply_redraw(&win, msg->obj.val.uint32);
        repaint();
        z_win_redraw_done(&win);
    }
}

/* The help (docs/app_help.md), built from the same lines the message
 * bar shows for an unknown command, so the two never differ: F1's box
 * on the desktop face, the screen's help on the screen face. */
static void build_help(void)
{
    size_t n = 0;
    int i;
    const char *line;
    n = (size_t)snprintf(help_text, sizeof(help_text), "%s\n\n", "Blackjack at a six-deck table: beat the dealer to 21 without going over. Type a command and press Enter, or use the keys.");
    for (i = 0; (line = bj_input_help_line(i)) != 0 && n < sizeof(help_text); i++)
        n += (size_t)snprintf(help_text + n, sizeof(help_text) - n, "%s\n", line);
    if (n < sizeof(help_text))
        snprintf(help_text + n, sizeof(help_text) - n,
            "\nF10: the table in colour, and back.\n"
            "F11 (or `game`): full screen; Esc comes back.");
}

static void show_help(void)
{
    z_dialog_ctx_t ctx = { &win, help_on_msg, NULL };
    z_dialog_help(&ctx, "Blackjack help", help_text, "blackjack");
    repaint();
}

static void handle_key(uint32_t sym, bool pressed)
{
    if (!pressed) return;

    /* The desktop face's: on the screen face these are the runtime's
     * (F10, F11, F1 there; Esc leaves its full screen). */
    if (sym == Z_KEY_FULL_SCREEN) { screen_on(true); return; }
    if (sym == Z_KEY_COLOUR) { screen_on(false); return; }
    if (sym == Z_KEY_HELP) { show_help(); return; }

    do_action(bj_input_key(&view, sym));
}

static void handle_mouse(int x, int y, bool down)
{
    static bool was_down;

    /* On the press edge only. Mouse messages are coalesced and repeat
     * while a button is held, so acting on the level would hit a dozen
     * times per click -- which in this game busts the hand. */
    if (down && !was_down) {
        relayout();
        do_action(bj_input_click(&view, &layout_rect, x, y));
    }

    was_down = down;
}

static void pump(void)
{
    z_msg_t msg;

    /* The screen face: the runtime reads the window's messages. */
    if (scr) {
        ar_event_t e;
        while (ar_poll(&e)) {
            if (e.type == AR_EV_KEY)
                handle_key(e.key, e.pressed);
            else if (e.type == AR_EV_MOUSE)
                handle_mouse(e.x, e.y, (e.buttons & 1) != 0);
            else if (e.type == AR_EV_QUIT)
                running = false;
            else if (e.type == AR_EV_DESKTOP)
                want_desktop = true;
        }
        return;
    }

    while (z_msg_read(&msg) == Z_OK) {

        switch (msg.subject) {

        case Z_WM_KEY:
            if (msg.obj.type == Z_UINT32)
                handle_key(Z_WM_UNPACK_KEY_KEYSYM(msg.obj.val.uint32),
                    Z_WM_UNPACK_KEY_PRESSED(msg.obj.val.uint32) != 0);
            break;

        case Z_WM_MOUSE:
            if (msg.obj.type == Z_UINT32) {
                uint32_t u = msg.obj.val.uint32;
                handle_mouse((int)Z_WM_UNPACK_MOUSE_X(u),
                    (int)Z_WM_UNPACK_MOUSE_Y(u),
                    (Z_WM_UNPACK_MOUSE_BUTTONS(u) & Z_MOUSE_BTN_LEFT) != 0);
            }
            break;

        /* The colour icon: the screen face. */
        case Z_WM_TITLEBAR_ICON:
            if (msg.obj.type == Z_UINT32 &&
                Z_WM_UNPACK_TBICON_KIND(msg.obj.val.uint32) == Z_WM_TBICON_COLOUR)
                screen_on(false);
            break;

        case Z_WM_SET_CLIP:
            if (!z_win_apply_clip(&win, &msg.obj))
                puts("blackjack: bad clip region message");
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
            relayout();
            break;

        case Z_WM_WINDOW_RESIZED:
            if (z_win_apply_resized(&win, &msg.obj)) {
                relayout();
                repaint();
            }
            break;

        case Z_WM_CLOSE:
            running = false;
            break;

        default:
            break;
        }
    }
}

/* -- startup --------------------------------------------------------------- */

static void apply_launch_arg(void)
{
    char arg[Z_WM_ARG_MAX];

    /* Claimed whether or not it is used: leaving one pending would hand
     * it to whatever the person opens next. */
    if (!z_launch_arg_take(arg, sizeof(arg))) return;
    if (!arg[0]) return;

    if (arg[0] >= '1' && arg[0] <= '8' && !arg[1])
        game.rules.ndecks = arg[0] - '0';
    else
        puts("blackjack: ignoring an argument it did not recognise");
}

int main(void)
{
    bj_rules_t rules;

    puts("blackjack: starting");

    /* The real generator: a ChaCha20 stream seeded from rtl/trng.v
     * where the board has one. NOT gated on z_rng_secure() -- zrng.h is
     * explicit that games should use it whatever its provenance and
     * that only keys should refuse. */
    zg_rng_use_system();
    view.trng = z_rng_secure();

    bj_rules_default(&rules);
    bj_game_init(&game, &rules);

    view.g = &game;
    view.chip_sel = 1;
    view.bet = bj_chip_values[1];

    apply_launch_arg();
    bj_game_init(&game, &game.rules);

    /* The screen face's palette and inks, and its flags on the window:
     * game mode (F11 is blackjack's), the colour window and its icon. */
    build_help();
    memcpy(desc.palette, bj_palette, sizeof(desc.palette));
    bj_mono_inks(desc.ink);
    ar_window_colour(true);

    if (z_win_create_flags(&win, "blackjack", WIN_W, WIN_H, -1, -1,
        Z_WIN_FLAG_CLOSE_ICON | Z_WIN_FLAG_CLOSE_KILLS_OWNER |
        Z_WIN_FLAG_RESIZABLE | ar_window_flags(&desc)) != Z_OK) {
        puts("blackjack: failed to create window -- is wm running?");
        return 1;
    }

    bj_board_init();
    load_bank();
    if (view.message[0] == '\0') say("Return to deal -- F1 for help");

    repaint();

    while (running) {
        pump();
        if (!running) break;

        if (want_desktop) {
            screen_off();
            continue;
        }

        /* Nothing here changes on its own, so this waits rather than
         * polls. The timeout exists only so a lost message cannot wedge
         * the app entirely. */

        /* THE BANK CHANGES UNDER THIS WINDOW.
         *
         * sw/apps/casino can hand out a loan while a game is open, and
         * another game can win while this one is idle -- /data/casino/bank.dat is
         * shared. Reading it only at startup meant a loan did not
         * appear until the game was restarted.
         *
         * Once a second: often enough to feel live, rare enough not to
         * be a poll loop, and only when the figure has actually moved. */
        {
            uint32_t now = z_uptime_ticks();
            if ((uint32_t)(now - bank_seen) >= Z_TICK_HZ) {
                zbank_t b;
                bank_seen = now;
                if (zbank_load(&b) == ZBANK_OK && b.chips != view.chips) {
                    view.chips = b.chips;
                    repaint();
                }
            }
        }

        /* The screen face draws and waits a frame at a time; the
         * desktop face sleeps until something arrives. */
        if (scr) ar_frame();
        else z_proc_wait(Z_TICK_HZ);
    }

    screen_off();

    z_win_destroy(&win);
    puts("blackjack: bye");

    return 0;
}
