/*
 * Zeitlos slots -- draw the machine on the build machine.
 *
 *   make render WHAT=idle|spin|win
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "slots_shim.h"
#include "../sl_board.h"

static sl_spin_t spin;
static sl_view_t view;
static sl_layout_t L;
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
    const char *path = argc > 1 ? argv[1] : "/tmp/slots.pbm";
    const char *what = argc > 2 ? argv[2] : "idle";
    int stops[SL_REELS] = { 7, 8, 8 };

    if (!z_render_open(&win, 320, 240)) {
        fprintf(stderr, "render: cannot map the framebuffer here\n");
        return 77;
    }
    sl_board_init();

    sl_spin_init(&spin);
    view.spin = &spin;
    view.chips = 1240;
    view.bet = 5;
    view.lines = SL_LINES;
    view.trng = true;
    strcpy(view.message, "press SPIN, or Return");

    if (!strcmp(what, "spin")) {
        int f;
        sl_spin_begin(&spin, stops);
        for (f = 0; f < 40; f++) sl_spin_step(&spin);
        strcpy(view.message, "reels turning");
        strcpy(view.cmd, "bet 10");
        view.cmd_len = 6;
    } else if (!strcmp(what, "win")) {
        int guard = 0;
        sl_spin_begin(&spin, stops);
        while (sl_spin_step(&spin) && guard++ < 500) { }
        view.have_result = true;
        view.last_win = sl_evaluate(stops, view.bet, view.lines,
            view.line_pays);
        {
            /* Find a set of stops that actually wins on more than one
             * line, so the render shows what a multi-line win looks
             * like rather than a lucky blank. */
            int a, b, c, best = 0, bs[3] = { 0, 0, 0 };
            for (a = 0; a < SL_STOPS; a++)
                for (b = 0; b < SL_STOPS; b++)
                    for (c = 0; c < SL_STOPS; c++) {
                        int s2[3], n = 0, i;
                        int32_t lp[SL_LINES];
                        s2[0] = a; s2[1] = b; s2[2] = c;
                        sl_evaluate(s2, 1, SL_LINES, lp);
                        for (i = 0; i < SL_LINES; i++) if (lp[i] > 0) n++;
                        if (n > best) {
                            best = n;
                            bs[0] = a; bs[1] = b; bs[2] = c;
                        }
                    }
            stops[0] = bs[0]; stops[1] = bs[1]; stops[2] = bs[2];
            sl_spin_init(&spin);
            sl_spin_begin(&spin, stops);
            guard = 0;
            while (sl_spin_step(&spin) && guard++ < 500) { }
            view.last_win = sl_evaluate(stops, view.bet, view.lines,
                view.line_pays);
        }
        {
            /* Built from the pays, not hardcoded: a fixture that says
             * one thing while the board shows another is worse than no
             * caption at all. */
            char m[SL_MSG_LEN]; char t[12]; int i, n = 0;
            m[0] = '\0';
            for (i = 0; i < SL_LINES; i++) {
                if (view.line_pays[i] <= 0) continue;
                if (n++) strcat(m, ", ");
                else strcat(m, "lines ");
                sprintf(t, "%d", i + 1);
                strcat(m, t);
            }
            sprintf(t, " win %d", (int)view.last_win);
            strcat(m, t);
            strcpy(view.message, m);
        }
    }

    clear_all();
    sl_board_layout(&L, &view, 2, 13, 316, 225);
    sl_board_draw(&L, &view);
    write_pbm(path, 320, 240, 2);

    return 0;
}
