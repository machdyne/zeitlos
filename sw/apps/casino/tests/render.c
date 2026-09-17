/*
 * Zeitlos Casino -- draw the front desk on the build machine.
 *
 *   make render
 *
 * casino.c is one file with no separate board module, so this includes
 * it and calls its statics with main() renamed out of the way. Worth
 * the trick: the layout is the part this tree has got wrong most often,
 * and it is the part only a picture shows.
 */

#include "casino_shim.h"

#define main casino_main_unused
#include "../casino.c"
#undef main

/* The bank's filesystem half is compiled out (-DZBANK_NO_FS), so these
 * stand in for it: the render is about the picture, not about
 * /casino.dat, and a harness that needed a filesystem would not run
 * here at all. */
static zbank_t fake_bank;
int zbank_load(zbank_t *b) { *b = fake_bank; return ZBANK_OK; }
int zbank_save(const zbank_t *b) { fake_bank = *b; return ZBANK_OK; }
int zbank_adjust(const char *g, int32_t d, int32_t *c)
{ (void)g; fake_bank.chips += d; if (c) *c = fake_bank.chips; return ZBANK_OK; }
int zbank_borrow(int32_t a, int32_t *c)
{ fake_bank.chips += a; fake_bank.debt += zbank_owed(a);
  if (c) *c = fake_bank.chips; return ZBANK_OK; }
int zbank_repay(int32_t a, int32_t *c, int32_t *p)
{ (void)a; if (c) *c = fake_bank.chips; if (p) *p = 0; return ZBANK_OK; }
int zbank_buyin(int32_t *c) { return zbank_borrow(ZBANK_START, c); }
int zbank_reset(void) { zbank_defaults(&fake_bank); return ZBANK_OK; }
/* z_proc_list() and z_proc_run() come from zeitlos.c, which is linked
 * here for everything else -- they are syscall trampolines that simply
 * fail off-device rather than needing a stand-in. Defining them here as
 * well is a duplicate symbol, which is how this was found. */


#include <stdio.h>

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
    const char *path = argc > 1 ? argv[1] : "/tmp/casino.pbm";
    const char *what = argc > 2 ? argv[2] : "flush";
    int x, y, outside = 0;

    if (!z_render_open(&win, 320, 240)) {
        fprintf(stderr, "render: cannot map the framebuffer here\n");
        return 77;
    }

    zbank_defaults(&bank);
    bank_state = ZBANK_OK;
    sel = 1;

    if (!strcmp(what, "debt")) {
        bank.chips = 240;
        bank.debt = 1200;
        say("borrowed, and you now owe 1200");
    } else if (!strcmp(what, "broke")) {
        bank.chips = 0;
        bank.debt = 3600;
        say("Blackjack is already open");
    } else {
        bank.chips = 2840;
        say("welcome");
        strcpy(cmd, "borrow 500");
        cmd_len = 10;
    }

    repaint();

    for (y = 0; y < Z_SCREEN_H; y++)
        for (x = 0; x < Z_SCREEN_W; x++)
            if (z_render_get(x, y) &&
                (x < content.x0 || x > content.x1 ||
                 y < content.y0 || y > content.y1)) outside++;
    if (outside) printf("render: WARNING %d px outside the content rect\n",
        outside);

    write_pbm(path, 320, 240, 2);
    return 0;
}
