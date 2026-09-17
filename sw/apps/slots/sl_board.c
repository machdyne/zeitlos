/*
 * Zeitlos slots -- the machine, drawn and clicked.
 * See sl_board.h for why the reels do not use the hardware scroll.
 */

#include "sl_board.h"
#include "../../common/zfont.h"
#include "../../common/zshape.h"

#define FONT (&z_font_5x8)
#define FW 5
#define FH 8

#define REEL_GAP 6

static bool use_hw_blit;

void sl_board_init(void)
{
    use_hw_blit = z_fb_hw_blit_mem_available();
}

/* -- text ---------------------------------------------------------------- */

char *sl_num(int32_t n, char *buf)
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
static void cat(char *buf, int len, const char *s)
{
    int n = slen(buf);
    while (s && *s && n < len - 1) buf[n++] = *s++;
    buf[n] = '\0';
}

static void cat_num(char *buf, int len, int32_t v)
{
    char t[12];
    cat(buf, len, sl_num(v, t));
}

/* -- clipped primitives --------------------------------------------------
 *
 * z_fb_hw_fill_rect() clamps to the screen and to wm's visible region
 * but knows nothing about this app's content rectangle.
 */
static void fill(const sl_layout_t *L, int x, int y, int w, int h, int c)
{
    int x1 = x + w - 1, y1 = y + h - 1;

    if (x < L->clip.x0) x = L->clip.x0;
    if (y < L->clip.y0) y = L->clip.y0;
    if (x1 > L->clip.x1) x1 = L->clip.x1;
    if (y1 > L->clip.y1) y1 = L->clip.y1;
    if (x1 < x || y1 < y) return;

    z_fb_hw_fill_rect(x, y, x1 - x + 1, y1 - y + 1, c);
}

static void frame(const sl_layout_t *L, int x, int y, int w, int h, int c)
{
    fill(L, x, y, w, 1, c);
    fill(L, x, y + h - 1, w, 1, c);
    fill(L, x, y, 1, h, c);
    fill(L, x + w - 1, y, 1, h, c);
}

static void shade(const sl_layout_t *L, int x, int y, int w, int h, int lvl)
{
    int x1 = x + w - 1, y1 = y + h - 1;

    if (x < L->clip.x0) x = L->clip.x0;
    if (y < L->clip.y0) y = L->clip.y0;
    if (x1 > L->clip.x1) x1 = L->clip.x1;
    if (y1 > L->clip.y1) y1 = L->clip.y1;
    if (x1 < x || y1 < y) return;

    z_fb_hw_fill_shade(x, y, x1 - x + 1, y1 - y + 1, lvl);
}

static void text(const sl_layout_t *L, int x, int y, const char *s, int c)
{
    z_fb_draw_text(x, y, s, c, FONT, &L->clip);
}

/* Ink on a SOLID cell, both colours in one call.
 *
 * z_fb_draw_text() with a colour of 0 does not reliably put dark ink on
 * a light box: the hardware glyph path sets its blit pattern from the
 * colour, so drawing "dark" text is a different operation from drawing
 * light text on a dark ground. Filling a white rectangle and then
 * asking for colour-0 text leaves nothing visible.
 *
 * z_fb_draw_text2() exists for exactly this -- it fills the glyph cell
 * with the background and lays the ink over it, which is what the
 * hardware does natively, so a clipped glyph and an unclipped one look
 * the same. */
static void text2(const sl_layout_t *L, int x, int y, const char *s,
    int fg, int bg)
{
    z_fb_draw_text2(x, y, s, fg, bg, FONT, &L->clip);
}

/* -- tiles ---------------------------------------------------------------- */

static void blit_tile(const z_clip_t *clip, const uint32_t *tile,
    int dx, int dy)
{
    int sx = 0, sy = 0, w = SL_ART_W, h = SL_ART_H;
    int i, passes;

    /* Clamp the destination to the clip and move the SOURCE origin in
     * step. Clamping only the destination would slide the wrong part of
     * the symbol into view -- a half-visible tile at the top of a reel
     * would show its middle rather than its bottom, which is exactly
     * the case a spinning reel is in on almost every frame. */
    if (dx < clip->x0) { int d = clip->x0 - dx; sx += d; w -= d; dx += d; }
    if (dy < clip->y0) { int d = clip->y0 - dy; sy += d; h -= d; dy += d; }
    if (dx + w - 1 > clip->x1) w = clip->x1 - dx + 1;
    if (dy + h - 1 > clip->y1) h = clip->y1 - dy + 1;
    if (w <= 0 || h <= 0) return;

    if (!use_hw_blit) {
        int px, py;
        for (py = 0; py < h; py++)
            for (px = 0; px < w; px++)
                z_fb_set_pixel(dx + px, dy + py,
                    (int)((tile[sy + py] >> (sx + px)) & 1u), clip);
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
        z_fb_hw_blit_mem(tile + sy, SL_ART_STRIDE, sx, 0, dx, dy, w, h);
    }

    z_gfx_blit_scissor_reset();
}

/* -- layout ---------------------------------------------------------------- */

void sl_board_layout(sl_layout_t *L, const sl_view_t *v, int ox, int oy,
    int w, int h)
{
    int i, reels_w;

    for (i = 0; i < (int)sizeof(sl_layout_t); i++) ((char *)L)[i] = 0;

    (void)v;

    L->ox = ox; L->oy = oy; L->w = w; L->h = h;
    L->clip.x0 = ox;
    L->clip.y0 = oy;
    L->clip.x1 = ox + w - 1;
    L->clip.y1 = oy + h - 1;

    L->status_y = oy;

    /* Bottom-anchored: the command line, the message and the buttons
     * keep their place whatever size the window is, because they are
     * what is typed into and clicked. */
    L->cmd_y = oy + h - FH;
    L->msg_y = L->cmd_y - SL_LINE_H;
    L->info_y = L->msg_y - 3 - FH;

    {
        int n = SL_NBTN;
        int bw = (w - (n - 1) * 3) / n;
        int by = L->info_y - 3 - SL_BTN_H;
        if (bw < 1) bw = 1;
        for (i = 0; i < n; i++) {
            L->btn[i].x = ox + i * (bw + 3);
            L->btn[i].y = by;
            L->btn[i].w = bw;
            L->btn[i].h = SL_BTN_H;
        }
    }

    /* THE REELS ARE CENTRED IN WHAT IS LEFT, not hung under the status
     * line.
     *
     * Everything else here is bottom-anchored, because the command line
     * and the buttons must not move when the window resizes. Anchoring
     * the reels to the top as well put every spare pixel in one place:
     * a band of nothing between the glass and the buttons, taller than
     * the reels themselves. sw/apps/poker and sw/apps/blackjack both
     * had exactly this and it was fixed the same way. */
    L->reel_h = SL_ROWS * SL_CELL;
    L->reel_pitch = SL_ART_W + REEL_GAP;
    reels_w = SL_REELS * L->reel_pitch - REEL_GAP;

    L->mark_l = ox + 4;

    /* TWO MARKER COLUMNS, A GAP, THEN THE REEL FRAME.
     *
     * 9 for the outer marker, 1 clear, 9 for the inner, 3 clear, and
     * the frame. The first version left 17, which put the inner
     * marker's right border on the same column as the reel frame and
     * made the two markers share a wall -- so a full repaint drew the
     * frame through the marker and the digit beside it came and went.
     *
     * tests/layout_test.c asserts the separation rather than the
     * number, so moving anything here fails loudly instead of
     * quietly. */
    L->reel_x = L->mark_l + SL_MARK_W * 2 + 1 + 3 + 2;
    {
        int top = oy + SL_LINE_H + 2;
        int avail = L->btn[0].y - 3 - top;
        int slack = avail - (L->reel_h + 4);
        if (slack < 0) slack = 0;
        L->reel_y = top + slack / 2;
    }

    L->mark_r = L->reel_x + reels_w + 3;
    L->lever_x = L->mark_r + SL_MARK_W * 2 + 3;
    L->pay_x = L->lever_x + 14;
    L->pay_y = L->reel_y;

    L->lever.x = L->lever_x;
    L->lever.y = L->reel_y - 8;
    L->lever.w = 12;
    L->lever.h = L->reel_h + 11;

    L->ok = (w >= SL_MIN_W) && (h >= SL_MIN_H) &&
        (L->reel_y + L->reel_h + 4 < L->btn[0].y) &&
        (L->pay_x + 96 <= ox + w);
}

/* A payline's marker sits at the row it passes through on that edge --
 * which is what says the diagonals exist. Rows 0 and 2 each carry two
 * lines, so the second of each pair is nudged sideways rather than
 * stacked, and the digits stay readable. */
void sl_mark_pos(const sl_layout_t *L, int line, bool right, int *x, int *y)
{
    int rows[SL_REELS];
    int row, shift;

    sl_line_rows(line, rows);
    row = right ? rows[SL_REELS - 1] : rows[0];

    /* One clear column between the pair, so neither draws on the
     * other's border. */
    shift = (line >= 3) ? (SL_MARK_W + 1) : 0;

    *x = right ? (L->mark_r + shift)
               : (L->mark_l + SL_MARK_W + 1 - shift);
    *y = L->reel_y + row * SL_CELL + (SL_CELL - SL_MARK_W) / 2;
}

bool sl_board_lever_at(const sl_layout_t *L, int x, int y)
{
    /* The whole rod and knob, not just the knob: a handle is a thing
     * you grab at whatever height your hand is. */
    return x >= L->lever.x && x < L->lever.x + L->lever.w &&
        y >= L->lever.y && y < L->lever.y + L->lever.h;
}

int sl_board_btn_at(const sl_layout_t *L, int x, int y)
{
    int i;
    for (i = 0; i < SL_NBTN; i++)
        if (x >= L->btn[i].x && x < L->btn[i].x + L->btn[i].w &&
            y >= L->btn[i].y && y < L->btn[i].y + L->btn[i].h) return i;
    return -1;
}

/* -- the reels ------------------------------------------------------------- */

void sl_board_draw_reels(const sl_layout_t *L, const sl_view_t *v)
{
    int r;

    for (r = 0; r < SL_REELS; r++) {
        z_clip_t win;
        int sub = sl_reel_subpixel(v->spin, r);
        int row;

        win.x0 = L->reel_x + r * L->reel_pitch;
        win.y0 = L->reel_y;
        win.x1 = win.x0 + SL_ART_W - 1;
        win.y1 = L->reel_y + L->reel_h - 1;

        /* Clip the reel window to the app's own rectangle as well, or a
         * resized window draws reels over its frame. */
        if (win.x0 < L->clip.x0) win.x0 = L->clip.x0;
        if (win.y0 < L->clip.y0) win.y0 = L->clip.y0;
        if (win.x1 > L->clip.x1) win.x1 = L->clip.x1;
        if (win.y1 > L->clip.y1) win.y1 = L->clip.y1;
        if (win.x1 < win.x0 || win.y1 < win.y0) continue;

        /* FIVE OPAQUE TILES, COVERING EVERY PIXEL EXACTLY ONCE.
         *
         * Rows -1 to 3: the cell scrolled partly off the top, the three
         * whole ones, and the cell arriving at the bottom. The tiles
         * abut, so nothing is cleared first and there is no moment when
         * any part of the reel is blank. That is the whole reason a cell
         * is exactly the tile height. */
        for (row = -1; row <= SL_ROWS; row++) {
            uint8_t sym = sl_reel_symbol(v->spin, r, row);
            int y = L->reel_y + row * SL_CELL - sub;
            blit_tile(&win, &sl_sym_tile[(int)sym * SL_ART_H],
                L->reel_x + r * L->reel_pitch, y);
        }
    }
}

/* -- the paylines, and the lever ------------------------------------------- */

/* The line numbers down both edges.
 *
 * WITHOUT THESE THE MACHINE IS UNREADABLE. Five lines are played at
 * once -- the three rows and both diagonals -- so wins land on symbols
 * that are nowhere near the middle, and the obvious conclusion is that
 * the payouts are arbitrary. A marker at the row each line touches is
 * what says the diagonals are there at all.
 *
 * A single marker on the centre row would be worse than none: it would
 * say the opposite of what is true.
 */
static void draw_marks(const sl_layout_t *L, const sl_view_t *v)
{
    int i;

    for (i = 0; i < SL_LINES; i++) {
        bool played = i < v->lines;
        bool won = v->have_result && v->line_pays[i] > 0;
        int side;

        for (side = 0; side < 2; side++) {
            int x, y;
            char d[2];

            sl_mark_pos(L, i, side != 0, &x, &y);

            fill(L, x, y, SL_MARK_W, SL_MARK_W, 0);
            frame(L, x, y, SL_MARK_W, SL_MARK_W, 1);

            /* A winning line is filled and its number knocked out, so
             * it reads at a glance without needing a second colour. */
            if (won) fill(L, x + 1, y + 1, SL_MARK_W - 2, SL_MARK_W - 2, 1);
            else if (!played)
                shade(L, x + 1, y + 1, SL_MARK_W - 2, SL_MARK_W - 2, 6);

            d[0] = (char)('1' + i);
            d[1] = '\0';
            if (won) text2(L, x + 2, y + 1, d, 0, 1);
            else text(L, x + 2, y + 1, d, 1);
        }
    }
}

/* The handle, because a slot machine has one.
 *
 * Down while the reels turn and up when they are not, which is the
 * cheapest possible animation and the one that says most: it is the
 * only part of the machine that answers "did my pull register".
 */
static void draw_lever(const sl_layout_t *L, const sl_view_t *v)
{
    bool down = v->spin->spinning;
    int top = L->reel_y + (down ? L->reel_h / 2 : 0);
    int bot = L->reel_y + L->reel_h;
    int cx = L->lever_x + 4;

    fill(L, L->lever_x, L->reel_y - 8, 12, L->reel_h + 10, 0);

    fill(L, cx, top + 6, 3, bot - top - 6, 1);      /* the rod */
    fill(L, cx - 4, bot, 11, 3, 1);                 /* its mounting */

    z_fb_fill_circle(cx + 1, top + 4, 5, 1, &L->clip);
    z_fb_fill_circle(cx + 1, top + 4, 2, 0, &L->clip);
}

/* -- the paytable ---------------------------------------------------------- */

static void draw_paytable(const sl_layout_t *L, const sl_view_t *v)
{
    static const int order[] = {
        SL_SEVEN, SL_BELL, SL_BAR3, SL_CHERRY, SL_BAR2, SL_BAR
    };
    int i, y = L->pay_y;
    char line[32];

    fill(L, L->pay_x, L->pay_y - 1, (L->ox + L->w) - L->pay_x - 2,
        L->reel_h + 2, 0);

    text(L, L->pay_x, y, "3 of a kind", 1);
    y += FH + 2;

    for (i = 0; i < (int)(sizeof order / sizeof order[0]); i++) {
        int s = order[i];
        line[0] = '\0';
        cat(line, sizeof line, sl_sym_name(s));
        text(L, L->pay_x, y, line, 1);

        line[0] = '\0';
        cat_num(line, sizeof line, sl_pay_three(s) * v->bet);
        text(L, L->ox + L->w - 3 - slen(line) * FW, y, line, 1);

        y += FH + 1;
    }

    /* The two rules a paytable has to spell out, because neither is
     * guessable from the symbols: mixed bars pay, and cherries pay from
     * the left. */
    line[0] = '\0';
    cat(line, sizeof line, "any 3 bars  ");
    cat_num(line, sizeof line, 2 * v->bet);
    text(L, L->pay_x, y, line, 1);
    y += FH + 1;

    line[0] = '\0';
    cat(line, sizeof line, "cherries from the left");
    text(L, L->pay_x, y, line, 1);
}

/* -- the rest -------------------------------------------------------------- */

static void button(const sl_layout_t *L, const sl_rect_t *r,
    const char *label, bool enabled)
{
    int tw = slen(label) * FW;
    int tx = r->x + (r->w - tw) / 2;
    int ty = r->y + (r->h - FH) / 2;

    fill(L, r->x, r->y, r->w, r->h, 0);
    frame(L, r->x, r->y, r->w, r->h, 1);

    if (!enabled) {
        /* Shaded, with a solid hole cut back out for the label. THERE
         * IS NO GREY HERE: a dither is black pixels and white pixels,
         * so a glyph drawn over one loses half of itself. */
        shade(L, r->x + 1, r->y + 1, r->w - 2, r->h - 2, 8);
        fill(L, tx - 1, ty, tw + 2, FH, 0);
    }

    text(L, tx, ty, label, 1);
}

void sl_board_draw_status(const sl_layout_t *L, const sl_view_t *v)
{
    char line[SL_MSG_LEN + 4];

    fill(L, L->ox, L->msg_y, L->w, FH, 0);
    text(L, L->ox + 2, L->msg_y, v->message, 1);

    fill(L, L->ox, L->cmd_y, L->w, FH, 0);
    line[0] = '\0';
    cat(line, sizeof line, "> ");
    cat(line, sizeof line, v->cmd);
    cat(line, sizeof line, "_");
    text(L, L->ox + 2, L->cmd_y, line, 1);
}

void sl_board_draw(const sl_layout_t *L, const sl_view_t *v)
{
    char line[48];
    bool idle = !v->spin->spinning;

    if (!L->ok) {
        fill(L, L->ox, L->oy, L->w, L->h, 0);
        text(L, L->ox + 2, L->oy + 2, "window too small", 1);
        text(L, L->ox + 2, L->oy + 2 + SL_LINE_H, "for a machine", 1);
        return;
    }

    fill(L, L->ox, L->oy, L->w, L->h, 0);

    line[0] = '\0';
    cat(line, sizeof line, "Slots  ");
    cat_num(line, sizeof line, v->lines);
    cat(line, sizeof line, " lines x ");
    cat_num(line, sizeof line, v->bet);
    text(L, L->ox + 2, L->status_y, line, 1);

    line[0] = '\0';
    cat_num(line, sizeof line, v->chips);
    cat(line, sizeof line, " chips");
    text(L, L->ox + L->w - 2 - slen(line) * FW, L->status_y, line, 1);

    sl_board_draw_reels(L, v);

    /* A frame round the reels, drawn AFTER them: the tiles are opaque
     * and reach the edge of the window, so a frame drawn first would be
     * blitted over. */
    frame(L, L->reel_x - 2, L->reel_y - 2,
        SL_REELS * L->reel_pitch - REEL_GAP + 4, L->reel_h + 4, 1);

    /* THE SYMBOLS THAT PAID, boxed.
     *
     * The line markers say which lines won; these say which three
     * symbols did it. Between them there is nothing left to guess, and
     * "why did I just win 40" stops being a question. */
    if (v->have_result && !v->spin->spinning) {
        int i;
        for (i = 0; i < SL_LINES; i++) {
            int rows[SL_REELS], r;
            if (v->line_pays[i] <= 0) continue;
            sl_line_rows(i, rows);
            for (r = 0; r < SL_REELS; r++)
                frame(L, L->reel_x + r * L->reel_pitch - 1,
                    L->reel_y + rows[r] * SL_CELL,
                    SL_ART_W + 2, SL_CELL, 1);
        }
    }

    draw_marks(L, v);
    draw_lever(L, v);
    draw_paytable(L, v);

    line[0] = '\0';
    if (v->have_result) {
        if (v->last_win > 0) {
            cat(line, sizeof line, "won ");
            cat_num(line, sizeof line, v->last_win);
        } else {
            cat(line, sizeof line, "no win");
        }
    }
    text(L, L->ox + 2, L->info_y, line, 1);

    line[0] = '\0';
    cat(line, sizeof line, "stake ");
    cat_num(line, sizeof line, v->bet * v->lines);
    text(L, L->ox + L->w - 2 - slen(line) * FW, L->info_y, line, 1);

    button(L, &L->btn[SL_BTN_SPIN], idle ? "SPIN" : "...", idle);
    button(L, &L->btn[SL_BTN_BET], "bet", idle);
    button(L, &L->btn[SL_BTN_LINES], "lines", idle);
    button(L, &L->btn[SL_BTN_MAX], "max bet", idle);

    sl_board_draw_status(L, v);
}
