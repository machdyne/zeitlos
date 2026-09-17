/*
 * Zeitlos blackjack -- the whole app, driven hard, under sanitizers.
 *
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 *   make stress
 *
 * The other test binaries each exercise one layer. This runs them
 * TOGETHER the way blackjack.c does: deal, act through the command line
 * and the mouse, redraw the table after every step, settle, repeat --
 * across every rule combination and one to eight decks.
 *
 * Built with -fsanitize=address,undefined. A one-byte overrun of a
 * hand's card array or a message buffer is invisible on the host
 * without it, and on the target it is whatever happened to be next in
 * memory.
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "blackjack_shim.h"
#include "../bj_input.h"

static bj_game_t g;
static bj_view_t view;
static bj_layout_t L;
static z_win_t win;

static long rounds, decisions, hint_follows;

static uint32_t st = 31337;

static uint32_t rnd(uint32_t n)
{
    st ^= st << 13; st ^= st >> 17; st ^= st << 5;
    return n ? st % n : 0;
}

static uint32_t src(void *ctx) { (void)ctx; return rnd(0xffffffffu); }

static void draw_everything(void)
{
    bj_board_layout(&L, &view, 60, 40, 316, 225);
    bj_board_draw(&L, &view);
    bj_board_draw_status(&L, &view);

    bj_board_layout(&L, &view, 0, 0, 320, 240);
    bj_board_draw(&L, &view);

    bj_board_layout(&L, &view, 5, 5, 260, 195);
    bj_board_draw(&L, &view);

    bj_board_layout(&L, &view, 60, 40, 316, 225);
}

/* Acts through whichever route the coin picks -- the point of doing
 * both is that bj_input.h claims they are the same operation. */
static void take_a_turn(void)
{
    static const char *const cmds[] = {
        "h", "s", "d", "p", "u", "hit", "stand", "double", "split",
        "surrender", "wibble", "", "bet 5", "chip 100", "hint"
    };

    if (rnd(2)) {
        bj_input_command(&view, cmds[rnd(15)]);
    } else {
        int b = (int)rnd(BJ_NBTN);
        bj_input_click(&view, &L, L.btn[b].x + 2, L.btn[b].y + 2);
    }

    decisions++;
}

int main(void)
{
    long r;

    printf("blackjack: stress\n");

    if (!z_render_open(&win, 320, 240)) {
        printf("blackjack: cannot map the framebuffer here -- skipping\n");
        return 77;
    }
    bj_board_init();

    zg_rng_set(src, NULL);

    for (r = 0; r < 2500; r++) {
        bj_rules_t rules;
        int guard = 0;
        int32_t bet;

        bj_rules_default(&rules);
        rules.dealer_hits_soft17 = (r & 1) != 0;
        rules.double_after_split = (r & 2) != 0;
        rules.resplit_aces = (r & 4) != 0;
        rules.surrender = (r & 8) != 0;
        if (r & 16) { rules.bj_pay_num = 6; rules.bj_pay_den = 5; }
        rules.ndecks = 1 + (int)(r % 8);

        memset(&view, 0, sizeof view);
        bj_game_init(&g, &rules);
        view.g = &g;
        view.chips = 500 + (int32_t)rnd(5000);
        view.chip_sel = (int)rnd(BJ_NCHIPS);
        view.hints = (r & 32) != 0;
        bet = 5 + (int32_t)rnd(100);
        view.bet = bet;

        bj_board_layout(&L, &view, 60, 40, 316, 225);

        if (!bj_round_begin(&g, bet)) continue;

        while (g.phase != BJ_PHASE_DONE && guard++ < 80) {
            draw_everything();

            if (g.phase == BJ_PHASE_INSURANCE) {
                bj_input_command(&view, rnd(2) ? "i" : "no");
                continue;
            }

            /* Half the time follow basic strategy, so the hint path is
             * exercised as hard as the random one. A hint must always
             * be an action the engine accepts. */
            if (rnd(2)) {
                int h = bj_hint(&g);
                if (h >= 0) {
                    if (!bj_act(&g, h)) {
                        printf("STRESS FAIL: hint %d refused in round %ld\n",
                            h, r);
                        return 1;
                    }
                    hint_follows++;
                    decisions++;
                    continue;
                }
            }

            take_a_turn();
        }

        if (g.phase != BJ_PHASE_DONE) {
            /* A round that never settles would hang the real app with
             * no legal action available. */
            printf("STRESS FAIL: round %ld did not settle\n", r);
            return 1;
        }

        /* THE PAYOUT CEILING: every hand at three to two, plus
         * insurance at two to one. Above that is money from nowhere. */
        if (g.returned > g.staked * 3 || g.returned < 0) {
            printf("STRESS FAIL: round %ld returned %d on a stake of %d\n",
                r, (int)g.returned, (int)g.staked);
            return 1;
        }

        {
            int i;
            for (i = 0; i < g.nhands; i++)
                if (g.hand[i].n > BJ_MAX_CARDS || g.hand[i].n < 1) {
                    printf("STRESS FAIL: hand %d holds %d cards\n",
                        i, g.hand[i].n);
                    return 1;
                }
            if (g.ndealer > BJ_MAX_CARDS) {
                printf("STRESS FAIL: dealer holds %d cards\n", g.ndealer);
                return 1;
            }
        }

        draw_everything();
        rounds++;
    }

    printf("%ld rounds, %ld decisions, %ld of them by hint -- no faults\n",
        rounds, decisions, hint_follows);

    return 0;
}
