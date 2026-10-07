/*
 * basic -- host tests for the BASIC computer: real programs, typed line
 * by line into the interpreter (sw/ext/basic) with the graphics
 * extensions (bext.c), checking what they print and the exact pixels.
 * `make test` in sw/apps/basic.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "../../../ext/basic/basic.h"
#include "../bplat.h"
#include "../../../common/zfont.h"

extern bscreen_t host_screen;
extern char host_text[];
extern int host_text_len;
extern bool host_full, host_full_available;
extern int host_syncs;
extern uint8_t host_keys[];
extern int host_nkeys;
extern const char *host_dir;

static int failures;

#define CHECK(c, ...) do { if (!(c)) { printf("FAIL %d: ", __LINE__); \
    printf(__VA_ARGS__); printf("\n"); failures++; } } while (0)

/* type lines (separated by \n) into BASIC, from a clean screen and text */
static const char *run(const char *lines) {
    char buf[256];
    host_text_len = 0;
    host_text[0] = 0;
    while (*lines) {
        int n = 0;
        while (*lines && *lines != '\n' && n < 255) buf[n++] = *lines++;
        buf[n] = 0;
        if (*lines == '\n') lines++;
        basic_yield((uint8_t *)buf);
    }
    return host_text;
}

static void fresh(void) {
    bs_init(&host_screen);
    run("NEW");
}

static int cell_is(int row, int col, int c) {
    int i = z_font_index(&z_font_8x8, (uint32_t)c);
    const uint8_t *g = z_font_8x8.glyphs + i * 8;
    for (int r = 0; r < 8; r++)
        for (int b = 0; b < 8; b++)
            if (bs_point(&host_screen, col * 8 + b, row * 8 + r) != ((g[r] >> (7 - b)) & 1))
                return 0;
    return 1;
}

static void printing(void) {
    fresh();
    CHECK(!strcmp(run("10 PRINT \"HI\"\nRUN"), "HI\n"), "PRINT: [%s]", host_text);
    CHECK(cell_is(0, 0, 'H') && cell_is(0, 1, 'I'), "on the screen");

    /* typed bytes are Latin-9: what the screen shows */
    fresh();
    run("10 PRINT \"Gr\xFC\xDF" "e\"\nRUN");
    CHECK(cell_is(0, 2, 0xFC) && cell_is(0, 3, 0xDF), "Latin-9 on screen");

    /* many lines scroll */
    fresh();
    run("10 FOR I = 1 TO 40: PRINT I: NEXT\nRUN");
    CHECK(strstr(host_text, "40\n") != 0, "40 lines printed");
    CHECK(cell_is(28, 0, '4') && cell_is(28, 1, '0'), "the last line near the bottom");

    /* INPUT: the program waits for the next line */
    fresh();
    run("10 INPUT A: PRINT A * 2\nRUN");
    CHECK(!strcmp(host_text, "? ") && basic_input, "INPUT asks: [%s]", host_text);
    CHECK(!strcmp(run("21"), "42\n") && !basic_input, "INPUT answered: [%s]", host_text);
}

static void graphics(void) {
    fresh();
    CHECK(!strcmp(run("10 PLOT 5, 5: PRINT POINT(5, 5); POINT(6, 5)\nRUN"), "10\n"),
          "PLOT and POINT: [%s]", host_text);

    fresh();
    run("10 CLS: LINE 0, 100, 319, 100: BOX 10, 10, 20, 20, 1: CIRCLE 160, 120, 30\nRUN");
    CHECK(bs_point(&host_screen, 0, 100) && bs_point(&host_screen, 319, 100), "LINE");
    CHECK(bs_point(&host_screen, 15, 15), "BOX filled");
    CHECK(bs_point(&host_screen, 190, 120) && !bs_point(&host_screen, 160, 125), "CIRCLE");

    /* COLOR 2 inverts: twice and it is gone */
    fresh();
    run("10 COLOR 2: FOR I = 1 TO 2: BOX 10, 10, 50, 50: CIRCLE 100, 100, 20: NEXT\n"
        "20 PRINT POINT(10, 10); POINT(120, 100)\nRUN");
    CHECK(strstr(host_text, "00\n") != 0, "COLOR 2 twice erases: [%s]", host_text);

    fresh();
    CHECK(!strcmp(run("10 COLOR 3\nRUN"), "OUT OF RANGE IN 10\n"), "COLOR 3: [%s]", host_text);
    fresh();
    CHECK(!strcmp(run("10 CIRCLE 1, 1, -1\nRUN"), "OUT OF RANGE IN 10\n"), "a negative radius");

    /* off the screen is not an error */
    fresh();
    CHECK(!strcmp(run("10 PLOT -5, 999: LINE -100, -100, 500, 500: PRINT \"OK\"\nRUN"), "OK\n"),
          "drawing off screen: [%s]", host_text);

    /* CLS clears and homes the cursor */
    fresh();
    run("10 PLOT 100, 100: CLS: PRINT \"X\"\nRUN");
    CHECK(!bs_point(&host_screen, 100, 100) && cell_is(0, 0, 'X'), "CLS");
}

static void text_position(void) {
    fresh();
    run("10 LOCATE 5, 10: PRINT \"Z\"\nRUN");
    CHECK(cell_is(5, 10, 'Z'), "LOCATE 5, 10");
    fresh();
    CHECK(!strcmp(run("10 LOCATE 30, 0\nRUN"), "OUT OF RANGE IN 10\n"), "LOCATE off screen");
}

static void screen_keys_sync(void) {
    fresh();
    host_full_available = true;
    run("10 SCREEN 1\nRUN");
    CHECK(host_full, "SCREEN 1");
    run("10 SCREEN 0\nRUN");
    CHECK(!host_full, "SCREEN 0");
    CHECK(!strcmp(run("10 SCREEN 2\nRUN"), "OUT OF RANGE IN 10\n"), "SCREEN 2");
    host_full_available = false;
    CHECK(!strcmp(run("10 SCREEN 1\nRUN"), "NOT SUPPORTED IN 10\n"), "no full screen here");
    host_full_available = true;

    fresh();
    host_keys[0] = 'A';
    host_keys[1] = BP_KEY_UP;
    host_nkeys = 2;
    CHECK(!strcmp(run("10 PRINT KEY; \" \"; KEY; \" \"; KEY\nRUN"), "65 128 0\n"),
          "KEY: [%s]", host_text);

    fresh();
    host_syncs = 0;
    run("10 FOR I = 1 TO 3: SYNC: NEXT\nRUN");
    CHECK(host_syncs == 3, "SYNC: %d", host_syncs);
}

static void system_parts(void) {
    fresh();
    CHECK(!strcmp(run("10 PINS -, -, -, -: PRINT \"OK\"\nRUN"), "OK\n"),
          "PINS unused: [%s]", host_text);
    CHECK(!strcmp(run("10 PINS IN, -, -, -\nRUN"), "NOT SUPPORTED IN 10\n"), "no pins");
    fresh();
    CHECK(!strcmp(run("FORMAT YES"), "NOT SUPPORTED\n"), "FORMAT: [%s]", host_text);

    /* files */
    mkdir(host_dir, 0755);
    remove("/tmp/basic_host_files/ZZTEST.BAS");
    fresh();
    run("10 PLOT 1, 2: CIRCLE 3, 4, 5\nSAVE ZZTEST\nNEW\nLOAD ZZTEST");
    CHECK(!strcmp(run("LIST"), "10 PLOT 1, 2: CIRCLE 3, 4, 5\n"), "SAVE, LOAD: [%s]", host_text);
    CHECK(strstr(run("DIR"), "ZZTEST.BAS") != 0, "DIR: [%s]", host_text);
    run("DEL ZZTEST");
    CHECK(strstr(run("DIR"), "ZZTEST.BAS") == 0, "DEL");

    /* the extensions in HELP */
    CHECK(strstr(run("HELP"), "CLS COLOR PLOT LINE BOX CIRCLE POINT LOCATE SCREEN KEY SYNC") != 0,
          "HELP: [%s]", host_text);
}

static void big_programs(void) {
    char line[64];
    fresh();
    for (int i = 1; i <= 2000; i++) {           /* about 20 KB */
        snprintf(line, sizeof(line), "%d LET A = A + 1", i * 10);
        run(line);
    }
    CHECK(strstr(host_text, "NO MEMORY") == 0, "a 20 KB program fits");
    CHECK(!strcmp(run("20010 PRINT A\nRUN"), "2000\n"), "and runs: [%s]", host_text);
}

static void colours(void) {
    fresh();
    CHECK(!strcmp(run("10 INK 5: PLOT 1, 1: PRINT POINT(1, 1)\nRUN"), "5\n"),
        "INK 5, POINT: [%s]", host_text);
    fresh();
    CHECK(!strcmp(run("10 INK 3: PAPER 9: CLS: PRINT POINT(100, 100)\nRUN"), "9\n"),
        "PAPER 9, CLS: [%s]", host_text);
    fresh();
    run("10 INK 7: COLOR 1: PLOT 2, 2\nRUN");
    CHECK(bs_point(&host_screen, 2, 2) == 1, "COLOR 1 is white again");
    fresh();
    CHECK(!strcmp(run("10 PALETTE 1, 15, 0, 0\nRUN"), "") &&
        host_screen.pal[1] == 0xf00, "PALETTE: [%s]", host_text);
    fresh();
    CHECK(!strcmp(run("10 INK 16\nRUN"), "OUT OF RANGE IN 10\n"), "INK 16: [%s]", host_text);
    fresh();
    CHECK(!strcmp(run("10 PALETTE 1, 2, 3\nRUN"), "SYNTAX ERROR IN 10\n") ||
        strstr(host_text, "ERROR") != 0, "PALETTE short: [%s]", host_text);
    /* LIST shows the new statements by name */
    fresh();
    CHECK(strstr(run("10 INK 2: PAPER 0: PALETTE 0, 1, 2, 3\nLIST"),
        "INK 2") != 0, "LIST: [%s]", host_text);
}

int main(void) {
    printing();
    graphics();
    text_position();
    screen_keys_sync();
    system_parts();
    big_programs();
    colours();
    if (failures) {
        printf("FAILED: %d\n", failures);
        return 1;
    }
    printf("all BASIC computer tests passed\n");
    return 0;
}
