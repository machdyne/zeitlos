/*
 * Zeitlos slots -- draw the machine on the build machine.
 *
 *   make render WHAT=idle|spin|win
 *   render DIR scr-<state>   that state on the screen face, in colour
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "slots_shim.h"
#include "../sl_board.h"
#include "../../../common/games/tests/ar_host.h"

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

/* -- the screen face: the same machine through the arcade runtime, in
 * colour: the window in black and white, its colour window, full
 * screen, every frame checked against a full repaint -- */
static void scr_paint(void *user, const ar_rect_t *r)
{
    sl_layout_t t;
    (void)user; (void)r;
    sl_board_layout(&t, &view, 0, 0, AR_W, AR_H);
    sl_board_draw(&t, &view);
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

    desc.name = "slots";
    desc.paint = scr_paint;
    memcpy(desc.palette, sl_palette, sizeof desc.palette);
    sl_mono_inks(desc.ink);
    sl_screen(true);
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

int main(int argc, char **argv)
{
    const char *path = argc > 1 ? argv[1] : "/tmp/slots.pbm";
    const char *what = argc > 2 ? argv[2] : "idle";
    int stops[SL_REELS] = { 7, 8, 8 };
    const char *scr = NULL;

    /* scr-<state>: that state on the screen face, pictures into the
     * directory `path` */
    if (!strncmp(what, "scr-", 4)) { scr = what; what += 4; }

    if (!scr && !z_render_open(&win, 320, 240)) {
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

    if (scr) return screen_face(path, scr);

    clear_all();
    sl_board_layout(&L, &view, 2, 13, 316, 225);
    sl_board_draw(&L, &view);
    write_pbm(path, 320, 240, 2);

    return 0;
}
