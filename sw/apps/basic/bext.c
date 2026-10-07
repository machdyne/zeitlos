/*
 * basic -- the BASIC computer's statements and functions beyond BASIC 1
 * (docs/basic_app.md), through the interpreter's extension interface
 * (sw/ext/basic/basic.h, BASIC_EXT).
 *
 * Coordinates are pixels, 0-319 across and 0-239 down. Drawing beyond
 * the screen is not an error: whatever is on screen is drawn. Text
 * positions, which must be on screen, are rows 0-29 and columns 0-39.
 */

#include "../../ext/basic/basic.h"
#include "bplat.h"

#define ARGS uint8_t n, const int16_t *a, uint8_t *e

static int16_t x_cls(ARGS) {
    (void)n; (void)a; (void)e;
    bs_cls(bp_screen());
    return 0;
}

static int16_t x_color(ARGS) {
    (void)n;
    if (a[0] < 0 || a[0] > 2) *e = BASIC_E_RANGE;
    else bp_screen()->op = a[0];
    return 0;
}

static int16_t x_plot(ARGS) {
    (void)n; (void)e;
    bs_plot(bp_screen(), a[0], a[1]);
    return 0;
}

static int16_t x_line(ARGS) {
    (void)n; (void)e;
    bs_line(bp_screen(), a[0], a[1], a[2], a[3]);
    return 0;
}

static int16_t x_box(ARGS) {
    (void)e;
    bs_box(bp_screen(), a[0], a[1], a[2], a[3], n == 5 && a[4]);
    return 0;
}

static int16_t x_circle(ARGS) {
    (void)n;
    if (a[2] < 0) *e = BASIC_E_RANGE;
    else bs_circle(bp_screen(), a[0], a[1], a[2]);
    return 0;
}

static int16_t x_point(ARGS) {
    (void)n; (void)e;
    return (int16_t)bs_point(bp_screen(), a[0], a[1]);
}

static int16_t x_locate(ARGS) {
    (void)n;
    if (!bs_locate(bp_screen(), a[0], a[1])) *e = BASIC_E_RANGE;
    return 0;
}

static int16_t x_screen(ARGS) {
    (void)n;
    if (a[0] < 0 || a[0] > 1) *e = BASIC_E_RANGE;
    else if (!bp_full_screen(a[0] == 1)) *e = BASIC_E_UNSUPPORTED;
    return 0;
}

static int16_t x_key(ARGS) {
    (void)n; (void)a; (void)e;
    return bp_key();
}

static int16_t x_sync(ARGS) {
    (void)n; (void)a; (void)e;
    bp_sync();
    return 0;
}

const basic_ext_t basic_ext[] = {
    /* name      function  min max */
    { "CLS",     0,        0,  0, x_cls },
    { "COLOR",   0,        1,  1, x_color },
    { "PLOT",    0,        2,  2, x_plot },
    { "LINE",    0,        4,  4, x_line },
    { "BOX",     0,        4,  5, x_box },
    { "CIRCLE",  0,        3,  3, x_circle },
    { "POINT",   1,        2,  2, x_point },
    { "LOCATE",  0,        2,  2, x_locate },
    { "SCREEN",  0,        1,  1, x_screen },
    { "KEY",     1,        0,  0, x_key },
    { "SYNC",    0,        0,  0, x_sync },
};
const uint8_t basic_ext_count = sizeof(basic_ext) / sizeof(basic_ext[0]);
