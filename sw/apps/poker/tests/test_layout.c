/*
 * Zeitlos poker -- layout and drawing geometry, unattended.
 *
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * The half that runs without anybody looking. tests/render.c is the
 * half that gets looked at, and sw/common/tests/zrender.h's header
 * explains why both are needed: a geometry assertion can only check a
 * relationship somebody thought to write down.
 *
 * The assertion that earns its keep most here is the last one --
 * NOTHING IS DRAWN OUTSIDE THE CONTENT RECTANGLE. sw/apps/logic
 * shipped its panel wrong three times, and all three were this: window
 * coordinates used where content coordinates were needed, widgets
 * drawn across the frames meant to contain them, and absolute
 * coordinates passed to helpers that wanted relative ones. Checking
 * every individual rectangle cannot catch the general case; drawing
 * the whole thing and looking for ink where there should be none can.
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "poker_shim.h"

#include "../table_ui.h"

static int checks = 0;
static int failures = 0;

static void check(bool ok, const char *what)
{
    checks++;
    if (ok) return;
    failures++;
    printf("FAIL: %s\n", what);
}

static void check_eq(long got, long want, const char *what)
{
    checks++;
    if (got == want) return;
    failures++;
    printf("FAIL: %s -- got %ld, want %ld\n", what, got, want);
}

static pk_game_t game;
static pt_view_t view;

static void names(int n)
{
    int i;
    for (i = 0; i < n; i++) {
        view.name[i][0] = (char)('A' + i);
        view.name[i][1] = '\0';
    }
}

static void setup(const pk_variant_t *v, int nseats)
{
    memset(&view, 0, sizeof view);
    pk_rng_seed(7);
    pk_game_init(&game, v, nseats, 1000, 5, 10);
    pk_hand_begin(&game);
    view.g = &game;
    view.hero = 0;
    view.level = 4;
    view.hand_no = 3;
    view.bet_to = 40;
    names(nseats);
    strcpy(view.message, "a message");
    strcpy(view.cmd, "lev");
    view.cmd_len = 3;
}

static bool inside(const pt_layout_t *L, int x, int y)
{
    return x >= L->clip.x0 && x <= L->clip.x1 &&
           y >= L->clip.y0 && y <= L->clip.y1;
}

/* -- what actually lands on the framebuffer ---------------------------- */

static bool opened = false;
static z_win_t rwin;

static bool z_render_open_once(void)
{
    if (opened) return true;
    if (!z_render_open(&rwin, 320, 240)) return false;
    opened = true;
    pt_init();
    return true;
}

static void z_render_clear_all(void)
{
    int x, y;
    for (y = 0; y < Z_SCREEN_H; y++)
        for (x = 0; x < Z_SCREEN_W; x++)
            z_fb_set_pixel(x, y, 0, NULL);
}

static int ink_in(int x0, int y0, int w, int h)
{
    int x, y, n = 0;
    for (y = y0; y < y0 + h; y++)
        for (x = x0; x < x0 + w; x++)
            if (shim_get(x, y)) n++;
    return n;
}

/* -- the layout is a pure function ----------------------------------- */

static void test_pure(void)
{
    pt_layout_t a, b;

    setup(&pk_variant_holdem, 6);

    pt_layout(&a, &view, 40, 30, 316, 225);
    pt_layout(&b, &view, 40, 30, 316, 225);
    check(memcmp(&a, &b, sizeof a) == 0,
        "the same rectangle produces the same layout");

    /* And it must actually MOVE when the origin does. A layout that
     * ignored its origin would pass every relative check below and
     * draw the whole table on top of the desktop. */
    pt_layout(&b, &view, 50, 30, 316, 225);
    check(b.hero_x == a.hero_x + 10, "the layout follows its origin in x");
    pt_layout(&b, &view, 40, 44, 316, 225);
    check(b.hero_y == a.hero_y + 14, "the layout follows its origin in y");
}

/* -- everything is where it should be -------------------------------- */

static void test_geometry(void)
{
    pt_layout_t L;
    int i;

    setup(&pk_variant_holdem, 6);
    pt_layout(&L, &view, 2, 13, 316, 225);

    check(L.ok, "a 316x225 window is big enough for a table");

    /* Stacked in the order a person reads them, with no two bands
     * overlapping. Each of these was a real risk: the bands are
     * computed bottom-up from the window's bottom edge, so an extra
     * pixel anywhere pushes one band into its neighbour. */
    check(L.status_y < L.opp_row_y[0], "status above the opponents");
    check(L.opp_row_y[0] + PT_SEAT_H <= L.opp_row_y[1],
        "the opponent rows do not overlap");
    check(L.opp_row_y[1] + PT_SEAT_H <= L.pot_y, "opponents above the pot");
    check(L.pot_y + 8 <= L.board_y, "the pot line is above the board");
    check(L.board_y + PK_ART_FULL_H <= L.hero_y,
        "the board is above the hero's cards");
    check(L.hero_y + PK_ART_FULL_H <= L.hero_info_y,
        "the hero's cards are above their chip count");
    check(L.hero_info_y + 8 <= L.btn[0].y, "the chip count is above the buttons");
    check(L.btn[0].y + PT_BTN_H <= L.msg_y, "the buttons are above the message");
    check(L.msg_y + 8 <= L.cmd_y, "the message is above the command line");
    check(L.cmd_y + 8 <= L.clip.y1 + 1, "the command line fits");

    /* Buttons: in order, not overlapping, all inside. */
    for (i = 0; i < PT_NBTN; i++) {
        check(inside(&L, L.btn[i].x, L.btn[i].y), "a button starts inside");
        check(inside(&L, L.btn[i].x + L.btn[i].w - 1,
            L.btn[i].y + L.btn[i].h - 1), "a button ends inside");
    }
    for (i = 1; i < 4; i++)
        check(L.btn[i].x >= L.btn[i - 1].x + L.btn[i - 1].w,
            "the action buttons do not overlap");
    check(L.btn[PT_BTN_LESS].x + L.btn[PT_BTN_LESS].w <= L.amount_x,
        "the minus button is left of the amount");
    check(L.amount_x + L.amount_w <= L.btn[PT_BTN_MORE].x,
        "the amount is left of the plus button");
    check(L.btn[3].x + L.btn[3].w <= L.btn[PT_BTN_LESS].x,
        "the action buttons clear the amount controls");

    /* Cards fit across the width. */
    check(L.board_x >= L.clip.x0, "the board starts inside");
    check(L.board_x + L.board_slots * (PK_ART_FULL_W + 3) - 3 <= L.clip.x1 + 1,
        "the whole board fits across");
    check(L.hero_x >= L.clip.x0, "the hero's cards start inside");
    check(L.hero_x + L.hero_slots * (PK_ART_FULL_W + 2) - 2 <= L.clip.x1 + 1,
        "the hero's cards fit across");

    /* Opponent cells tile the width without overlapping. */
    for (i = 0; i < L.opp_rows; i++) {
        int total = L.opp_row_n[i] * L.opp_cell_w[i];
        check(total <= L.w, "an opponent row fits across the width");
        check(L.opp_cell_w[i] >= PK_ART_MINI_W, "a seat is at least a card wide");
    }
}

/* -- seven-card stud is the hard case -------------------------------- */

static void test_stud_fits(void)
{
    pt_layout_t L;
    int fan;

    setup(&pk_variant_stud7, 6);
    pt_layout(&L, &view, 2, 13, 316, 225);

    check(L.ok, "a six-handed stud table fits in a window");
    check_eq(L.hero_slots, 7, "the hero has room for seven cards");

    /* The whole reason the mini cards overlap. Seven cards at the fan
     * stride must fit inside one seat cell, or a full stud hand runs
     * into the next player. */
    fan = 6 * PK_ART_MINI_STRIDE + PK_ART_MINI_W;
    check(fan <= L.opp_cell_w[0] - 4,
        "a seven-card fan fits inside a seat cell");

    /* And the hero's seven full-size cards fit across the table. */
    check(L.hero_x >= L.clip.x0, "seven full cards start inside");
    check(L.hero_x + 7 * (PK_ART_FULL_W + 2) - 2 <= L.clip.x1 + 1,
        "seven full cards fit across");

    /* Eight-handed is the worst case the engine allows. */
    setup(&pk_variant_stud7, 8);
    pt_layout(&L, &view, 0, 0, 320, 240);
    check(L.ok, "an eight-handed stud table fits a game-mode page");
    check_eq(L.opp_rows, 2, "seven opponents take two rows");
    check_eq(L.opp_row_n[0] + L.opp_row_n[1], 7, "and all seven are placed");
}

/* -- both modes -------------------------------------------------------- */

static void test_both_modes(void)
{
    pt_layout_t win, page;

    setup(&pk_variant_holdem, 6);

    /* A window's content rectangle and a game-mode page are different
     * rectangles, and the whole design rests on one renderer serving
     * both. Neither may be a special case of the other. */
    pt_layout(&win, &view, 2, 13, 316, 225);
    pt_layout(&page, &view, 0, 0, 320, 240);

    check(win.ok && page.ok, "both modes produce a usable layout");
    check_eq(win.opp_rows, page.opp_rows,
        "the same table has the same seat arrangement in both modes");
    check_eq(win.board_slots, page.board_slots,
        "and the same number of board slots");

    /* The page is taller, so everything bottom-anchored sits lower. */
    check(page.cmd_y > win.cmd_y - 13,
        "the page's command line is at its own bottom");
}

/* -- too small --------------------------------------------------------- */

static void test_too_small(void)
{
    pt_layout_t L;

    setup(&pk_variant_holdem, 6);

    pt_layout(&L, &view, 0, 0, 120, 225);
    check(!L.ok, "a very narrow window is refused");

    pt_layout(&L, &view, 316, 0, 316, 60);
    check(!L.ok, "a very short window is refused");

    /* Refused must still be DRAWABLE. An app that resizes down to
     * nothing must not crash, and the note it draws has to stay inside
     * the window like everything else. */
    if (!z_render_open_once()) return;
    z_render_clear_all();
    pt_layout(&L, &view, 40, 40, 100, 50);
    pt_draw_all(&L, &view);
    check(true, "drawing a refused layout does not crash");
}

/* -- hit testing ------------------------------------------------------- */

static void test_hits(void)
{
    pt_layout_t L;
    int i, x, y;

    setup(&pk_variant_holdem, 6);
    pt_layout(&L, &view, 2, 13, 316, 225);

    for (i = 0; i < PT_NBTN; i++) {
        int cx = L.btn[i].x + L.btn[i].w / 2;
        int cy = L.btn[i].y + L.btn[i].h / 2;
        check_eq(pt_button_at(&L, cx, cy), i, "a button's centre hits it");
        check_eq(pt_button_at(&L, L.btn[i].x, L.btn[i].y), i,
            "and so does its top-left corner");
    }

    check_eq(pt_button_at(&L, L.btn[0].x, L.btn[0].y - 1), -1,
        "a point above the buttons hits nothing");
    check_eq(pt_button_at(&L, L.clip.x1, L.clip.y0), -1,
        "and neither does the far corner");

    /* A card's hit box has to agree with where it was DRAWN, or a
     * click lands on the card next to the one under the pointer --
     * which in five-card draw means discarding the wrong card, with
     * nothing on screen to suggest anything went wrong. */
    for (i = 0; i < game.seat[0].nhole; i++) {
        pt_hero_card_xy(&L, i, &x, &y);
        check_eq(pt_hero_card_at(&L, &view, x, y), i,
            "a hero card's corner hits it");
        check_eq(pt_hero_card_at(&L, &view, x + PK_ART_FULL_W / 2,
            y + PK_ART_FULL_H / 2), i, "and so does its middle");
    }

    check_eq(pt_hero_card_at(&L, &view, L.hero_x, L.hero_y - 1), -1,
        "a point above the hero's cards hits none of them");

    /* THE GAP BETWEEN TWO CARDS MUST HIT NEITHER.
     *
     * Checking only points inside cards cannot distinguish the right
     * stride from a wrong one: with the cards two pixels closer
     * together every point inside card i is still inside card i, and a
     * hit test built on the wrong stride passes. The gap is the only
     * place the two disagree, and in five-card draw disagreeing means
     * discarding the card next to the one that was clicked. */
    for (i = 1; i < game.seat[0].nhole; i++) {
        pt_hero_card_xy(&L, i, &x, &y);
        check_eq(pt_hero_card_at(&L, &view, x - 1, y + PK_ART_FULL_H / 2), -1,
            "the gap between two cards hits neither");
    }
}

static void test_drawing(void)
{
    pt_layout_t L;
    int x, y, outside = 0;
    int i, empty_seats = 0;

    if (!z_render_open_once()) {
        printf("test_layout: cannot map the framebuffer here "
            "(Linux/x86-64 only) -- skipping the drawing checks\n");
        return;
    }

    setup(&pk_variant_stud7, 6);
    view.reveal = true;

    /* Deliberately NOT at the origin. A renderer that confuses window
     * coordinates with content coordinates draws in the right shape at
     * the wrong place, and only an offset origin shows it. */
    z_render_clear_all();
    pt_layout(&L, &view, 60, 40, 316, 225);
    pt_draw_all(&L, &view);

    /* THE assertion. Nothing outside the content rectangle may have
     * been touched -- see this file's header for the three shipped
     * bugs this shape of check exists for. */
    for (y = 0; y < Z_SCREEN_H; y++)
        for (x = 0; x < Z_SCREEN_W; x++)
            if (shim_get(x, y) && !inside(&L, x, y)) outside++;

    check_eq(outside, 0, "nothing is drawn outside the content rectangle");

    /* And something WAS drawn, or the check above passes trivially. */
    check(ink_in(L.clip.x0, L.clip.y0, L.w, L.h) > 2000,
        "the table is actually drawn");

    /* Every band has ink in it. A band left blank by an arithmetic
     * slip is invisible to the overlap assertions, which only ever
     * said the bands did not collide. */
    check(ink_in(L.ox, L.status_y, L.w, 8) > 0, "the status line has content");
    check(ink_in(L.ox, L.pot_y, L.w, 8) > 0, "the pot line has content");
    check(ink_in(L.hero_x, L.hero_y, L.w - (L.hero_x - L.ox),
        PK_ART_FULL_H) > 0, "the hero's cards are drawn");
    check(ink_in(L.ox, L.btn[0].y, L.w, PT_BTN_H) > 0, "the buttons are drawn");
    check(ink_in(L.ox, L.msg_y, L.w, 8) > 0, "the message line has content");
    check(ink_in(L.ox, L.cmd_y, L.w, 8) > 0, "the command line has content");

    /* EVERY opponent seat must have something in it. Five opponents
     * and four drawn is the kind of thing that looks like a design
     * decision on a screenshot. */
    for (i = 0; i < L.opp_rows; i++) {
        int col;
        for (col = 0; col < L.opp_row_n[i]; col++)
            if (ink_in(L.ox + col * L.opp_cell_w[i], L.opp_row_y[i],
                L.opp_cell_w[i] - 2, PT_SEAT_H) == 0) empty_seats++;
    }
    check_eq(empty_seats, 0, "every opponent seat is drawn");

    /* A DRAWN CARD MUST MATCH ITS TILE BIT FOR BIT.
     *
     * Every pixel of the hero's first card is compared against the
     * generated art. That catches the whole class of blit mistakes at
     * once, and one in particular that nothing else here could:
     * z_fb_hw_blit_mem() reads its source LEAST significant bit
     * leftmost, which is the opposite of the MSB-first order z_font_t
     * and zicon.h use (see cards.h). Get it backwards and every card
     * is MIRRORED -- still exactly the right size, still in exactly
     * the right place, still passing every geometry assertion in this
     * file.
     *
     * An earlier version of this check drew one card over another and
     * compared against a clean draw, meaning to prove the tile was
     * opaque. It proved nothing, because pt_draw_all() clears the
     * content area before it draws anything, so the two cases were
     * identical by construction -- and an ORing blit passed it. On a
     * cleared background OR and COPY genuinely do agree; the copying
     * shim still matches the hardware, but it is not what this test
     * was testing. */
    {
        int px, py, diff = 0;
        uint8_t card;
        const uint32_t *tile;

        setup(&pk_variant_holdem, 4);
        pt_layout(&L, &view, 60, 40, 316, 225);

        z_render_clear_all();
        pt_draw_all(&L, &view);

        card = game.seat[0].hole[0];
        tile = &pk_card_full[(int)card * PK_ART_FULL_H];

        for (py = 0; py < PK_ART_FULL_H; py++)
            for (px = 0; px < PK_ART_FULL_W; px++) {
                int want = (int)((tile[py] >> px) & 1u);
                if (shim_get(L.hero_x + px, L.hero_y + py) != want) diff++;
            }

        check_eq(diff, 0, "a drawn card matches its tile exactly");

        /* And it is NOT symmetric, so the check above can actually
         * tell a mirrored card from a correct one. A tile that
         * happened to be its own mirror image would make the
         * comparison vacuous. */
        {
            int asym = 0;
            for (py = 0; py < PK_ART_FULL_H; py++)
                for (px = 0; px < PK_ART_FULL_W; px++)
                    if (((tile[py] >> px) & 1u) !=
                        ((tile[py] >> (PK_ART_FULL_W - 1 - px)) & 1u)) asym++;
            check(asym > 0, "the card art is not its own mirror image");
        }
    }

    /* A HALF-CLIPPED CARD MUST SHOW ITS RIGHT HALF, NOT ITS LEFT.
     *
     * When another window covers part of this one, wm narrows the
     * visible region and a card at the edge is drawn partially. The
     * blitter is told a smaller rectangle, and the SOURCE origin has
     * to move with it -- clamping only the destination slides the
     * wrong part of the card into view, so a card cut in half would
     * show its middle where its right half belongs.
     *
     * Nothing else in this file ever clips a card, because the layout
     * works hard to keep them all inside. So this narrows the clip by
     * hand, which is exactly what an occluded window does. */
    {
        static uint8_t whole[PK_ART_FULL_W * PK_ART_FULL_H];
        static uint8_t part[PK_ART_FULL_W * PK_ART_FULL_H];
        const int cut = 9;
        int px, py, n, diff = 0;

        setup(&pk_variant_holdem, 4);
        pt_layout(&L, &view, 60, 40, 316, 225);

        z_render_clear_all();
        pt_draw_all(&L, &view);
        n = 0;
        for (py = 0; py < PK_ART_FULL_H; py++)
            for (px = 0; px < PK_ART_FULL_W; px++)
                whole[n++] = (uint8_t)shim_get(L.hero_x + px, L.hero_y + py);

        z_render_clear_all();
        L.clip.x0 = L.hero_x + cut;
        pt_draw_all(&L, &view);
        n = 0;
        for (py = 0; py < PK_ART_FULL_H; py++)
            for (px = 0; px < PK_ART_FULL_W; px++)
                part[n++] = (uint8_t)shim_get(L.hero_x + px, L.hero_y + py);

        /* Only the columns that survived the cut are compared. The
         * ones before it should be blank and are checked separately. */
        for (py = 0; py < PK_ART_FULL_H; py++) {
            for (px = cut; px < PK_ART_FULL_W; px++) {
                int k = py * PK_ART_FULL_W + px;
                if (whole[k] != part[k]) diff++;
            }
            for (px = 0; px < cut; px++)
                if (part[py * PK_ART_FULL_W + px]) diff++;
        }

        check_eq(diff, 0, "a clipped card shows the correct part of itself");
    }

    /* A DISABLED LABEL MUST BE DRAWN WITH THE SAME PIXELS AS AN
     * ENABLED ONE.
     *
     * There is no grey on a 1bpp display: a dither is black pixels and
     * white pixels, so a glyph drawn over one in either colour loses
     * half its pixels to the background and stops being a shape. Two
     * versions of this shipped wrong -- white text on a light stipple,
     * then black text on a heavy one -- because in a 2x render the
     * second looked faint rather than absent.
     *
     * "Faint but present" in a render is "unreadable" on the hardware,
     * and no amount of looking at a PNG settles it. So this asserts
     * the only thing that actually guarantees legibility: the label's
     * pixels are identical either way, and the shading is somewhere
     * else. */
    {
        /* Sized from the button's real dimensions. The first version
         * of this was [64 * 8] while the loops below walk
         * w * PT_BTN_H = 720 entries, so it overran by 208 bytes and
         * reported 24 phantom differing pixels -- a test failing on
         * its own bug, which is the most expensive kind. */
        static uint8_t on[128 * PT_BTN_H], off[128 * PT_BTN_H];
        int px, py, n, diff = 0, ink = 0;
        const int bh = PT_BTN_H;

        setup(&pk_variant_holdem, 4);
        pt_layout(&L, &view, 60, 40, 316, 225);

        /* pt_draw_actions() decides enabled-ness from the game, so the
         * two states are produced by asking it twice: once with the
         * hero to act and once with the hand over. */
        while (game.phase == PK_PHASE_BETTING && game.actor != view.hero) {
            pk_options_t o;
            pk_options(&game, game.actor, &o);
            pk_act(&game, o.can_check ? PK_CHECK : PK_CALL, 0);
        }

        z_render_clear_all();
        pt_draw_actions(&L, &view);
        n = 0;
        for (py = 0; py < bh; py++)
            for (px = 0; px < L.btn[PT_BTN_FOLD].w && px < 128; px++) {
                on[n] = (uint8_t)shim_get(L.btn[PT_BTN_FOLD].x + px,
                    L.btn[PT_BTN_FOLD].y + py);
                if (on[n]) ink++;
                n++;
            }
        check(ink > 0, "an enabled button draws something");

        /* Now make it not the hero's turn, so every button greys. */
        game.phase = PK_PHASE_COMPLETE;
        game.actor = -1;

        z_render_clear_all();
        pt_draw_actions(&L, &view);
        n = 0;
        for (py = 0; py < bh; py++)
            for (px = 0; px < L.btn[PT_BTN_FOLD].w && px < 128; px++) {
                off[n] = (uint8_t)shim_get(L.btn[PT_BTN_FOLD].x + px,
                    L.btn[PT_BTN_FOLD].y + py);
                n++;
            }

        /* Compare ONLY the glyph area. Everything around it is
         * supposed to differ -- that is the shading. */
        {
            int tw = 4 * 5;     /* "fold", five pixels a character */
            int tx = L.btn[PT_BTN_FOLD].w / 2 - tw / 2;
            int ty = (bh - 8) / 2;
            for (py = ty; py < ty + 8; py++)
                for (px = tx; px < tx + tw; px++) {
                    int k = py * L.btn[PT_BTN_FOLD].w + px;
                    if (on[k] != off[k]) diff++;
                }
        }

        check_eq(diff, 0,
            "a disabled label is drawn with the same pixels as an enabled one");

        /* And the button as a whole DID change, or the check above
         * would pass on a renderer that simply ignores the disabled
         * state. */
        {
            int total = 0;
            for (n = 0; n < bh * L.btn[PT_BTN_FOLD].w; n++)
                if (on[n] != off[n]) total++;
            check(total > 20, "but the button is visibly shaded");
        }
    }

    /* NOTHING IS DRAWN OUTSIDE A NARROWED CLIP EITHER.
     *
     * The whole-window check above never exercises the clamping in the
     * fills, the shade or the blits, because the layout keeps
     * everything inside by construction. A narrowed clip is what an
     * occluded window actually gets. */
    {
        int outside2 = 0;
        setup(&pk_variant_stud7, 6);
        z_render_clear_all();
        pt_layout(&L, &view, 60, 40, 316, 225);
        L.clip.x0 += 40;
        L.clip.y1 -= 30;
        pt_draw_all(&L, &view);
        for (y = 0; y < Z_SCREEN_H; y++)
            for (x = 0; x < Z_SCREEN_W; x++)
                if (shim_get(x, y) && !inside(&L, x, y)) outside2++;
        check_eq(outside2, 0, "nor outside a narrowed one");
    }

    /* Game mode uses the whole page, with no window inset. */
    z_render_clear_all();
    setup(&pk_variant_holdem, 4);
    pt_layout(&L, &view, 0, 0, 320, 240);
    pt_draw_all(&L, &view);

    outside = 0;
    for (y = 0; y < Z_SCREEN_H; y++)
        for (x = 0; x < Z_SCREEN_W; x++)
            if (shim_get(x, y) && (x >= 320 || y >= 240)) outside++;
    check_eq(outside, 0, "game mode stays inside its 320x240 page");
}

int main(void)
{
    printf("poker: layout tests\n");

    test_pure();
    test_geometry();
    test_stud_fits();
    test_both_modes();
    test_hits();
    test_too_small();
    test_drawing();

    printf("%d checks, %d failures\n", checks, failures);

    return failures ? 1 : 0;
}
