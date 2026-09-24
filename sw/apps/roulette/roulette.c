/*
 * Zeitlos roulette.
 *
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * A European or American wheel, in a window or full screen, with the
 * chips coming from /user/casino.dat so a win here is spendable in every
 * other game (sw/common/games/zbank.h).
 *
 * -- the spin is the only thing in this app with a frame rate --
 *
 * Everything else is event driven: nothing happens until a key or a
 * click arrives. A spin is the exception, and it shapes this file the
 * way the opponents' thinking time shapes sw/apps/poker.
 *
 * The rules are the same: an app that stops servicing its message queue
 * stalls the WINDOW MANAGER, not just itself, because wm blocks waiting
 * for a redraw acknowledgement (docs/window_manager.md). So the spin
 * loop pumps every frame, and while it runs, input is restricted to the
 * things that are safe.
 *
 * -- why the spin does not double buffer --
 *
 * Game mode offers two pages and a flip. This app does not use it, and
 * that is deliberate rather than an oversight.
 *
 * Each frame changes two small rectangles: where the ball was and where
 * it is. Repairing the first and drawing the second is a few hundred
 * pixels. A page flip would instead require the WHOLE table to be
 * redrawn into the back page every frame -- the wheel, the 37 pockets,
 * the grid, every chip -- which is orders of magnitude more work for an
 * animation that has nothing to tear.
 *
 * So game mode is used for the full-screen page and not for the flip:
 * the view stays on page 0 and the drawing goes there. The same code
 * then runs in a window, where there is no second page at all.
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

#include "rl_board.h"
#include "rl_input.h"

#define WIN_W 320
#define WIN_H 240

/* Four seconds at 30 frames a second. The old 84 was under three, and
 * a pure ease-out crowded most of that into the first second -- see
 * rl_wheel.c's speed profile for what replaced it. */
#define SPIN_FRAMES 120

static z_win_t     win;
static rl_round_t  round_;
static rl_wheel_t  wheel;
static rl_view_t   view;
static rl_layout_t layout_rect;

static bool running = true;
static uint32_t bank_seen;
static bool game_mode;
static bool exit_armed;

static void repaint(void);
static void pump(bool allow_input);

/* -- time ---------------------------------------------------------------
 *
 * z_uptime_ticks() runs at Z_TICK_HZ, which is Z_SYSCLK_HZ / 65536 and
 * therefore neither round nor the same on every board. Deriving the
 * conversion from it at compile time rather than writing out a constant
 * is what stops a differently clocked board running the spin fast or
 * slow with nothing to point at. Fixed point with 10 fractional bits,
 * so this is a multiply and a shift rather than a __divsi3 call on an
 * rv32i build.
 */
#define MS_PER_TICK_Q10 ((1024u * 1000u + Z_TICK_HZ / 2u) / Z_TICK_HZ)

static uint32_t now_ms(void)
{
    return (uint32_t)(((uint64_t)z_uptime_ticks() * MS_PER_TICK_Q10) >> 10);
}

/* -- text ---------------------------------------------------------------- */

static void set_str(char *dst, int len, const char *s)
{
    int i = 0;
    while (s[i] && i < len - 1) { dst[i] = s[i]; i++; }
    dst[i] = '\0';
}

static void say(const char *s)
{
    set_str(view.message, RL_MSG_LEN, s);
}

static void say_num(const char *a, int32_t n, const char *b)
{
    char t[12];
    int i = 0, k;

    while (a[i] && i < RL_MSG_LEN - 1) { view.message[i] = a[i]; i++; }
    rl_num(n, t);
    for (k = 0; t[k] && i < RL_MSG_LEN - 1; k++) view.message[i++] = t[k];
    for (k = 0; b[k] && i < RL_MSG_LEN - 1; k++) view.message[i++] = b[k];
    view.message[i] = '\0';
}

/* -- drawing ------------------------------------------------------------- */

static void relayout(void)
{
    if (game_mode) {
        rl_board_layout(&layout_rect, &view, 0, 0,
            Z_GAME_VIEW_W, Z_GAME_VIEW_H);
    } else {
        z_clip_t c;
        /* Loads this window's visible region as a side effect, which
         * confines every subsequent fill and blit to the part of the
         * window not covered by something in front. */
        z_win_content_rect(&win, &c);
        rl_board_layout(&layout_rect, &view, c.x0, c.y0,
            c.x1 - c.x0 + 1, c.y1 - c.y0 + 1);
    }

    wheel.cx = layout_rect.wheel_cx;
    wheel.cy = layout_rect.wheel_cy;
    wheel.r = layout_rect.wheel_r;
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
    rl_board_draw(&layout_rect, &view);
}

/* -- game mode ----------------------------------------------------------- */

static void enter_game_mode(void)
{
    if (!z_game_available()) {
        say("this bitstream has no game mode");
        puts("roulette: no game mode -- rebuild the gateware with `GAME "
            "in rtl/boards.vh");
        return;
    }

    game_mode = true;
    exit_armed = false;

    z_gfx_clear_visible();
    z_gfx_blit_scissor_reset();
    z_fb_hw_fill_rect(0, 0, 640, 480, 0);

    z_game_set_enabled(true, false);

    /* Page 0, and it stays there. See this file's header: the spin
     * repairs two small rectangles a frame, so there is nothing for a
     * flip to do except force a full redraw. */
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
     * wm repairs damage it caused itself; this came from outside it, so
     * it has to be told. */
    if (z_pid_lookup("wm0", &wm_pid))
        z_msg_new_send(wm_pid, Z_WM_REPAINT, 0, z_obj_uint32(0));

    repaint();
}

/* -- the bank ------------------------------------------------------------ */

static void load_bank(void)
{
    zbank_t b;
    int rv = zbank_load(&b);

    view.chips = b.chips;

    if (rv == ZBANK_CORRUPT) {
        /* Said out loud rather than quietly replaced. zbank.h refuses
         * to write over a damaged file precisely so a real balance is
         * never swapped for a default one behind somebody's back --
         * which means the app has to be the thing that mentions it. */
        say("bank file damaged -- `buyin` to start over");
        puts("roulette: /user/casino.dat is damaged; not writing to it");
    }
}

static void settle(int result)
{
    int32_t ret = rl_round_returns(&round_, result);
    int32_t delta = ret - round_.staked;
    int32_t chips = view.chips;
    int rv, i;

    view.result = result;
    view.last_return = ret;
    view.last_staked = round_.staked;

    /* ONE ADJUSTMENT FOR THE WHOLE ROUND: the net, not a deduction at
     * bet time and a credit at payout.
     *
     * The stake never leaves the bank until the wheel has stopped, so a
     * crash, a power cut or a window closed mid-spin costs nothing
     * rather than costing the stake. And zbank_adjust() re-reads before
     * it writes, so another game that paid out meanwhile is not
     * clobbered -- see docs/casino_bank.md. */
    rv = zbank_adjust("roulette", delta, &chips);

    if (rv == ZBANK_OK) {
        view.chips = chips;
    } else {
        /* The round still happened, so the balance still moves -- it
         * just does not persist. Better than pretending the spin did
         * not occur. */
        view.chips += delta;
        say(zbank_strerror(rv));
    }

    for (i = RL_HISTORY - 1; i > 0; i--) view.history[i] = view.history[i - 1];
    view.history[0] = result;
    if (view.nhistory < RL_HISTORY) view.nhistory++;

    {
        char p[4];
        char line[RL_MSG_LEN];
        int k = 0, j;
        const char *nm = rl_pocket_str(result, p);

        line[0] = '\0';
        for (j = 0; nm[j] && k < RL_MSG_LEN - 1; j++) line[k++] = nm[j];
        line[k] = '\0';
        set_str(view.message, RL_MSG_LEN, line);

        (void)k;
    }

    if (delta > 0) say_num("you win ", delta, "");
    else if (delta == 0) say("you break even");
    else say_num("the house takes ", -delta, "");

    rl_round_clear(&round_, round_.wheel);
}

/* -- the spin ------------------------------------------------------------ */

static void spin_frame_wait(uint32_t *last)
{
    /* Paced by the clock, not by a busy count. In game mode the display
     * is the pace-setter; in a window there is nothing to sync to, so
     * the target is 30 frames a second, which is plenty for one moving
     * ball and leaves the rest of the desktop some cycles. */
    if (game_mode) {
        z_game_wait_frame();
        return;
    }

    for (;;) {
        uint32_t now = now_ms();
        if ((uint32_t)(now - *last) >= 33u) { *last = now; return; }
        pump(false);
        if (!running) return;
    }
}

static void do_spin(void)
{
    int np = rl_pockets(round_.wheel);
    int result;
    uint32_t last = now_ms();

    if (round_.nbets == 0) { say("nothing on the table"); return; }

    /* THE RESULT IS DECIDED HERE, before a single frame is drawn, and
     * the animation is then made to land on it. Spinning freely and
     * reading off wherever it stopped would make the odds a function of
     * frame timing -- so a faster board would pay differently, which is
     * not a wheel. See docs/roulette.md.
     *
     * zg_rng_below() rather than z_rng_below(): the pocket count is 37
     * or 38 so this particular call is safe either way, but the shared
     * one hangs on any power-of-two bound and no game should be one
     * refactor away from that. */
    result = (int)zg_rng_below((uint32_t)np);

    view.spinning = true;

    wheel.ball_r = (wheel.r * 94) / 100;
    rl_wheel_spin(&wheel, result, SPIN_FRAMES);

    say("no more bets");
    repaint();

    while (running) {
        z_clip_t old_ball, old_hub;
        bool more;

        /* FROZEN: a drag is in progress. wm has emptied this window's
         * region and is keeping the last paint on the glass, so
         * docs/window_manager.md asks for exactly this -- draw nothing,
         * change nothing, keep it pending.
         *
         * The wheel still TURNS: a spin that stalled because somebody
         * moved the window would land on a different pocket depending
         * on the drag, and the result was decided before the first
         * frame. Only the drawing is skipped; wm repaints on the thaw. */
        if (z_win_frozen(&win)) {
            more = rl_wheel_step(&wheel);
            spin_frame_wait(&last);
            pump(false);
            if (!more) break;
            continue;
        }

        /* RELOAD THE CLIP EVERY FRAME.
         *
         * The compositor is damage-based now: z_win_content_rect()
         * loads the redraw's damage rather than the whole region, and
         * that restriction stays programmed until something reloads it.
         * These frames draw straight to z_fb_* without going through
         * that chokepoint, so a spin that began after a partial redraw
         * would be clipped to whatever strip that redraw had
         * invalidated -- a ball visible only inside a band, for the
         * rest of the spin. */
        if (!game_mode) relayout();

        (void)old_ball;
        (void)old_hub;

        more = rl_wheel_step(&wheel);

        /* NOTHING IS CLEARED THAT ANYONE CAN SEE.
         *
         * The rim is repainted IN PLACE, each pocket in its own colour,
         * so the band is written once per frame rather than cleared,
         * filled white and cut back -- three repaints of the same ring
         * per frame, straight to the visible page, was what "it flashes
         * a lot" was.
         *
         * The ball's track is then cleared outright, which costs
         * nothing to look at because that band holds nothing but the
         * ball: black over black everywhere it is not. That replaces
         * the old-rectangle bookkeeping entirely -- there is no erase
         * that can disagree with the draw about where the ball was.
         *
         * The wheel is redrawn whole rather than chased with dirty
         * rectangles because every pocket moves: the dirty area IS the
         * wheel. */
        {
            z_clip_t area;
            int pad = wheel.r + (wheel.r / 9) + 4;

            area.x0 = wheel.cx - pad;
            area.y0 = wheel.cy - pad;
            area.x1 = wheel.cx + pad;
            area.y1 = wheel.cy + pad;
            if (area.x0 < layout_rect.clip.x0) area.x0 = layout_rect.clip.x0;
            if (area.y0 < layout_rect.clip.y0) area.y0 = layout_rect.clip.y0;
            if (area.x1 > layout_rect.clip.x1) area.x1 = layout_rect.clip.x1;
            if (area.y1 > layout_rect.clip.y1) area.y1 = layout_rect.clip.y1;

            /* Clear FIRST, then repaint, then the ball -- so nothing
             * at all sits between the last thing that erases the ball
             * and the call that draws it again. The clear used to be in
             * between, adding its whole cost to the window in which the
             * ball is not on the screen. */
            /* THE ERASERS GO LAST, BOTH OF THEM.
             *
             * Two different calls erase the ball, depending on where it
             * is. Out on the track it is the track clear; during the
             * drop it is inside the band, so it is the rim repaint.
             * Whichever one did it, the ball is off the screen from
             * that moment until it is drawn again -- and this draws
             * straight to the visible page, so that window is seen.
             *
             * Putting the track clear first fixed the drop and broke
             * the opening: the ball was erased and then the whole rim
             * ran before it came back. Putting the rim first did the
             * reverse. The order below is the only one where NEITHER
             * eraser has anything expensive after it:
             *
             *   rim          -- erases the ball during the drop
             *   track clear  -- erases the streak on the track
             *   ball         -- immediately after both
             *
             * The rim is also skipped on frames where the head has not
             * moved a pixel, which is most of them; on those the ball
             * is off the screen for the length of a small patch. */
            if (rl_wheel_rim_due(&wheel)) {
                rl_wheel_rim(&wheel, &area);
            } else {
                z_clip_t was;
                rl_wheel_ball_bbox(&wheel, &was);
                if (was.x0 < area.x0) was.x0 = area.x0;
                if (was.y0 < area.y0) was.y0 = area.y0;
                if (was.x1 > area.x1) was.x1 = area.x1;
                if (was.y1 > area.y1) was.y1 = area.y1;
                rl_wheel_patch(&wheel, &was);
            }

            rl_wheel_track_clear(&wheel, &area);

            rl_wheel_ball(&wheel, &area);
        }

        spin_frame_wait(&last);
        pump(false);

        if (!more) break;
    }

    view.spinning = false;

    if (!running) return;

    settle(result);
    repaint();
}

/* -- actions -------------------------------------------------------------- */

static void new_table(void)
{
    rl_round_clear(&round_, round_.wheel);
    rl_wheel_init(&wheel, round_.wheel, layout_rect.wheel_cx,
        layout_rect.wheel_cy, layout_rect.wheel_r > 0 ?
        layout_rect.wheel_r : RL_WHEEL_MIN_R);
    view.result = -1;
    view.nhistory = 0;
}

static void do_action(ri_action_t a)
{
    switch (a) {

    case RI_QUIT:
        running = false;
        break;

    case RI_GAME_MODE:
        if (game_mode) leave_game_mode();
        else enter_game_mode();
        break;

    case RI_NEWGAME:
        new_table();
        repaint();
        break;

    case RI_BUYIN: {
        int32_t chips = view.chips;
        int rv = zbank_buyin(&chips);
        if (rv == ZBANK_OK && chips > view.chips) {
            view.chips = chips;
            say_num("bought in for ", chips, " chips");
        } else if (rv != ZBANK_OK) {
            say(zbank_strerror(rv));
        } else {
            say("you still have chips -- spend them first");
        }
        repaint();
        break;
    }

    case RI_SPIN:
        do_spin();
        break;

    case RI_STATUS:
        /* Two rows of text, not the whole table. relayout() first
         * because in a window that is what reloads the clip. */
        relayout();
        if (layout_rect.ok) rl_board_draw_status(&layout_rect, &view);
        break;

    case RI_REDRAW:
        repaint();
        break;

    default:
        break;
    }
}

/* -- messages ------------------------------------------------------------- */

static void handle_key(uint32_t packed, bool allow_input)
{
    uint32_t sym = Z_WM_UNPACK_KEY_KEYSYM(packed);
    bool pressed = Z_WM_UNPACK_KEY_PRESSED(packed) != 0;

    if (game_mode) {
        /* Escape leaves full screen, but only once RELEASED. Entering
         * game mode by any route that leaves a key still held would
         * otherwise see the key-up for a key that was already down and
         * leave immediately. Same technique as sw/apps/chip8 and
         * sw/apps/chess. */
        if (sym == 0x1b) {
            if (!pressed) exit_armed = true;
            else if (exit_armed) leave_game_mode();
            return;
        }
    }

    if (!pressed) return;
    if (!allow_input) return;

    if (sym == Z_KEY_F2) { do_action(RI_GAME_MODE); return; }

    if (sym == Z_KEY_F1) {
        static int hl;
        const char *s = rl_input_help_line(hl++);
        if (!s) { hl = 0; s = rl_input_help_line(hl++); }
        say(s);
        repaint();
        return;
    }

    do_action(rl_input_key(&view, sym));
}

static void handle_mouse(uint32_t packed)
{
    static bool was_left, was_right;
    uint32_t b = Z_WM_UNPACK_MOUSE_BUTTONS(packed);
    bool left = (b & Z_MOUSE_BTN_LEFT) != 0;
    bool right = (b & Z_MOUSE_BTN_RIGHT) != 0;
    int x = (int)Z_WM_UNPACK_MOUSE_X(packed);
    int y = (int)Z_WM_UNPACK_MOUSE_Y(packed);

    /* On the press edge only. Mouse messages are coalesced and repeat
     * while a button is held, so acting on the level would stack a
     * dozen chips per click. */
    if (left && !was_left) {
        relayout();
        do_action(rl_input_click(&view, &layout_rect, x, y, false));
    } else if (right && !was_right) {
        relayout();
        do_action(rl_input_click(&view, &layout_rect, x, y, true));
    }

    was_left = left;
    was_right = right;
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
            /* Ignored entirely during a spin: a click can place a bet,
             * and the result has already been decided. */
            if (allow_input && !game_mode && msg.obj.type == Z_UINT32)
                handle_mouse(msg.obj.val.uint32);
            break;

        case Z_WM_SET_CLIP:
            if (!z_win_apply_clip(&win, &msg.obj))
                puts("roulette: bad clip region message");
            break;

        case Z_WM_REDRAW:
            if (msg.obj.type != Z_UINT32) break;
            if (z_win_redraw_id(msg.obj.val.uint32) != win.id) break;
            z_win_apply_redraw(&win, msg.obj.val.uint32);
            /* Answered even in game mode and even mid-spin. The window
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

    /* Claimed whether or not it is used: leaving one pending would hand
     * it to whatever the person opens next. */
    if (!z_launch_arg_take(arg, sizeof(arg))) return;
    if (!arg[0]) return;

    if (arg[0] == 'a' || arg[0] == 'A') round_.wheel = RL_AMERICAN;
    else if (arg[0] == 'e' || arg[0] == 'E') round_.wheel = RL_EURO;
    else puts("roulette: ignoring an argument it did not recognise");
}

int main(void)
{
    puts("roulette: starting");

    /* The real generator -- a ChaCha20 stream seeded from rtl/trng.v
     * where the board has one. NOT gated on z_rng_secure(): zrng.h is
     * explicit that games should use it whatever its provenance and
     * that only keys should refuse. Whether it was hardware-seeded is
     * SHOWN on the table instead, which is the useful thing to do with
     * that bit. */
    zg_rng_use_system();
    view.trng = z_rng_secure();

    /* AMERICAN by default. It is the worse game -- the second zero
     * takes the house edge from 2.70% to 5.26%, which is the single
     * biggest fact about roulette -- but it is the wheel people
     * picture, and `wheel euro` is one command away. The table prints
     * which one is in play. */
    rl_round_clear(&round_, RL_AMERICAN);
    view.round = &round_;
    view.wheel = &wheel;
    view.chip_sel = 1;
    view.result = -1;

    apply_launch_arg();

    if (z_win_create_flags(&win, "roulette", WIN_W, WIN_H, -1, -1,
        Z_WIN_FLAG_CLOSE_ICON | Z_WIN_FLAG_CLOSE_KILLS_OWNER |
        Z_WIN_FLAG_RESIZABLE) != Z_OK) {
        puts("roulette: failed to create window -- is wm running?");
        return 1;
    }

    rl_board_init();
    relayout();
    rl_wheel_init(&wheel, round_.wheel, layout_rect.wheel_cx,
        layout_rect.wheel_cy, layout_rect.wheel_r);

    load_bank();
    if (view.message[0] == '\0') say("place your bets -- F1 for help");

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
         * another game can win while this one is idle -- /user/casino.dat is
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
    puts("roulette: bye");

    return 0;
}
