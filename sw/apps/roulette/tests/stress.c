/*
 * Zeitlos roulette -- the whole app, driven hard, under sanitizers.
 *
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 *   make stress
 *
 * The other test binaries each exercise one layer. This runs them
 * TOGETHER the way roulette.c does: place bets through both the command
 * line and the mouse, spin the wheel frame by frame with the repair
 * redraws the real app performs, settle, repeat -- on both wheels, at
 * several window sizes, for thousands of rounds.
 *
 * Built with -fsanitize=address,undefined, which is the point. A
 * one-byte overrun of a message buffer or a read one past the end of a
 * bet array is invisible on the host without it, and on the target it
 * is whatever happened to be next in memory: usually nothing,
 * occasionally a crash at startup with no window and no clue.
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "roulette_shim.h"
#include "../rl_input.h"

static rl_round_t round_;
static rl_wheel_t wheel;
static rl_view_t view;
static rl_layout_t L;
static z_win_t win;

static long rounds, bets_placed, frames, clicks;

static uint32_t st = 2024;

static uint32_t rnd(uint32_t n)
{
    st ^= st << 13; st ^= st >> 17; st ^= st << 5;
    return n ? st % n : 0;
}

static uint32_t src(void *ctx) { (void)ctx; return rnd(0xffffffffu); }

static void draw_everything(void)
{
    /* Both rectangles the app ever draws into, plus an awkward one,
     * because a window can be resized to anything. */
    rl_board_layout(&L, &view, 60, 40, 316, 225);
    rl_board_draw(&L, &view);
    rl_board_draw_status(&L, &view);

    rl_board_layout(&L, &view, 0, 0, 320, 240);
    rl_board_draw(&L, &view);

    rl_board_layout(&L, &view, 5, 5, 302, 201);
    rl_board_draw(&L, &view);

    rl_board_layout(&L, &view, 60, 40, 316, 225);
}

/* The spin loop from roulette.c, reproduced: capture the previous
 * footprint, step, repair, draw. If the repair ever reads or writes
 * outside a card or a rectangle, this is where it shows. */
static void spin(int result)
{
    int guard = 0;

    view.spinning = true;
    wheel.ball_r = (wheel.r * 94) / 100;
    rl_wheel_spin(&wheel, result, 20 + (int)rnd(60));

    for (;;) {
        z_clip_t old_ball, old_hub, now_ball;
        bool more;

        rl_wheel_ball_rect(&wheel, wheel.ball_a, wheel.ball_r, &old_ball);
        rl_wheel_hub_rect(&wheel, &old_hub);

        more = rl_wheel_step(&wheel);

        rl_wheel_draw(&wheel, &old_ball);
        rl_wheel_draw(&wheel, &old_hub);
        rl_wheel_ball_rect(&wheel, wheel.ball_a, wheel.ball_r, &now_ball);
        rl_wheel_draw(&wheel, &now_ball);

        frames++;
        if (!more || guard++ > 500) break;
    }

    view.spinning = false;
}

static void place_bets(void)
{
    int n = 1 + (int)rnd(8), i;

    for (i = 0; i < n; i++) {
        if (rnd(2)) {
            /* Through the mouse, at a point anywhere on the table. */
            int x = L.ox + (int)rnd((uint32_t)L.w);
            int y = L.oy + (int)rnd((uint32_t)L.h);
            if (rl_input_click(&view, &L, x, y, rnd(4) == 0) == RI_REDRAW)
                clicks++;
        } else {
            /* Through the command line, in the person's own spelling. */
            static const char *const cmds[] = {
                "17", "red", "black 10", "odd", "dozen 2", "column 3",
                "street 5", "six 4", "split 17 20", "corner 17",
                "even 5", "low", "high 3", "00", "chip 25", "37",
                "split 3 4", "corner 36", "dozen 9", "wibble", ""
            };
            const char *c = cmds[rnd(21)];
            if (rl_input_command(&view, c) == RI_REDRAW) bets_placed++;
        }
    }
}

int main(void)
{
    long r;

    printf("roulette: stress\n");

    if (!z_render_open(&win, 320, 240)) {
        printf("roulette: cannot map the framebuffer here -- skipping\n");
        return 77;
    }

    zg_rng_set(src, NULL);

    for (r = 0; r < 2500; r++) {
        int wheel_type = (r & 1) ? RL_AMERICAN : RL_EURO;
        int np, result;
        int32_t ret;

        memset(&view, 0, sizeof view);
        rl_round_clear(&round_, wheel_type);
        view.round = &round_;
        view.wheel = &wheel;
        view.chips = 100 + (int32_t)rnd(5000);
        view.chip_sel = (int)rnd(4);
        view.result = -1;

        rl_board_layout(&L, &view, 60, 40, 316, 225);
        rl_wheel_init(&wheel, wheel_type, L.wheel_cx, L.wheel_cy, L.wheel_r);

        place_bets();
        draw_everything();

        np = rl_pockets(wheel_type);
        result = (int)rnd((uint32_t)np);

        spin(result);

        ret = rl_round_returns(&round_, result);

        /* THE PAYOUT CEILING. The most any round can return is 36 times
         * what was staked -- every bet on the table is 36:1 or better
         * only in the sense that no single bet returns more. Anything
         * above that is money from nowhere. */
        if (ret > round_.staked * 36) {
            printf("STRESS FAIL: round %ld returned %d on a stake of %d\n",
                r, (int)ret, (int)round_.staked);
            return 1;
        }
        if (ret < 0 || round_.staked < 0) {
            printf("STRESS FAIL: negative money in round %ld\n", r);
            return 1;
        }

        /* Nobody may stake more than they hold. */
        if (round_.staked > view.chips) {
            printf("STRESS FAIL: staked %d of %d chips in round %ld\n",
                (int)round_.staked, (int)view.chips, r);
            return 1;
        }

        view.result = result;
        view.last_return = ret;
        view.last_staked = round_.staked;
        {
            int i;
            for (i = RL_HISTORY - 1; i > 0; i--)
                view.history[i] = view.history[i - 1];
            view.history[0] = result;
            if (view.nhistory < RL_HISTORY) view.nhistory++;
        }

        draw_everything();

        /* And every hit test over the whole table, which is how an
         * out-of-range selector would surface. */
        if ((r & 63) == 0) {
            int x, y, type, sel;
            for (y = L.clip.y0; y <= L.clip.y1; y += 2)
                for (x = L.clip.x0; x <= L.clip.x1; x += 2)
                    if (rl_board_hit(&L, &view, x, y, &type, &sel))
                        if (!rl_bet_valid(wheel_type, type, sel)) {
                            printf("STRESS FAIL: (%d,%d) resolved to an "
                                "invalid bet\n", x, y);
                            return 1;
                        }
        }

        rounds++;
    }

    printf("%ld rounds, %ld bets, %ld clicks, %ld frames -- no faults\n",
        rounds, bets_placed, clicks, frames);

    return 0;
}
