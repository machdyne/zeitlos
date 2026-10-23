/*
 * basic -- the BASIC computer itself on a build host, driven by a
 * script of keys, every screen written out as a picture.
 * make -C sw/apps/basic render
 *
 * basic_app.c is compiled with -DAR_HOST_TESTS -Dmain=game_main and
 * runs on the arcade runtime's software framebuffer
 * (sw/common/games/tests/ar_host.c), every frame checked against a full
 * repaint (AR_HOST_VERIFY). No files, pins or terminal here.
 *
 *   render_basic DIR    circles in black and white; sixteen colours, in
 *                       the window (black and white), its colour window,
 *                       covered, off and on again (F10), full screen
 *                       in colour and in mono; a
 *                       program stopped with Escape full screen; the
 *                       help (F1) on BASIC's screen
 */

#undef main

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../../ext/basic/basic.h"
#include "../../../common/games/tests/ar_host.h"

int game_main(void);

/* ---- no files, no pins ---- */

int hw_fopen(const char *name, uint8_t mode) { (void)name; (void)mode; return FS_ERR_NOT_FOUND; }
int hw_fread(uint8_t *buf, uint16_t len) { (void)buf; (void)len; return FS_ERR_IO; }
int hw_fwrite(const uint8_t *buf, uint16_t len) { (void)buf; (void)len; return FS_ERR_IO; }
int hw_fclose(void) { return FS_OK; }
void hw_fabort(void) { }
int hw_fdelete(const char *name) { (void)name; return FS_ERR_NOT_FOUND; }
int hw_fdir(fs_dir_cb cb) { (void)cb; return FS_OK; }
int hw_fformat(void) { return HW_ERR_UNSUPPORTED; }
int hw_pin_mode(uint8_t pin, uint8_t mode) { (void)pin; return mode == PM_NONE ? 0 : HW_ERR_UNSUPPORTED; }
void hw_pin_write(uint8_t pin, uint8_t level) { (void)pin; (void)level; }
uint8_t hw_pin_read(uint8_t pin) { (void)pin; return 0; }
int16_t hw_adc(uint8_t pin) { (void)pin; return -1; }
void hw_led(uint8_t on) { (void)on; }
int hw_i2c(uint8_t addr, const uint8_t *w, uint8_t wn, uint8_t *r, uint8_t rn) {
    (void)addr; (void)w; (void)wn; (void)r; (void)rn;
    return -1;
}

/* ---- the script ---- */

static ar_host_step_t steps[2048];
static int nsteps;
static uint32_t at = 8;                 /* the frame the next step is at */

static void step(int kind, uint32_t a, const char *name) {
    ar_host_step_t *s = &steps[nsteps++];
    memset(s, 0, sizeof(*s));
    s->frame = at;
    s->kind = kind;
    s->a = a;
    s->name = name;
}

/* A line typed, a frame for it, Enter: the next line a frame later. */
static void type(const char *line) {
    while (*line) step(ARH_TAP, (uint8_t)*line++, NULL);
    step(ARH_TAP, '\r', NULL);
    at += 2;
}

static void shot(const char *name) { at += 2; step(ARH_SHOT, 0, name); at += 2; }
static void wait(uint32_t frames) { at += frames; }

int main(int argc, char **argv) {
    const char *dir = argc > 1 ? argv[1] : "out";
    setvbuf(stdout, NULL, _IOLBF, 0);

    shot("01_ready");
    type("10 FOR R = 10 TO 100 STEP 10");
    type("20 CIRCLE 160, 120, R");
    type("30 NEXT");
    type("RUN");
    wait(60);
    shot("02_circles");

    type("NEW");
    type("10 PAPER 6: INK 1: CLS");
    type("20 FOR C = 2 TO 15");
    type("30 INK C: BOX C * 20, 40, C * 20 + 15, 200, 1");
    type("40 NEXT");
    type("50 INK 7: LOCATE 27, 1: PRINT \"SIXTEEN COLOURS\"");
    type("RUN");
    wait(120);
    shot("03_colour_in_black_and_white");
    step(ARH_WCOLOR, 1, NULL);
    shot("04_colour_window");
    at += 1; { ar_host_step_t *s = &steps[nsteps]; step(ARH_COVER, 0, NULL);
        s->x = 200; s->y = 150; s->w = 160; s->h = 120; }
    shot("05_covered");
    { ar_host_step_t *s = &steps[nsteps]; step(ARH_COVER, 0, NULL); (void)s; }
    shot("06_uncovered");
    step(ARH_TAP, ARH_K_F10, NULL);        /* the colour window off */
    shot("06b_f10_off");
    step(ARH_TAP, ARH_K_F10, NULL);        /* and on */
    shot("06c_f10_on");
    step(ARH_FULL_COLOR, 0, NULL);
    shot("07_full_colour");
    step(ARH_FULL_MONO, 0, NULL);
    shot("08_full_mono");
    step(ARH_FULL_COLOR, 0, NULL);

    /* an endless loop, stopped with Escape full screen */
    type("NEW");
    type("10 X = 0");
    type("20 CLS: CIRCLE X, 120, 10: SYNC");
    type("30 X = X + 2: IF X > 300 THEN X = 0");
    type("40 GOTO 20");
    type("RUN");
    wait(50);
    shot("09_moving_full");
    step(ARH_TAP, 0x1b, NULL);
    wait(4);
    shot("10_stopped");
    step(ARH_WINDOW, 0, NULL);
    shot("11_window_colour_again");
    step(ARH_TAP, ARH_K_F1, NULL);
    shot("12_help");
    step(ARH_TAP, 0x1b, NULL);
    shot("13_help_closed");
    step(ARH_WCOLOR, 0, NULL);
    shot("14_focus_elsewhere");
    step(ARH_END, 0, NULL);

    ar_host_run(steps, dir);
    game_main();
    {
        extern uint32_t ar_host_verify_bad(void);
        printf("render_basic: %d pictures, %u frames verified, %u differ\n",
            ar_host_shots(), ar_host_verified(), ar_host_verify_bad());
        return ar_host_verify_bad() ? 1 : 0;
    }
}
