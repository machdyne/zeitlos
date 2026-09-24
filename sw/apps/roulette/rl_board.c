/*
 * Zeitlos roulette -- the betting layout, drawn and clicked.
 * See rl_board.h for the grid orientation and the line-click model.
 */

#include "rl_board.h"
#include "../../common/zfont.h"
#include "../../common/zshape.h"

#define FONT (&z_font_5x8)
#define FW 5
#define FH 8

const int32_t rl_chip_values[RL_NCHIPS] = { 1, 5, 25, 100 };

/* -- text --------------------------------------------------------------
 *
 * Hand-rolled, for the reason docs/app_runtime.md gives: one conversion
 * specifier links picolibc's formatter at a cost of around 100KB.
 */

char *rl_num(int32_t n, char *buf)
{
    char tmp[12];
    int i = 0, j = 0;
    bool neg = false;

    if (n < 0) { neg = true; n = -n; }
    if (n == 0) tmp[i++] = '0';
    while (n > 0 && i < 11) { tmp[i++] = (char)('0' + n % 10); n /= 10; }
    if (neg) buf[j++] = '-';
    while (i > 0) buf[j++] = tmp[--i];
    buf[j] = '\0';

    return buf;
}

static int slen(const char *s)
{
    int n = 0;
    while (s && s[n]) n++;
    return n;
}

static void cat(char *buf, int len, const char *s)
{
    int n = slen(buf);
    while (s && *s && n < len - 1) buf[n++] = *s++;
    buf[n] = '\0';
}

static void cat_num(char *buf, int len, int32_t v)
{
    char t[12];
    cat(buf, len, rl_num(v, t));
}

/* -- clipped primitives ------------------------------------------------
 *
 * z_fb_hw_fill_rect() clamps to the screen and to the window manager's
 * visible region but knows nothing about this app's content rectangle,
 * so the clamp here is the only thing stopping a fill painting over the
 * window frame.
 */
static void fill(const rl_layout_t *L, int x, int y, int w, int h, int color)
{
    int x1 = x + w - 1, y1 = y + h - 1;

    if (x < L->clip.x0) x = L->clip.x0;
    if (y < L->clip.y0) y = L->clip.y0;
    if (x1 > L->clip.x1) x1 = L->clip.x1;
    if (y1 > L->clip.y1) y1 = L->clip.y1;
    if (x1 < x || y1 < y) return;

    z_fb_hw_fill_rect(x, y, x1 - x + 1, y1 - y + 1, color);
}

static void frame(const rl_layout_t *L, int x, int y, int w, int h, int c)
{
    fill(L, x, y, w, 1, c);
    fill(L, x, y + h - 1, w, 1, c);
    fill(L, x, y, 1, h, c);
    fill(L, x + w - 1, y, 1, h, c);
}

static void text(const rl_layout_t *L, int x, int y, const char *s, int c)
{
    z_fb_draw_text(x, y, s, c, FONT, &L->clip);
}

static void text_center(const rl_layout_t *L, int cx, int y, const char *s,
    int c)
{
    text(L, cx - (slen(s) * FW) / 2, y, s, c);
}

/* One glyph, blown up by an integer factor.
 *
 * z_fb_draw_text() has one size, and the winning number deserves more
 * than five pixels: seeing where the ball landed is the payoff for
 * watching the spin, and it was being reported in the same 5x8 as
 * everything else on the panel.
 *
 * Drawn as filled rectangles from the font's own bitmap rather than
 * from new art -- z_font_t glyph rows are MSB-first in the top `w` bits
 * of each byte (zfont.h), which is the opposite of the card tiles'
 * packing and the one thing to get right here. */
static void big_glyph(const rl_layout_t *L, int x, int y, char ch, int s)
{
    const z_font_t *f = FONT;
    const uint8_t *g;
    int row, col;

    if (z_font_index(f, (unsigned char)ch) < 0) return;
    g = f->glyphs + z_font_index(f, (unsigned char)ch) * f->h;

    for (row = 0; row < f->h; row++)
        for (col = 0; col < f->w; col++)
            if (g[row] & (0x80 >> col))
                fill(L, x + col * s, y + row * s, s, s, 1);
}

static void big_text(const rl_layout_t *L, int x, int y, const char *t, int s)
{
    int i;
    for (i = 0; t[i]; i++) big_glyph(L, x + i * (FW + 1) * s, y, t[i], s);
}

static void shade(const rl_layout_t *L, int x, int y, int w, int h, int lvl)
{
    int x1 = x + w - 1, y1 = y + h - 1;

    if (x < L->clip.x0) x = L->clip.x0;
    if (y < L->clip.y0) y = L->clip.y0;
    if (x1 > L->clip.x1) x1 = L->clip.x1;
    if (y1 > L->clip.y1) y1 = L->clip.y1;
    if (x1 < x || y1 < y) return;

    z_fb_hw_fill_shade(x, y, x1 - x + 1, y1 - y + 1, lvl);
}

/* -- the grid -----------------------------------------------------------
 *
 * number(col, row) = col * 3 + (3 - row). See rl_board.h: the printed
 * layout runs upward, so the TOP row is 3, 6, 9 and the BOTTOM row is
 * 1, 4, 7.
 */
static int grid_number(int col, int row)
{
    return col * 3 + (3 - row);
}

/* Which column bet belongs to a grid row. Row 2 holds 1, 4, 7 ... which
 * is column-bet selector 0. They run opposite ways, which is exactly
 * the sort of thing that is right until somebody "tidies" it. */
static int row_colbet(int row)
{
    return 2 - row;
}

void rl_board_init(void)
{
}

void rl_board_layout(rl_layout_t *L, const rl_view_t *v, int ox, int oy,
    int w, int h)
{
    int i, board_h, wheel_h;
    int grid_w = 12 * RL_CELL_W;

    for (i = 0; i < (int)sizeof(rl_layout_t); i++) ((char *)L)[i] = 0;

    (void)v;

    L->ox = ox; L->oy = oy; L->w = w; L->h = h;
    L->clip.x0 = ox;
    L->clip.y0 = oy;
    L->clip.x1 = ox + w - 1;
    L->clip.y1 = oy + h - 1;

    L->status_y = oy;

    /* Bottom-anchored: the command line and the message keep the same
     * place whatever size the window is, because they are what is
     * being typed into. */
    L->cmd_y = oy + h - FH;
    L->msg_y = L->cmd_y - RL_LINE_H;

    L->even_y = L->msg_y - 2 - RL_OUTER_H;
    L->dozen_y = L->even_y - 1 - RL_OUTER_H;
    L->street_y = L->dozen_y - 1 - 5;        /* the thin street strip */
    L->grid_y = L->street_y - 3 * RL_CELL_H;

    /* The zero, the numbers and the column bets, centred as one block
     * so the table is symmetric in the window. */
    {
        int total = RL_ZERO_W + grid_w + RL_ZERO_W;
        L->zero_x = ox + (w - total) / 2;
        L->grid_x = L->zero_x + RL_ZERO_W;
        L->colbet_x = L->grid_x + grid_w;
        if (L->zero_x < ox) L->zero_x = ox;
    }

    board_h = (L->msg_y - 2) - L->grid_y;
    wheel_h = L->grid_y - 2 - (oy + RL_LINE_H);

    /* The wheel takes the space left over above the board, on the left,
     * with the information panel beside it. */
    L->wheel_r = wheel_h / 2 - 2;
    if (L->wheel_r > 58) L->wheel_r = 58;

    /* THE WHEEL'S FOOTPRINT IS BIGGER THAN ITS RADIUS. The ball rides
     * outside the wall and rl_wheel_patch() clears a disc that covers
     * it, so the space the wheel actually occupies is r + ball + 2. Fit
     * the radius to the band rather than the other way round: sized to
     * r alone, the clear reached into the status line and ate the
     * title -- which only showed once the ball was made bigger. */
    L->wheel_r -= L->wheel_r / 9 + 3;
    if (L->wheel_r < RL_WHEEL_MIN_R) L->wheel_r = RL_WHEEL_MIN_R;
    L->wheel_cx = ox + 4 + L->wheel_r;
    L->wheel_cy = oy + RL_LINE_H + wheel_h / 2;

    L->panel_x = L->wheel_cx + L->wheel_r + L->wheel_r / 9 + 9;
    L->panel_y = oy + RL_LINE_H;
    L->panel_w = (ox + w) - L->panel_x - 2;

    /* The chip selector and the two buttons live at the bottom of the
     * panel, beside the wheel, where there is room for them. */
    {
        int bw = 30, bh = 12;
        int bx = L->panel_x;
        int by = L->panel_y + wheel_h - bh - 14;

        for (i = 0; i < RL_NCHIPS; i++) {
            L->chip[i].x = bx + i * (bw + 2);
            L->chip[i].y = by;
            L->chip[i].w = bw;
            L->chip[i].h = bh;
        }

        L->spin.x = bx;
        L->spin.y = by + bh + 2;
        L->spin.w = 62;
        L->spin.h = bh;

        L->clear.x = bx + 64;
        L->clear.y = L->spin.y;
        L->clear.w = 62;
        L->clear.h = bh;
    }

    L->ok = (w >= RL_MIN_W) && (h >= RL_MIN_H) &&
        (L->wheel_r >= RL_WHEEL_MIN_R) && (L->grid_y > oy + RL_LINE_H);

    (void)board_h;
}

void rl_board_cell(const rl_layout_t *L, int number, rl_rect_t *out)
{
    int col, row;

    if (number == RL_ZERO || number == RL_DOUBLE_ZERO) {
        out->x = L->zero_x;
        out->y = L->grid_y;
        out->w = RL_ZERO_W;
        out->h = 3 * RL_CELL_H;
        return;
    }

    col = (number - 1) / 3;
    row = 3 - (number - col * 3);

    out->x = L->grid_x + col * RL_CELL_W;
    out->y = L->grid_y + row * RL_CELL_H;
    out->w = RL_CELL_W;
    out->h = RL_CELL_H;
}

/* -- hit testing -------------------------------------------------------- */

static bool in_rect(const rl_rect_t *r, int x, int y)
{
    return x >= r->x && x < r->x + r->w && y >= r->y && y < r->y + r->h;
}

int rl_board_chip_at(const rl_layout_t *L, int x, int y)
{
    int i;
    for (i = 0; i < RL_NCHIPS; i++)
        if (in_rect(&L->chip[i], x, y)) return i;
    return -1;
}

bool rl_board_hit(const rl_layout_t *L, const rl_view_t *v, int x, int y,
    int *type, int *sel)
{
    int gx = x - L->grid_x, gy = y - L->grid_y;
    int grid_w = 12 * RL_CELL_W, grid_h = 3 * RL_CELL_H;
    int wheel = v->round->wheel;

    /* The zero, which on a European table is one tall cell beside the
     * grid. */
    if (x >= L->zero_x && x < L->zero_x + RL_ZERO_W &&
        y >= L->grid_y && y < L->grid_y + grid_h) {
        /* An American table has two zeros sharing that space, split
         * top and bottom. */
        if (wheel == RL_AMERICAN) {
            *type = RL_STRAIGHT;
            *sel = (gy < grid_h / 2) ? RL_ZERO : RL_DOUBLE_ZERO;
        } else {
            *type = RL_STRAIGHT;
            *sel = RL_ZERO;
        }
        return true;
    }

    /* The column bets, right of the grid. */
    if (x >= L->colbet_x && x < L->colbet_x + RL_ZERO_W &&
        y >= L->grid_y && y < L->grid_y + grid_h) {
        int row = gy / RL_CELL_H;
        if (row < 0) row = 0;
        if (row > 2) row = 2;
        *type = RL_COLUMN;
        *sel = row_colbet(row);
        return true;
    }

    /* Inside the numbers grid: a cell, an edge, or a corner.
     *
     * THE EDGE BANDS ARE A THIRD OF A CELL. Generous enough to hit with
     * a mouse on a 320-pixel screen, tight enough that the middle of a
     * cell is unambiguously a straight-up bet. */
    if (gx >= 0 && gx < grid_w && gy >= 0 && gy < grid_h) {
        int col = gx / RL_CELL_W, row = gy / RL_CELL_H;
        int fx = gx - col * RL_CELL_W, fy = gy - row * RL_CELL_H;
        int bx = RL_CELL_W / 3, by = RL_CELL_H / 3;
        int near_left = fx < bx && col > 0;
        int near_right = fx >= RL_CELL_W - bx && col < 11;
        int near_top = fy < by && row > 0;
        int near_bot = fy >= RL_CELL_H - by && row < 2;
        int c, r;

        /* A corner first: it is the intersection of two edge bands, so
         * testing edges first would swallow it. */
        if ((near_left || near_right) && (near_top || near_bot)) {
            c = near_left ? col - 1 : col;
            r = near_top ? row - 1 : row;
            *type = RL_CORNER;
            /* rl_corner_set() indexes by street gap and which of the
             * two interior points; see rl_table.c. The grid row and the
             * corner's column run opposite ways. */
            *sel = c * 2 + (1 - r);
            return true;
        }

        if (near_left || near_right) {
            /* Horizontally adjacent on screen is a split BETWEEN two
             * streets, which rl_table.c indexes after the within-street
             * ones. */
            c = near_left ? col - 1 : col;
            *type = RL_SPLIT;
            *sel = (12 * 2) + c * 3 + (2 - row);
            return true;
        }

        if (near_top || near_bot) {
            r = near_top ? row - 1 : row;
            *type = RL_SPLIT;
            *sel = col * 2 + (1 - r);
            return true;
        }

        *type = RL_STRAIGHT;
        *sel = grid_number(col, row);
        return true;
    }

    /* The strip below the grid: a street under a column, a six line on
     * the boundary between two. */
    if (gx >= 0 && gx < grid_w &&
        y >= L->street_y && y < L->street_y + 5) {
        int col = gx / RL_CELL_W;
        int fx = gx - col * RL_CELL_W;

        if (fx < RL_CELL_W / 3 && col > 0) {
            *type = RL_SIXLINE;
            *sel = col - 1;
            return true;
        }
        if (fx >= RL_CELL_W - RL_CELL_W / 3 && col < 11) {
            *type = RL_SIXLINE;
            *sel = col;
            return true;
        }

        *type = RL_STREET;
        *sel = col;
        return true;
    }

    /* The dozens. */
    if (gx >= 0 && gx < grid_w &&
        y >= L->dozen_y && y < L->dozen_y + RL_OUTER_H) {
        *type = RL_DOZEN;
        *sel = gx / (grid_w / 3);
        if (*sel > 2) *sel = 2;
        return true;
    }

    /* And the six even-money bets. */
    if (gx >= 0 && gx < grid_w &&
        y >= L->even_y && y < L->even_y + RL_OUTER_H) {
        static const int order[6] = {
            RL_LOW, RL_EVEN, RL_RED, RL_BLACK, RL_ODD, RL_HIGH };
        int i = gx / (grid_w / 6);
        if (i > 5) i = 5;
        *type = order[i];
        *sel = 0;
        return true;
    }

    return false;
}

/* -- drawing ------------------------------------------------------------ */

/* A chip: its value in a bordered box, centred on the bet.
 *
 * -- it covers the number, and that is right --
 *
 * The first version drew a small disc with the value BESIDE it, to keep
 * the number underneath readable. The render showed why that is the
 * wrong instinct: the value spilled two cells sideways, so a chip on 17
 * sat across 20 and 23 and the table became impossible to read.
 *
 * A real chip sits ON the number and hides it, and nobody minds --
 * because you placed it, so you know what it is on, and the ones you
 * can still read are the ones you have not bet. Boxed and opaque is
 * both more legible and more like the thing it represents.
 *
 * Sized to the text, so a 1 is narrow and a 100 fills most of a cell
 * without overflowing it. */
static void draw_chip(const rl_layout_t *L, int x, int y, int32_t amount)
{
    char buf[12];
    int tw, w, h = FH + 2;

    rl_num(amount, buf);
    tw = slen(buf) * FW;
    w = tw + 4;

    fill(L, x - w / 2, y - h / 2, w, h, 0);
    frame(L, x - w / 2, y - h / 2, w, h, 1);
    text(L, x - tw / 2, y - h / 2 + 1, buf, 1);
}

static void draw_bets(const rl_layout_t *L, const rl_view_t *v)
{
    int i;

    for (i = 0; i < v->round->nbets; i++) {
        const rl_bet_t *b = &v->round->bet[i];
        rl_rect_t cell;
        int x, y;

        switch (b->type) {

        case RL_STRAIGHT:
            rl_board_cell(L, b->sel, &cell);
            x = cell.x + cell.w / 2;
            y = cell.y + cell.h / 2;
            break;

        case RL_COLUMN:
            x = L->colbet_x + RL_ZERO_W / 2;
            y = L->grid_y + (2 - b->sel) * RL_CELL_H + RL_CELL_H / 2;
            break;

        case RL_DOZEN:
            x = L->grid_x + b->sel * (12 * RL_CELL_W / 3) +
                (12 * RL_CELL_W / 6);
            y = L->dozen_y + RL_OUTER_H / 2;
            break;

        case RL_STREET:
            x = L->grid_x + b->sel * RL_CELL_W + RL_CELL_W / 2;
            y = L->street_y + 2;
            break;

        case RL_SIXLINE:
            x = L->grid_x + (b->sel + 1) * RL_CELL_W;
            y = L->street_y + 2;
            break;

        case RL_SPLIT: {
            int pair[2];
            rl_rect_t a, c;
            rl_split_pair(b->sel, pair);
            rl_board_cell(L, pair[0], &a);
            rl_board_cell(L, pair[1], &c);
            x = (a.x + c.x) / 2 + RL_CELL_W / 2;
            y = (a.y + c.y) / 2 + RL_CELL_H / 2;
            break;
        }

        case RL_CORNER: {
            int set[4];
            rl_rect_t a, c;
            rl_corner_set(b->sel, set);
            rl_board_cell(L, set[0], &a);
            rl_board_cell(L, set[3], &c);
            x = (a.x + c.x) / 2 + RL_CELL_W / 2;
            y = (a.y + c.y) / 2 + RL_CELL_H / 2;
            break;
        }

        default: {
            /* The six even-money bets, in the order they are drawn. */
            static const int order[6] = {
                RL_LOW, RL_EVEN, RL_RED, RL_BLACK, RL_ODD, RL_HIGH };
            int k, slot = 0;
            for (k = 0; k < 6; k++) if (order[k] == b->type) slot = k;
            x = L->grid_x + slot * (12 * RL_CELL_W / 6) +
                (12 * RL_CELL_W / 12);
            y = L->even_y + RL_OUTER_H / 2;
            break;
        }
        }

        draw_chip(L, x, y, b->amount);
    }
}

static void draw_grid(const rl_layout_t *L, const rl_view_t *v)
{
    int col, row, i;
    int grid_w = 12 * RL_CELL_W;
    char buf[8];

    /* The zero. On an American table it is two cells; on a European one,
     * a single tall one. */
    if (v->round->wheel == RL_AMERICAN) {
        int hh = (3 * RL_CELL_H) / 2;
        frame(L, L->zero_x, L->grid_y, RL_ZERO_W, hh, 1);
        text_center(L, L->zero_x + RL_ZERO_W / 2, L->grid_y + hh / 2 - 4,
            "0", 1);
        frame(L, L->zero_x, L->grid_y + hh, RL_ZERO_W, 3 * RL_CELL_H - hh, 1);
        text_center(L, L->zero_x + RL_ZERO_W / 2,
            L->grid_y + hh + (3 * RL_CELL_H - hh) / 2 - 4, "00", 1);
    } else {
        frame(L, L->zero_x, L->grid_y, RL_ZERO_W, 3 * RL_CELL_H, 1);
        text_center(L, L->zero_x + RL_ZERO_W / 2,
            L->grid_y + (3 * RL_CELL_H) / 2 - 4, "0", 1);
    }

    for (col = 0; col < 12; col++) {
        for (row = 0; row < 3; row++) {
            int n = grid_number(col, row);
            int cx = L->grid_x + col * RL_CELL_W;
            int cy = L->grid_y + row * RL_CELL_H;

            /* RED CELLS ARE SHADED, black ones left dark, both framed.
             * On a two-colour display a solid fill would make the
             * number inside it unreadable, and the number is the whole
             * point of the cell. The dither reads as colour and leaves
             * the glyph legible. */
            if (rl_is_red(n))
                shade(L, cx + 1, cy + 1, RL_CELL_W - 2, RL_CELL_H - 2, 6);

            frame(L, cx, cy, RL_CELL_W, RL_CELL_H, 1);

            rl_num(n, buf);
            /* On a solid strip, so the dither never eats the digits --
             * the same reasoning as sw/apps/poker's disabled buttons,
             * which shipped unreadable for exactly this. */
            {
                int tw = slen(buf) * FW;
                int tx = cx + (RL_CELL_W - tw) / 2;
                int ty = cy + (RL_CELL_H - FH) / 2;
                fill(L, tx - 1, ty, tw + 2, FH, 0);
                text(L, tx, ty, buf, 1);
            }
        }
    }

    /* The column bets. */
    for (row = 0; row < 3; row++) {
        int cy = L->grid_y + row * RL_CELL_H;
        frame(L, L->colbet_x, cy, RL_ZERO_W, RL_CELL_H, 1);
        text_center(L, L->colbet_x + RL_ZERO_W / 2, cy + (RL_CELL_H - FH) / 2,
            "2:1", 1);
    }

    /* The street strip. Thin, and unlabelled: it is a line to place a
     * chip on, and a label would not fit in five pixels. */
    fill(L, L->grid_x, L->street_y, grid_w, 5, 0);
    frame(L, L->grid_x, L->street_y, grid_w, 5, 1);
    for (col = 1; col < 12; col++)
        fill(L, L->grid_x + col * RL_CELL_W, L->street_y, 1, 5, 1);

    /* The dozens. */
    for (i = 0; i < 3; i++) {
        static const char *const nm[3] = { "1st 12", "2nd 12", "3rd 12" };
        int cw = grid_w / 3;
        int cx = L->grid_x + i * cw;
        frame(L, cx, L->dozen_y, cw, RL_OUTER_H, 1);
        text_center(L, cx + cw / 2, L->dozen_y + (RL_OUTER_H - FH) / 2,
            nm[i], 1);
    }

    /* And the even-money bets. */
    for (i = 0; i < 6; i++) {
        static const char *const nm[6] = {
            "1-18", "EVEN", "RED", "BLACK", "ODD", "19-36" };
        int cw = grid_w / 6;
        int cx = L->grid_x + i * cw;
        frame(L, cx, L->even_y, cw, RL_OUTER_H, 1);
        /* RED gets the same dither its cells do, so the word and the
         * numbers agree about what red looks like. */
        if (i == 2) shade(L, cx + 1, L->even_y + 1, cw - 2, RL_OUTER_H - 2, 6);
        {
            int tw = slen(nm[i]) * FW;
            int tx = cx + (cw - tw) / 2;
            int ty = L->even_y + (RL_OUTER_H - FH) / 2;
            fill(L, tx - 1, ty, tw + 2, FH, 0);
            text(L, tx, ty, nm[i], 1);
        }
    }
}

void rl_board_draw_wheel(const rl_layout_t *L, const rl_view_t *v,
    const z_clip_t *clip)
{
    rl_wheel_draw(v->wheel, clip ? clip : &L->clip);
}

static void button(const rl_layout_t *L, const rl_rect_t *r,
    const char *label, bool on)
{
    int tw = slen(label) * FW;
    int tx = r->x + (r->w - tw) / 2;
    int ty = r->y + (r->h - FH) / 2;

    fill(L, r->x, r->y, r->w, r->h, 0);
    frame(L, r->x, r->y, r->w, r->h, 1);

    if (on) {
        shade(L, r->x + 1, r->y + 1, r->w - 2, r->h - 2, 9);
        fill(L, tx - 1, ty, tw + 2, FH, 0);
    }

    text(L, tx, ty, label, 1);
}

static void draw_panel(const rl_layout_t *L, const rl_view_t *v)
{
    char line[48];
    int i, y = L->panel_y;

    fill(L, L->panel_x, L->panel_y, L->panel_w,
        L->grid_y - 2 - L->panel_y, 0);

    line[0] = '\0';
    cat(line, sizeof line, "chips ");
    cat_num(line, sizeof line, v->chips);
    text(L, L->panel_x, y, line, 1);
    y += RL_LINE_H;

    line[0] = '\0';
    cat(line, sizeof line, "staked ");
    cat_num(line, sizeof line, v->round->staked);
    text(L, L->panel_x, y, line, 1);
    y += RL_LINE_H;

    if (v->result >= 0) {
        char p[4];
        int32_t net = v->last_return - v->last_staked;
        int bw;

        /* THE WINNING NUMBER, LARGE. It is what the spin was for, and
         * at 5x8 on a line of its own it was the least prominent thing
         * on a panel that also says how many chips you have. */
        rl_pocket_str(v->result, p);
        bw = (int)slen(p) * (FW + 1) * 3;
        big_text(L, L->panel_x, y, p, 3);

        /* Its colour spelled out beside it, because on a two-colour
         * display the number cannot carry that itself -- and red or
         * black is half of what the person just bet on. */
        line[0] = '\0';
        if (v->result == RL_ZERO || v->result == RL_DOUBLE_ZERO)
            cat(line, sizeof line, "zero");
        else cat(line, sizeof line, rl_is_red(v->result) ? "red" : "black");
        text(L, L->panel_x + bw + 4, y, line, 1);

        line[0] = '\0';
        cat(line, sizeof line, net >= 0 ? "+" : "");
        cat_num(line, sizeof line, net);
        text(L, L->panel_x + bw + 4, y + FH + 2, line, 1);
    }
    y += 3 * FH;

    /* The recent results, newest first. The one thing every roulette
     * table in the world puts on a board, and the one thing that has no
     * bearing whatsoever on the next spin. It is here because people
     * want it, not because it means anything. */
    if (v->nhistory > 0) {
        line[0] = '\0';
        for (i = 0; i < v->nhistory && i < 6; i++) {
            char p[4];
            cat(line, sizeof line, rl_pocket_str(v->history[i], p));
            cat(line, sizeof line, " ");
        }
        text(L, L->panel_x, y, line, 1);
    }

    /* Whether the wheel is hardware-seeded. Shown rather than acted on:
     * sw/common/zrng.h is explicit that a game should use the generator
     * whatever its provenance and that only keys should refuse. This is
     * the useful thing to do with that bit. */
    text(L, L->panel_x, L->chip[0].y - RL_LINE_H,
        v->trng ? "wheel: TRNG seeded" : "wheel: no TRNG", 1);

    for (i = 0; i < RL_NCHIPS; i++) {
        char b[8];
        rl_num(rl_chip_values[i], b);
        button(L, &L->chip[i], b, i == v->chip_sel);
    }

    button(L, &L->spin, v->spinning ? "spinning" : "SPIN", false);
    button(L, &L->clear, "clear", false);
}

void rl_board_draw_status(const rl_layout_t *L, const rl_view_t *v)
{
    char line[RL_MSG_LEN + 4];

    fill(L, L->ox, L->msg_y, L->w, FH, 0);
    text(L, L->ox + 2, L->msg_y, v->message, 1);

    fill(L, L->ox, L->cmd_y, L->w, FH, 0);
    line[0] = '\0';
    cat(line, sizeof line, "> ");
    cat(line, sizeof line, v->cmd);
    cat(line, sizeof line, "_");
    text(L, L->ox + 2, L->cmd_y, line, 1);
}

void rl_board_draw(const rl_layout_t *L, const rl_view_t *v)
{
    char line[48];

    if (!L->ok) {
        fill(L, L->ox, L->oy, L->w, L->h, 0);
        text(L, L->ox + 2, L->oy + 2, "window too small", 1);
        text(L, L->ox + 2, L->oy + 2 + RL_LINE_H, "for a table", 1);
        return;
    }

    fill(L, L->ox, L->oy, L->w, L->h, 0);

    line[0] = '\0';
    cat(line, sizeof line, v->round->wheel == RL_AMERICAN ?
        "Roulette - american" : "Roulette - european");
    text(L, L->ox + 2, L->status_y, line, 1);

    line[0] = '\0';
    cat(line, sizeof line, "chip ");
    cat_num(line, sizeof line, rl_chip_values[v->chip_sel]);
    text(L, L->ox + L->w - 2 - slen(line) * FW, L->status_y, line, 1);

    rl_board_draw_wheel(L, v, &L->clip);
    draw_panel(L, v);
    draw_grid(L, v);
    draw_bets(L, v);
    rl_board_draw_status(L, v);
}
