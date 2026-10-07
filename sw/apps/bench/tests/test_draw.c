/*
 * bench -- host tests for drawing (draw.c): layout, hit testing, net
 * information, and a rendered image for a look (BENCH_PBM=dir).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../core.h"
#include "../netlist.h"
#include "../draw.h"

static int failures;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL %d: ", __LINE__); \
    printf(__VA_ARGS__); printf("\n"); failures++; } } while (0)

static const char panel[] =
    "# panel.net\n"
    "tca9535 x1 addr=0x20\n"
    "bus main x1\n"
    "led l0 x1.P00\nled l1 x1.P01\nled l2 x1.P02\nled l3 x1.P03\n"
    "button b0 x1.P10 pullup\nbutton b1 x1.P11 pullup\n"
    "load lamp \"Grow light\" x1.P04\n"
    "switch s1 x1.P12 pullup\n";

static void pbm(canvas_t *c, const char *name) {
    const char *dir = getenv("BENCH_PBM");
    char path[256];
    if (!dir) return;
    snprintf(path, sizeof(path), "%s/%s.pbm", dir, name);
    FILE *f = fopen(path, "w");
    if (!f) return;
    fprintf(f, "P1\n%d %d\n", c->w, c->h);
    for (int y = 0; y < c->h; y++) {
        for (int x = 0; x < c->w; x++)
            fputc((c->px[y * c->wpl + (x >> 5)] >> (x & 31)) & 1 ? '1' : '0', f);
        fputc('\n', f);
    }
    fclose(f);
}

int main(void) {
    char err[128], info[160];
    static uint32_t buf[20 * 1024];
    canvas_t c;
    int pin;

    CHECK(bn_load(panel, err, sizeof(err)) == 0, "%s", err);
    bn_xfer(0, 0x20, (const uint8_t []){ 6, 0x00, 0xFF }, 3, 0, 0, 0);
    bn_xfer(0, 0x20, (const uint8_t []){ 2, 0x55 }, 2, 0, 0, 0);
    bn_xfer(0, 0x21, (const uint8_t []){ 0 }, 1, 0, 0, 0);
    bn_part_find("b0")->type->click(bn_part_find("b0"), true);

    /* 400 pixels across: five cells a row; the expander takes three */
    int h = bd_layout(400);
    CHECK(bd_cards[0].x == 0 && bd_cards[0].y == 0 && bd_cards[0].w == 3 * BD_CELL_W,
          "the expander first, three cells wide");
    CHECK(bd_cards[1].x == 3 * BD_CELL_W && bd_cards[1].y == 0, "l0 beside it");
    CHECK(h > 2 * BD_CELL_H, "taller than two rows: %d", h);

    /* narrower: the same parts, more rows */
    int h2 = bd_layout(220);
    CHECK(h2 > h, "a narrow window: taller (%d > %d)", h2, h);

    c.w = 400;
    c.h = bd_layout(400);
    c.wpl = (c.w + 31) / 32;
    c.px = buf;
    bd_draw(&c, 0, 0);
    pbm(&c, "panel");

    /* a view onto the document: drawn from an origin, it is exactly that
     * piece of the whole (the app's canvas is the view, not the document) */
    {
        static uint32_t part[20 * 128];
        canvas_t v = { part, 150, 100, 5 };
        int ox = 37, oy = 61, bad = 0;
        bd_draw(&v, ox, oy);
        for (int y = 0; y < v.h; y++)
            for (int x = 0; x < v.w; x++) {
                int a = (part[y * v.wpl + (x >> 5)] >> (x & 31)) & 1;
                int b = (buf[(y + oy) * c.wpl + ((x + ox) >> 5)] >> ((x + ox) & 31)) & 1;
                if (a != b) bad++;
            }
        CHECK(!bad, "a view at (37, 61): %d pixels differ from the whole", bad);
        bd_draw(&c, 0, 0);
    }

    /* clicks: a pin of the expander, an LED */
    int i = bd_hit(bd_cards[0].x + 2 + 6 + 3, bd_cards[0].y + 2 + 16 + 12, &pin);
    CHECK(i == 0 && pin == 0, "the expander's P00: %d %d", i, pin);
    bd_net_info(&bn_parts[i], pin, info, sizeof(info));
    CHECK(!strcmp(info, "x1.P00: high -- x1.P00 l0.PIN"), "net info: [%s]", info);
    i = bd_hit(bd_cards[0].x + 2 + 6 + 26 * 2 + 3, bd_cards[0].y + 2 + 16 + 30 + 12, &pin);
    bd_net_info(&bn_parts[i], pin, info, sizeof(info));
    CHECK(!strcmp(info, "x1.P12: low, pull-up -- x1.P12 s1.PIN") ||
          !strcmp(info, "x1.P12: high, pull-up -- x1.P12 s1.PIN"), "the switch's net: [%s]", info);
    i = bd_hit(bd_cards[2].x + 30, bd_cards[2].y + 25, &pin);
    CHECK(i == 2 && pin == 0, "l1's card");
    CHECK(bd_hit(5000, 5000, &pin) == -1, "nothing there");

    /* the log */
    int lh = bd_draw_log(0, 0);
    c.h = lh;
    bd_draw_log(&c, 0);
    pbm(&c, "log");
    CHECK(lh == 4 + 3 * 10 + 4, "three transactions: %d", lh);

    if (failures) {
        printf("FAILED: %d\n", failures);
        return 1;
    }
    printf("all bench drawing tests passed\n");
    return 0;
}
