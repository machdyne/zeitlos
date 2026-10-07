/*
 * basic -- host tests for the screen (bscreen.c): exact pixels, not
 * "something was drawn". `make test` in sw/apps/basic.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../bscreen.h"
#include "../../../common/zfont.h"

static int failures;

#define CHECK(c, ...) do { if (!(c)) { printf("FAIL %d: ", __LINE__); \
    printf(__VA_ARGS__); printf("\n"); failures++; } } while (0)

static bscreen_t s;

static int count(void) {
    int n = 0;
    for (int y = 0; y < BS_H; y++)
        for (int x = 0; x < BS_W; x++) n += bs_point(&s, x, y);
    return n;
}

/* the glyph for byte c, as drawn at cell (row, col) */
static int cell_is(int row, int col, int c) {
    int i = z_font_index(&z_font_8x8, (uint32_t)c);
    const uint8_t *g = z_font_8x8.glyphs + i * 8;
    for (int r = 0; r < 8; r++)
        for (int b = 0; b < 8; b++)
            if (bs_point(&s, col * 8 + b, row * 8 + r) != ((g[r] >> (7 - b)) & 1))
                return 0;
    return 1;
}

static void pbm(const char *name) {
    const char *dir = getenv("BASIC_PBM");
    if (!dir) return;
    char path[256];
    snprintf(path, sizeof(path), "%s/%s.pbm", dir, name);
    FILE *f = fopen(path, "w");
    if (!f) return;
    fprintf(f, "P1\n%d %d\n", BS_W, BS_H);
    for (int y = 0; y < BS_H; y++) {
        for (int x = 0; x < BS_W; x++) fputc(bs_point(&s, x, y) ? '1' : '0', f);
        fputc('\n', f);
    }
    fclose(f);
}

static void text(void) {
    bs_init(&s);
    for (const char *p = "READY."; *p; p++) bs_putc(&s, (uint8_t)*p);
    CHECK(cell_is(0, 0, 'R') && cell_is(0, 5, '.'), "glyphs at the right cells");
    CHECK(s.row == 0 && s.col == 6, "cursor after the text (%d, %d)", s.row, s.col);
    bs_putc(&s, 0xE4);                          /* a-umlaut, Latin-9 */
    CHECK(cell_is(0, 6, 0xE4), "Latin-9 a-umlaut");
    bs_putc(&s, '\b');
    CHECK(cell_is(0, 6, ' ') && s.col == 6, "backspace erases");
    bs_putc(&s, '\r');
    bs_putc(&s, '\n');
    CHECK(s.row == 1 && s.col == 0, "CR LF");

    /* a full row continues on the next */
    bs_cls(&s);
    for (int i = 0; i < BS_COLS + 2; i++) bs_putc(&s, 'X');
    CHECK(s.row == 1 && s.col == 2 && cell_is(1, 1, 'X'), "a row wraps at 40");

    /* the bottom row scrolls the whole screen */
    bs_cls(&s);
    bs_putc(&s, 'T');
    bs_plot(&s, 100, 20);                       /* graphics scroll too */
    for (int i = 0; i < BS_ROWS; i++) bs_putc(&s, '\n');
    CHECK(!cell_is(0, 0, 'T') && s.row == BS_ROWS - 1, "scrolled off the top");
    bs_cls(&s);
    bs_putc(&s, 'T');
    bs_plot(&s, 100, 20);
    for (int i = 0; i < BS_ROWS - 1; i++) bs_putc(&s, '\n');
    bs_putc(&s, '\n');                          /* one scroll */
    CHECK(bs_point(&s, 100, 12) && !bs_point(&s, 100, 20), "graphics scroll with text");

    /* the cursor restores exactly */
    bs_cls(&s);
    bs_putc(&s, 'A');
    uint32_t before[BS_H * BS_WORDS];
    memcpy(before, s.px, sizeof(before));
    bs_cursor(&s, true);
    CHECK(memcmp(before, s.px, sizeof(before)) != 0, "the cursor shows");
    bs_putc(&s, 'B');
    bs_cursor(&s, false);
    CHECK(cell_is(0, 1, 'B') && cell_is(0, 2, ' '), "typing with the cursor on");
    bs_putc(&s, '\b');
    memcpy(before, s.px, sizeof(before));
    bs_cursor(&s, true);
    bs_cursor(&s, false);
    CHECK(memcmp(before, s.px, sizeof(before)) == 0, "the cursor leaves no trace");
    CHECK(bs_locate(&s, 29, 39) && !bs_locate(&s, 30, 0) && !bs_locate(&s, 0, 40),
          "LOCATE: 0-29, 0-39");
    pbm("text");
}

/* every shape drawn twice with COLOR 2 leaves nothing: each pixel once */
static void invert_twice(void) {
    bs_init(&s);
    s.op = BS_INVERT;
    for (int k = 0; k < 2; k++) {
        bs_line(&s, 3, 7, 300, 200);
        bs_line(&s, 300, 10, 10, 11);           /* shallow, backwards */
        bs_line(&s, 50, 50, 50, 50);            /* a point */
        bs_box(&s, 10, 10, 60, 40, false);
        bs_box(&s, 70, 10, 70, 40, false);      /* a vertical line */
        bs_box(&s, 80, 10, 120, 10, false);     /* a horizontal line */
        bs_box(&s, 130, 10, 130, 10, false);    /* a point */
        bs_box(&s, 200, 100, 150, 60, true);    /* filled, corners swapped */
        for (int r = 0; r <= 40; r++) bs_circle(&s, 160, 120, r);
        bs_circle(&s, 5, 5, 20);                /* partly off screen */
    }
    CHECK(count() == 0, "COLOR 2 twice: %d pixels left", count());
}

static void shapes(void) {
    bs_init(&s);
    bs_circle(&s, 160, 120, 50);
    CHECK(bs_point(&s, 210, 120) && bs_point(&s, 110, 120) &&
          bs_point(&s, 160, 70) && bs_point(&s, 160, 170), "circle: its four extremes");
    CHECK(!bs_point(&s, 160, 120) && !bs_point(&s, 211, 120), "circle: not inside or beyond");
    bs_box(&s, 10, 10, 20, 15, false);
    CHECK(bs_point(&s, 10, 10) && bs_point(&s, 20, 15) && !bs_point(&s, 15, 12),
          "box: corners, hollow");
    bs_box(&s, 30, 10, 40, 15, true);
    CHECK(bs_point(&s, 35, 12), "box: filled");
    int before = count();
    bs_plot(&s, -1, 5);
    bs_plot(&s, 320, 5);
    bs_line(&s, -50, -50, -10, -10);
    CHECK(count() == before, "off screen: nothing drawn");
    CHECK(bs_point(&s, -1, 0) == 0 && bs_point(&s, 0, 240) == 0, "POINT off screen: 0");
    s.op = BS_CLEAR;
    bs_box(&s, 30, 10, 40, 15, true);
    CHECK(!bs_point(&s, 35, 12), "COLOR 0 clears");
    s.op = BS_SET;
    bs_line(&s, 0, 239, 319, 0);
    CHECK(bs_point(&s, 0, 239) && bs_point(&s, 319, 0), "a line reaches both ends");
    pbm("shapes");
}

static void dirty_rows(void) {
    int y0, y1;
    bs_init(&s);
    bs_clean(&s);
    CHECK(!bs_dirty(&s, &y0, &y1), "clean");
    bs_plot(&s, 5, 100);
    bs_line(&s, 0, 50, 10, 60);
    CHECK(bs_dirty(&s, &y0, &y1) && y0 == 50 && y1 == 101, "dirty rows 50-100 (%d-%d)", y0, y1);
}

int main(void) {
    text();
    invert_twice();
    shapes();
    dirty_rows();
    if (failures) {
        printf("FAILED: %d\n", failures);
        return 1;
    }
    printf("all screen tests passed\n");
    return 0;
}
