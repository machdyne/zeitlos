/*
 * Zeitlos poker.
 *
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * Texas hold'em, five-card draw, five-card stud and seven-card stud,
 * in a window or full screen, against eight difficulty levels, driven
 * entirely from the keyboard or entirely with a mouse.
 *
 * -- the one thing that shapes this whole file --
 *
 * The opponents can think for a second or two at a time, and this is a
 * cooperatively drawn desktop: an app that stops servicing its message
 * queue stalls the WINDOW MANAGER, not just itself. wm blocks waiting
 * for a redraw acknowledgement and eventually times out (see
 * docs/window_manager.md), so an opponent that disappeared for two
 * seconds would freeze every other window on screen for two seconds.
 *
 * So pk_ai_decide() is given a poll callback -- ai_poll() below --
 * which runs the message pump every few hundred rollouts. Two
 * consequences run through the rest of the file, and they are the same
 * two sw/apps/chess has:
 *
 *   1. While an opponent is thinking, input is restricted to the
 *      things that are SAFE: window events, and Escape to stop it.
 *      Running a command mid-decision would change the game underneath
 *      the estimate being computed from it.
 *
 *   2. Repainting from inside the poll callback is fine here, unlike
 *      in chess. A poker AI does not mutate the game while it thinks
 *      -- it deals private copies of the deck and never touches
 *      pk_game_t -- so the table on screen is always the real one.
 *      That is worth stating because chess's equivalent comment says
 *      the opposite, for a genuinely different reason.
 *
 * -- windowed and full screen --
 *
 * Both, from one renderer. table_ui.c draws against an origin and a
 * clip rectangle; windowed passes the window's content rectangle and
 * game mode passes a 320x240 page. See table_ui.h.
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

#include "table_ui.h"
#include "input.h"

#define WIN_W 320
#define WIN_H 240

#define START_STACK 1000
#define SMALL_BLIND 5
#define BIG_BLIND   10

static z_win_t     win;
static pk_game_t   game;
static pt_view_t   view;
static pt_layout_t layout_rect;
static pk_ai_t     ai[PK_MAX_SEATS];

static bool running = true;
static uint32_t bank_seen;
static bool game_mode;
static int  game_front;
static bool exit_armed;
static bool thinking;
static bool cancel_think;

static int  seats = 6;
static int  limit_mode = PK_LIMIT_NONE;
static const pk_variant_t *variant = &pk_variant_holdem;

static void repaint(void);
static void pump(bool allow_input);

/* -- time -------------------------------------------------------------
 *
 * z_uptime_ticks() runs at Z_TICK_HZ -- the KTIMER interrupt rate,
 * which is Z_SYSCLK_HZ / 65536 and therefore not a round number and
 * NOT the same on every board.
 *
 * The conversion is derived from Z_TICK_HZ at compile time rather than
 * written out. A hardcoded 48MHz constant would build fine on a board
 * clocked differently and simply run the opponents' clock fast or
 * slow, which shows up as the difficulty levels thinking for the wrong
 * length of time with nothing to point at.
 *
 * Fixed point with 10 fractional bits, so this is a multiply and a
 * shift rather than a __divsi3 call on an rv32i build. Copied in shape
 * from sw/apps/chess, which had to solve exactly this.
 */
#define MS_PER_TICK_Q10 ((1024u * 1000u + Z_TICK_HZ / 2u) / Z_TICK_HZ)

static uint32_t now_ms(void)
{
    return (uint32_t)(((uint64_t)z_uptime_ticks() * MS_PER_TICK_Q10) >> 10);
}

/* -- randomness --------------------------------------------------------
 *
 * zdeck.c takes its generator through a function pointer so the host
 * tests can make a deal reproducible. On hardware, zg_rng_use_system()
 * installs the real one: a ChaCha20 stream seeded from rtl/trng.v where
 * the board has one.
 *
 * This app used to carry its own three-line wrapper around z_rng_u32()
 * here, and its own copy of the rejection-sampling bound in pk_deck.c.
 * Both moved to sw/common/games/zrand.c when roulette needed the same
 * thing -- see docs/casino_bank.md on why two copies of that particular
 * arithmetic produced two different bugs.
 *
 * NOT gated on z_rng_secure(). sw/common/zrng.h is explicit about which
 * question to ask: shuffling wants unpredictable-to-a-person, and a
 * board with no TRNG deals a perfectly good game of poker. Refusing to
 * play on such a board would be treating a card game like a key
 * exchange.
 */

/* -- names ------------------------------------------------------------- */

static void set_str(char *dst, int len, const char *s)
{
    int i = 0;
    while (s[i] && i < len - 1) { dst[i] = s[i]; i++; }
    dst[i] = '\0';
}

static void set_names(void)
{
    static const char *const who[PK_MAX_SEATS] = {
        "you", "Anna", "Bo", "Cy", "Dee", "Eli", "Fay", "Gus" };
    int i;
    for (i = 0; i < PK_MAX_SEATS; i++)
        set_str(view.name[i], sizeof view.name[i], who[i]);
}

static void clear_tags(void)
{
    int i;
    for (i = 0; i < PK_MAX_SEATS; i++) view.tag[i][0] = '\0';
}

static void tag_action(int seat, int action, int32_t before_bet)
{
    char t[PT_TAG_LEN];
    int32_t put;

    switch (action) {
    case PK_FOLD:  set_str(t, sizeof t, "folds"); break;
    case PK_CHECK: set_str(t, sizeof t, "checks"); break;
    case PK_CALL:  set_str(t, sizeof t, "calls"); break;
    case PK_BET:   set_str(t, sizeof t, "bets"); break;
    case PK_RAISE: set_str(t, sizeof t, "raises"); break;
    default:       set_str(t, sizeof t, ""); break;
    }

    put = game.seat[seat].bet - before_bet;
    if (put > 0) {
        char num[12];
        int i = 0, k;
        while (t[i]) i++;
        if (i < PT_TAG_LEN - 1) t[i++] = ' ';
        pt_num(put, num);
        for (k = 0; num[k] && i < PT_TAG_LEN - 1; k++) t[i++] = num[k];
        t[i] = '\0';
    }

    set_str(view.tag[seat], PT_TAG_LEN, t);
}

/* -- drawing ----------------------------------------------------------- */

static void relayout(void)
{
    if (game_mode) {
        int px = (game_front ^ 1) * Z_GAME_VIEW_W;    /* the back page */
        pt_layout(&layout_rect, &view, px, 0,
            Z_GAME_VIEW_W, Z_GAME_VIEW_H);
    } else {
        z_clip_t c;
        /* Loads this window's visible region as a side effect, which
         * confines every subsequent fill and blit to the part of the
         * window not covered by something in front. See
         * z_win_content_rect() in zwin.c. */
        z_win_content_rect(&win, &c);
        pt_layout(&layout_rect, &view, c.x0, c.y0,
            c.x1 - c.x0 + 1, c.y1 - c.y0 + 1);
    }
}

static void repaint(void)
{
    if (game_mode) {

        /* Nothing is in front of a game-mode page, so the window
         * manager's clip region has to be OUT of the way -- left in
         * place it would clip drawing to wherever this app's window
         * happens to sit on the desktop behind. */
        z_gfx_clear_visible();
        z_gfx_blit_scissor_reset();

        relayout();
        pt_draw_all(&layout_rect, &view);

        z_game_set_view((uint32_t)((game_front ^ 1) * Z_GAME_VIEW_W), 0);
        z_game_wait_frame();
        game_front ^= 1;

        return;
    }

    relayout();
    pt_draw_all(&layout_rect, &view);
}

/* The message line alone, drawn into the page that is ALREADY visible.
 *
 * For the thinking indicator. A full repaint would flip pages, and
 * flipping sixty times during one decision to update a line of text is
 * a lot of blitting for a word. Text on a cleared strip cannot tear in
 * any way a person would see. */
static void repaint_message(void)
{
    pt_layout_t t;

    if (game_mode) {
        z_gfx_clear_visible();
        z_gfx_blit_scissor_reset();
        pt_layout(&t, &view, game_front * Z_GAME_VIEW_W, 0,
            Z_GAME_VIEW_W, Z_GAME_VIEW_H);
    } else {
        z_clip_t c;
        z_win_content_rect(&win, &c);
        pt_layout(&t, &view, c.x0, c.y0, c.x1 - c.x0 + 1, c.y1 - c.y0 + 1);
    }

    if (t.ok) pt_draw_status(&t, &view);
}

/* -- game mode --------------------------------------------------------- */

static void enter_game_mode(void)
{
    if (!z_game_available()) {
        set_str(view.message, PT_MSG_LEN, "this bitstream has no game mode");
        puts("poker: no game mode -- rebuild the gateware with `GAME "
            "in rtl/boards.vh");
        return;
    }

    game_mode = true;
    game_front = 0;
    exit_armed = false;

    /* Both pages, before the mode change, so the first frame scanned
     * out is already this app's background rather than whatever the
     * desktop left behind. */
    z_gfx_clear_visible();
    z_gfx_blit_scissor_reset();
    z_fb_hw_fill_rect(0, 0, 640, 480, 0);

    z_game_set_enabled(true, false);

    repaint();
    repaint();       /* both pages, so a flip never shows a blank one */
}

static void leave_game_mode(void)
{
    uint32_t wm_pid;

    z_game_set_enabled(false, false);
    game_mode = false;

    /* Every window is still alive and still where it was, and this app
     * just drew over all of their pixels without any of them knowing.
     * wm repairs damage it caused itself; this came from outside it,
     * so it has to be told. See Z_WM_REPAINT in zwm.h. */
    if (z_pid_lookup("wm0", &wm_pid))
        z_msg_new_send(wm_pid, Z_WM_REPAINT, 0, z_obj_uint32(0));

    repaint();
}

/* -- the opponents' turn ------------------------------------------------ */

static uint32_t poll_count;

static bool ai_poll(void *user)
{
    (void)user;

    /* allow_input false: window events and Escape only. A command run
     * from here would change the game the estimate is being computed
     * from. */
    pump(false);

    /* Roughly every eight poll intervals. Often enough that the
     * indicator does not look frozen, rare enough that redrawing it is
     * not where the time goes.
     *
     * Skipped entirely while wm has this window's clip FROZEN for a
     * drag: docs/window_manager.md asks a periodic render to draw
     * nothing and change nothing until the thaw, and a draw attempted
     * meanwhile only earns a redundant redraw when the region comes
     * back. The opponent keeps thinking either way. */
    if ((++poll_count & 7) == 0 && !z_win_frozen(&win)) repaint_message();

    return !cancel_think && running;
}

static void ai_turn(void)
{
    int seat = game.actor;
    int action, i;
    int32_t to, before;

    if (seat < 0 || seat == view.hero) return;

    thinking = true;
    cancel_think = false;
    poll_count = 0;
    view.thinking = true;
    view.thinking_seat = seat;

    repaint();

    pk_ai_set_level(&ai[seat], view.level);
    pk_ai_decide(&ai[seat], &game, seat, &action, &to);

    thinking = false;
    view.thinking = false;

    if (!running) return;

    before = game.seat[seat].bet;

    if (!pk_legal(&game, seat, action, to)) {
        /* Should be impossible -- tests/ai_test.c plays thousands of
         * hands at every level in every variant asserting exactly
         * this. Handled anyway, because the alternative if it ever
         * does happen is a hand that stops with nobody to act and no
         * way to tell why. */
        puts("poker: an opponent proposed an illegal action");
        action = PK_FOLD;
        to = 0;
    }

    for (i = 0; i < game.nseats; i++)
        pk_ai_observe(&ai[i], &game, seat, action);

    pk_act(&game, action, to);
    tag_action(seat, action, before);

    repaint();
}

static void ai_draw_turn(void)
{
    int seat = game.actor;
    uint8_t idx[PK_MAX_HOLE];
    int n = 0;

    if (seat < 0 || seat == view.hero) return;

    /* A crude draw: throw away everything below a pair.
     *
     * Deliberately not a rollout. Choosing a discard well needs the
     * equity of every subset of the hand, which is 31 estimates where
     * a betting decision needs one, and five-card draw is the only
     * variant that would use it. The opponents play the betting well
     * and draw plainly, which is a reasonable place to stop and is
     * recorded here rather than left to be discovered. */
    {
        const pk_seat_t *s = &game.seat[seat];
        int rc[Z_NRANKS];
        int i;
        for (i = 0; i < Z_NRANKS; i++) rc[i] = 0;
        for (i = 0; i < s->nhole; i++) rc[Z_RANK(s->hole[i])]++;
        for (i = 0; i < s->nhole; i++)
            if (rc[Z_RANK(s->hole[i])] < 2 && n < 3) idx[n++] = (uint8_t)i;
    }

    pk_draw(&game, idx, n);
    set_str(view.tag[seat], PT_TAG_LEN, n ? "draws" : "pat");
    repaint();
}

/* -- hands -------------------------------------------------------------- */

static void announce_result(void)
{
    int i, best = -1;
    char line[PT_MSG_LEN];

    view.reveal = true;

    for (i = 0; i < game.nseats; i++)
        if (game.seat[i].won > 0 &&
            (best < 0 || game.seat[i].won > game.seat[best].won)) best = i;

    if (best < 0) { set_str(view.message, PT_MSG_LEN, "hand over"); return; }

    line[0] = '\0';
    {
        int k = 0, j;
        const char *nm = view.name[best];
        for (j = 0; nm[j] && k < PT_MSG_LEN - 1; j++) line[k++] = nm[j];
        {
            static const char mid[] = " wins ";
            char num[12];
            for (j = 0; mid[j] && k < PT_MSG_LEN - 1; j++) line[k++] = mid[j];
            pt_num(game.seat[best].won, num);
            for (j = 0; num[j] && k < PT_MSG_LEN - 1; j++) line[k++] = num[j];
        }
        if (game.showdown) {
            uint32_t val = pk_seat_value(&game, best, 0);
            if (val != PK_EVAL_NONE) {
                char nm2[40];
                int j2;
                pk_eval_name(val, nm2, sizeof nm2);
                if (k < PT_MSG_LEN - 7) {
                    static const char with[] = " with ";
                    for (j2 = 0; with[j2] && k < PT_MSG_LEN - 1; j2++)
                        line[k++] = with[j2];
                    for (j2 = 0; nm2[j2] && k < PT_MSG_LEN - 1; j2++)
                        line[k++] = nm2[j2];
                }
            }
        }
        line[k] = '\0';
    }

    set_str(view.message, PT_MSG_LEN, line);
}

/* -- the shared bank ------------------------------------------------------
 *
 * Poker is the only game here whose boundary is not a round. A stack
 * rises and falls across many hands, so the bank is settled when the
 * TABLE ends -- a buy-in and a cash-out, which is how a card room
 * works and is also the only boundary that exists.
 *
 * NOTHING LEAVES /user/casino.dat WHEN YOU SIT DOWN. The buy-in is
 * recorded and the chips stay in the bank until the table is over, so a
 * window closed mid-hand costs nothing -- the same arrangement
 * sw/apps/craps uses for the same reason. What the player is worth is
 * the bank plus whatever is in front of them, and the status line shows
 * both.
 */
static void load_bank(void)
{
    zbank_t b;
    int rv = zbank_load(&b);

    view.bank = b.chips;

    if (rv == ZBANK_CORRUPT) {
        set_str(view.message, PT_MSG_LEN,
            "bank file damaged -- `buyin` to start over");
        puts("poker: /user/casino.dat is damaged; not writing to it");
    }
}

/* Cashes out. Whatever is in front of the hero goes back, less what was
 * taken out to sit down -- so a table that broke even moves nothing. */
static void leave_table(void)
{
    int32_t delta, chips;
    int rv;

    if (view.buyin <= 0) return;

    delta = game.seat[view.hero].stack - view.buyin;
    view.buyin = 0;

    /* zbank_adjust() re-reads before it writes, so a win in another
     * game meanwhile is not clobbered. */
    rv = zbank_adjust("poker", delta, &chips);

    if (rv == ZBANK_OK) view.bank = chips;
    else view.bank += delta;
}

static void new_game(void)
{
    int i;

    /* Cash out the table being replaced before funding the next one. */
    leave_table();

    if (view.pending_variant) { variant = view.pending_variant;
        view.pending_variant = 0; }
    if (view.pending_seats) { seats = view.pending_seats;
        view.pending_seats = 0; }
    if (view.pending_limit >= 0) limit_mode = view.pending_limit;

    if (seats > variant->max_seats) seats = variant->max_seats;

    pk_game_init(&game, variant, seats, START_STACK, SMALL_BLIND, BIG_BLIND);
    pk_game_set_limit(&game, limit_mode);

    /* THE OPPONENTS ARE THE HOUSE'S MONEY; the hero's is not.
     *
     * pk_game_init() seats everyone with the same stack, which is right
     * for the other seats -- they are not drawing on /user/casino.dat.
     * The hero buys in for what the bank can cover, up to a full stack.
     *
     * A short buy-in is a real disadvantage at a table where everyone
     * else has a full stack, which is exactly what being short of money
     * in a card room is, so it is left as it falls rather than scaled
     * away. */
    view.buyin = view.bank < START_STACK ? view.bank : START_STACK;
    if (view.buyin < 0) view.buyin = 0;
    game.seat[view.hero].stack = view.buyin;

    for (i = 0; i < PK_MAX_SEATS; i++) {
        pk_ai_init(&ai[i], view.level);
        /* The reads are cleared with the table, and only with the
         * table. A read that did not survive the hand it was taken in
         * would be worth nothing. */
    }

    view.hand_no = 0;
    view.g = &game;
    view.hero = 0;
}

static void new_hand(void)
{
    int i, alive = 0;

    for (i = 0; i < game.nseats; i++)
        if (game.seat[i].stack > 0) alive++;

    if (game.seat[view.hero].stack <= 0) {
        /* Busted, so the table IS over -- settle now rather than
         * waiting for `new`. Leaving it open would let somebody walk
         * away from a loss by closing the window, which is exactly the
         * hole the other games do not have. */
        leave_table();
        set_str(view.message, PT_MSG_LEN,
            view.bank > 0 ? "you are out -- type new"
                          : "out of chips -- borrow at the casino");
        return;
    }

    if (alive < 2) {
        set_str(view.message, PT_MSG_LEN, "you have it all -- type new");
        return;
    }

    for (i = 0; i < PK_MAX_HOLE; i++) view.discard[i] = false;
    clear_tags();
    view.reveal = false;
    view.bet_to = 0;
    view.hand_no++;

    pk_hand_begin(&game);

    input_bet_step(&view, 0);
    set_str(view.message, PT_MSG_LEN, "your move -- F1 for help");
}

/* -- the odds command ---------------------------------------------------- */

static void show_odds(void)
{
    int32_t eq;
    char line[PT_MSG_LEN];
    char num[12];
    int k = 0, j;
    static const char pre[] = "you win about ";
    static const char post[] = "% of the time from here";

    view.want_odds = false;

    if (game.seat[view.hero].nhole == 0) {
        set_str(view.message, PT_MSG_LEN, "no hand to price");
        return;
    }

    thinking = true;
    cancel_think = false;
    poll_count = 0;
    eq = pk_ai_equity_game(&ai[view.hero], &game, view.hero, 4000);
    thinking = false;

    for (j = 0; pre[j] && k < PT_MSG_LEN - 1; j++) line[k++] = pre[j];
    pt_num((eq + 5) / 10, num);
    for (j = 0; num[j] && k < PT_MSG_LEN - 1; j++) line[k++] = num[j];
    for (j = 0; post[j] && k < PT_MSG_LEN - 1; j++) line[k++] = post[j];
    line[k] = '\0';

    set_str(view.message, PT_MSG_LEN, line);
}

/* -- actions ------------------------------------------------------------- */

static void do_action(pi_action_t a)
{
    switch (a) {

    case PI_QUIT:
        running = false;
        break;

    case PI_GAME_MODE:
        if (game_mode) leave_game_mode();
        else enter_game_mode();
        break;

    case PI_NEWGAME:
        new_game();
        new_hand();
        repaint();
        break;

    case PI_NEWHAND:
        new_hand();
        repaint();
        break;

    case PI_ACTED:
        input_bet_step(&view, 0);
        repaint();
        break;

    case PI_STATUS:
        /* Two rows of text. relayout() first because in a window that
         * is what reloads the clip. */
        relayout();
        if (layout_rect.ok) pt_draw_status(&layout_rect, &view);
        break;

    case PI_REDRAW:
        if (view.want_odds) show_odds();
        repaint();
        break;

    default:
        break;
    }
}

/* -- messages ------------------------------------------------------------ */

static void handle_key(uint32_t packed, bool allow_input)
{
    uint32_t sym = Z_WM_UNPACK_KEY_KEYSYM(packed);
    bool pressed = Z_WM_UNPACK_KEY_PRESSED(packed) != 0;

    if (game_mode) {
        /* Escape leaves full screen -- but only once it has been
         * RELEASED. Entering game mode with the `game` command and
         * then pressing Escape is fine, but entering it by any route
         * that leaves a key still held would otherwise see the key-up
         * for a key that was already down and leave immediately. Same
         * technique, and the same reason, as sw/apps/chip8 and
         * sw/apps/chess. */
        if (sym == 0x1b) {
            if (!pressed) exit_armed = true;
            else if (exit_armed) leave_game_mode();
            return;
        }
    }

    if (!pressed) return;

    if (thinking) {
        /* The only key that means anything while an opponent is
         * thinking. */
        if (sym == 0x1b) {
            cancel_think = true;
            set_str(view.message, PT_MSG_LEN, "hurrying it up...");
        }
        return;
    }

    if (!allow_input) return;

    if (sym == Z_KEY_F2) { do_action(PI_GAME_MODE); return; }

    if (sym == Z_KEY_F1) {
        /* A rolling tour of the help text on the message line. The
         * table is too narrow for all of it at once and a modal help
         * window would be a second window to service. */
        static int hl;
        const char *s = input_help_line(hl++);
        if (!s) { hl = 0; s = input_help_line(hl++); }
        set_str(view.message, PT_MSG_LEN, s);
        repaint();
        return;
    }

    do_action(input_key(&view, sym));
}

static void handle_mouse(uint32_t packed)
{
    static bool was_down;
    bool down = (Z_WM_UNPACK_MOUSE_BUTTONS(packed) & Z_MOUSE_BTN_LEFT) != 0;
    int x = (int)Z_WM_UNPACK_MOUSE_X(packed);
    int y = (int)Z_WM_UNPACK_MOUSE_Y(packed);

    /* On the press edge only. Mouse messages are coalesced and repeat
     * while a button is held, so acting on the level would fold, deal
     * and fold again several times per click. */
    if (down && !was_down) {
        relayout();
        do_action(input_click(&view, &layout_rect, x, y));
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
            /* Ignored entirely while an opponent is thinking: a click
             * can fold or deal, and the estimate running is computed
             * from the state a click would change. */
            if (allow_input && !game_mode && msg.obj.type == Z_UINT32)
                handle_mouse(msg.obj.val.uint32);
            break;

        /* The part of this window not covered by the windows in front
         * of it. The ack this sends is not optional -- wm waits for it
         * when a region narrows. */
        case Z_WM_SET_CLIP:
            if (!z_win_apply_clip(&win, &msg.obj))
                puts("poker: bad clip region message");
            break;

        case Z_WM_REDRAW:
            if (msg.obj.type != Z_UINT32) break;
            if (z_win_redraw_id(msg.obj.val.uint32) != win.id) break;
            z_win_apply_redraw(&win, msg.obj.val.uint32);
            /* Answered even in game mode, and answered even while an
             * opponent is thinking. The window is still a window as
             * far as wm is concerned, and an unanswered redraw is what
             * stalls the desktop. */
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

/* -- startup -------------------------------------------------------------- */

static void apply_launch_arg(void)
{
    char arg[Z_WM_ARG_MAX];
    const pk_variant_t *v;

    /* Claimed whether or not it is used: leaving one pending would
     * hand it to whatever the person opens next. */
    if (!z_launch_arg_take(arg, sizeof(arg))) return;
    if (!arg[0]) return;

    /* A bare digit is a difficulty; anything else is tried as a
     * variant name, so `run poker 6` and `run poker stud7` both do the
     * obvious thing. */
    if (arg[0] >= '1' && arg[0] <= '8' && !arg[1]) {
        view.level = arg[0] - '0';
        return;
    }

    v = pk_variant_find(arg);
    if (v) variant = v;
    else puts("poker: ignoring an argument it did not recognise");
}

int main(void)
{
    puts("poker: starting");

    zg_rng_use_system();

    /* Before the first table is seated: new_game() buys in from this. */
    load_bank();
    pk_ai_set_clock(now_ms);
    pk_ai_set_poll(ai_poll, 0);

    view.level = 4;
    view.hero = 0;
    view.pending_limit = -1;
    set_names();

    apply_launch_arg();

    if (z_win_create_flags(&win, "poker", WIN_W, WIN_H, -1, -1,
        Z_WIN_FLAG_CLOSE_ICON | Z_WIN_FLAG_CLOSE_KILLS_OWNER |
        Z_WIN_FLAG_RESIZABLE) != Z_OK) {
        puts("poker: failed to create window -- is wm running?");
        return 1;
    }

    pt_init();
    new_game();
    new_hand();
    repaint();

    while (running) {

        pump(true);

        if (!running) break;

        if (game.phase == PK_PHASE_BETTING && game.actor != view.hero) {
            ai_turn();
            continue;
        }

        if (game.phase == PK_PHASE_DRAW && game.actor != view.hero) {
            ai_draw_turn();
            continue;
        }

        if (game.phase == PK_PHASE_COMPLETE && !view.reveal) {
            announce_result();
            repaint();
            continue;
        }

        /* Nothing to do until something arrives. The timeout exists
         * only so a stuck message never wedges the app entirely; it is
         * not a polling interval, because nothing here changes on its
         * own. */


        /* THE BANK CHANGES UNDER THIS WINDOW. sw/apps/casino can hand
         * out a loan while a table is open, and another game can win
         * while this one is idle -- /user/casino.dat is shared. Once a
         * second, and only when the figure has actually moved. */
        {
            uint32_t now = z_uptime_ticks();
            if ((uint32_t)(now - bank_seen) >= Z_TICK_HZ) {
                zbank_t b;
                bank_seen = now;
                if (zbank_load(&b) == ZBANK_OK && b.chips != view.bank) {
                    view.bank = b.chips;
                    repaint();
                }
            }
        }

        z_proc_wait(Z_TICK_HZ);
    }

    if (game_mode) leave_game_mode();

    /* Cash out on the way out, so closing the window banks the table
     * rather than discarding it. */
    leave_table();

    z_win_destroy(&win);
    puts("poker: bye");

    return 0;
}
