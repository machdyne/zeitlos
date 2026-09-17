/*
 * Zeitlos craps -- the table, drawn and clicked.
 * See cr_board.h for what the layout leaves out and why the dice need
 * no clearing.
 */

#include "cr_board.h"
#include "../../common/zfont.h"
#include "../../common/zshape.h"

#define FONT (&z_font_5x8)
#define FW 5
#define FH 8

static bool use_hw_blit;

void cr_board_init(void)
{
    use_hw_blit = z_fb_hw_blit_mem_available();
}

/* -- text ------------------------------------------------------------------ */

char *cr_num(int32_t n, char *buf)
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

/* Hand-rolled, for the reason docs/app_runtime.md gives: one conversion
 * specifier links picolibc's formatter at a cost of around 100KB. */
static void cat(char *b, int len, const char *s)
{
    int n = slen(b);
    while (s && *s && n < len - 1) b[n++] = *s++;
    b[n] = '\0';
}

static void cat_num(char *b, int len, int32_t v)
{
    char t[12];
    cat(b, len, cr_num(v, t));
}

/* -- clipped primitives ---------------------------------------------------- */

static void fill(const cr_layout_t *L, int x, int y, int w, int h, int c)
{
    int x1 = x + w - 1, y1 = y + h - 1;

    if (x < L->clip.x0) x = L->clip.x0;
    if (y < L->clip.y0) y = L->clip.y0;
    if (x1 > L->clip.x1) x1 = L->clip.x1;
    if (y1 > L->clip.y1) y1 = L->clip.y1;
    if (x1 < x || y1 < y) return;

    z_fb_hw_fill_rect(x, y, x1 - x + 1, y1 - y + 1, c);
}

static void frame(const cr_layout_t *L, int x, int y, int w, int h, int c)
{
    fill(L, x, y, w, 1, c); fill(L, x, y + h - 1, w, 1, c);
    fill(L, x, y, 1, h, c); fill(L, x + w - 1, y, 1, h, c);
}

static void shade(const cr_layout_t *L, int x, int y, int w, int h, int lvl)
{
    int x1 = x + w - 1, y1 = y + h - 1;

    if (x < L->clip.x0) x = L->clip.x0;
    if (y < L->clip.y0) y = L->clip.y0;
    if (x1 > L->clip.x1) x1 = L->clip.x1;
    if (y1 > L->clip.y1) y1 = L->clip.y1;
    if (x1 < x || y1 < y) return;

    z_fb_hw_fill_shade(x, y, x1 - x + 1, y1 - y + 1, lvl);
}

static void text(const cr_layout_t *L, int x, int y, const char *s, int c)
{
    z_fb_draw_text(x, y, s, c, FONT, &L->clip);
}

/* Ink on a SOLID cell, both colours in one call.
 *
 * z_fb_draw_text() with a colour of 0 does not reliably put black ink
 * on a white box: the hardware glyph path sets its blit pattern from
 * the colour, so drawing "dark" text is not the same operation as
 * drawing light text on a dark ground. Filling a white rectangle and
 * then asking for colour-0 text left the number invisible.
 *
 * z_fb_draw_text2() exists for exactly this -- it fills the glyph cell
 * with the background and lays the ink over it, which is what the
 * hardware does natively, so a clipped glyph and an unclipped one look
 * the same. */
static void text2(const cr_layout_t *L, int x, int y, const char *s,
    int fg, int bg)
{
    z_fb_draw_text2(x, y, s, fg, bg, FONT, &L->clip);
}

static void text_mid(const cr_layout_t *L, const cr_rect_t *r, const char *s,
    int c)
{
    text(L, r->x + (r->w - slen(s) * FW) / 2,
        r->y + (r->h - FH) / 2, s, c);
}

/* -- the dice -------------------------------------------------------------- */

static void blit_die(const cr_layout_t *L, int face, int dx, int dy)
{
    const uint32_t *tile = z_dice_tile(Z_DICE_BIG, face);
    int sx = 0, sy = 0, w = Z_DICE_BIG, h = Z_DICE_BIG;
    int i, passes;

    if (!tile) return;

    /* Clamp the destination to the clip and move the SOURCE origin in
     * step. Clamping only the destination slides the wrong part of the
     * die into view. */
    if (dx < L->clip.x0) { int d = L->clip.x0 - dx; sx += d; w -= d; dx += d; }
    if (dy < L->clip.y0) { int d = L->clip.y0 - dy; sy += d; h -= d; dy += d; }
    if (dx + w - 1 > L->clip.x1) w = L->clip.x1 - dx + 1;
    if (dy + h - 1 > L->clip.y1) h = L->clip.y1 - dy + 1;
    if (w <= 0 || h <= 0) return;

    if (!use_hw_blit) {
        int px, py;
        for (py = 0; py < h; py++)
            for (px = 0; px < w; px++)
                z_fb_set_pixel(dx + px, dy + py,
                    (int)((tile[sy + py] >> (sx + px)) & 1u), &L->clip);
        return;
    }

    /* The blitter's scissor is persistent hardware state and
     * z_fb_hw_blit_mem() sets CTRL_CLIP without programming it, so a
     * blit issued without this inherits whatever rectangle the last
     * unrelated operation left behind. */
    passes = z_gfx_visible_count();
    if (passes < 1) passes = 1;

    for (i = 0; i < passes; i++) {
        z_clip_t c;
        c.x0 = dx; c.y0 = dy; c.x1 = dx + w - 1; c.y1 = dy + h - 1;
        if (!z_gfx_blit_scissor(i, &c)) continue;
        z_fb_hw_blit_mem(tile + sy, Z_DICE_STRIDE, sx, 0, dx, dy, w, h);
    }

    z_gfx_blit_scissor_reset();
}

void cr_board_draw_dice(const cr_layout_t *L, const cr_view_t *v)
{
    /* TWO OPAQUE BLITS AT A FIXED POSITION, and nothing cleared. A die
     * tile has a lit body, so the new face covers the old one exactly
     * -- there is no frame on which either die is blank. */
    blit_die(L, v->show_d1, L->dice_x, L->dice_y);
    blit_die(L, v->show_d2, L->dice_x + Z_DICE_BIG + 4, L->dice_y);
}

/* -- layout ----------------------------------------------------------------
 *
 * One end of a real table, top to bottom: the place numbers, the come
 * box, the field, the two line bets, then the propositions.
 */
static void add(cr_layout_t *L, int x, int y, int w, int h, int type, int sel,
    const char *label)
{
    cr_spot_t *s;

    if (L->nspots >= CR_NSPOTS) return;

    s = &L->spot[L->nspots++];
    s->r.x = x; s->r.y = y; s->r.w = w; s->r.h = h;
    s->type = (uint8_t)type;
    s->sel = (uint8_t)sel;
    s->label = label;
}

void cr_board_layout(cr_layout_t *L, const cr_view_t *v, int ox, int oy,
    int w, int h)
{
    static const int placenum[6] = { 4, 5, 6, 8, 9, 10 };
    static const char *const placelab[6] = { "4", "5", "6", "8", "9", "10" };
    static const int hardnum[4] = { 4, 6, 8, 10 };
    static const char *const hardlab[4] = { "H4", "H6", "H8", "H10" };
    int i, y, pw;

    for (i = 0; i < (int)sizeof(cr_layout_t); i++) ((char *)L)[i] = 0;

    (void)v;

    L->ox = ox; L->oy = oy; L->w = w; L->h = h;
    L->clip.x0 = ox; L->clip.y0 = oy;
    L->clip.x1 = ox + w - 1; L->clip.y1 = oy + h - 1;

    L->status_y = oy;

    L->cmd_y = oy + h - FH;
    L->msg_y = L->cmd_y - CR_LINE_H;

    /* The dice and the point puck share a strip under the status line.
     * Top-anchored: they are what a player looks at first, and the felt
     * below them is what moves. */
    L->dice_x = ox + 6;
    L->dice_y = oy + CR_LINE_H + 2;
    L->puck_x = L->dice_x + (Z_DICE_BIG + 4) * 2 + 10;
    L->puck_y = L->dice_y + 4;
    L->info_x = L->puck_x + 40;

    y = L->dice_y + Z_DICE_BIG + 4;

    /* THE FELT FILLS WHAT IS LEFT.
     *
     * Six bands, sized from the space between the dice and the message
     * rather than from constants. Fixed heights left a third of the
     * window empty below the proposition strip -- the same "all the
     * slack in one place" that poker, blackjack and slots each had, and
     * the same fix.
     *
     * The come box gets the extra, because it is the only band a player
     * aims at without a number to guide them. */
    {
        int avail = (L->msg_y - 3) - y;
        int gap = 2;
        int band = (avail - 5 * gap) / 6;
        int extra;

        if (band < 14) band = 14;
        extra = avail - 5 * gap - band * 6;
        if (extra < 0) extra = 0;
        if (extra > band) extra = band;

        /* The place numbers, across the top of the felt. */
        pw = (w - 4) / 6;
        for (i = 0; i < 6; i++)
            add(L, ox + 2 + i * pw, y, pw - 2, band, CR_PLACE, placenum[i],
                placelab[i]);
        y += band + gap;

        add(L, ox + 2, y, w - 4, band + extra, CR_COME, 0, "COME");
        y += band + extra + gap;

        add(L, ox + 2, y, w - 4, band, CR_FIELD, 0,
            "FIELD  2 3 4 9 10 11 12");
        y += band + gap;

        /* The two line bets. Don't pass first, because on a real table
         * it sits inside the pass line and a player reads outward. */
        add(L, ox + 2, y, (w - 6) / 2, band, CR_DONT_PASS, 0, "DON'T PASS");
        add(L, ox + 2 + (w - 6) / 2 + 2, y, (w - 6) / 2, band,
            CR_PASS, 0, "PASS LINE");
        y += band + gap;

        /* THE ODDS GO BEHIND THE LINE, which is both where a real table
         * puts them and the only way they are visible at all. Without a
         * spot of their own they were riding on the point with nothing
         * on the felt to show it -- on the one bet that is worth more
         * than everything else here put together. */
        add(L, ox + 2, y, (w - 6) / 2, band, CR_DONT_ODDS, 0, "lay odds");
        add(L, ox + 2 + (w - 6) / 2 + 2, y, (w - 6) / 2, band,
            CR_PASS_ODDS, 0, "ODDS");
        y += band + gap;

        /* The proposition strip: hardways, then the one-roll bets. */
        pw = (w - 4) / 8;
        for (i = 0; i < 4; i++)
            add(L, ox + 2 + i * pw, y, pw - 2, band, CR_HARD, hardnum[i],
                hardlab[i]);
        add(L, ox + 2 + 4 * pw, y, pw - 2, band, CR_ANY7, 0, "7");
        add(L, ox + 2 + 5 * pw, y, pw - 2, band, CR_ANY_CRAPS, 0, "C");
        add(L, ox + 2 + 6 * pw, y, pw - 2, band, CR_ELEVEN, 0, "11");
        add(L, ox + 2 + 7 * pw, y, pw - 2, band, CR_TWO, 0, "2/12");
        y += band;
    }

    L->ok = (w >= CR_MIN_W) && (h >= CR_MIN_H) && (y + 2 <= L->msg_y);
}

bool cr_board_dice_at(const cr_layout_t *L, int x, int y)
{
    /* Both dice and the gap between them, with a margin -- a target
     * this size wants to be forgiving, and there is nothing else
     * nearby to hit by mistake. */
    int x0 = L->dice_x - 2;
    int x1 = L->dice_x + (Z_DICE_BIG + 4) * 2;
    int y0 = L->dice_y - 2;
    int y1 = L->dice_y + Z_DICE_BIG + 2;

    return x >= x0 && x <= x1 && y >= y0 && y <= y1;
}

int cr_board_spot_at(const cr_layout_t *L, int x, int y)
{
    int i;

    for (i = 0; i < L->nspots; i++) {
        const cr_rect_t *r = &L->spot[i].r;
        if (x >= r->x && x < r->x + r->w && y >= r->y && y < r->y + r->h)
            return i;
    }

    return CR_SPOT_NONE;
}

/* -- drawing --------------------------------------------------------------- */

static void draw_puck(const cr_layout_t *L, const cr_view_t *v)
{
    char buf[8];
    int cx = L->puck_x + 12, cy = L->puck_y + 12;

    fill(L, L->puck_x, L->puck_y, 26, 26, 0);

    z_fb_circle(cx, cy, 12, 1, &L->clip);

    if (v->g->point == 0) {
        /* OFF, and said so rather than left blank. "No point" and "the
         * display has not drawn yet" look identical otherwise, and one
         * of them means a pass line bet is about to be very different. */
        z_fb_circle(cx, cy, 10, 1, &L->clip);
        text(L, cx - 3 * FW / 2, cy - 4, "OFF", 1);
        return;
    }

    /* On, and filled, because a point being on changes what every bet
     * on the table is doing. */
    z_fb_fill_circle(cx, cy, 10, 1, &L->clip);
    cr_num(v->g->point, buf);
    text2(L, cx - slen(buf) * FW / 2, cy - 4, buf, 0, 1);
}

/* The odds spots have no number of their own -- the POINT is their
 * number, and it changes under them. Resolving it here rather than at
 * layout time is what lets the felt be laid out once and still follow
 * the game. */
int cr_spot_sel(const cr_layout_t *L, const cr_view_t *v, int i)
{
    const cr_spot_t *s = &L->spot[i];

    if (s->type == CR_PASS_ODDS || s->type == CR_DONT_ODDS)
        return v->g->point;

    return s->sel;
}

static void draw_spot(const cr_layout_t *L, const cr_view_t *v, int i)
{
    const cr_spot_t *s = &L->spot[i];
    int sel = cr_spot_sel(L, v, i);
    int32_t on = cr_bet_on(v->g, s->type, sel);
    bool live = cr_can_place(v->g, s->type, sel, v->bet);

    char lab[20];

    /* THE LABEL IS BUILT FIRST, because the hole cut out of the dither
     * has to be the size of what is actually drawn. The odds boxes name
     * the point they are riding on, and the first version cut a hole
     * for "ODDS" and then drew "ODDS 8" -- putting the number that
     * matters most on the one part of the box that eats glyphs. */
    lab[0] = '\0';
    cat(lab, sizeof lab, s->label);
    if ((s->type == CR_PASS_ODDS || s->type == CR_DONT_ODDS) && sel) {
        cat(lab, sizeof lab, " ");
        cat_num(lab, sizeof lab, sel);
    }

    fill(L, s->r.x, s->r.y, s->r.w, s->r.h, 0);
    frame(L, s->r.x, s->r.y, s->r.w, s->r.h, 1);

    /* A spot that cannot be bet right now is shaded, with a solid hole
     * cut back out for its label. THERE IS NO GREY HERE: a dither is
     * black pixels and white pixels, so a glyph drawn over one loses
     * half of itself. */
    if (!live) {
        int tw = slen(lab) * FW;
        int tx = s->r.x + (s->r.w - tw) / 2;
        int ty = s->r.y + (s->r.h - FH) / 2;
        shade(L, s->r.x + 1, s->r.y + 1, s->r.w - 2, s->r.h - 2, 6);
        fill(L, tx - 1, ty, tw + 2, FH, 0);
    }

    text_mid(L, &s->r, lab, 1);

    /* What is riding on it, in the corner. A craps table's whole state
     * is where the chips are, so this is not decoration. */
    if (on > 0) {
        char buf[12];
        cr_num(on, buf);
        /* A dark number on a light chip, in one call. The two-step
         * version left it invisible -- see text2(). */
        text2(L, s->r.x + 2, s->r.y + 1, buf, 0, 1);
    }
}

void cr_board_draw_status(const cr_layout_t *L, const cr_view_t *v)
{
    char line[CR_CMD_LEN + 4];

    fill(L, L->ox, L->msg_y, L->w, FH, 0);
    text(L, L->ox + 2, L->msg_y, v->message, 1);

    fill(L, L->ox, L->cmd_y, L->w, FH, 0);
    line[0] = '\0';
    cat(line, sizeof line, "> ");
    cat(line, sizeof line, v->cmd);
    cat(line, sizeof line, "_");
    text(L, L->ox + 2, L->cmd_y, line, 1);
}

void cr_board_draw(const cr_layout_t *L, const cr_view_t *v)
{
    char line[64];
    int i;

    if (!L->ok) {
        fill(L, L->ox, L->oy, L->w, L->h, 0);
        text(L, L->ox + 2, L->oy + 2, "window too small", 1);
        text(L, L->ox + 2, L->oy + 2 + CR_LINE_H, "for a craps table", 1);
        return;
    }

    fill(L, L->ox, L->oy, L->w, L->h, 0);

    line[0] = '\0';
    cat(line, sizeof line, "Craps  bet ");
    cat_num(line, sizeof line, v->bet);
    text(L, L->ox + 2, L->status_y, line, 1);

    line[0] = '\0';
    cat_num(line, sizeof line, v->chips);
    cat(line, sizeof line, " chips");
    text(L, L->ox + L->w - 2 - slen(line) * FW, L->status_y, line, 1);

    cr_board_draw_dice(L, v);
    draw_puck(L, v);

    /* What is at risk, beside the puck. On a craps table the amount on
     * the felt is easy to lose track of, because it accumulates across
     * rolls rather than being staked once. */
    line[0] = '\0';
    cat(line, sizeof line, "at risk ");
    cat_num(line, sizeof line, cr_at_risk(v->g));
    text(L, L->info_x, L->dice_y + 4, line, 1);

    line[0] = '\0';
    cat(line, sizeof line, "roll ");
    if (v->rolling) cat(line, sizeof line, "...");
    else if (v->g->rolls == 0) cat(line, sizeof line, "-");
    else cat_num(line, sizeof line, v->g->d1 + v->g->d2);
    text(L, L->info_x, L->dice_y + 4 + CR_LINE_H, line, 1);

    if (!v->rolling && v->g->rolls > 0) {
        line[0] = '\0';
        if (v->g->seven_out) cat(line, sizeof line, "seven out");
        else if (v->g->point_made) cat(line, sizeof line, "point made");
        else if (v->g->last_won > 0) {
            cat(line, sizeof line, "won ");
            cat_num(line, sizeof line, v->g->last_won);
        }
        text(L, L->info_x, L->dice_y + 4 + CR_LINE_H * 2, line, 1);
    }

    for (i = 0; i < L->nspots; i++) draw_spot(L, v, i);

    cr_board_draw_status(L, v);
}
