/*
 * Zeitlos roulette -- draw the wheel on the build machine.
 *
 *   cd sw/apps/roulette && make render
 *   make render WHAT=wheel|spin|small
 *
 * See sw/common/tests/zrender.h for why this exists. The wheel is the
 * clearest case in the tree for it: whether a 37-pocket rim reads as a
 * wheel at 44 pixels, whether the red pockets are distinguishable from
 * the black at that size, and whether the spin decelerates in a way
 * that looks like a ball rather than a cursor, are all things no
 * assertion can answer and one look can.
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "roulette_shim.h"
#include "../rl_board.h"
#include "../../../common/games/tests/ar_host.h"

/* -- the screen face: the table through the arcade runtime, in colour
 * (`render DIR scr-board`, `scr-amer`): the window in black and white,
 * its colour window, full screen, every frame checked against a full
 * repaint -- */
static rl_round_t s_round;
static rl_wheel_t s_wheel;
static rl_view_t s_view;

static void scr_paint(void *user, const ar_rect_t *r)
{
    rl_layout_t t;
    (void)user; (void)r;
    rl_board_layout(&t, &s_view, 0, 0, AR_W, AR_H);
    s_wheel.cx = t.wheel_cx; s_wheel.cy = t.wheel_cy; s_wheel.r = t.wheel_r;
    s_wheel.ball_r = (t.wheel_r * 74) / 100;
    s_wheel.prev_ball_r = s_wheel.ball_r;
    rl_board_draw(&t, &s_view);
}

#define SHOT(f, n)      { (f), ARH_SHOT, 0, 0, (n), 0, 0, 0, 0 }
#define MODE(f, m)      { (f), (m), 0, 0, NULL, 0, 0, 0, 0 }
#define WCOLOR(f, on)   { (f), ARH_WCOLOR, (on), 0, NULL, 0, 0, 0, 0 }
#define END(f)          { (f), ARH_END, 0, 0, NULL, 0, 0, 0, 0 }

static int screen_face(const char *dir, const char *what)
{
    static ar_desc_t desc;
    static char n1[48], n2[48], n3[48];
    ar_host_step_t script[8];
    ar_event_t e;
    int k = 0, wl = strcmp(what, "scr-amer") ? RL_EURO : RL_AMERICAN;

    rl_round_clear(&s_round, wl);
    rl_wheel_init(&s_wheel, wl, 0, 0, 40);
    s_view.round = &s_round;
    s_view.wheel = &s_wheel;
    s_view.chips = 864;
    s_view.chip_sel = 2;
    s_view.trng = true;
    s_view.result = wl == RL_AMERICAN ? RL_DOUBLE_ZERO : 17;
    s_view.last_staked = 40;
    s_view.last_return = 75;
    s_view.nhistory = 5;
    s_view.history[0] = 17; s_view.history[1] = 0; s_view.history[2] = 33;
    s_view.history[3] = 26; s_view.history[4] = 5;
    strcpy(s_view.message, "click a number, a line or a corner");
    strcpy(s_view.cmd, "red 25");
    s_view.cmd_len = 6;
    rl_bet_place(&s_round, RL_STRAIGHT, 17, 25);
    rl_bet_place(&s_round, RL_RED, 0, 5);
    rl_bet_place(&s_round, RL_BLACK, 0, 1);
    rl_bet_place(&s_round, RL_CORNER, 4, 1);
    rl_bet_place(&s_round, RL_SPLIT, 3, 5);
    rl_bet_place(&s_round, RL_DOZEN, 1, 100);
    rl_bet_place(&s_round, RL_COLUMN, 0, 5);
    s_wheel.ball_a = rl_wheel_angle(wl, 17);
    s_wheel.prev_ball_a = s_wheel.ball_a;

    snprintf(n1, sizeof n1, "%s_window", what);
    snprintf(n2, sizeof n2, "%s_colour_window", what);
    snprintf(n3, sizeof n3, "%s_full", what);
    script[k++] = (ar_host_step_t)SHOT(3, n1);
    script[k++] = (ar_host_step_t)WCOLOR(4, 1);
    script[k++] = (ar_host_step_t)SHOT(7, n2);
    script[k++] = (ar_host_step_t)MODE(8, ARH_FULL_COLOR);
    script[k++] = (ar_host_step_t)SHOT(11, n3);
    script[k++] = (ar_host_step_t)END(14);

    desc.name = "roulette";
    desc.paint = scr_paint;
    memcpy(desc.palette, rl_palette, sizeof desc.palette);
    rl_mono_inks(desc.ink);
    rl_screen(true);
    ar_window_colour(true);
    ar_host_run(script, dir);
    if (!ar_init(&desc)) return 1;
    for (;;) {
        bool quit = false;
        while (ar_poll(&e)) if (e.type == AR_EV_QUIT) quit = true;
        if (quit) break;
        ar_frame();
    }
    printf("render: %s, %d pictures in %s, %u frames verified, %u differ\n",
        what, ar_host_shots(), dir, ar_host_verified(), ar_host_verify_bad());
    return ar_host_verify_bad() ? 1 : 0;
}

static z_win_t win;

static void clear_all(void)
{
    int x, y;
    for (y = 0; y < Z_SCREEN_H; y++)
        for (x = 0; x < Z_SCREEN_W; x++)
            z_fb_set_pixel(x, y, 0, NULL);
}

static void write_pbm(const char *path, int w, int h, int scale)
{
    FILE *f = fopen(path, "wb");
    int x, y, sx, sy;

    if (!f) { perror(path); exit(1); }
    fprintf(f, "P1\n%d %d\n", w * scale, h * scale);
    for (y = 0; y < h; y++)
        for (sy = 0; sy < scale; sy++) {
            for (x = 0; x < w; x++)
                for (sx = 0; sx < scale; sx++)
                    fputc(z_render_get(x, y) ? '1' : '0', f);
            fputc('\n', f);
        }
    fclose(f);
    printf("render: wrote %s\n", path);
}

int main(int argc, char **argv)
{
    const char *path = argc > 1 ? argv[1] : "/tmp/roulette.pbm";
    const char *what = argc > 2 ? argv[2] : "wheel";
    z_clip_t clip;
    rl_wheel_t w;

    if (!strncmp(what, "scr-", 4)) return screen_face(path, what);

    if (!z_render_open(&win, 320, 240)) {
        fprintf(stderr, "render: cannot map the framebuffer here "
            "(Linux/x86-64 only) -- skipping\n");
        return 77;
    }

    clip.x0 = 0; clip.y0 = 0;
    clip.x1 = Z_SCREEN_W - 1; clip.y1 = Z_SCREEN_H - 1;
    clear_all();

    if (!strcmp(what, "board") || !strcmp(what, "game")) {
        static rl_round_t round_;
        static rl_wheel_t wh;
        static rl_view_t v;
        static rl_layout_t L;
        bool game = !strcmp(what, "game");

        rl_round_clear(&round_, RL_EURO);
        rl_wheel_init(&wh, RL_EURO, 0, 0, 40);
        v.round = &round_;
        v.wheel = &wh;
        v.chips = 864;
        v.chip_sel = 2;
        v.trng = true;
        v.result = 17;
        v.last_staked = 40;
        v.last_return = 75;
        v.nhistory = 5;
        v.history[0] = 17; v.history[1] = 0; v.history[2] = 33;
        v.history[3] = 26; v.history[4] = 5;
        strcpy(v.message, "click a number, a line or a corner");
        strcpy(v.cmd, "red 25");
        v.cmd_len = 6;

        rl_bet_place(&round_, RL_STRAIGHT, 17, 25);
        rl_bet_place(&round_, RL_RED, 0, 5);
        rl_bet_place(&round_, RL_CORNER, 4, 1);
        rl_bet_place(&round_, RL_SPLIT, 3, 5);
        rl_bet_place(&round_, RL_DOZEN, 1, 100);
        rl_bet_place(&round_, RL_STREET, 7, 5);
        rl_bet_place(&round_, RL_SIXLINE, 2, 5);
        rl_bet_place(&round_, RL_COLUMN, 0, 5);

        if (game) rl_board_layout(&L, &v, 0, 0, 320, 240);
        else rl_board_layout(&L, &v, 2, 13, 316, 225);

        wh.cx = L.wheel_cx; wh.cy = L.wheel_cy; wh.r = L.wheel_r;
        wh.ball_a = rl_wheel_angle(RL_EURO, 17);
        wh.ball_r = (L.wheel_r * 74) / 100;

        rl_board_draw(&L, &v);
        write_pbm(path, 320, 240, 2);
        return 0;
    }

    if (!strcmp(what, "small")) {
        /* Three sizes at once, to find where it stops reading. */
        rl_wheel_init(&w, RL_EURO, 40, 40, 30);
        rl_wheel_draw(&w, &clip);
        rl_wheel_init(&w, RL_EURO, 130, 50, 44);
        rl_wheel_draw(&w, &clip);
        rl_wheel_init(&w, RL_EURO, 250, 60, 58);
        rl_wheel_draw(&w, &clip);
        rl_wheel_init(&w, RL_AMERICAN, 70, 170, 50);
        rl_wheel_draw(&w, &clip);
        rl_wheel_init(&w, RL_EURO, 220, 175, 50);
        w.ball_a = rl_wheel_angle(RL_EURO, 17);
        rl_wheel_draw(&w, &clip);

    } else if (!strcmp(what, "frames")) {
        /* Six frames across one spin at the size the board gives the
         * wheel, drawn exactly as roulette.c draws them. */
        static const int at[6] = { 3, 22, 50, 80, 104, 119 };
        int i, k = 0;
        rl_wheel_init(&w, RL_AMERICAN, 60, 60, 44);
        w.ball_r = (w.r * 94) / 100;
        rl_wheel_spin(&w, 17, 120);
        clear_all();
        rl_wheel_draw(&w, &clip);
        for (i = 0; i < 6; i++) {
            char path2[64];
            int guard = 0;
            while (w.frame < at[i] && guard++ < 300) {
                z_clip_t area;
                int pad = w.r + (w.r / 9) + 4;
                if (!rl_wheel_step(&w)) break;
                area.x0 = w.cx - pad; area.y0 = w.cy - pad;
                area.x1 = w.cx + pad; area.y1 = w.cy + pad;
                rl_wheel_rim(&w, &area);
                rl_wheel_track_clear(&w, &area);
                rl_wheel_ball(&w, &area);
            }
            snprintf(path2, sizeof path2, "/tmp/rw_f%d.pbm", k++);
            write_pbm(path2, 120, 120, 2);
        }
        return 0;

    } else if (!strcmp(what, "spin")) {
        /* Eight frames across one spin, tiled, so the deceleration and
         * the drop are visible side by side. */
        static const int at[8] = { 1, 6, 14, 26, 42, 58, 74, 89 };
        int i;
        rl_wheel_init(&w, RL_EURO, 0, 0, 36);
        rl_wheel_spin(&w, 17, 90);
        for (i = 0; i < 8; i++) {
            int guard = 0;
            while (w.frame < at[i] && rl_wheel_step(&w) && guard++ < 200) { }
            w.cx = 40 + (i % 4) * 80;
            w.cy = 60 + (i / 4) * 120;
            rl_wheel_draw(&w, &clip);
        }

    } else {
        rl_wheel_init(&w, RL_EURO, 160, 120, 90);
        w.ball_a = rl_wheel_angle(RL_EURO, 32);
        rl_wheel_draw(&w, &clip);
    }

    write_pbm(path, 320, 240, 2);
    return 0;
}
