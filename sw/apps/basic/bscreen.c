/*
 * basic -- the BASIC computer's screen (bscreen.h). Portable: nothing
 * here touches the hardware.
 *
 * Every shape draws each of its pixels exactly once. That matters for
 * COLOR 2 (invert): a pixel drawn twice would invert back, leaving holes
 * in a box's corners or a circle's diagonals, and drawing a shape a
 * second time would not erase it cleanly.
 */

#include <string.h>
#include "bscreen.h"
#include "../../common/zfont.h"

#define FONT    (&z_font_8x8)

static void dirty(bscreen_t *s, int y0, int y1) {
    if (y0 < 0) y0 = 0;
    if (y1 > BS_H) y1 = BS_H;
    if (y0 >= y1) return;
    if (s->dirty_y0 >= s->dirty_y1) {
        s->dirty_y0 = y0;
        s->dirty_y1 = y1;
        return;
    }
    if (y0 < s->dirty_y0) s->dirty_y0 = y0;
    if (y1 > s->dirty_y1) s->dirty_y1 = y1;
}

/* one pixel with the current operation, no bounds check */
static inline void pix(bscreen_t *s, int x, int y) {
    uint32_t *w = &s->px[y * BS_WORDS + (x >> 5)], m = 1u << (x & 31);
    if (s->op == BS_SET) *w |= m;
    else if (s->op == BS_CLEAR) *w &= ~m;
    else *w ^= m;
}

static inline void pix_clip(bscreen_t *s, int x, int y) {
    if (x >= 0 && x < BS_W && y >= 0 && y < BS_H) pix(s, x, y);
}

/* ---- text ---- */

static uint8_t reverse8(uint8_t b) {
    b = (uint8_t)((b & 0xF0) >> 4 | (b & 0x0F) << 4);
    b = (uint8_t)((b & 0xCC) >> 2 | (b & 0x33) << 2);
    return (uint8_t)((b & 0xAA) >> 1 | (b & 0x55) << 1);
}

/* the 8x8 cell at (row, col): its pixels replaced by glyph c, or XORed
 * with a solid block for the cursor */
static void cell(bscreen_t *s, int row, int col, int c, bool invert) {
    const uint8_t *g = 0;
    int x = col * 8, shift = x & 31;
    if (!invert) {
        int i = z_font_index(FONT, (uint32_t)c);
        if (i < 0) i = z_font_index(FONT, Z_GLYPH_MISSING);
        g = FONT->glyphs + i * FONT->h;
    }
    for (int r = 0; r < 8; r++) {
        uint32_t *w = &s->px[(row * 8 + r) * BS_WORDS + (x >> 5)];
        if (invert) *w ^= 0xFFu << shift;
        else *w = (*w & ~(0xFFu << shift)) | ((uint32_t)reverse8(g[r]) << shift);
    }
    dirty(s, row * 8, row * 8 + 8);
}

static void cursor_draw(bscreen_t *s) {
    cell(s, s->row, s->col, 0, true);
}

static void scroll(bscreen_t *s) {
    memmove(s->px, s->px + 8 * BS_WORDS, sizeof(s->px) - 8 * BS_STRIDE);
    memset(s->px + (BS_H - 8) * BS_WORDS, 0, 8 * BS_STRIDE);
    dirty(s, 0, BS_H);
}

static void newline(bscreen_t *s) {
    s->col = 0;
    if (++s->row == BS_ROWS) {
        scroll(s);
        s->row = BS_ROWS - 1;
    }
}

void bs_init(bscreen_t *s) {
    memset(s, 0, sizeof(*s));
    s->op = BS_SET;
    dirty(s, 0, BS_H);
}

void bs_cls(bscreen_t *s) {
    memset(s->px, 0, sizeof(s->px));
    s->row = s->col = 0;
    if (s->cursor) cursor_draw(s);
    dirty(s, 0, BS_H);
}

void bs_putc(bscreen_t *s, uint8_t c) {
    bool cur = s->cursor;
    if (cur) bs_cursor(s, false);
    if (c == '\r') {
        s->col = 0;
    } else if (c == '\n') {
        newline(s);
    } else if (c == '\b') {
        /* back one, onto the previous row if a line had wrapped */
        if (s->col > 0 || s->row > 0) {
            if (s->col-- == 0) {
                s->col = BS_COLS - 1;
                s->row--;
            }
            cell(s, s->row, s->col, ' ', false);
        }
    } else if (c >= 0x20 && c != 0x7F && !(c >= 0x80 && c < 0xA0)) {
        cell(s, s->row, s->col, c, false);
        if (++s->col == BS_COLS) newline(s);
    }
    if (cur) bs_cursor(s, true);
}

bool bs_locate(bscreen_t *s, int row, int col) {
    if (row < 0 || row >= BS_ROWS || col < 0 || col >= BS_COLS) return false;
    bool cur = s->cursor;
    if (cur) bs_cursor(s, false);
    s->row = row;
    s->col = col;
    if (cur) bs_cursor(s, true);
    return true;
}

void bs_cursor(bscreen_t *s, bool on) {
    if (on == s->cursor) return;
    cursor_draw(s);
    s->cursor = on;
}

/* ---- drawing ---- */

void bs_plot(bscreen_t *s, int x, int y) {
    pix_clip(s, x, y);
    dirty(s, y, y + 1);
}

/* Bresenham: every pixel from (x0, y0) to (x1, y1) once */
void bs_line(bscreen_t *s, int x0, int y0, int x1, int y1) {
    int dx = x1 > x0 ? x1 - x0 : x0 - x1, sx = x0 < x1 ? 1 : -1;
    int dy = y1 > y0 ? y0 - y1 : y1 - y0, sy = y0 < y1 ? 1 : -1;
    int e = dx + dy;
    dirty(s, y0 < y1 ? y0 : y1, (y0 > y1 ? y0 : y1) + 1);
    for (;;) {
        pix_clip(s, x0, y0);
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * e;
        if (e2 >= dy) { e += dy; x0 += sx; }
        if (e2 <= dx) { e += dx; y0 += sy; }
    }
}

void bs_box(bscreen_t *s, int x0, int y0, int x1, int y1, bool fill) {
    int t;
    if (x0 > x1) { t = x0; x0 = x1; x1 = t; }
    if (y0 > y1) { t = y0; y0 = y1; y1 = t; }
    dirty(s, y0, y1 + 1);
    if (fill) {
        for (int y = y0; y <= y1; y++)
            for (int x = x0; x <= x1; x++) pix_clip(s, x, y);
        return;
    }
    for (int x = x0; x <= x1; x++) {
        pix_clip(s, x, y0);
        if (y1 != y0) pix_clip(s, x, y1);
    }
    if (x1 != x0)
        for (int y = y0 + 1; y < y1; y++) {
            pix_clip(s, x0, y);
            pix_clip(s, x1, y);
        }
    else
        for (int y = y0 + 1; y < y1; y++) pix_clip(s, x0, y);
}

/* The midpoint circle. Its eight symmetric points coincide where x == 0,
 * y == x or y == 0; each of those is drawn once. */
void bs_circle(bscreen_t *s, int cx, int cy, int r) {
    if (r < 0) return;
    dirty(s, cy - r, cy + r + 1);
    if (r == 0) {
        pix_clip(s, cx, cy);
        return;
    }
    int x = r, y = 0, d = 1 - r;
    while (y <= x) {
        /* (x, y) in the first octant; mirror it, skipping coincidences */
        int pts[8][2] = {
            { x, y }, { y, x }, { -y, x }, { -x, y },
            { -x, -y }, { -y, -x }, { y, -x }, { x, -y },
        };
        for (int i = 0; i < 8; i++) {
            int px = pts[i][0], py = pts[i][1], dup = 0;
            for (int j = 0; j < i && !dup; j++)
                dup = pts[j][0] == px && pts[j][1] == py;
            if (!dup) pix_clip(s, cx + px, cy + py);
        }
        y++;
        if (d < 0) d += 2 * y + 1;
        else d += 2 * (y - --x) + 1;
    }
}

int bs_point(const bscreen_t *s, int x, int y) {
    if (x < 0 || x >= BS_W || y < 0 || y >= BS_H) return 0;
    return (s->px[y * BS_WORDS + (x >> 5)] >> (x & 31)) & 1;
}

bool bs_dirty(const bscreen_t *s, int *y0, int *y1) {
    if (s->dirty_y0 >= s->dirty_y1) return false;
    *y0 = s->dirty_y0;
    *y1 = s->dirty_y1;
    return true;
}

void bs_clean(bscreen_t *s) {
    s->dirty_y0 = s->dirty_y1 = 0;
}
