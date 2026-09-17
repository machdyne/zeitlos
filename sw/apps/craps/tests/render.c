/*
 * Zeitlos craps -- draw the table on the build machine.
 *
 *   make render WHAT=comeout|point|rolling
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "craps_shim.h"
#include "../cr_board.h"

static cr_game_t game;
static cr_view_t view;
static cr_layout_t L;
static z_win_t win;

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
    const char *path = argc > 1 ? argv[1] : "/tmp/craps.pbm";
    const char *what = argc > 2 ? argv[2] : "point";
    int x, y, outside = 0;

    if (!z_render_open(&win, 320, 240)) {
        fprintf(stderr, "render: cannot map the framebuffer here\n");
        return 77;
    }
    cr_board_init();

    cr_game_init(&game);
    view.g = &game;
    view.chips = 1240;
    view.bet = 25;
    view.trng = true;
    view.show_d1 = 3;
    view.show_d2 = 4;

    if (!strcmp(what, "comeout")) {
        cr_place(&game, CR_PASS, 0, 25);
        cr_place(&game, CR_DONT_PASS, 0, 10);
        strcpy(view.message, "come out -- click PASS LINE, then roll");
        strcpy(view.cmd, "bet 50");
        view.cmd_len = 6;
    } else if (!strcmp(what, "rolling")) {
        cr_place(&game, CR_PASS, 0, 25);
        cr_roll(&game, 4, 4);
        cr_place(&game, CR_PASS_ODDS, 8, 125);
        cr_place(&game, CR_PLACE, 6, 60);
        view.rolling = true;
        view.show_d1 = 2;
        view.show_d2 = 5;
        strcpy(view.message, "rolling...");
    } else {
        cr_place(&game, CR_PASS, 0, 25);
        cr_roll(&game, 4, 4);
        cr_place(&game, CR_PASS_ODDS, 8, 125);
        cr_place(&game, CR_PLACE, 6, 60);
        cr_place(&game, CR_HARD, 8, 5);
        cr_place(&game, CR_FIELD, 0, 10);
        cr_roll(&game, 5, 4);
        view.show_d1 = 5;
        view.show_d2 = 4;
        strcpy(view.message, "nine -- the field pays");
    }

    cr_board_layout(&L, &view, 2, 13, 316, 225);
    cr_board_draw(&L, &view);

    for (y = 0; y < Z_SCREEN_H; y++)
        for (x = 0; x < Z_SCREEN_W; x++)
            if (z_render_get(x, y) &&
                (x < L.clip.x0 || x > L.clip.x1 ||
                 y < L.clip.y0 || y > L.clip.y1)) outside++;
    if (outside) printf("render: WARNING %d px outside the content rect\n",
        outside);

    write_pbm(path, 320, 240, 2);
    return 0;
}
