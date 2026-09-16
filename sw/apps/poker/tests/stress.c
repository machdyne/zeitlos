/*
 * Zeitlos poker -- the whole app, driven hard, under sanitizers.
 *
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 *   make stress
 *
 * -- why this exists --
 *
 * The other test binaries each exercise one layer. This one runs the
 * layers TOGETHER the way poker.c does: deal, let the opponents think
 * and act, draw the table after every single step, take the hero's
 * turn through the command line, settle, repeat -- across every
 * variant, every seat count and every betting structure.
 *
 * Built with -fsanitize=address,undefined, which is the point. A
 * one-byte overrun of a string buffer or a read one past the end of an
 * array is invisible on the host without it, and on the target it is
 * whatever happened to be next in memory: usually nothing, sometimes a
 * crash at startup with no window and no message.
 *
 * It is a separate target rather than part of `make test` because the
 * sanitizers make it slow, and because a build machine without them
 * should still be able to run the suite.
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "poker_shim.h"

#include "../input.h"

static pk_game_t game;
static pt_view_t view;
static pt_layout_t L;
static pk_ai_t ai[PK_MAX_SEATS];
static z_win_t win;

static long steps, hands, draws_done;

static uint32_t st = 12345;

static uint32_t rnd(uint32_t n)
{
    st ^= st << 13; st ^= st >> 17; st ^= st << 5;
    return n ? st % n : 0;
}

static void set_names(void)
{
    static const char *const who[PK_MAX_SEATS] = {
        "you", "Anna", "Bo", "Cy", "Dee", "Eli", "Fay", "Gus" };
    int i, k;
    for (i = 0; i < PK_MAX_SEATS; i++) {
        for (k = 0; who[i][k] && k < 7; k++) view.name[i][k] = who[i][k];
        view.name[i][k] = '\0';
    }
}

/* The same string-building poker.c does for a seat's tag, reproduced
 * here because it is a buffer this size and the sanitizer should see
 * it. */
static void tag_action(int seat, int action, int32_t before_bet)
{
    char t[PT_TAG_LEN];
    int32_t put;
    int i = 0, k;
    const char *base;

    switch (action) {
    case PK_FOLD:  base = "folds"; break;
    case PK_CHECK: base = "checks"; break;
    case PK_CALL:  base = "calls"; break;
    case PK_BET:   base = "bets"; break;
    case PK_RAISE: base = "raises"; break;
    default:       base = ""; break;
    }

    while (base[i] && i < PT_TAG_LEN - 1) { t[i] = base[i]; i++; }
    t[i] = '\0';

    put = game.seat[seat].bet - before_bet;
    if (put > 0) {
        char num[12];
        if (i < PT_TAG_LEN - 1) t[i++] = ' ';
        pt_num(put, num);
        for (k = 0; num[k] && i < PT_TAG_LEN - 1; k++) t[i++] = num[k];
        t[i] = '\0';
    }

    for (k = 0; t[k] && k < PT_TAG_LEN - 1; k++) view.tag[seat][k] = t[k];
    view.tag[seat][k] = '\0';
}

/* Every repaint poker.c would do, at a non-zero origin, in both the
 * windowed rectangle and the game-mode page. Drawing is where a bad
 * index turns into a write outside the framebuffer. */
static void draw_everything(void)
{
    pt_layout(&L, &view, 60, 40, 316, 225);
    pt_draw_all(&L, &view);
    pt_draw_actions(&L, &view);
    pt_draw_status(&L, &view);

    pt_layout(&L, &view, 0, 0, 320, 240);
    pt_draw_all(&L, &view);

    /* And an awkward size, since a window can be resized to one. */
    pt_layout(&L, &view, 5, 5, 240, 200);
    pt_draw_all(&L, &view);

    /* Back to the one the hit tests will use. */
    pt_layout(&L, &view, 60, 40, 316, 225);
}

static void hero_turn(void)
{
    pk_options_t o;
    char line[PT_CMD_LEN];
    int pick;

    pk_options(&game, view.hero, &o);
    pick = (int)rnd(10);

    if (pick < 2 && o.can_fold) { input_command(&view, "f"); return; }

    if (pick < 7 || (!o.can_bet && !o.can_raise)) {
        input_command(&view, "c");
        return;
    }

    /* A typed amount, through the same parser the person uses. */
    {
        int32_t span = o.max_to - o.min_to;
        int32_t to = o.min_to + (int32_t)rnd((uint32_t)span + 1);
        char num[12];
        int i = 0, k;
        line[i++] = 'r'; line[i++] = ' ';
        pt_num(to, num);
        for (k = 0; num[k] && i < PT_CMD_LEN - 1; k++) line[i++] = num[k];
        line[i] = '\0';
    }

    if (input_command(&view, line) != PI_ACTED) input_command(&view, "c");
}

static void hero_draw(void)
{
    int i, n = (int)rnd(4);
    for (i = 0; i < PK_MAX_HOLE; i++) view.discard[i] = false;
    for (i = 0; i < n && i < game.seat[view.hero].nhole; i++)
        view.discard[i] = true;
    input_command(&view, "d 1 2");
    if (game.phase == PK_PHASE_DRAW && game.actor == view.hero)
        input_command(&view, "p");
}

static void ai_seat_turn(void)
{
    int seat = game.actor;
    int action, i;
    int32_t to, before;

    view.thinking = true;
    view.thinking_seat = seat;
    draw_everything();

    pk_ai_set_level(&ai[seat], view.level);
    pk_ai_decide(&ai[seat], &game, seat, &action, &to);

    view.thinking = false;

    if (!pk_legal(&game, seat, action, to)) {
        printf("STRESS FAIL: illegal AI action, seat %d\n", seat);
        exit(1);
    }

    before = game.seat[seat].bet;
    for (i = 0; i < game.nseats; i++)
        pk_ai_observe(&ai[i], &game, seat, action);
    pk_act(&game, action, to);
    tag_action(seat, action, before);
}

static void play_one(const pk_variant_t *v, int seats, int limit, int level)
{
    int guard = 0, i;

    memset(&view, 0, sizeof view);
    view.hero = 0;
    view.level = level;
    view.pending_limit = -1;
    set_names();

    pk_game_init(&game, v, seats, 1000, 5, 10);
    pk_game_set_limit(&game, limit);
    for (i = 0; i < PK_MAX_SEATS; i++) pk_ai_init(&ai[i], level);
    view.g = &game;

    /* Several hands off the same table, so the opponent reads
     * accumulate and the stacks move -- which is what produces the
     * short stacks, the all-ins and the side pots. */
    for (i = 0; i < 6; i++) {

        int alive = 0, s;
        for (s = 0; s < game.nseats; s++)
            if (game.seat[s].stack > 0) alive++;
        if (alive < 2 || game.seat[view.hero].stack <= 0) break;

        memset(view.discard, 0, sizeof view.discard);
        for (s = 0; s < PK_MAX_SEATS; s++) view.tag[s][0] = '\0';
        view.reveal = false;
        view.bet_to = 0;
        view.hand_no++;

        pk_hand_begin(&game);
        input_bet_step(&view, 0);
        hands++;

        guard = 0;
        while (game.phase != PK_PHASE_COMPLETE && guard++ < 300) {
            steps++;
            draw_everything();

            if (game.phase == PK_PHASE_DRAW) {
                if (game.actor == view.hero) { hero_draw(); draws_done++; }
                else {
                    uint8_t di[3] = { 0, 1, 2 };
                    pk_draw(&game, di, (int)rnd(3));
                }
                continue;
            }

            if (game.actor == view.hero) hero_turn();
            else ai_seat_turn();
        }

        if (game.phase != PK_PHASE_COMPLETE) {
            printf("STRESS FAIL: a hand did not settle\n");
            exit(1);
        }

        /* The showdown, which is the only time every seat's cards are
         * turned over and every hand is named -- and therefore the
         * only time those string buffers are filled. */
        view.reveal = true;
        draw_everything();

        /* And the hit tests, over every pixel of the table, which is
         * how an out-of-range card index would show up. */
        {
            int x, y;
            for (y = L.clip.y0; y <= L.clip.y1; y += 3)
                for (x = L.clip.x0; x <= L.clip.x1; x += 3) {
                    (void)pt_button_at(&L, x, y);
                    (void)pt_hero_card_at(&L, &view, x, y);
                }
        }
    }
}

int main(void)
{
    static const pk_variant_t *const vs[] = {
        &pk_variant_holdem, &pk_variant_draw5,
        &pk_variant_stud5, &pk_variant_stud7
    };
    static const int limits[] = {
        PK_LIMIT_NONE, PK_LIMIT_FIXED, PK_LIMIT_POT
    };
    int vi, li, seats, level;

    printf("poker: stress\n");

    if (!z_render_open(&win, 320, 240)) {
        printf("poker: cannot map the framebuffer here -- skipping\n");
        return 77;
    }
    pt_init();

    pk_rng_seed(0xBADC0FFE);

    for (vi = 0; vi < 4; vi++)
        for (li = 0; li < 3; li++)
            for (seats = 2; seats <= vs[vi]->max_seats; seats++)
                for (level = 1; level <= 8; level += 3)
                    play_one(vs[vi], seats, limits[li], level);

    printf("%ld hands, %ld steps, %ld draws -- no faults\n",
        hands, steps, draws_done);

    return 0;
}
