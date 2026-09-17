/*
 * Zeitlos blackjack -- the table, drawn and clicked.
 * See bj_board.h for why the card stride is computed rather than fixed.
 */

#include "bj_board.h"
#include "../../common/zfont.h"

#define FONT (&z_font_5x8)
#define FW 5
#define FH 8

const int32_t bj_chip_values[BJ_NCHIPS] = { 5, 25, 100, 500 };

static bool use_hw_blit;

void bj_board_init(void)
{
    use_hw_blit = z_fb_hw_blit_mem_available();
}

/* -- text ---------------------------------------------------------------- */

char *bj_num(int32_t n, char *buf)
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
    cat(buf, len, bj_num(v, t));
}

/* -- clipped primitives --------------------------------------------------
 *
 * z_fb_hw_fill_rect() clamps to the screen and to wm's visible region
 * but knows nothing about this app's content rectangle, so the clamp
 * here is the only thing stopping a fill painting over the window
 * frame.
 */
static void fill(const bj_layout_t *L, int x, int y, int w, int h, int c)
{
    int x1 = x + w - 1, y1 = y + h - 1;

    if (x < L->clip.x0) x = L->clip.x0;
    if (y < L->clip.y0) y = L->clip.y0;
    if (x1 > L->clip.x1) x1 = L->clip.x1;
    if (y1 > L->clip.y1) y1 = L->clip.y1;
    if (x1 < x || y1 < y) return;

    z_fb_hw_fill_rect(x, y, x1 - x + 1, y1 - y + 1, c);
}

static void frame(const bj_layout_t *L, int x, int y, int w, int h, int c)
{
    fill(L, x, y, w, 1, c);
    fill(L, x, y + h - 1, w, 1, c);
    fill(L, x, y, 1, h, c);
    fill(L, x + w - 1, y, 1, h, c);
}

static void shade(const bj_layout_t *L, int x, int y, int w, int h, int lvl)
{
    int x1 = x + w - 1, y1 = y + h - 1;

    if (x < L->clip.x0) x = L->clip.x0;
    if (y < L->clip.y0) y = L->clip.y0;
    if (x1 > L->clip.x1) x1 = L->clip.x1;
    if (y1 > L->clip.y1) y1 = L->clip.y1;
    if (x1 < x || y1 < y) return;

    z_fb_hw_fill_shade(x, y, x1 - x + 1, y1 - y + 1, lvl);
}

static void text(const bj_layout_t *L, int x, int y, const char *s, int c)
{
    z_fb_draw_text(x, y, s, c, FONT, &L->clip);
}

static void text_center(const bj_layout_t *L, int cx, int y, const char *s,
    int c)
{
    text(L, cx - (slen(s) * FW) / 2, y, s, c);
}

/* -- cards ---------------------------------------------------------------- */

static void blit_card(const bj_layout_t *L, const uint32_t *tile,
    int tw, int th, int dx, int dy)
{
    int sx = 0, sy = 0, w = tw, h = th;
    int i, passes;

    /* Clamp the destination to the content rectangle, moving the SOURCE
     * origin in step. Clamping only the destination would slide the
     * wrong part of the card into view -- a half-visible card at the
     * edge would show its middle rather than its left half, which is
     * the half with the rank on it. */
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
        z_fb_hw_blit_mem(tile + sy, Z_ART_STRIDE, sx, 0, dx, dy, w, h);
    }

    z_gfx_blit_scissor_reset();
}

static void draw_card(const bj_layout_t *L, uint8_t card, int x, int y,
    bool mini)
{
    const uint32_t *t;

    if (mini) {
        t = (card < Z_NCARDS) ? &z_card_mini[(int)card * Z_ART_MINI_H]
                              : z_back_mini;
        blit_card(L, t, Z_ART_MINI_W, Z_ART_MINI_H, x, y);
    } else {
        t = (card < Z_NCARDS) ? &z_card_full[(int)card * Z_ART_FULL_H]
                              : z_back_full;
        blit_card(L, t, Z_ART_FULL_W, Z_ART_FULL_H, x, y);
    }
}

/* -- layout ---------------------------------------------------------------- */

void bj_board_layout(bj_layout_t *L, const bj_view_t *v, int ox, int oy,
    int w, int h)
{
    int i;

    for (i = 0; i < (int)sizeof(bj_layout_t); i++) ((char *)L)[i] = 0;

    L->ox = ox; L->oy = oy; L->w = w; L->h = h;
    L->clip.x0 = ox;
    L->clip.y0 = oy;
    L->clip.x1 = ox + w - 1;
    L->clip.y1 = oy + h - 1;

    L->status_y = oy;

    /* Bottom-anchored: the command line, the message and the buttons
     * keep the same place whatever size the window is, because they are
     * what is being typed into and clicked. */
    L->cmd_y = oy + h - FH;
    L->msg_y = L->cmd_y - BJ_LINE_H;
    L->btn[0].y = L->msg_y - 3 - BJ_BTN_H;

    {
        int n = BJ_NBTN;
        int bw = (w - (n - 1) * 2) / n;
        if (bw < 1) bw = 1;
        for (i = 0; i < n; i++) {
            L->btn[i].x = ox + i * (bw + 2);
            L->btn[i].y = L->btn[0].y;
            L->btn[i].w = bw;
            L->btn[i].h = BJ_BTN_H;
        }
    }

    {
        int cw = 34;
        int total = BJ_NCHIPS * (cw + 2) - 2;
        int cx = ox + w - total - 2;
        int cy = L->btn[0].y - 2 - BJ_BTN_H;
        for (i = 0; i < BJ_NCHIPS; i++) {
            L->chip[i].x = cx + i * (cw + 2);
            L->chip[i].y = cy;
            L->chip[i].w = cw;
            L->chip[i].h = BJ_BTN_H;
        }
        L->shoe_x = ox + 2;
        L->shoe_y = cy - 8;
        L->shoe_w = w - 4;
        if (L->shoe_w < 10) L->shoe_w = 10;
    }

    /* The shoe bar gets its own band, with its caption above it.
     *
     * It was tucked beside the chips at first, and its caption landed
     * on the same row as the hands' totals -- "25" and "18" cut in half
     * by "shoe 208 of 312". Nothing asserts that two pieces of text do
     * not overlap, and the render showed it immediately. */
    L->hand_info_y = L->shoe_y - FH - 3 - FH;
    L->hands_y = L->hand_info_y - 2 - Z_ART_FULL_H;

    /* THE DEALER IS ANCHORED TO THE TOP, not stacked above the hands.
     *
     * Everything else here is anchored to the bottom, because the
     * command line and the buttons must not move when the window
     * resizes. Anchoring the dealer to them as well left ALL the slack
     * in one place -- a third of the window, empty, above the dealer's
     * cards. sw/apps/poker had exactly this and it was fixed the same
     * way: the gap belongs in the MIDDLE of a table, which is where
     * felt is. */
    L->dealer_y = oy + BJ_LINE_H;
    L->dealer_info_y = L->dealer_y + Z_ART_FULL_H + 2;

    L->dealer_x = ox + 4;

    {
        int nh = (v && v->g && v->g->nhands > 0) ? v->g->nhands : 1;
        L->hand_w = w / nh;
    }

    L->ok = (w >= BJ_MIN_W) && (h >= BJ_MIN_H) &&
        (L->dealer_info_y + FH + 4 < L->hands_y);
}

void bj_hand_geom(const bj_layout_t *L, const bj_view_t *v, int i,
    int *x, int *y, int *stride, bool *mini)
{
    int n = (v && v->g) ? v->g->hand[i].n : 2;
    int cell = L->hand_w - 6;
    int cw, s, floor_s;

    /* SEVERAL HANDS MEANS SMALL CARDS. A quarter of the table is not
     * enough for 20-pixel cards, and the minis put the rank and suit in
     * columns 2 to 6 so an overlapped one is still readable. */
    *mini = (v && v->g && v->g->nhands > 1);
    cw = *mini ? Z_ART_MINI_W : Z_ART_FULL_W;

    /* The stride is whatever makes the hand fit, floored where the rank
     * would start to disappear. A blackjack hand has no fixed length --
     * twelve cards is reachable -- so a constant stride either runs off
     * the table or wastes the width in the usual two-card case. */
    if (n < 2) n = 2;
    s = (cell - cw) / (n - 1);

    /* THE FLOOR GIVES BEFORE THE CELL DOES.
     *
     * Seven pixels keeps a mini card's rank and suit fully visible, and
     * that is the right stride for the hands anybody actually sees. But
     * four split hands of eleven cards do not fit at seven, and the
     * test walks every length to twelve because twelve is reachable --
     * four aces, four twos, four threes.
     *
     * At that point the ranks overlap and the PRINTED TOTAL is what you
     * read, which is fine: a hand that long is about its total, and the
     * alternative is cards running into the next player's. */
    floor_s = *mini ? 4 : 6;
    if (s > cw + 2) s = cw + 2;
    if (s < floor_s) s = floor_s;

    *stride = s;
    *x = L->ox + i * L->hand_w + 3;
    *y = L->hands_y;
}

int bj_board_btn_at(const bj_layout_t *L, int x, int y)
{
    int i;
    for (i = 0; i < BJ_NBTN; i++)
        if (x >= L->btn[i].x && x < L->btn[i].x + L->btn[i].w &&
            y >= L->btn[i].y && y < L->btn[i].y + L->btn[i].h) return i;
    return -1;
}

int bj_board_chip_at(const bj_layout_t *L, int x, int y)
{
    int i;
    for (i = 0; i < BJ_NCHIPS; i++)
        if (x >= L->chip[i].x && x < L->chip[i].x + L->chip[i].w &&
            y >= L->chip[i].y && y < L->chip[i].y + L->chip[i].h) return i;
    return -1;
}

/* -- drawing --------------------------------------------------------------- */

static void button(const bj_layout_t *L, const bj_rect_t *r,
    const char *label, bool enabled)
{
    int tw = slen(label) * FW;
    int tx = r->x + (r->w - tw) / 2;
    int ty = r->y + (r->h - FH) / 2;

    fill(L, r->x, r->y, r->w, r->h, 0);
    frame(L, r->x, r->y, r->w, r->h, 1);

    if (!enabled) {
        /* Shaded, with a solid hole cut back out for the label.
         *
         * THERE IS NO GREY ON THIS DISPLAY: a dither is black pixels
         * and white pixels, so a glyph drawn over one loses half of
         * itself to the background. sw/apps/poker shipped its disabled
         * buttons unreadable twice before this was settled. */
        shade(L, r->x + 1, r->y + 1, r->w - 2, r->h - 2, 8);
        fill(L, tx - 1, ty, tw + 2, FH, 0);
    }

    text(L, tx, ty, label, 1);
}

static void draw_total(const bj_layout_t *L, int x, int y, int cx,
    const uint8_t *cards, int n, bool center)
{
    char line[24];
    bool soft = false;
    int t = bj_total(cards, n, &soft);

    line[0] = '\0';
    if (n == 0) return;

    if (t > 21) {
        cat(line, sizeof line, "bust ");
        cat_num(line, sizeof line, t);
    } else if (bj_is_natural(cards, n)) {
        cat(line, sizeof line, "blackjack");
    } else {
        /* Soft totals are shown as such. It is the only thing telling
         * soft 17 from hard 17, and a person deciding whether to hit
         * needs it as much as the dealer rules do. */
        if (soft) cat(line, sizeof line, "soft ");
        cat_num(line, sizeof line, t);
    }

    if (center) text_center(L, cx, y, line, 1);
    else text(L, x, y, line, 1);
}

static void draw_dealer(const bj_layout_t *L, const bj_view_t *v)
{
    const bj_game_t *g = v->g;
    int i, x = L->dealer_x;
    bool shown = bj_hole_shown(g);
    uint8_t seen[BJ_MAX_CARDS];
    int nseen = 0;

    fill(L, L->ox, L->dealer_y, L->w, Z_ART_FULL_H + 2 + FH, 0);

    text(L, L->ox + 2, L->dealer_info_y - 0, "", 1);

    for (i = 0; i < g->ndealer; i++) {
        /* The hole card stays face down until the showdown. Drawing it
         * and relying on the player not to look would be the same bug
         * as dealing it face up. */
        bool hide = (i == 1 && !shown);
        draw_card(L, hide ? Z_CARD_NONE : g->dealer[i], x, L->dealer_y, false);
        if (!hide) seen[nseen++] = g->dealer[i];
        x += Z_ART_FULL_W + 2;
    }

    {
        char line[32];
        line[0] = '\0';
        cat(line, sizeof line, "dealer");
        text(L, L->ox + 2, L->dealer_info_y, line, 1);
    }

    /* Only what is VISIBLE is totalled. Showing the true total while
     * the hole card is down would give the game away completely, and is
     * exactly the sort of thing that looks like a helpful feature. */
    draw_total(L, L->dealer_x + 44, L->dealer_info_y, 0, seen, nseen, false);
}

static void draw_hands(const bj_layout_t *L, const bj_view_t *v)
{
    const bj_game_t *g = v->g;
    int i, k;

    fill(L, L->ox, L->hands_y, L->w, Z_ART_FULL_H + 2 + FH, 0);

    for (i = 0; i < g->nhands; i++) {
        const bj_hand_t *h = &g->hand[i];
        int x, y, stride;
        bool mini;
        char line[32];
        int cx = L->ox + i * L->hand_w + L->hand_w / 2;

        bj_hand_geom(L, v, i, &x, &y, &stride, &mini);

        /* The hand being played is framed rather than highlighted. On a
         * two-colour display an inverted cell would be the loudest
         * thing on the table, and the loudest thing should be the
         * cards. */
        if (g->phase == BJ_PHASE_PLAYER && i == g->active)
            frame(L, L->ox + i * L->hand_w, y - 2, L->hand_w - 2,
                Z_ART_FULL_H + 4, 1);

        for (k = 0; k < h->n; k++)
            draw_card(L, h->card[k], x + k * stride, y, mini);

        line[0] = '\0';
        cat_num(line, sizeof line, h->bet);
        if (h->doubled) cat(line, sizeof line, "x2");
        if (h->surrendered) cat(line, sizeof line, "s");
        text(L, L->ox + i * L->hand_w + 3, L->hand_info_y, line, 1);

        /* The total is right-aligned in the cell and the bet is
         * left-aligned, so the two cannot run into each other however
         * long either gets -- "bust 37" beside a four-figure bet was
         * the case that showed it. */
        {
            char tl[24];
            bool soft = false;
            int t = bj_total(h->card, h->n, &soft);
            tl[0] = '\0';
            if (h->n == 0) tl[0] = '\0';
            else if (t > 21) { cat(tl, sizeof tl, "bust"); }
            else if (bj_is_natural(h->card, h->n)) cat(tl, sizeof tl, "BJ");
            else { if (soft) cat(tl, sizeof tl, "s"); cat_num(tl, sizeof tl, t); }
            text(L, L->ox + (i + 1) * L->hand_w - 4 - slen(tl) * FW,
                L->hand_info_y, tl, 1);
        }
        (void)cx;
    }
}

/* The shoe, as a bar that empties. A card counter's only honest tell,
 * and the one thing a player genuinely needs to know that the cards
 * themselves do not show: how close the reshuffle is. */
static void draw_shoe(const bj_layout_t *L, const bj_view_t *v)
{
    const zdeck_t *d = &v->g->shoe;
    int left = zdeck_remaining(d);
    int total = d->n > 0 ? d->n : 1;
    int filled = (L->shoe_w - 2) * left / total;
    char line[24];

    fill(L, L->shoe_x, L->shoe_y - FH - 2, L->shoe_w, FH + 2 + 8, 0);

    line[0] = '\0';
    cat(line, sizeof line, "shoe ");
    cat_num(line, sizeof line, left);
    cat(line, sizeof line, " of ");
    cat_num(line, sizeof line, total);
    text(L, L->shoe_x, L->shoe_y - FH - 1, line, 1);

    frame(L, L->shoe_x, L->shoe_y, L->shoe_w, 6, 1);
    if (filled > 0) fill(L, L->shoe_x + 1, L->shoe_y + 1, filled, 4, 1);
}

void bj_board_draw_status(const bj_layout_t *L, const bj_view_t *v)
{
    char line[BJ_MSG_LEN + 4];

    fill(L, L->ox, L->msg_y, L->w, FH, 0);
    text(L, L->ox + 2, L->msg_y, v->message, 1);

    fill(L, L->ox, L->cmd_y, L->w, FH, 0);
    line[0] = '\0';
    cat(line, sizeof line, "> ");
    cat(line, sizeof line, v->cmd);
    cat(line, sizeof line, "_");
    text(L, L->ox + 2, L->cmd_y, line, 1);

    /* The correct play, when asked for.
     *
     * Drawn on the message row's right rather than in the message
     * itself, so it does not fight with whatever the app is saying --
     * and drawn only while a decision is actually open, because a hint
     * that lingers after the hand is advice about nothing.
     *
     * AFTER the status line, not before -- and now INSIDE it. Clearing
     * the message row before writing the message meant a hint drawn
     * first was erased by it, which the render showed as a table with
     * hints enabled and no hint anywhere. Living in this function keeps
     * the two in step, and means the light "only the text changed"
     * repaint carries the hint too rather than wiping it. */
    if (v->hints && v->g->phase == BJ_PHASE_PLAYER) {
        int hint = bj_hint(v->g);
        if (hint >= 0) {
            char hl[32];
            hl[0] = '\0';
            cat(hl, sizeof hl, bj_hint_name(hint));
            /* Flagged when the rules in play are not the ones the chart
             * was built for. A hint that is quietly approximate is
             * worse than one that says so. */
            if (!bj_hint_exact(&v->g->rules)) cat(hl, sizeof hl, "?");
            fill(L, L->ox + L->w - 2 - slen(hl) * FW - 2, L->msg_y,
                slen(hl) * FW + 4, FH, 0);
            text(L, L->ox + L->w - 2 - slen(hl) * FW, L->msg_y, hl, 1);
        }
    }
}

void bj_board_draw(const bj_layout_t *L, const bj_view_t *v)
{
    const bj_game_t *g = v->g;
    bj_options_t o;
    char line[64];
    bool betting = (g->phase == BJ_PHASE_DONE ||
        g->phase == BJ_PHASE_BETTING);

    if (!L->ok) {
        fill(L, L->ox, L->oy, L->w, L->h, 0);
        text(L, L->ox + 2, L->oy + 2, "window too small", 1);
        text(L, L->ox + 2, L->oy + 2 + BJ_LINE_H, "for a table", 1);
        return;
    }

    fill(L, L->ox, L->oy, L->w, L->h, 0);

    /* The rules are on the table, because every one of them moves the
     * house edge and a person should not have to read the source to
     * find out which game they are playing. */
    line[0] = '\0';
    cat(line, sizeof line, "Blackjack  ");
    cat_num(line, sizeof line, g->rules.ndecks);
    cat(line, sizeof line, "dk ");
    cat(line, sizeof line, g->rules.dealer_hits_soft17 ? "H17 " : "S17 ");
    cat_num(line, sizeof line, g->rules.bj_pay_num);
    cat(line, sizeof line, ":");
    cat_num(line, sizeof line, g->rules.bj_pay_den);
    text(L, L->ox + 2, L->status_y, line, 1);

    line[0] = '\0';
    cat_num(line, sizeof line, v->chips);
    cat(line, sizeof line, " chips");
    text(L, L->ox + L->w - 2 - slen(line) * FW, L->status_y, line, 1);

    draw_dealer(L, v);
    draw_hands(L, v);
    draw_shoe(L, v);

    bj_options(g, &o);

    button(L, &L->btn[BJ_BTN_HIT], "hit", o.can_hit);
    button(L, &L->btn[BJ_BTN_STAND], "stand", o.can_stand);
    button(L, &L->btn[BJ_BTN_DOUBLE], "double", o.can_double);
    button(L, &L->btn[BJ_BTN_SPLIT], "split", o.can_split);
    button(L, &L->btn[BJ_BTN_SURRENDER], "surr", o.can_surrender);
    button(L, &L->btn[BJ_BTN_DEAL],
        g->phase == BJ_PHASE_INSURANCE ? "insure" : "deal",
        betting || g->phase == BJ_PHASE_INSURANCE);

    {
        int i;
        for (i = 0; i < BJ_NCHIPS; i++) {
            char b[8];
            bj_num(bj_chip_values[i], b);
            /* The selected denomination is the ENABLED-looking one, so
             * "shaded" means selected here and unavailable on the row
             * above. Two meanings for one treatment would be confusing
             * if they were adjacent; they are not, and the row of chips
             * reads as a group. */
            button(L, &L->chip[i], b, i != v->chip_sel);
        }
    }

    bj_board_draw_status(L, v);
}
