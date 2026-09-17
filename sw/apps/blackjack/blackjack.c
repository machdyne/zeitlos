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
 * Chips come from /USER/casino.dat, shared with poker and roulette. Cards,
 * the shoe and the shuffle come from sw/common/games.
 *
 * -- nothing here has a frame rate --
 *
 * Unlike roulette, this app is entirely event driven: nothing happens
 * until a key or a click arrives. There is no animation to pace and no
 * search to bound, so the whole file is a message pump and a redraw.
 *
 * What it does share with the other two is the rule that matters: an
 * app that stops servicing its queue stalls the WINDOW MANAGER, not
 * just itself, because wm blocks waiting for a redraw acknowledgement.
 * Nothing in this file blocks, which is the easiest way to satisfy
 * that.
 */

#include <stdio.h>

#include "../../common/zeitlos.h"
#include "../../common/zwin.h"
#include "../../common/zwm.h"
#include "../../common/zgfx.h"
#include "../../common/zsoc.h"
#include "../../common/zkbd.h"
#include "../../common/zrng.h"
#include "../../common/games/zrand.h"
#include "../../common/games/zbank.h"

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
static bool game_mode;
static bool exit_armed;

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

static void relayout(void)
{
    if (game_mode) {
        bj_board_layout(&layout_rect, &view, 0, 0,
            Z_GAME_VIEW_W, Z_GAME_VIEW_H);
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
    if (game_mode) {
        /* Nothing is in front of a game-mode page, so wm's clip region
         * has to be out of the way -- left in place it would clip
         * drawing to wherever this app's window sits on the desktop
         * behind. */
        z_gfx_clear_visible();
        z_gfx_blit_scissor_reset();
    }

    relayout();
    bj_board_draw(&layout_rect, &view);
}

static void enter_game_mode(void)
{
    if (!z_game_available()) {
        say("this bitstream has no game mode");
        puts("blackjack: no game mode -- rebuild the gateware with `GAME "
            "in rtl/boards.vh");
        return;
    }

    game_mode = true;
    exit_armed = false;

    z_gfx_clear_visible();
    z_gfx_blit_scissor_reset();
    z_fb_hw_fill_rect(0, 0, 640, 480, 0);

    z_game_set_enabled(true, false);
    z_game_set_view(0, 0);

    repaint();
}

static void leave_game_mode(void)
{
    uint32_t wm_pid;

    z_game_set_enabled(false, false);
    game_mode = false;

    /* Every window is still alive and still where it was, and this app
     * just drew over all of their pixels without any of them knowing.
     * wm repairs damage it caused itself; this came from outside it. */
    if (z_pid_lookup("wm0", &wm_pid))
        z_msg_new_send(wm_pid, Z_WM_REPAINT, 0, z_obj_uint32(0));

    repaint();
}

/* -- the bank -------------------------------------------------------------- */

static void load_bank(void)
{
    zbank_t b;
    int rv = zbank_load(&b);

    view.chips = b.chips;

    if (rv == ZBANK_CORRUPT) {
        say("bank file damaged -- `buyin` to start over");
        puts("blackjack: /USER/casino.dat is damaged; not writing to it");
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
        if (game_mode) leave_game_mode();
        else enter_game_mode();
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

static void handle_key(uint32_t packed)
{
    uint32_t sym = Z_WM_UNPACK_KEY_KEYSYM(packed);
    bool pressed = Z_WM_UNPACK_KEY_PRESSED(packed) != 0;

    if (game_mode) {
        /* Escape leaves full screen, but only once RELEASED. Entering
         * game mode by any route that leaves a key still held would
         * otherwise see the key-up for a key that was already down and
         * leave immediately. */
        if (sym == 0x1b) {
            if (!pressed) exit_armed = true;
            else if (exit_armed) leave_game_mode();
            return;
        }
    }

    if (!pressed) return;

    if (sym == Z_KEY_F2) { do_action(BI_GAME_MODE); return; }

    if (sym == Z_KEY_F1) {
        static int hl;
        const char *s = bj_input_help_line(hl++);
        if (!s) { hl = 0; s = bj_input_help_line(hl++); }
        say(s);
        repaint();
        return;
    }

    do_action(bj_input_key(&view, sym));
}

static void handle_mouse(uint32_t packed)
{
    static bool was_down;
    bool down = (Z_WM_UNPACK_MOUSE_BUTTONS(packed) & Z_MOUSE_BTN_LEFT) != 0;
    int x = (int)Z_WM_UNPACK_MOUSE_X(packed);
    int y = (int)Z_WM_UNPACK_MOUSE_Y(packed);

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

    while (z_msg_read(&msg) == Z_OK) {

        switch (msg.subject) {

        case Z_WM_KEY:
            if (msg.obj.type == Z_UINT32) handle_key(msg.obj.val.uint32);
            break;

        case Z_WM_MOUSE:
            if (!game_mode && msg.obj.type == Z_UINT32)
                handle_mouse(msg.obj.val.uint32);
            break;

        case Z_WM_SET_CLIP:
            if (!z_win_apply_clip(&win, &msg.obj))
                puts("blackjack: bad clip region message");
            break;

        case Z_WM_REDRAW:
            if (msg.obj.type != Z_UINT32) break;
            if (z_win_redraw_id(msg.obj.val.uint32) != win.id) break;
            z_win_apply_redraw(&win, msg.obj.val.uint32);
            /* Answered even in game mode. The window is still a window
             * as far as wm is concerned, and an unanswered redraw is
             * what stalls the desktop. */
            if (!game_mode) repaint();
            z_win_redraw_done(&win);
            break;

        case Z_WM_WINDOW_MOVED:
            z_win_parse_rect(&win, &msg.obj);
            relayout();
            break;

        case Z_WM_WINDOW_RESIZED:
            if (z_win_apply_resized(&win, &msg.obj) && !game_mode) {
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

    if (z_win_create_flags(&win, "blackjack", WIN_W, WIN_H, -1, -1,
        Z_WIN_FLAG_CLOSE_ICON | Z_WIN_FLAG_CLOSE_KILLS_OWNER |
        Z_WIN_FLAG_RESIZABLE) != Z_OK) {
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

        /* Nothing here changes on its own, so this waits rather than
         * polls. The timeout exists only so a lost message cannot wedge
         * the app entirely. */

        /* THE BANK CHANGES UNDER THIS WINDOW.
         *
         * sw/apps/casino can hand out a loan while a game is open, and
         * another game can win while this one is idle -- /USER/casino.dat is
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

        z_proc_wait(Z_TICK_HZ);
    }

    if (game_mode) leave_game_mode();

    z_win_destroy(&win);
    puts("blackjack: bye");

    return 0;
}
