/*
 * Zeitlos slots.
 *
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * Three reels, five paylines, and a house edge of 5.359% that
 * sl_reels.c pins to an exact integer. Chips come from /data/casino/bank.dat,
 * shared with poker, roulette and blackjack.
 *
 * -- the spin is the only thing here with a frame rate --
 *
 * Everything else is event driven. The rule that shapes the loop is the
 * same one the other three obey: an app that stops servicing its
 * message queue stalls the WINDOW MANAGER, not just itself, because wm
 * blocks waiting for a redraw acknowledgement. So the spin pumps every
 * frame.
 *
 * -- and it does not flicker, because it never clears --
 *
 * A frame redraws the three reels and nothing else. A reel is five
 * opaque tile blits that abut exactly, so every pixel is written once
 * and no part of it is ever blank. sw/apps/roulette flashes because a
 * rotating rim has no such covering -- it has to be cleared first. That
 * is the whole difference, and it is a property of the shape being
 * animated rather than of which blit is used.
 *
 * -- two faces: the desktop and the screen (a dual app) --
 *
 * The desktop face draws the machine in one bit in a resizable window.
 * F10, the colour icon or F11 lend the window to the arcade runtime
 * (ar_attach(), sw/common/games/zarcade.h) and the same machine is
 * drawn on its 320x240 screen in colour (sl_screen()), full screen with
 * F11; the reels then turn a runtime frame at a time. F10, the icon or
 * a resize ask for the desktop face back (AR_EV_DESKTOP), taken between
 * spins. docs/slots.md, "In colour"; docs/app_kinds.md.
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

#include "sl_board.h"
#include "sl_input.h"

#define WIN_W 320
#define WIN_H 240

static z_win_t     win;
static sl_spin_t   spin;
static sl_view_t   view;
static sl_layout_t layout_rect;

static bool running = true;
static uint32_t bank_seen;
static bool scr;            /* the screen face: the runtime has the window */
static bool want_desktop;   /* it asked for the desktop face (AR_EV_DESKTOP) */

static void repaint(void);
static void pump(bool allow_input);

/* z_uptime_ticks() runs at Z_TICK_HZ, which is Z_SYSCLK_HZ / 65536 and
 * neither round nor the same on every board. Derived at compile time so
 * a differently clocked board does not run the reels fast or slow, in
 * fixed point so this is a multiply and a shift rather than a __divsi3
 * call on rv32i. */
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

static void say(const char *s) { set_str(view.message, SL_MSG_LEN, s); }

static int slen_(const char *s)
{
    int n = 0;
    while (s[n]) n++;
    return n;
}

static void say_num(const char *a, int32_t n, const char *b)
{
    char t[12];
    int i = 0, k;
    while (a[i] && i < SL_MSG_LEN - 1) { view.message[i] = a[i]; i++; }
    sl_num(n, t);
    for (k = 0; t[k] && i < SL_MSG_LEN - 1; k++) view.message[i++] = t[k];
    for (k = 0; b[k] && i < SL_MSG_LEN - 1; k++) view.message[i++] = b[k];
    view.message[i] = '\0';
}

/* -- drawing -------------------------------------------------------------- */

/* The screen face's picture, for the runtime: the whole machine, cut to
 * what it repaints. */
static void scr_paint(void *user, const ar_rect_t *r)
{
    sl_layout_t t;
    (void)user; (void)r;
    sl_board_layout(&t, &view, 0, 0, AR_W, AR_H);
    sl_board_draw(&t, &view);
}

static char help_text[768];

static ar_desc_t desc = {
    .name = "slots",
    .help_title = "Slots help",
    .help_text = help_text,
    .help_doc = "slots",
    .paint = scr_paint,
    .pointer = true,
};

static void relayout(void)
{
    if (scr) {
        sl_board_layout(&layout_rect, &view, 0, 0, AR_W, AR_H);
    } else {
        z_clip_t c;
        /* Loads this window's visible region as a side effect, which
         * confines every subsequent fill and blit to the part not
         * covered by something in front. */
        z_win_content_rect(&win, &c);
        sl_board_layout(&layout_rect, &view, c.x0, c.y0,
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
    sl_board_draw(&layout_rect, &view);
}

/* -- the screen face ------------------------------------------------------- */

static void screen_on(bool full)
{
    if (!scr) {
        scr = true;
        sl_screen(true);
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
    sl_screen(false);
    scr = false;
    /* wm's REDRAW, at the desktop size again, repaints the machine */
}

/* -- the bank -------------------------------------------------------------- */

static void load_bank(void)
{
    zbank_t b;
    int rv = zbank_load(&b);

    view.chips = b.chips;

    if (rv == ZBANK_CORRUPT) {
        say("bank file damaged -- `buyin` to start over");
        puts("slots: " ZBANK_PATH " is damaged; not writing to it");
    }
}

/* -- the spin -------------------------------------------------------------- */

static void frame_wait(uint32_t *last)
{
    /* Paced by the clock, not by a busy count. Thirty frames a second
     * is plenty for three reels and leaves the desktop some cycles. */
    /* The screen face: two runtime frames (30 a second at 60Hz). */
    if (scr) {
        ar_frame();
        ar_frame();
        return;
    }

    for (;;) {
        uint32_t now = now_ms();
        if ((uint32_t)(now - *last) >= 33u) { *last = now; return; }
        pump(false);
        if (!running) return;
    }
}

static void settle(const int *stops)
{
    int32_t stake = sl_stake(&view);
    int32_t won, delta, chips = view.chips;
    int rv;

    won = sl_evaluate(stops, view.bet, view.lines, view.line_pays);

    view.last_win = won;
    view.have_result = true;

    delta = won - stake;

    /* ONE ADJUSTMENT FOR THE WHOLE SPIN: the net, not a deduction when
     * the reels start and a credit when they stop.
     *
     * The stake never leaves the bank until the reels have landed, so a
     * crash or a window closed mid-spin costs nothing rather than
     * costing the bet. And zbank_adjust() re-reads before it writes, so
     * a win in another game meanwhile is not clobbered. */
    rv = zbank_adjust("slots", delta, &chips);

    if (rv == ZBANK_OK) view.chips = chips;
    else { view.chips += delta; say(zbank_strerror(rv)); return; }

    if (won > 0) {
        /* NAMES THE LINES. "won 40" on a machine with five paylines is
         * a number with no story; "line 2 and line 4 won 40" is the
         * whole explanation, and the boxes on screen say which symbols
         * did it. */
        char m[SL_MSG_LEN];
        int i, n = 0;

        m[0] = '\0';
        for (i = 0; i < SL_LINES; i++) {
            char t[8];
            if (view.line_pays[i] <= 0) continue;
            if (n++) set_str(m + slen_(m), SL_MSG_LEN - slen_(m), ", ");
            sl_num(i + 1, t);
            set_str(m + slen_(m), SL_MSG_LEN - slen_(m), "line ");
            set_str(m + slen_(m), SL_MSG_LEN - slen_(m), t);
        }
        set_str(m + slen_(m), SL_MSG_LEN - slen_(m), n > 1 ? " win " : " wins ");
        {
            char t[12];
            sl_num(won, t);
            set_str(m + slen_(m), SL_MSG_LEN - slen_(m), t);
        }
        say(m);
    } else say("no win");
}

static void do_spin(void)
{
    int stops[SL_REELS];
    uint32_t last = now_ms();
    int r, guard = 0;

    if (sl_stake(&view) > view.chips) {
        say("not enough chips for that stake");
        repaint();
        return;
    }

    /* THE STOPS ARE CHOSEN HERE, before a frame is drawn, and the
     * animation is made to land on them. Spinning freely and reading
     * off wherever the reels happened to stop would make the odds a
     * function of frame timing -- and the whole point of sl_reels.c is
     * that the edge is an exact number.
     *
     * zg_rng_below(SL_STOPS) with SL_STOPS == 32 is exactly the
     * power-of-two bound that used to hang: z_rng_below()'s accept
     * bound 2^32 - (2^32 % 32) is 2^32 and truncates to zero. A slot
     * machine asks for it three times a spin. */
    for (r = 0; r < SL_REELS; r++)
        stops[r] = (int)zg_rng_below(SL_STOPS);

    view.have_result = false;
    sl_spin_begin(&spin, stops);

    say("reels turning");
    repaint();

    while (running && guard++ < 1000) {
        bool more;

        /* FROZEN: a drag is in progress. wm has emptied this window's
         * region and is keeping the last paint on the glass, so
         * docs/window_manager.md asks a periodic render to draw nothing
         * and change nothing until the thaw.
         *
         * The reels still TURN: the stops were decided before the first
         * frame, so a spin that paused because somebody moved the
         * window would still land on the same symbols. Only the drawing
         * is skipped. */
        if (!scr && z_win_frozen(&win)) {
            more = sl_spin_step(&spin);
            frame_wait(&last);
            pump(false);
            if (!more) break;
            continue;
        }

        /* Reloaded every frame. The compositor is damage-based:
         * z_win_content_rect() loads the current redraw's damage rather
         * than the whole region, and that restriction stays programmed
         * until something reloads it. These frames draw straight to
         * z_fb_* without going through that chokepoint. */
        relayout();

        more = sl_spin_step(&spin);

        /* Only the reels. Five opaque tiles each, abutting exactly, so
         * every pixel is written once and nothing is ever cleared --
         * which is why this does not flicker and roulette does. */
        sl_board_draw_reels(&layout_rect, &view);

        frame_wait(&last);
        pump(false);

        if (!more) break;
    }

    if (!running) return;

    settle(stops);
    repaint();
}

/* -- actions --------------------------------------------------------------- */

static void do_action(si_action_t a)
{
    switch (a) {

    case SI_QUIT:
        running = false;
        break;

    case SI_GAME_MODE:
        /* full screen is the screen face's */
        if (!scr) screen_on(true);
        else ar_set_fullscreen(!ar_fullscreen());
        break;

    case SI_BUYIN: {
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

    case SI_SPIN:
        do_spin();
        break;

    case SI_STATUS:
        /* Two rows of text, not the whole machine. relayout() first
         * because in a window that is what reloads the clip. */
        relayout();
        if (layout_rect.ok) sl_board_draw_status(&layout_rect, &view);
        break;

    case SI_REDRAW:
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
    n = (size_t)snprintf(help_text, sizeof(help_text), "%s\n\n", "A slot machine. Type a command and press Enter.");
    for (i = 0; (line = sl_input_help_line(i)) != 0 && n < sizeof(help_text); i++)
        n += (size_t)snprintf(help_text + n, sizeof(help_text) - n, "%s\n", line);
    if (n < sizeof(help_text))
        snprintf(help_text + n, sizeof(help_text) - n,
            "\nF10: the machine in colour, and back.\n"
            "F11 (or `game`): full screen; Esc comes back.");
}

static void show_help(void)
{
    z_dialog_ctx_t ctx = { &win, help_on_msg, NULL };
    z_dialog_help(&ctx, "Slots help", help_text, "slots");
    repaint();
}

static void handle_key(uint32_t sym, bool pressed, bool allow_input)
{
    if (!pressed) return;
    if (!allow_input) return;

    /* The desktop face's: on the screen face these are the runtime's
     * (F10, F11, F1 there; Esc leaves its full screen). */
    if (sym == Z_KEY_FULL_SCREEN) { screen_on(true); return; }
    if (sym == Z_KEY_COLOUR) { screen_on(false); return; }
    if (sym == Z_KEY_HELP) { show_help(); return; }

    do_action(sl_input_key(&view, sym));
}

static void handle_mouse(int x, int y, bool down)
{
    static bool was_down;

    /* On the press edge only. Mouse messages are coalesced and repeat
     * while a button is held, so acting on the level would cycle the
     * bet a dozen times per click. */
    if (down && !was_down) {
        relayout();
        do_action(sl_input_click(&view, &layout_rect, x, y));
    }

    was_down = down;
}

static void pump(bool allow_input)
{
    z_msg_t msg;

    /* The screen face: the runtime reads the window's messages. */
    if (scr) {
        ar_event_t e;
        while (ar_poll(&e)) {
            if (e.type == AR_EV_KEY)
                handle_key(e.key, e.pressed, allow_input);
            else if (e.type == AR_EV_MOUSE && allow_input)
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
                    Z_WM_UNPACK_KEY_PRESSED(msg.obj.val.uint32) != 0,
                    allow_input);
            break;

        case Z_WM_MOUSE:
            /* Ignored during a spin: a click could start another one,
             * and the stops for this one are already decided. */
            if (allow_input && msg.obj.type == Z_UINT32) {
                uint32_t u = msg.obj.val.uint32;
                handle_mouse((int)Z_WM_UNPACK_MOUSE_X(u),
                    (int)Z_WM_UNPACK_MOUSE_Y(u),
                    (Z_WM_UNPACK_MOUSE_BUTTONS(u) & Z_MOUSE_BTN_LEFT) != 0);
            }
            break;

        /* The colour icon: the screen face. */
        case Z_WM_TITLEBAR_ICON:
            if (msg.obj.type == Z_UINT32 && allow_input &&
                Z_WM_UNPACK_TBICON_KIND(msg.obj.val.uint32) == Z_WM_TBICON_COLOUR)
                screen_on(false);
            break;

        case Z_WM_SET_CLIP:
            if (!z_win_apply_clip(&win, &msg.obj))
                puts("slots: bad clip region message");
            break;

        case Z_WM_REDRAW:
            if (msg.obj.type != Z_UINT32) break;
            if (z_win_redraw_id(msg.obj.val.uint32) != win.id) break;
            z_win_apply_redraw(&win, msg.obj.val.uint32);
            /* Answered even mid-spin: an unanswered redraw is what
             * stalls the desktop. */
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

    if (arg[0] >= '1' && arg[0] <= '5' && !arg[1])
        view.lines = arg[0] - '0';
    else
        puts("slots: ignoring an argument it did not recognise");
}

int main(void)
{
    puts("slots: starting");

    /* The real generator: a ChaCha20 stream seeded from rtl/trng.v
     * where the board has one. NOT gated on z_rng_secure() -- zrng.h is
     * explicit that games should use it whatever its provenance and
     * that only keys should refuse. */
    zg_rng_use_system();
    view.trng = z_rng_secure();

    sl_spin_init(&spin);
    view.spin = &spin;
    view.bet = 5;
    view.lines = SL_LINES;

    apply_launch_arg();

    /* The screen face's palette and inks, and its flags on the window:
     * game mode (F11 is slots's), the colour window and its icon. */
    build_help();
    memcpy(desc.palette, sl_palette, sizeof(desc.palette));
    sl_mono_inks(desc.ink);
    ar_window_colour(true);

    if (z_win_create_flags(&win, "slots", WIN_W, WIN_H, -1, -1,
        Z_WIN_FLAG_CLOSE_ICON | Z_WIN_FLAG_CLOSE_KILLS_OWNER |
        Z_WIN_FLAG_RESIZABLE | ar_window_flags(&desc)) != Z_OK) {
        puts("slots: failed to create window -- is wm running?");
        return 1;
    }

    sl_board_init();
    load_bank();
    if (view.message[0] == '\0') say("Return to spin -- F1 for help");

    repaint();

    while (running) {
        pump(true);
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
    puts("slots: bye");

    return 0;
}
