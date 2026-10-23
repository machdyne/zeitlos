/*
 * Zeitlos craps -- draw the table on the build machine.
 *
 *   make render WHAT=comeout|point|rolling
 *   render DIR scr-<state>   that state on the screen face, in colour
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "craps_shim.h"
#include "../cr_board.h"
#include "../../../common/games/tests/ar_host.h"

static cr_game_t game;
static cr_view_t view;
static cr_layout_t L;
static z_win_t win;

/* -- the screen face: the same table through the arcade runtime, in
 * colour: the window in black and white, its colour window, full
 * screen, every frame checked against a full repaint -- */
static void scr_paint(void *user, const ar_rect_t *r)
{
    cr_layout_t t;
    (void)user; (void)r;
    cr_board_layout(&t, &view, 0, 0, AR_W, AR_H);
    cr_board_draw(&t, &view);
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
    int k = 0;

    snprintf(n1, sizeof n1, "%s_window", what);
    snprintf(n2, sizeof n2, "%s_colour_window", what);
    snprintf(n3, sizeof n3, "%s_full", what);
    script[k++] = (ar_host_step_t)SHOT(3, n1);
    script[k++] = (ar_host_step_t)WCOLOR(4, 1);
    script[k++] = (ar_host_step_t)SHOT(7, n2);
    script[k++] = (ar_host_step_t)MODE(8, ARH_FULL_COLOR);
    script[k++] = (ar_host_step_t)SHOT(11, n3);
    script[k++] = (ar_host_step_t)END(14);

    desc.name = "craps";
    desc.paint = scr_paint;
    memcpy(desc.palette, cr_palette, sizeof desc.palette);
    cr_mono_inks(desc.ink);
    cr_screen(true);
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
    const char *scr = NULL;
    int x, y, outside = 0;

    /* scr-<state>: that state on the screen face, pictures into the
     * directory `path` */
    if (!strncmp(what, "scr-", 4)) { scr = what; what += 4; }

    if (!scr && !z_render_open(&win, 320, 240)) {
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

    if (scr) return screen_face(path, scr);

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
