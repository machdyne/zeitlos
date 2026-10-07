/*
 * bench -- drawing (draw.h).
 */

#include <stdio.h>
#include <string.h>
#include "draw.h"
#include "../../common/zfont.h"

#define FONT        (&z_font_5x8)
#define CH_W        6           /* a character's advance */

card_t bd_cards[BN_PARTS];

/* ---- the canvas ---- */

static canvas_t *cv;
static int org_x, org_y;            /* the document's point at the canvas's 0,0 */

static void px(int x, int y, int on) {
    x -= org_x;
    y -= org_y;
    if (x < 0 || y < 0 || x >= cv->w || y >= cv->h) return;
    uint32_t *w = &cv->px[y * cv->wpl + (x >> 5)], m = 1u << (x & 31);
    if (on) *w |= m;
    else *w &= ~m;
}

static void hline(int x0, int x1, int y, int on) {
    for (int x = x0; x <= x1; x++) px(x, y, on);
}

static void vline(int x, int y0, int y1, int on) {
    for (int y = y0; y <= y1; y++) px(x, y, on);
}

static void box(int x, int y, int w, int h) {
    hline(x, x + w - 1, y, 1);
    hline(x, x + w - 1, y + h - 1, 1);
    vline(x, y, y + h - 1, 1);
    vline(x + w - 1, y, y + h - 1, 1);
}

static void dotted(int x, int y, int w, int h) {
    for (int i = 0; i < w; i += 2) {
        px(x + i, y, 1);
        px(x + i, y + h - 1, 1);
    }
    for (int i = 0; i < h; i += 2) {
        px(x, y + i, 1);
        px(x + w - 1, y + i, 1);
    }
}

static void fill(int x, int y, int w, int h, int on) {
    for (int j = 0; j < h; j++) hline(x, x + w - 1, y + j, on);
}

static void circle(int cx, int cy, int r, bool solid) {
    for (int y = -r; y <= r; y++)
        for (int x = -r; x <= r; x++) {
            int d = x * x + y * y;
            if (solid ? d <= r * r + r : (d <= r * r + r && d >= r * r - r)) px(cx + x, cy + y, 1);
        }
}

/* text in the small font; at most max characters (0: all) */
static void text(int x, int y, const char *s, int max, int on) {
    for (int n = 0; *s && (!max || n < max); s++, n++, x += CH_W) {
        int i = z_font_index(FONT, (uint8_t)*s);
        if (i < 0) i = z_font_index(FONT, Z_GLYPH_MISSING);
        const uint8_t *g = FONT->glyphs + i * FONT->h;
        for (int r = 0; r < FONT->h; r++)
            for (int b = 0; b < FONT->w; b++)
                if (g[r] & (0x80 >> b)) px(x + b, y + r, on);
    }
}

/* ---- layout ---- */

static int rows_of(const part_t *p) {
    return (p->type->npins + 7) / 8;
}

static void span(const part_t *p, int *cols, int *rows) {
    if (p->type->npins <= 1) {
        *cols = *rows = 1;
        return;
    }
    *cols = 3;
    /* a header, then each row of pins: name, box, direction */
    *rows = (14 + rows_of(p) * 30 + 4 + BD_CELL_H - 1) / BD_CELL_H;
}

int bd_layout(int width) {
    static uint8_t used[64][16];        /* rows x columns of cells */
    int ncol = width / BD_CELL_W, bottom = 0;
    if (ncol < 3) ncol = 3;             /* the widest card always fits */
    if (ncol > 16) ncol = 16;
    memset(used, 0, sizeof(used));
    memset(bd_cards, 0, sizeof(bd_cards));

    for (int pass = 0; pass < 2; pass++)        /* placed parts first */
        for (int i = 0; i < bn_nparts; i++) {
            part_t *p = &bn_parts[i];
            int cols, rows, c = 0, r = 0, ok = 0;
            if ((p->col >= 0) != (pass == 0)) continue;
            span(p, &cols, &rows);
            if (pass == 0) {
                c = p->col;
                r = p->row;
                if (c + cols > ncol) c = ncol - cols;
                if (c < 0) c = 0;
                if (r < 0) r = 0;
                if (r + rows > 64) r = 64 - rows;
                ok = 1;
            } else {
                for (r = 0; r + rows <= 64 && !ok; r++)
                    for (c = 0; c + cols <= ncol && !ok; c++) {
                        ok = 1;
                        for (int y = 0; y < rows && ok; y++)
                            for (int x = 0; x < cols && ok; x++)
                                if (used[r + y][c + x]) ok = 0;
                        if (ok) goto found;
                    }
                continue;                       /* no room: not drawn */
            found:;
            }
            for (int y = 0; y < rows; y++)
                for (int x = 0; x < cols; x++) used[r + y][c + x] = 1;
            bd_cards[i].x = (int16_t)(c * BD_CELL_W);
            bd_cards[i].y = (int16_t)(r * BD_CELL_H);
            bd_cards[i].w = (int16_t)(cols * BD_CELL_W);
            bd_cards[i].h = (int16_t)(rows * BD_CELL_H);
            if ((r + rows) * BD_CELL_H > bottom) bottom = (r + rows) * BD_CELL_H;
        }
    return bottom + 2;
}

/* ---- cards ---- */

/* the box of a pin: filled high, empty low, dotted floating, X conflict */
static void pin_box(int x, int y, int lv) {
    if (lv == BN_L1) fill(x, y, 10, 10, 1);
    else if (lv == BN_L0) box(x, y, 10, 10);
    else if (lv == BN_FLOAT) {
        dotted(x, y, 10, 10);
        text(x + 3, y + 1, "?", 1, 1);
    } else {
        box(x, y, 10, 10);
        for (int i = 0; i < 10; i++) {
            px(x + i, y + i, 1);
            px(x + 9 - i, y + i, 1);
        }
    }
}

/* what a pin connects to, besides itself: the first other part's pin */
static void connection(part_t *p, int pin, char *buf, int len) {
    int n = p->net[pin];
    buf[0] = 0;
    if (n < 0) {
        snprintf(buf, (size_t)len, "-");
        return;
    }
    for (int i = 0; i < bn_nparts; i++) {
        part_t *q = &bn_parts[i];
        if (q == p) continue;
        for (int j = 0; j < q->type->npins; j++)
            if (q->net[j] == n) {
                snprintf(buf, (size_t)len, "%s.%s", q->name, q->type->pins[j]);
                return;
            }
    }
    snprintf(buf, (size_t)len, "%s", bn_nets[n].name);
}

static void card_multi(part_t *p, const card_t *k) {
    char head[48];
    int x0 = k->x + 2, y0 = k->y + 2;
    box(x0, y0, k->w - 4, k->h - 4);
    if (p->type->addr_hi && p->bus >= 0)
        snprintf(head, sizeof(head), "%s %s %02x %s", p->name, p->type->name, p->addr,
                 bn_buses[p->bus].name);
    else
        snprintf(head, sizeof(head), "%s %s", p->name, p->type->name);
    text(x0 + 4, y0 + 3, head, (k->w - 12) / CH_W, 1);
    for (int i = 0; i < p->type->npins; i++) {
        int x = x0 + 6 + (i % 8) * 26, y = y0 + 16 + (i / 8) * 30;
        text(x, y, p->type->pins[i], 4, 1);
        pin_box(x + 2, y + 9, bn_pin_get(p, i));
        int d = p->drive[i];
        text(x + 2, y + 20, d == BN_HIGH || d == BN_LOW ? "o" : "i", 1, 1);
    }
}

static void card_one(part_t *p, const card_t *k) {
    char conn[32];
    const char *t = p->type->name;
    int x0 = k->x + 2, y0 = k->y + 2, cx = k->x + k->w / 2, cy = y0 + 23;
    bool on = p->type->lit ? p->type->lit(p) : false;
    box(x0, y0, k->w - 4, k->h - 4);
    text(x0 + 3, y0 + 3, p->label[0] ? p->label : p->name, (k->w - 10) / CH_W, 1);
    if (!strcmp(t, "led")) {
        circle(cx, cy, 7, on);
    } else if (!strcmp(t, "load")) {
        circle(cx, cy, 6, on);
        if (on)
            for (int a = 0; a < 8; a++) {
                static const int8_t dx[8] = { 0, 7, 10, 7, 0, -7, -10, -7 };
                static const int8_t dy[8] = { -10, -7, 0, 7, 10, 7, 0, -7 };
                px(cx + dx[a], cy + dy[a], 1);
                px(cx + dx[a] * 9 / 10, cy + dy[a] * 9 / 10, 1);
            }
    } else if (!strcmp(t, "button")) {
        if (on) {
            fill(cx - 14, cy - 7, 29, 14, 1);
            text(cx - 11, cy - 4, "down", 4, 0);
        } else {
            box(cx - 14, cy - 7, 29, 14);
            text(cx - 5, cy - 4, "up", 2, 1);
        }
    } else if (!strcmp(t, "switch")) {
        if (on) {
            fill(cx - 14, cy - 7, 29, 14, 1);
            text(cx - 5, cy - 4, "on", 2, 0);
        } else {
            box(cx - 14, cy - 7, 29, 14);
            text(cx - 8, cy - 4, "off", 3, 1);
        }
    } else {
        pin_box(cx - 5, cy - 5, bn_pin_get(p, 0));
    }
    connection(p, 0, conn, sizeof(conn));
    text(x0 + 3, y0 + k->h - 15, conn, (k->w - 10) / CH_W, 1);
}

void bd_draw(canvas_t *c, int ox, int oy) {
    cv = c;
    org_x = ox;
    org_y = oy;
    memset(c->px, 0, (size_t)c->wpl * 4 * (size_t)c->h);
    for (int i = 0; i < bn_nparts; i++) {
        const card_t *k = &bd_cards[i];
        if (!k->w) continue;
        if (bn_parts[i].type->npins > 1) card_multi(&bn_parts[i], k);
        else card_one(&bn_parts[i], k);
    }
}

int bd_hit(int x, int y, int *pin) {
    *pin = -1;
    for (int i = 0; i < bn_nparts; i++) {
        const card_t *k = &bd_cards[i];
        if (!k->w || x < k->x || y < k->y || x >= k->x + k->w || y >= k->y + k->h) continue;
        part_t *p = &bn_parts[i];
        if (p->type->npins <= 1) {
            *pin = 0;
            return i;
        }
        for (int j = 0; j < p->type->npins; j++) {
            int px0 = k->x + 2 + 6 + (j % 8) * 26, py0 = k->y + 2 + 16 + (j / 8) * 30;
            if (x >= px0 && x < px0 + 24 && y >= py0 && y < py0 + 28) {
                *pin = j;
                break;
            }
        }
        return i;
    }
    return -1;
}

void bd_net_info(part_t *p, int pin, char *buf, int len) {
    static const char *const lv[] = { "low", "high", "floating", "CONFLICT" };
    int n = p->net[pin], o;
    if (n < 0) {
        snprintf(buf, (size_t)len, "%s.%s: not connected", p->name, p->type->pins[pin]);
        return;
    }
    o = snprintf(buf, (size_t)len, "%s: %s%s --", bn_nets[n].name, lv[bn_nets[n].level & 3],
                 bn_nets[n].pull == BN_PULLUP ? ", pull-up" :
                 bn_nets[n].pull == BN_PULLDOWN ? ", pull-down" : "");
    for (int i = 0; i < bn_nparts && o < len - 1; i++)
        for (int j = 0; j < bn_parts[i].type->npins && o < len - 1; j++)
            if (bn_parts[i].net[j] == n)
                o += snprintf(buf + o, (size_t)(len - o), " %s.%s", bn_parts[i].name,
                              bn_parts[i].type->pins[j]);
}

/* ---- text ---- */

int bd_draw_text(canvas_t *c, const char *t, int oy) {
    int lines = 1;
    for (const char *p = t; *p; p++) lines += *p == '\n';
    if (!c) return 4 + lines * 10 + 4;
    cv = c;
    org_x = 0;
    org_y = oy;
    memset(c->px, 0, (size_t)c->wpl * 4 * (size_t)c->h);
    if (!*t) {
        text(4, 4, "no module has printed anything", 0, 1);
        return 0;
    }
    for (int y = 4; *t; y += 10) {
        char line[110];
        int n = 0;
        while (*t && *t != '\n') {
            if (n < (int)sizeof(line) - 1) line[n++] = *t;
            t++;
        }
        if (*t) t++;
        line[n] = 0;
        text(4, y, line, 0, 1);
    }
    return 0;
}

/* ---- the log ---- */

int bd_draw_log(canvas_t *c, int oy) {
    int lines = 0, y = 4;
    cv = c;
    org_x = 0;
    org_y = oy;
    for (int k = 0; k < BN_LOG; k++)
        if (bn_log[(bn_log_next + k) % BN_LOG].wn || bn_log[(bn_log_next + k) % BN_LOG].rn ||
            bn_log[(bn_log_next + k) % BN_LOG].addr)
            lines++;
    if (!c) return 4 + (lines ? lines : 1) * 10 + 4;
    memset(c->px, 0, (size_t)c->wpl * 4 * (size_t)c->h);
    if (!lines) {
        text(4, y, "no transactions yet", 0, 1);
        return 0;
    }
    for (int k = 0; k < BN_LOG; k++) {
        const xfer_log_t *l = &bn_log[(bn_log_next + k) % BN_LOG];
        char s[96];
        int o;
        if (!l->wn && !l->rn && !l->addr) continue;
        o = snprintf(s, sizeof(s), "%3lu.%03lu %-6.6s %02x", (unsigned long)(l->ms / 1000),
                     (unsigned long)(l->ms % 1000), bn_buses[l->bus].name, l->addr);
        if (l->wn) {
            o += snprintf(s + o, sizeof(s) - (size_t)o, " W");
            for (int i = 0; i < l->wn && i < 8; i++)
                o += snprintf(s + o, sizeof(s) - (size_t)o, " %02x", l->w[i]);
            if (l->wn > 8) o += snprintf(s + o, sizeof(s) - (size_t)o, " ..");
        }
        if (l->rn && l->status == BN_ACK) {
            o += snprintf(s + o, sizeof(s) - (size_t)o, " R");
            for (int i = 0; i < l->rn && i < 8; i++)
                o += snprintf(s + o, sizeof(s) - (size_t)o, " %02x", l->r[i]);
            if (l->rn > 8) o += snprintf(s + o, sizeof(s) - (size_t)o, " ..");
        }
        if (l->status == BN_NACK_ADDR) snprintf(s + o, sizeof(s) - (size_t)o, "  nack");
        else if (l->status == BN_NACK_DATA) snprintf(s + o, sizeof(s) - (size_t)o, "  nack data");
        text(4, y, s, 0, 1);
        y += 10;
    }
    return 0;
}
