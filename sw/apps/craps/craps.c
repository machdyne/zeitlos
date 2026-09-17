/*
 * Zeitlos craps.
 *
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * Two dice, a felt with more bets on it than any other game here, and
 * the only wager in a casino with no house edge at all.
 *
 * -- the bank is settled when the table empties --
 *
 * The other five games have a round: you stake, it resolves, the net
 * goes to /USER/casino.dat. Craps has no such boundary. Bets go up and come
 * down across many rolls, a place bet wins and stays working, a come
 * bet takes its own number and outlives the point that was on when it
 * was made.
 *
 * So the bank is touched when cr_at_risk() reaches zero -- when the
 * felt is genuinely clear, which is what a seven-out is. Until then the
 * stake is tracked in the game and the display shows what is left to
 * bet rather than what is in the bank.
 *
 * That keeps the same crash-safety the others have: chips do not leave
 * /USER/casino.dat until the shooter's run is over, so a window closed
 * mid-hand costs nothing. It also means a long hot run is not banked
 * until it ends, which is exactly what a craps table feels like.
 *
 * -- the dice need no clearing --
 *
 * A zdice tile has a LIT body, so it is opaque: the new face covers the
 * old one exactly. A roll is two blits a frame at a fixed position,
 * with nothing erased and no frame on which either die is blank.
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

#include "cr_board.h"
#include "cr_input.h"

#define WIN_W 320
#define WIN_H 240

/* Long enough to be a roll rather than a jump cut. At 40ms a frame
 * that is just under a second. */
#define TUMBLE_FRAMES 22
#define TUMBLE_MS     40

static z_win_t     win;
static cr_game_t   game;
static cr_view_t   view;
static cr_layout_t layout_rect;

static bool running = true;
static uint32_t bank_seen;
static bool game_mode;
static bool exit_armed;

static void repaint(void);
static void pump(bool allow_input);

/* z_uptime_ticks() runs at Z_TICK_HZ, which is Z_SYSCLK_HZ / 65536 and
 * neither round nor the same on every board. Derived at compile time so
 * a differently clocked board does not roll fast or slow. */
#define MS_PER_TICK_Q10 ((1024u * 1000u + Z_TICK_HZ / 2u) / Z_TICK_HZ)

static uint32_t now_ms(void)
{
    return (uint32_t)(((uint64_t)z_uptime_ticks() * MS_PER_TICK_Q10) >> 10);
}

static void set_str(char *dst, int len, const char *s)
{
    int i = 0;
    while (s[i] && i < len - 1) { dst[i] = s[i]; i++; }
    dst[i] = '\0';
}

static void say(const char *s) { set_str(view.message, CR_MSG_LEN, s); }

static void say_num(const char *a, int32_t n, const char *b)
{
    char t[12];
    int i = 0, k;
    while (a[i] && i < CR_MSG_LEN - 1) { view.message[i] = a[i]; i++; }
    cr_num(n, t);
    for (k = 0; t[k] && i < CR_MSG_LEN - 1; k++) view.message[i++] = t[k];
    for (k = 0; b[k] && i < CR_MSG_LEN - 1; k++) view.message[i++] = b[k];
    view.message[i] = '\0';
}

/* -- drawing --------------------------------------------------------------- */

static void relayout(void)
{
    if (game_mode) {
        cr_board_layout(&layout_rect, &view, 0, 0,
            Z_GAME_VIEW_W, Z_GAME_VIEW_H);
    } else {
        z_clip_t c;
        /* Loads this window's visible region as a side effect, which
         * confines every subsequent fill and blit to the part not
         * covered by something in front. */
        z_win_content_rect(&win, &c);
        cr_board_layout(&layout_rect, &view, c.x0, c.y0,
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
    cr_board_draw(&layout_rect, &view);
}

static void enter_game_mode(void)
{
    if (!z_game_available()) {
        say("this bitstream has no game mode");
        puts("craps: no game mode -- rebuild the gateware with `GAME "
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
        puts("craps: /USER/casino.dat is damaged; not writing to it");
    }
}

static void settle_if_clear(void)
{
    int32_t delta, chips;
    int rv;

    /* Only when the felt is genuinely empty. See the header: craps has
     * no round, so this is the boundary it does have. */
    if (cr_at_risk(&game) > 0) return;
    if (game.staked == 0 && game.returned == 0) return;

    delta = game.returned - game.staked;

    /* zbank_adjust() re-reads before it writes, so a win in another
     * game meanwhile is not clobbered. */
    rv = zbank_adjust("craps", delta, &chips);

    if (rv == ZBANK_OK) view.chips = chips;
    else { view.chips += delta; say(zbank_strerror(rv)); }

    game.staked = 0;
    game.returned = 0;
}

/* -- the roll -------------------------------------------------------------- */

static void frame_wait(uint32_t *last)
{
    if (game_mode) {
        z_game_wait_frame();
        return;
    }

    for (;;) {
        uint32_t now = now_ms();
        if ((uint32_t)(now - *last) >= TUMBLE_MS) { *last = now; return; }
        pump(false);
        if (!running) return;
    }
}

static void do_roll(void)
{
    int d1, d2, i;
    uint32_t last = now_ms();
    int32_t back;

    if (game.nbets == 0) {
        say("nothing on the felt -- click a spot first");
        repaint();
        return;
    }

    /* THE DICE ARE THROWN HERE, before a frame is drawn, and the
     * tumble is decoration. Reading the result off whatever the
     * animation happened to stop on would make the odds a function of
     * frame timing -- and the whole point of cr_table.c is that the
     * edge is an exact fraction. */
    d1 = (int)zg_rng_below(6) + 1;
    d2 = (int)zg_rng_below(6) + 1;

    view.rolling = true;
    say("rolling");
    repaint();

    for (i = 0; i < TUMBLE_FRAMES && running; i++) {
        /* FROZEN: a drag is in progress. wm has emptied this window's
         * region and is keeping the last paint on the glass, so
         * docs/window_manager.md asks a periodic render to draw nothing
         * and change nothing until the thaw. The dice were already
         * thrown, so pausing the tumble changes nothing about the
         * result. */
        if (!z_win_frozen(&win)) {
            if (!game_mode) relayout();

            /* Two opaque blits at a fixed position. Nothing is cleared
             * and no frame has either die blank. */
            view.show_d1 = (int)zg_rng_below(6) + 1;
            view.show_d2 = (int)zg_rng_below(6) + 1;
            cr_board_draw_dice(&layout_rect, &view);
        }

        frame_wait(&last);
        pump(false);
    }

    if (!running) return;

    view.rolling = false;
    view.show_d1 = d1;
    view.show_d2 = d2;

    back = cr_roll(&game, d1, d2);

    if (game.seven_out) say("seven out");
    else if (game.point_made) say_num("point made -- ", game.last_won, " back");
    else if (game.rolls == 1 && game.point) say_num("the point is ", game.point, "");
    else if (back > 0) say_num("paid ", back, "");
    else if (game.last_lost > 0) say_num("lost ", game.last_lost, "");
    else say_num("", d1 + d2, "");

    settle_if_clear();
    repaint();
}

/* -- actions --------------------------------------------------------------- */

static void do_action(ci_action_t a)
{
    switch (a) {

    case CI_QUIT:
        running = false;
        break;

    case CI_GAME_MODE:
        if (game_mode) leave_game_mode();
        else enter_game_mode();
        break;

    case CI_BUYIN: {
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

    case CI_ROLL:
        do_roll();
        break;

    case CI_STATUS:
        /* Two rows of text, not the whole felt. relayout() first
         * because in a window that is what reloads the clip. */
        relayout();
        if (layout_rect.ok) cr_board_draw_status(&layout_rect, &view);
        break;

    case CI_REDRAW:
        repaint();
        break;

    default:
        break;
    }
}

/* -- messages -------------------------------------------------------------- */

static void handle_key(uint32_t packed, bool allow_input)
{
    uint32_t sym = Z_WM_UNPACK_KEY_KEYSYM(packed);
    bool pressed = Z_WM_UNPACK_KEY_PRESSED(packed) != 0;

    if (game_mode) {
        /* Escape leaves full screen, but only once RELEASED. Entering
         * game mode by a route that leaves a key held would otherwise
         * see the key-up for a key that was already down and leave
         * immediately. */
        if (sym == 0x1b) {
            if (!pressed) exit_armed = true;
            else if (exit_armed) leave_game_mode();
            return;
        }
    }

    if (!pressed) return;
    if (!allow_input) return;

    if (sym == Z_KEY_F2) { do_action(CI_GAME_MODE); return; }

    if (sym == Z_KEY_F1) {
        static int hl;
        const char *s = cr_input_help_line(hl++);
        if (!s) { hl = 0; s = cr_input_help_line(hl++); }
        say(s);
        do_action(CI_STATUS);
        return;
    }

    do_action(cr_input_key(&view, sym));
}

static void handle_mouse(uint32_t packed)
{
    static bool was_down;
    bool down = (Z_WM_UNPACK_MOUSE_BUTTONS(packed) & Z_MOUSE_BTN_LEFT) != 0;
    int x = (int)Z_WM_UNPACK_MOUSE_X(packed);
    int y = (int)Z_WM_UNPACK_MOUSE_Y(packed);

    /* On the press edge only. Mouse messages are coalesced and repeat
     * while a button is held, so acting on the level would stack a
     * dozen chips per click. */
    if (down && !was_down) {
        relayout();
        do_action(cr_input_click(&view, &layout_rect, x, y));
    }

    was_down = down;
}

static void pump(bool allow_input)
{
    z_msg_t msg;

    while (z_msg_read(&msg) == Z_OK) {

        switch (msg.subject) {

        case Z_WM_KEY:
            if (msg.obj.type == Z_UINT32)
                handle_key(msg.obj.val.uint32, allow_input);
            break;

        case Z_WM_MOUSE:
            /* Ignored during a roll: the dice are already thrown. */
            if (allow_input && !game_mode && msg.obj.type == Z_UINT32)
                handle_mouse(msg.obj.val.uint32);
            break;

        case Z_WM_SET_CLIP:
            if (!z_win_apply_clip(&win, &msg.obj))
                puts("craps: bad clip region message");
            break;

        case Z_WM_REDRAW:
            if (msg.obj.type != Z_UINT32) break;
            if (z_win_redraw_id(msg.obj.val.uint32) != win.id) break;
            z_win_apply_redraw(&win, msg.obj.val.uint32);
            /* Answered even in game mode and even mid-roll. The window
             * is still a window as far as wm is concerned, and an
             * unanswered redraw is what stalls the desktop. */
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
    int32_t v = 0;
    int i;

    /* Claimed whether or not it is used: leaving one pending would hand
     * it to whatever the person opens next. */
    if (!z_launch_arg_take(arg, sizeof(arg))) return;
    if (!arg[0]) return;

    for (i = 0; arg[i]; i++) {
        if (arg[i] < '0' || arg[i] > '9') { v = 0; break; }
        v = v * 10 + (arg[i] - '0');
    }

    if (v > 0) view.bet = v;
    else puts("craps: ignoring an argument it did not recognise");
}

int main(void)
{
    puts("craps: starting");

    /* The real generator: a ChaCha20 stream seeded from rtl/trng.v
     * where the board has one. NOT gated on z_rng_secure() -- zrng.h is
     * explicit that games should use it whatever its provenance and
     * that only keys should refuse. */
    zg_rng_use_system();
    view.trng = z_rng_secure();

    cr_game_init(&game);
    view.g = &game;
    view.bet = 25;
    view.show_d1 = 3;
    view.show_d2 = 4;

    apply_launch_arg();

    if (z_win_create_flags(&win, "craps", WIN_W, WIN_H, -1, -1,
        Z_WIN_FLAG_CLOSE_ICON | Z_WIN_FLAG_CLOSE_KILLS_OWNER |
        Z_WIN_FLAG_RESIZABLE) != Z_OK) {
        puts("craps: failed to create window -- is wm running?");
        return 1;
    }

    cr_board_init();
    load_bank();
    if (view.message[0] == '\0')
        say("click the felt, then Return to roll -- F1 for help");

    repaint();

    while (running) {
        pump(true);
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
    puts("craps: bye");

    return 0;
}
