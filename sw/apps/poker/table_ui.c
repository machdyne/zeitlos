/*
 * Zeitlos poker -- the table, drawn.
 * See table_ui.h for why there is one renderer and not two.
 */

#include "table_ui.h"
#include "../../common/zfont.h"

#define FONT (&z_font_5x8)
#define FW 5
#define FH 8

/* z_fb_hw_blit_mem() is a bitstream feature, not a given -- zgfx.h
 * says to probe once at startup and keep a software path, the same way
 * sw/apps/chess, draw and view do. Probed here rather than per call
 * because the answer cannot change while the app is running. */
static bool use_hw_blit;

void pt_init(void)
{
    use_hw_blit = z_fb_hw_blit_mem_available();
}

/* -- text ------------------------------------------------------------ */

char *pt_num(int32_t n, char *buf)
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

/* Appends without ever writing past the end. Every string this app
 * builds goes through here, because the alternative is snprintf and
 * one conversion specifier links picolibc's formatter at a cost of
 * around 100KB -- enough to push the binary past the space the loader
 * has for it (docs/app_runtime.md). */
static void cat(char *buf, int len, const char *s)
{
    int n = slen(buf);
    while (s && *s && n < len - 1) buf[n++] = *s++;
    buf[n] = '\0';
}

static void cat_num(char *buf, int len, int32_t v)
{
    char t[12];
    cat(buf, len, pt_num(v, t));
}

/* -- clipped primitives ----------------------------------------------
 *
 * z_fb_hw_fill_rect() clamps to the SCREEN and clips to the visible
 * region, but knows nothing about this app's content rectangle -- so
 * the clamp here is not belt and braces, it is the only thing stopping
 * a fill painting over the window frame or the desktop.
 */
static void fill(const pt_layout_t *L, int x, int y, int w, int h, int color)
{
    int x1 = x + w - 1, y1 = y + h - 1;

    if (x < L->clip.x0) x = L->clip.x0;
    if (y < L->clip.y0) y = L->clip.y0;
    if (x1 > L->clip.x1) x1 = L->clip.x1;
    if (y1 > L->clip.y1) y1 = L->clip.y1;
    if (x1 < x || y1 < y) return;

    z_fb_hw_fill_rect(x, y, x1 - x + 1, y1 - y + 1, color);
}

/* z_fb_hw_fill_shade() clamps to the screen and knows nothing about
 * this app's content rectangle, exactly like z_fb_hw_fill_rect(). It
 * was being called with unclamped coordinates, which was only safe
 * because the buttons happen to sit well inside the window -- true
 * until somebody moves them. */
static void shade(const pt_layout_t *L, int x, int y, int w, int h, int level)
{
    int x1 = x + w - 1, y1 = y + h - 1;

    if (x < L->clip.x0) x = L->clip.x0;
    if (y < L->clip.y0) y = L->clip.y0;
    if (x1 > L->clip.x1) x1 = L->clip.x1;
    if (y1 > L->clip.y1) y1 = L->clip.y1;
    if (x1 < x || y1 < y) return;

    z_fb_hw_fill_shade(x, y, x1 - x + 1, y1 - y + 1, level);
}

static void frame(const pt_layout_t *L, int x, int y, int w, int h, int color)
{
    fill(L, x, y, w, 1, color);
    fill(L, x, y + h - 1, w, 1, color);
    fill(L, x, y, 1, h, color);
    fill(L, x + w - 1, y, 1, h, color);
}

static void text(const pt_layout_t *L, int x, int y, const char *s, int color)
{
    z_fb_draw_text(x, y, s, color, FONT, &L->clip);
}

static void text_right(const pt_layout_t *L, int x1, int y, const char *s,
    int color)
{
    text(L, x1 - slen(s) * FW, y, s, color);
}

static void text_center(const pt_layout_t *L, int cx, int y, const char *s,
    int color)
{
    text(L, cx - (slen(s) * FW) / 2, y, s, color);
}

/* -- card blitting --------------------------------------------------- */

static void blit_card(const pt_layout_t *L, const uint32_t *tile,
    int tw, int th, int dx, int dy)
{
    int sx = 0, sy = 0, w = tw, h = th;
    int i, passes;

    /* Clamp the destination to the content rectangle, moving the
     * SOURCE origin in step. Clamping only the destination would slide
     * the wrong part of the card into view -- a half-visible card at
     * the edge of a resized window would show its middle rather than
     * its left half, which is exactly the half with the rank on it. */
    if (dx < L->clip.x0) { int d = L->clip.x0 - dx; sx += d; w -= d; dx += d; }
    if (dy < L->clip.y0) { int d = L->clip.y0 - dy; sy += d; h -= d; dy += d; }
    if (dx + w - 1 > L->clip.x1) w = L->clip.x1 - dx + 1;
    if (dy + h - 1 > L->clip.y1) h = L->clip.y1 - dy + 1;
    if (w <= 0 || h <= 0) return;

    if (!use_hw_blit) {
        /* Software fallback, for a bitstream whose blitter has no
         * memory-source mode. Slow and correct, which is the right
         * trade for a path that only runs on old gateware. */
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
     * unrelated operation left behind. Program it per visible
     * rectangle, exactly as zgfx.c's own fill does, and reset after. */
    passes = z_gfx_visible_count();
    if (passes < 1) passes = 1;

    for (i = 0; i < passes; i++) {
        z_clip_t c;
        c.x0 = dx; c.y0 = dy; c.x1 = dx + w - 1; c.y1 = dy + h - 1;
        if (!z_gfx_blit_scissor(i, &c)) continue;
        z_fb_hw_blit_mem(tile + sy, Z_ART_STRIDE, sx, 0, dx, dy, w, h);
    }

    z_gfx_blit_scissor_reset();
}

static void draw_full(const pt_layout_t *L, uint8_t card, int x, int y)
{
    const uint32_t *t = (card < Z_NCARDS)
        ? &z_card_full[(int)card * Z_ART_FULL_H] : z_back_full;
    blit_card(L, t, Z_ART_FULL_W, Z_ART_FULL_H, x, y);
}

static void draw_mini(const pt_layout_t *L, uint8_t card, int x, int y)
{
    const uint32_t *t = (card < Z_NCARDS)
        ? &z_card_mini[(int)card * Z_ART_MINI_H] : z_back_mini;
    blit_card(L, t, Z_ART_MINI_W, Z_ART_MINI_H, x, y);
}

/* -- layout ---------------------------------------------------------- */

static int hero_stride(void) { return Z_ART_FULL_W + 2; }
static int board_stride(void) { return Z_ART_FULL_W + 3; }

void pt_layout(pt_layout_t *L, const pt_view_t *v, int ox, int oy,
    int w, int h)
{
    int i, need, opp_bottom;
    int nopp = 0, final_board = 0, final_hole = 0, s;

    for (i = 0; i < (int)sizeof(pt_layout_t); i++) ((char *)L)[i] = 0;

    L->ox = ox; L->oy = oy; L->w = w; L->h = h;
    L->clip.x0 = ox;
    L->clip.y0 = oy;
    L->clip.x1 = ox + w - 1;
    L->clip.y1 = oy + h - 1;

    if (v && v->g) {
        for (i = 0; i < v->g->nseats; i++)
            if (i != v->hero && v->g->seat[i].state != PK_SEAT_EMPTY) nopp++;
        for (s = 0; s < v->g->v->nstreets; s++) {
            final_board += v->g->v->deal_board[s];
            final_hole += v->g->v->deal_down[s] + v->g->v->deal_up[s];
        }
        if (v->g->nboard > final_board) final_board = v->g->nboard;
    }

    L->nopp = nopp;
    L->has_board = final_board > 0;
    L->board_slots = final_board;
    L->hero_slots = final_hole ? final_hole : 2;

    /* One row up to three opponents, two rows beyond that, with the
     * fuller row on top so a five-handed table does not leave a single
     * seat stranded above four. */
    if (nopp <= 3) {
        L->opp_rows = nopp ? 1 : 0;
        L->opp_row_n[0] = nopp;
    } else {
        L->opp_rows = 2;
        L->opp_row_n[0] = (nopp + 1) / 2;
        L->opp_row_n[1] = nopp - L->opp_row_n[0];
    }

    for (i = 0; i < L->opp_rows; i++)
        L->opp_cell_w[i] = L->opp_row_n[i] ? w / L->opp_row_n[i] : w;

    /* Bottom-anchored, top-down for the rest.
     *
     * The command line, the message line and the buttons keep the same
     * position whatever size the window is, because they are the parts
     * being typed into and clicked. Everything above them absorbs the
     * slack. */
    L->cmd_y = oy + h - FH;
    L->msg_y = L->cmd_y - PT_LINE_H;
    L->btn[0].y = L->msg_y - PT_GAP - PT_BTN_H;
    L->hero_info_y = L->btn[0].y - PT_GAP - FH;
    L->hero_y = L->hero_info_y - 2 - Z_ART_FULL_H;

    L->status_y = oy;

    /* Opponent rows sit directly under the status line. */
    L->opp_row_y[0] = oy + PT_LINE_H;
    L->opp_row_y[1] = L->opp_row_y[0] + PT_SEAT_H + 2;

    opp_bottom = (L->opp_rows > 0)
        ? L->opp_row_y[L->opp_rows - 1] + PT_SEAT_H
        : oy + PT_LINE_H;

    /* THE POT AND BOARD ARE CENTRED IN WHAT IS LEFT, not stacked
     * directly on top of the hero's cards.
     *
     * Everything else is anchored: the status line to the top, and the
     * command line, message and buttons to the bottom, because those
     * are the parts being typed into and clicked and they must not
     * move when the window is resized. That leaves ALL the slack in
     * one place, in the middle, and the first version simply let it
     * pool there -- which the render showed as a band of empty black
     * between the other players and the board, about a third of the
     * window in five-card draw.
     *
     * No assertion caught it, and none could have: every band was
     * inside the window, in the right order, not overlapping anything.
     * It was just ugly. That is precisely the category
     * sw/common/tests/zrender.h exists for.
     */
    {
        int block = L->has_board ? (FH + 2 + Z_ART_FULL_H) : FH;
        int top = opp_bottom + PT_GAP;
        int bot = L->hero_y - PT_GAP;
        int free = (bot - top) - block;

        need = PT_LINE_H + L->opp_rows * (PT_SEAT_H + 2);
        L->ok = (w >= PT_MIN_W) && (free >= 0) && (oy + need <= bot);

        if (free > 0) top += free / 2;

        L->pot_y = top;
        L->board_y = L->has_board ? (top + FH + 2) : L->hero_y;
    }

    /* The board and the hero's cards are centred on the whole width,
     * so they line up with each other whatever the seat arithmetic
     * above did. */
    L->board_x = ox + (w - (L->board_slots * board_stride() - 3)) / 2;
    L->hero_x = ox + (w - (L->hero_slots * hero_stride() - 2)) / 2;
    if (L->board_x < ox) L->board_x = ox;
    if (L->hero_x < ox) L->hero_x = ox;

    /* Four action buttons on the left, the bet amount between a minus
     * and a plus on the right. */
    {
        int right = 62;
        int avail = w - right - PT_GAP;
        int bw = (avail - 3 * PT_GAP) / 4;
        if (bw < 1) bw = 1;

        for (i = 0; i < 4; i++) {
            L->btn[i].x = ox + i * (bw + PT_GAP);
            L->btn[i].y = L->btn[0].y;
            L->btn[i].w = bw;
            L->btn[i].h = PT_BTN_H;
        }

        L->btn[PT_BTN_LESS].x = ox + w - right;
        L->btn[PT_BTN_LESS].w = 11;
        L->btn[PT_BTN_MORE].x = ox + w - 11;
        L->btn[PT_BTN_MORE].w = 11;
        for (i = 4; i < PT_NBTN; i++) {
            L->btn[i].y = L->btn[0].y;
            L->btn[i].h = PT_BTN_H;
        }

        L->amount_x = L->btn[PT_BTN_LESS].x + 12;
        L->amount_w = right - 24;
    }
}

void pt_hero_card_xy(const pt_layout_t *L, int i, int *x, int *y)
{
    *x = L->hero_x + i * hero_stride();
    *y = L->hero_y;
}

int pt_hero_card_at(const pt_layout_t *L, const pt_view_t *v, int x, int y)
{
    int i, n;

    if (!v || !v->g) return -1;
    n = v->g->seat[v->hero].nhole;
    if (y < L->hero_y || y >= L->hero_y + Z_ART_FULL_H) return -1;

    for (i = 0; i < n; i++) {
        int cx = L->hero_x + i * hero_stride();
        if (x >= cx && x < cx + Z_ART_FULL_W) return i;
    }

    return -1;
}

int pt_button_at(const pt_layout_t *L, int x, int y)
{
    int i;

    for (i = 0; i < PT_NBTN; i++) {
        const pt_rect_t *r = &L->btn[i];
        if (x >= r->x && x < r->x + r->w && y >= r->y && y < r->y + r->h)
            return i;
    }

    return -1;
}

/* -- pieces of the table --------------------------------------------- */

static void draw_seat(const pt_layout_t *L, const pt_view_t *v, int seat,
    int x, int y, int cw)
{
    const pk_seat_t *s = &v->g->seat[seat];
    char line[32];
    uint8_t shown[PK_MAX_HOLE];
    int n, i, cx;
    bool acting = (v->g->phase != PK_PHASE_COMPLETE && v->g->actor == seat);

    fill(L, x, y, cw - 2, PT_SEAT_H, 0);

    /* The seat to act is framed rather than highlighted, because on a
     * display with two colours an inverted seat would be the loudest
     * thing on the table and the loudest thing should be the cards. */
    if (acting) frame(L, x, y, cw - 2, PT_SEAT_H, 1);

    line[0] = '\0';
    cat(line, sizeof line, v->name[seat]);
    /* No "folded" marker on the name line: the card area already says
     * so in full, and the render showed the two together reading as
     * two different pieces of information rather than one. */
    if (s->state == PK_SEAT_ALLIN) cat(line, sizeof line, " ALL IN");
    text(L, x + 2, y + 1, line, 1);

    line[0] = '\0';
    cat_num(line, sizeof line, s->stack);
    text_right(L, x + cw - 4, y + 1, line, 1);

    /* Cards. Overlapped at Z_ART_MINI_STRIDE, which keeps a
     * seven-card stud hand inside a six-handed seat -- see cards.h. */
    n = pk_seat_shown(v->g, seat, shown, v->reveal);
    cx = x + 2;

    if (s->state == PK_SEAT_FOLDED) {
        text(L, x + 2, y + PT_LINE_H + 4, "-- folded --", 1);
    } else {
        int down = s->nhole - n;
        for (i = 0; i < n; i++, cx += Z_ART_MINI_STRIDE)
            draw_mini(L, shown[i], cx, y + PT_LINE_H);
        for (i = 0; i < down; i++, cx += Z_ART_MINI_STRIDE)
            draw_mini(L, Z_CARD_NONE, cx, y + PT_LINE_H);
    }

    line[0] = '\0';
    if (v->reveal && s->state != PK_SEAT_FOLDED) {
        uint32_t val = pk_seat_value(v->g, seat, 0);
        if (val != PK_EVAL_NONE) pk_eval_name(val, line, sizeof line);
    } else if (s->bet > 0 && s->state != PK_SEAT_FOLDED) {
        cat(line, sizeof line, "bet ");
        cat_num(line, sizeof line, s->bet);
    } else {
        cat(line, sizeof line, v->tag[seat]);
    }
    text(L, x + 2, y + PT_LINE_H + Z_ART_MINI_H + 1, line, 1);

    if (s->won > 0) {
        char w[20];
        w[0] = '\0';
        cat(w, sizeof w, "+");
        cat_num(w, sizeof w, s->won);
        text_right(L, x + cw - 4, y + PT_LINE_H + Z_ART_MINI_H + 1, w, 1);
    }
}

static void draw_opponents(const pt_layout_t *L, const pt_view_t *v)
{
    int row, col, seat = v->hero, placed = 0;

    /* Walked clockwise from the hero, so the seat order on screen is
     * the order the action moves in. Laying them out by seat index
     * instead would put the player to the hero's left in an arbitrary
     * place, which makes the act order impossible to follow. */
    for (row = 0; row < L->opp_rows; row++) {
        for (col = 0; col < L->opp_row_n[row]; col++) {
            int guard = 0;
            do {
                seat = (seat + 1) % v->g->nseats;
            } while (v->g->seat[seat].state == PK_SEAT_EMPTY &&
                ++guard < v->g->nseats);
            if (seat == v->hero) return;

            draw_seat(L, v, seat,
                L->ox + col * L->opp_cell_w[row],
                L->opp_row_y[row], L->opp_cell_w[row]);
            placed++;
        }
    }

    (void)placed;
}

static void draw_board(const pt_layout_t *L, const pt_view_t *v)
{
    char line[40];
    int i;

    line[0] = '\0';
    cat(line, sizeof line, "pot ");
    cat_num(line, sizeof line, v->g->pot);

    if (v->g->npots > 1) {
        cat(line, sizeof line, " (");
        cat_num(line, sizeof line, v->g->npots);
        cat(line, sizeof line, " pots)");
    }

    fill(L, L->ox, L->pot_y, L->w, FH, 0);
    text_center(L, L->ox + L->w / 2, L->pot_y, line, 1);

    if (!L->has_board) return;

    fill(L, L->ox, L->board_y, L->w, Z_ART_FULL_H, 0);

    for (i = 0; i < L->board_slots; i++) {
        int x = L->board_x + i * board_stride();
        if (i < v->g->nboard) {
            draw_full(L, v->g->board[i], x, L->board_y);
        } else {
            /* An empty slot is an outline, not a face-down card. A
             * card that has not been dealt is a different thing from
             * one that has been dealt and hidden, and on this table
             * both appear at once in stud. */
            frame(L, x, L->board_y, Z_ART_FULL_W, Z_ART_FULL_H, 1);
        }
    }
}

static void draw_hero(const pt_layout_t *L, const pt_view_t *v)
{
    const pk_seat_t *s = &v->g->seat[v->hero];
    char line[48];
    int i;

    fill(L, L->ox, L->hero_y, L->w, Z_ART_FULL_H + 2 + FH, 0);

    for (i = 0; i < s->nhole; i++) {
        int x, y;
        pt_hero_card_xy(L, i, &x, &y);
        draw_full(L, s->hole[i], x, y);

        /* Marked for the draw. A bar UNDER the card rather than
         * anything over it, because the whole point of picking cards
         * to throw away is being able to see the ones you are
         * keeping. */
        if (v->discard[i])
            fill(L, x, y + Z_ART_FULL_H, Z_ART_FULL_W, 2, 1);
    }

    line[0] = '\0';
    cat(line, sizeof line, v->name[v->hero]);
    cat(line, sizeof line, "  ");
    cat_num(line, sizeof line, s->stack);
    if (s->bet > 0) {
        cat(line, sizeof line, "  bet ");
        cat_num(line, sizeof line, s->bet);
    }
    text(L, L->ox + 2, L->hero_info_y, line, 1);

    /* What the hero actually holds, named. The single most useful
     * thing on the table for somebody who is still learning which
     * hand beats which, and free to compute -- the evaluator is
     * already running. */
    if (s->nhole) {
        uint32_t val = pk_seat_value(v->g, v->hero, 0);
        if (val != PK_EVAL_NONE) {
            char nm[40];
            pk_eval_name(val, nm, sizeof nm);
            text_right(L, L->ox + L->w - 2, L->hero_info_y, nm, 1);
        }
    }
}

static void button(const pt_layout_t *L, const pt_rect_t *r,
    const char *label, bool enabled)
{
    int tw = slen(label) * FW;
    int tx = r->x + (r->w - tw) / 2;
    int ty = r->y + (r->h - FH) / 2;

    fill(L, r->x, r->y, r->w, r->h, 0);
    frame(L, r->x, r->y, r->w, r->h, 1);

    if (!enabled) {
        /* Shaded rather than hidden or blank: a control that vanishes
         * when it is unavailable is a control whose neighbours move
         * under the pointer.
         *
         * -- but the label is NOT drawn on the shade --
         *
         * THERE IS NO GREY ON THIS DISPLAY. A dither is black pixels
         * and white pixels, so a black glyph over one is black pixels
         * among black pixels: the shape disappears. Two attempts got
         * this wrong -- white text over a light stipple first, then
         * black text over a heavy one -- and the second shipped,
         * because in a 2x render it looked faint rather than absent
         * and the response was to tune the dither level instead of to
         * stop drawing text on texture.
         *
         * zgfx.h makes it worse than a contrast problem: on a
         * bitstream with no dither support, fill_shade falls back to a
         * plain black or white fill. So black-on-shade is sometimes
         * black on solid black.
         *
         * The label therefore gets a solid hole cut back out for it
         * and is drawn white, exactly as an enabled one is. The shade
         * surrounds it instead of sitting under it, which still reads
         * as a greyed control and cannot stop being legible whatever
         * the bitstream does with the dither.
         *
         * tests/test_layout.c asserts the disabled label is drawn with
         * the same pixels as the enabled one. */
        shade(L, r->x + 1, r->y + 1, r->w - 2, r->h - 2, 8);
        fill(L, tx - 1, ty, tw + 2, FH, 0);
    }

    text(L, tx, ty, label, 1);
}

void pt_draw_actions(const pt_layout_t *L, const pt_view_t *v)
{
    pk_options_t o;
    char amt[16];
    bool mine = (v->g->phase == PK_PHASE_BETTING && v->g->actor == v->hero);
    bool draw_phase = (v->g->phase == PK_PHASE_DRAW && v->g->actor == v->hero);

    pk_options(v->g, v->hero, &o);
    if (!mine) {
        o.can_fold = o.can_check = o.can_call = false;
        o.can_bet = o.can_raise = false;
    }

    fill(L, L->ox, L->btn[0].y, L->w, PT_BTN_H, 0);

    if (draw_phase) {
        /* The draw needs one button, not four, and re-labelling the
         * existing row is better than a second row that only exists
         * for one variant. */
        button(L, &L->btn[PT_BTN_FOLD], "fold", true);
        button(L, &L->btn[PT_BTN_CALL], "draw", true);
        button(L, &L->btn[PT_BTN_RAISE], "stand pat", true);
        button(L, &L->btn[PT_BTN_ALLIN], "", false);
        return;
    }

    button(L, &L->btn[PT_BTN_FOLD], "fold", o.can_fold);
    button(L, &L->btn[PT_BTN_CALL],
        o.can_check ? "check" : "call", o.can_check || o.can_call);
    button(L, &L->btn[PT_BTN_RAISE],
        (v->g->bet_to_match > 0) ? "raise" : "bet", o.can_bet || o.can_raise);
    button(L, &L->btn[PT_BTN_ALLIN], "all in", o.can_bet || o.can_raise);

    button(L, &L->btn[PT_BTN_LESS], "-", o.can_bet || o.can_raise);
    button(L, &L->btn[PT_BTN_MORE], "+", o.can_bet || o.can_raise);

    amt[0] = '\0';
    if (o.can_bet || o.can_raise) cat_num(amt, sizeof amt, v->bet_to);
    else if (o.can_call) { cat(amt, sizeof amt, "-"); cat_num(amt, sizeof amt, o.call_cost); }
    text_center(L, L->amount_x + L->amount_w / 2,
        L->btn[0].y + (PT_BTN_H - FH) / 2, amt, 1);
}

void pt_draw_status(const pt_layout_t *L, const pt_view_t *v)
{
    char line[PT_MSG_LEN + 4];

    fill(L, L->ox, L->msg_y, L->w, FH, 0);
    text(L, L->ox + 2, L->msg_y, v->message, 1);

    fill(L, L->ox, L->cmd_y, L->w, FH, 0);
    line[0] = '\0';
    cat(line, sizeof line, "> ");
    cat(line, sizeof line, v->cmd);
    cat(line, sizeof line, "_");
    text(L, L->ox + 2, L->cmd_y, line, 1);
}

static void draw_titlebar(const pt_layout_t *L, const pt_view_t *v)
{
    char line[48];

    fill(L, L->ox, L->status_y, L->w, FH, 0);

    line[0] = '\0';
    cat(line, sizeof line, v->g->v->label);
    if (v->g->phase != PK_PHASE_COMPLETE) {
        cat(line, sizeof line, " - ");
        cat(line, sizeof line, pk_street_name(v->g, v->g->street));
    }
    text(L, L->ox + 2, L->status_y, line, 1);

    line[0] = '\0';
    cat(line, sizeof line, "hand ");
    cat_num(line, sizeof line, v->hand_no);
    cat(line, sizeof line, "  lvl ");
    cat_num(line, sizeof line, v->level);

    /* What is left in the bank, when there is any -- the stack on the
     * table is already drawn at the hero's seat, and showing both is
     * what says "this is the same money as the other games". */
    if (v->bank > 0) {
        cat(line, sizeof line, "  bank ");
        cat_num(line, sizeof line, v->bank);
    }

    text_right(L, L->ox + L->w - 2, L->status_y, line, 1);
}

void pt_draw_all(const pt_layout_t *L, const pt_view_t *v)
{
    if (!L->ok) {
        /* Too small to play in. Chess shrinks by dropping its panel;
         * there is nothing here that can be dropped and still leave a
         * table somebody can act on, so it says so instead of drawing
         * something misleading. */
        fill(L, L->ox, L->oy, L->w, L->h, 0);
        text(L, L->ox + 2, L->oy + 2, "window too small", 1);
        text(L, L->ox + 2, L->oy + 2 + PT_LINE_H, "for a table", 1);
        return;
    }

    fill(L, L->ox, L->oy, L->w, L->h, 0);

    draw_titlebar(L, v);
    draw_opponents(L, v);
    draw_board(L, v);
    draw_hero(L, v);
    pt_draw_actions(L, v);
    pt_draw_status(L, v);

    if (v->thinking) {
        char line[40];
        line[0] = '\0';
        cat(line, sizeof line, v->name[v->thinking_seat]);
        cat(line, sizeof line, " is thinking");
        fill(L, L->ox, L->msg_y, L->w, FH, 0);
        text(L, L->ox + 2, L->msg_y, line, 1);
    }
}
