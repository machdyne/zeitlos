/*
 * Zeitlos -- sheet's charts: bars, stacked bars, lines and pies, in
 * patterns rather than colours. docs/sheet_app.md, "Charts".
 *
 * The display has one bit a pixel, which is what 1-2-3 had on a
 * Hercules card and what a laser printer has: series are told apart by
 * hatching and markers, the way those charts were. Bars are filled with
 * an 8x8 pattern and outlined; lines carry a marker shape per series;
 * a pie's wedges are patterned and edged. Everything is drawn with the
 * blitter (pattern fills, boxes, lines), a whole chart in a few hundred
 * operations; the pie is the one thing worked out per pixel, with one
 * integer division each.
 *
 * -- what is charted --
 *
 * The chart's range (the selection when the chart was made, saved with
 * the sheet as "!g bar A1:D9"), read the way people lay a table out:
 *
 *   - a first column with text in it is the categories (the x axis);
 *   - a first row with text in it is the series' names (the legend);
 *   - every other column is a series, up to six; every other row a
 *     point, up to CHART_MAX_PTS.
 *
 * Text and empty cells in the data are gaps; errors are gaps too.
 */

#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#include "../../common/zwin.h"
#include "../../common/zfont.h"
#include "chart.h"

#define MAX_SER		6
#define CHART_MAX_PTS	120

// Eight patterns that stay distinct at any size, solid first.
static const uint8_t pats[8][8] = {
	{ 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF },	// solid
	{ 0x11, 0x22, 0x44, 0x88, 0x11, 0x22, 0x44, 0x88 },	// rising diagonal
	{ 0xAA, 0x55, 0xAA, 0x55, 0xAA, 0x55, 0xAA, 0x55 },	// 50% grey
	{ 0x88, 0x44, 0x22, 0x11, 0x88, 0x44, 0x22, 0x11 },	// falling diagonal
	{ 0xFF, 0x00, 0x00, 0x00, 0xFF, 0x00, 0x00, 0x00 },	// horizontal lines
	{ 0x81, 0x42, 0x24, 0x18, 0x18, 0x24, 0x42, 0x81 },	// cross-hatch
	{ 0x88, 0x88, 0x88, 0x88, 0x88, 0x88, 0x88, 0x88 },	// vertical lines
	{ 0x80, 0x00, 0x08, 0x00, 0x80, 0x00, 0x08, 0x00 },	// sparse dots
};
static const uint8_t dots[8] = { 0x88, 0x88, 0x88, 0x88, 0x88, 0x88, 0x88, 0x88 };	// a dotted rule, at any row (patterns are anchored to the screen)

typedef struct {
	const z_win_t	*win;
	const z_font_t	*font;
	int				ox, oy;				// the content area's screen origin
} cv_t;

static void rect(cv_t *c, int x, int y, int w, int h, int col) { if (w > 0 && h > 0) z_win_fill_rect(c->win, x, y, w, h, col); }
static void pat(cv_t *c, int x, int y, int w, int h, const uint8_t *p) { if (w > 0 && h > 0) z_win_fill_pattern(c->win, x, y, w, h, p); }
static void box(cv_t *c, int x0, int y0, int x1, int y1) { z_win_hw_box(c->win, c->ox + x0, c->oy + y0, c->ox + x1, c->oy + y1, 1); }
static void line(cv_t *c, int x0, int y0, int x1, int y1) { z_win_hw_line(c->win, c->ox + x0, c->oy + y0, c->ox + x1, c->oy + y1, 1); }
static void text(cv_t *c, int x, int y, const char *s) { z_win_draw_text(c->win, x, y, s, 1, c->font); }

// A cell's text, cut to `max` characters.
static void cell_text(sheet_t *s, int r, int col, char *out, int max) {
	const char *t = sheet_src(s, r, col);
	if (*t == '\'') t++;
	if (sheet_kind(s, r, col) != SHEET_KIND_TEXT) {
		sheet_display(s, r, col, out, max + 1, max, NULL);
		return;
	}
	int n = 0;
	while (t[n] && n < max) { out[n] = t[n]; n++; }
	out[n] = 0;
}

static bool num_at(sheet_t *s, int r, int col, double *v) {
	if (sheet_empty(s, r, col) || sheet_kind(s, r, col) == SHEET_KIND_TEXT) return false;
	z_fix_t f;
	if (sheet_value(s, r, col, &f) != SHEET_OK) return false;
	*v = (double)f / (double)z_fix_scale(SHEET_DP);
	return true;
}

// -- numbers on the axis --

// A tidy step for about `want` divisions of `span`: 1, 2 or 5 times a
// power of ten.
static double nice_step(double span, int want, int *dec) {
	double raw = span / want, mag = 1;
	int d = 0;
	if (raw <= 0) raw = 1;
	while (mag * 10 <= raw) mag *= 10;
	while (mag > raw) { mag /= 10; d++; }
	double m = raw / mag, step;
	if (m <= 1) step = mag;
	else if (m <= 2) step = 2 * mag;
	else if (m <= 5) step = 5 * mag;
	else { step = 10 * mag; if (d) d--; }
	*dec = d;
	return step;
}

// v with `dec` decimals, and thousands as "k" past a million's room.
static void fmt_num(double v, int dec, char *out, int cap) {
	char t[32];
	int n = 0;
	bool neg = v < 0;
	if (neg) v = -v;
	double sc = 1;
	for (int i = 0; i < dec; i++) sc *= 10;
	double r = v * sc + 0.5;
	if (r > 9e17) { out[0] = '#'; out[1] = 0; return; }
	uint64_t u = (uint64_t)r;
	uint64_t ip = u, fp = 0;
	for (int i = 0; i < dec; i++) ip /= 10;
	uint64_t p10 = 1;
	for (int i = 0; i < dec; i++) p10 *= 10;
	fp = u - ip * p10;
	char d[24];
	int dn = 0;
	do { d[dn++] = (char)('0' + ip % 10); ip /= 10; } while (ip && dn < 20);
	if (neg && (u != 0)) t[n++] = '-';
	while (dn) t[n++] = d[--dn];
	if (dec) {
		t[n++] = '.';
		for (int i = dec - 1; i >= 0; i--) { uint64_t q = fp; for (int k = 0; k < i; k++) q /= 10; t[n++] = (char)('0' + q % 10); }
	}
	if (n >= cap) n = cap - 1;
	memcpy(out, t, (size_t)n);
	out[n] = 0;
}

// -- markers, for lines --

static void marker(cv_t *c, int x, int y, int k) {
	switch (k % MAX_SER) {
		case 0: rect(c, x - 2, y - 2, 5, 5, 1); break;							// filled square
		case 1: box(c, x - 2, y - 2, x + 2, y + 2); rect(c, x - 1, y - 1, 3, 3, 0); break;	// open square
		case 2: line(c, x - 2, y - 2, x + 2, y + 2); line(c, x - 2, y + 2, x + 2, y - 2); break;	// x
		case 3: rect(c, x - 3, y, 7, 1, 1); rect(c, x, y - 3, 1, 7, 1); break;	// +
		case 4: rect(c, x - 1, y - 2, 3, 5, 1); rect(c, x - 2, y - 1, 5, 3, 1); break;	// filled diamond
		default: line(c, x, y - 3, x + 3, y); line(c, x + 3, y, x, y + 3);
			line(c, x, y + 3, x - 3, y); line(c, x - 3, y, x, y - 3); break;		// open diamond
	}
}

// -- sine and cosine, for the pie's edges --
//
// Only where a wedge begins and ends: a Taylor series on [0, pi/4]
// after reduction, which is ample for placing a line on a screen.

#define PI 3.14159265358979323846

static double sin_r(double x) {			// |x| <= pi/4
	double x2 = x * x;
	return x * (1 - x2 / 6 * (1 - x2 / 20 * (1 - x2 / 42 * (1 - x2 / 72))));
}
static double cos_r(double x) {
	double x2 = x * x;
	return 1 - x2 / 2 * (1 - x2 / 12 * (1 - x2 / 30 * (1 - x2 / 56)));
}

// sin and cos of 2*pi*f, f in [0, 1].
static void sincos_frac(double f, double *s, double *c) {
	double a = f * 4;					// quarters
	int q = (int)a;
	double t = (a - q) * (PI / 2);		// [0, pi/2)
	double ss, cc;
	if (t <= PI / 4) { ss = sin_r(t); cc = cos_r(t); }
	else { ss = cos_r(PI / 2 - t); cc = sin_r(PI / 2 - t); }
	switch (q & 3) {
		case 0: *s = ss;  *c = cc;  break;
		case 1: *s = cc;  *c = -ss; break;
		case 2: *s = -ss; *c = -cc; break;
		default: *s = -cc; *c = ss; break;
	}
}

// A clockwise angle from twelve o'clock that only has to be monotonic:
// 0..4 by quarters, as an integer, with one division. (ux, uy) is up-
// and right-positive.
#define PS 65536
static int32_t pseudo(int32_t ux, int32_t uy) {
	if (ux >= 0 && uy > 0) return (int32_t)((int64_t)ux * PS / (ux + uy));
	if (ux > 0 && uy <= 0) return PS + (int32_t)((int64_t)(-uy) * PS / (ux - uy));
	if (ux <= 0 && uy < 0) return 2 * PS + (int32_t)((int64_t)(-ux) * PS / (-ux - uy));
	if (ux < 0) return 3 * PS + (int32_t)((int64_t)uy * PS / (uy - ux));
	return 0;
}

static int32_t isqrt32(int32_t v) {
	int32_t r = 0;
	while ((r + 1) * (r + 1) <= v) r++;
	return r;
}

// -- the chart --

void chart_draw(const z_win_t *win, sheet_t *s, const sheet_chart_t *g,
	int x, int y, int w, int h, const z_font_t *font) {

	cv_t cv = { win, font, 0, 0 };
	cv_t *c = &cv;
	z_clip_t cr;
	z_win_content_rect(win, &cr);
	cv.ox = cr.x0;
	cv.oy = cr.y0;

	int cw = font->w, lh = font->h + 2;
	rect(c, x, y, w, h, 0);

	if (!g->set) { text(c, x + 4, y + 4, "No chart: select the data, then F10."); return; }

	int r0 = g->r0, r1 = g->r1, c0 = g->c0, c1 = g->c1;

	// the shape of the table
	bool head = false, labels = false;
	for (int col = c0 + (c1 > c0 ? 1 : 0); col <= c1; col++)
		if (!sheet_empty(s, r0, col) && sheet_kind(s, r0, col) == SHEET_KIND_TEXT) head = true;
	for (int r = r0 + (head ? 1 : 0); r <= r1; r++)
		if (!sheet_empty(s, r, c0) && sheet_kind(s, r, c0) == SHEET_KIND_TEXT) labels = true;
	if (c1 == c0) labels = false;

	int dr0 = r0 + (head ? 1 : 0), dc0 = c0 + (labels ? 1 : 0);
	int npts = r1 - dr0 + 1, nser = c1 - dc0 + 1;
	if (npts > CHART_MAX_PTS) npts = CHART_MAX_PTS;
	if (nser > MAX_SER) nser = MAX_SER;
	if (g->kind == SHEET_CHART_PIE) nser = 1;
	if (npts < 1 || nser < 1) { text(c, x + 4, y + 4, "Nothing to chart in the range."); return; }

	// the range of the values
	double lo = 0, hi = 0;
	bool any = false;
	for (int p = 0; p < npts; p++) {
		double pos = 0, neg = 0;
		for (int k = 0; k < nser; k++) {
			double v;
			if (!num_at(s, dr0 + p, dc0 + k, &v)) continue;
			any = true;
			if (g->kind == SHEET_CHART_STACK) { if (v >= 0) pos += v; else neg += v; }
			else { if (v > hi) hi = v; if (v < lo) lo = v; }
		}
		if (g->kind == SHEET_CHART_STACK) { if (pos > hi) hi = pos; if (neg < lo) lo = neg; }
	}
	if (!any) { text(c, x + 4, y + 4, "No numbers in the range to chart."); return; }

	// title: the corner cell, when both headings are there
	int top = y + 2;
	if (head && labels && !sheet_empty(s, r0, c0)) {
		char t[48];
		cell_text(s, r0, c0, t, (w - 8) / cw < 47 ? (w - 8) / cw : 47);
		text(c, x + (w - (int)strlen(t) * cw) / 2, top, t);
		top += lh + 2;
	}

	// the legend, at the right: what each pattern or marker is
	int legw = 0;
	char names[MAX_SER][14];
	int nleg = g->kind == SHEET_CHART_PIE ? (npts < 24 ? npts : 24) : nser;
	if (g->kind == SHEET_CHART_PIE || nser > 1 || head) {
		for (int k = 0; k < nleg && k < MAX_SER; k++) names[k][0] = 0;
		int maxc = 0;
		for (int k = 0; k < nleg; k++) {
			char t[24];
			if (g->kind == SHEET_CHART_PIE) {
				if (labels) cell_text(s, dr0 + k, c0, t, 10); else fmt_num(k + 1, 0, t, sizeof t);
			} else if (head) cell_text(s, r0, dc0 + k, t, 12);
			else { t[0] = 'S'; fmt_num(k + 1, 0, t + 1, sizeof t - 1); }
			int l = (int)strlen(t) + (g->kind == SHEET_CHART_PIE ? 5 : 0);
			if (l > maxc) maxc = l;
			if (k < MAX_SER) memcpy(names[k], t, 14), names[k][13] = 0;
		}
		legw = maxc * cw + 18;
		if (legw > w / 3) legw = w / 3;
	}

	int px0 = x + 4, py0 = top, px1 = x + w - 4 - legw, py1 = y + h - 4;

	// -- the pie --

	if (g->kind == SHEET_CHART_PIE) {
		double vals[24], tot = 0;
		int n = npts < 24 ? npts : 24;
		for (int p = 0; p < n; p++) {
			double v = 0;
			if (!num_at(s, dr0 + p, dc0, &v) || v < 0) v = 0;
			vals[p] = v;
			tot += v;
		}
		if (tot <= 0) { text(c, x + 4, y + 4, "A pie needs some positive numbers."); return; }

		int rad = ((px1 - px0) < (py1 - py0) ? (px1 - px0) : (py1 - py0)) / 2 - 2;
		if (rad < 8) return;
		int cx = (px0 + px1) / 2, cy = (py0 + py1) / 2;

		// where each wedge ends, clockwise from twelve o'clock
		int32_t edge[24];
		double acc = 0, ex[24], ey[24];
		for (int p = 0; p < n; p++) {
			acc += vals[p];
			double f = acc / tot, sn, cs;
			if (f > 1) f = 1;
			sincos_frac(f, &sn, &cs);
			ex[p] = sn;
			ey[p] = cs;
			edge[p] = (p == n - 1 || f >= 1) ? 4 * PS : pseudo((int32_t)(sn * 10000), (int32_t)(cs * 10000));
		}

		// row by row: runs of one wedge are one pattern fill
		for (int dy = -rad; dy <= rad; dy++) {
			int dx = isqrt32(rad * rad - dy * dy);
			int run0 = -dx, cur = -1;
			for (int xx = -dx; xx <= dx + 1; xx++) {
				int wdg = -1;
				if (xx <= dx) {
					int32_t a = pseudo(xx, -dy);
					wdg = 0;
					while (wdg < n - 1 && a >= edge[wdg]) wdg++;
				}
				if (wdg != cur) {
					if (cur >= 0) pat(c, cx + run0, cy + dy, xx - run0, 1, pats[cur % 8]);
					cur = wdg;
					run0 = xx;
				}
			}
			// the rim
			rect(c, cx - dx, cy + dy, 1, 1, 1);
			rect(c, cx + dx, cy + dy, 1, 1, 1);
		}
		rect(c, cx - 1, cy - rad, 3, 1, 1);
		rect(c, cx - 1, cy + rad, 3, 1, 1);

		// the edges between wedges, and the legend with each share
		for (int p = 0; p < n; p++) {
			if (vals[p] <= 0 && p) continue;
			if (n > 1) line(c, cx, cy, cx + (int)(ex[p] * rad), cy - (int)(ey[p] * rad));
		}
		int ly = py0;
		for (int p = 0; p < n && ly + lh <= py1; p++) {
			int lx = px1 + 6;
			pat(c, lx, ly + 1, 10, lh - 3, pats[p % 8]);
			box(c, lx, ly + 1, lx + 9, ly + lh - 3);
			char t[24], pc[12];
			if (p < MAX_SER) memcpy(t, names[p], 14);
			else if (labels) cell_text(s, dr0 + p, c0, t, 10);
			else fmt_num(p + 1, 0, t, sizeof t);
			fmt_num(vals[p] * 100 / tot, 0, pc, sizeof pc);
			int l = (int)strlen(t);
			t[l++] = ' ';
			for (int i = 0; pc[i] && l < 22; i++) t[l++] = pc[i];
			t[l++] = '%';
			t[l] = 0;
			text(c, lx + 14, ly, t);
			ly += lh;
		}
		return;
	}

	// -- axes --

	if (lo == hi) { if (hi > 0) lo = 0; else if (lo < 0) hi = 0; else hi = 1; }
	int dec;
	double step = nice_step(hi - lo, (py1 - py0) / (lh * 2) > 2 ? (py1 - py0) / (lh * 2) : 2, &dec);
	double a_lo = step * (double)(int64_t)(lo / step - (lo < 0 ? 0.999999 : 0));
	double a_hi = step * (double)(int64_t)(hi / step + (hi > 0 ? 0.999999 : 0));
	if (a_hi <= a_lo) a_hi = a_lo + step;

	// room for the longest tick label at the left, the categories below
	int ylab = 0;
	for (double v = a_lo; v <= a_hi + step / 2; v += step) {
		char t[24];
		fmt_num(v, dec, t, sizeof t);
		int l = (int)strlen(t);
		if (l > ylab) ylab = l;
	}
	px0 += ylab * cw + 4;
	py1 -= lh + 2;
	if (px1 - px0 < 20 || py1 - py0 < 20) return;

	double span = a_hi - a_lo;
	#define YPIX(v) (py1 - (int)(((v) - a_lo) / span * (py1 - py0) + 0.5))

	for (double v = a_lo; v <= a_hi + step / 2; v += step) {
		int yy = YPIX(v);
		char t[24];
		fmt_num(v, dec, t, sizeof t);
		text(c, px0 - 4 - (int)strlen(t) * cw, yy - font->h / 2, t);
		pat(c, px0, yy, px1 - px0, 1, dots);
		rect(c, px0 - 3, yy, 3, 1, 1);
	}
	rect(c, px0, py0, 1, py1 - py0 + 1, 1);
	int zero = YPIX(a_lo < 0 && a_hi > 0 ? 0 : a_lo);
	rect(c, px0, zero, px1 - px0, 1, 1);

	// categories: a slot each; their names under them, as many as fit
	int slot_w = (px1 - px0) / npts;
	if (slot_w < 1) slot_w = 1;
	int every = 1;
	{
		int maxl = 1;
		if (labels) for (int p = 0; p < npts; p++) {
			char t[24];
			cell_text(s, dr0 + p, c0, t, 12);
			int l = (int)strlen(t);
			if (l > maxl) maxl = l;
		}
		int fit = slot_w / cw;
		if (fit < 1) fit = 1;
		if (maxl > fit && fit < 4) every = (4 * cw + slot_w - 1) / slot_w;
		for (int p = 0; p < npts; p += every) {
			char t[24];
			int room = (slot_w * every) / cw;
			if (room > 12) room = 12;
			if (room < 1) continue;
			if (labels) cell_text(s, dr0 + p, c0, t, room);
			else fmt_num(p + 1, 0, t, sizeof t), t[room] = 0;
			int tx = px0 + p * slot_w + (slot_w - (int)strlen(t) * cw) / 2;
			if (tx < px0) tx = px0;
			text(c, tx, py1 + 3, t);
		}
	}

	// -- the data --

	if (g->kind == SHEET_CHART_LINE) {
		for (int k = 0; k < nser; k++) {
			int lx = -1, ly = 0;
			for (int p = 0; p < npts; p++) {
				double v;
				if (!num_at(s, dr0 + p, dc0 + k, &v)) { lx = -1; continue; }
				int xx = px0 + p * slot_w + slot_w / 2, yy = YPIX(v);
				if (lx >= 0) line(c, lx, ly, xx, yy);
				lx = xx;
				ly = yy;
			}
			for (int p = 0; p < npts; p++) {
				double v;
				if (num_at(s, dr0 + p, dc0 + k, &v)) marker(c, px0 + p * slot_w + slot_w / 2, YPIX(v), k);
			}
		}
	} else {
		int gw = slot_w * 3 / 4;
		if (gw < 1) gw = 1;
		int bw = g->kind == SHEET_CHART_STACK ? gw : gw / nser;
		if (bw < 1) bw = 1;
		for (int p = 0; p < npts; p++) {
			int gx = px0 + p * slot_w + (slot_w - (g->kind == SHEET_CHART_STACK ? bw : bw * nser)) / 2;
			double up = 0, down = 0;
			for (int k = 0; k < nser; k++) {
				double v;
				if (!num_at(s, dr0 + p, dc0 + k, &v)) continue;
				double b0, b1;
				int bx = g->kind == SHEET_CHART_STACK ? gx : gx + k * bw;
				if (g->kind == SHEET_CHART_STACK) {
					if (v >= 0) { b0 = up; up += v; b1 = up; }
					else { b0 = down; down += v; b1 = down; }
				} else { b0 = (a_lo < 0 && a_hi > 0) ? 0 : a_lo; b1 = v; }
				int ya = YPIX(b0), yb = YPIX(b1);
				int yt = ya < yb ? ya : yb, yh = (ya < yb ? yb - ya : ya - yb) + 1;
				pat(c, bx, yt, bw, yh, pats[k % 8]);
				if (bw >= 3 && yh >= 2) box(c, bx, yt, bx + bw - 1, yt + yh - 1);
			}
		}
	}

	// the legend
	if (legw) {
		int ly = py0;
		for (int k = 0; k < nser && ly + lh <= y + h; k++) {
			int lx = px1 + 6;
			if (g->kind == SHEET_CHART_LINE) {
				rect(c, lx, ly + lh / 2 - 1, 11, 1, 1);
				marker(c, lx + 5, ly + lh / 2 - 1, k);
			} else {
				pat(c, lx, ly + 1, 10, lh - 3, pats[k % 8]);
				box(c, lx, ly + 1, lx + 9, ly + lh - 3);
			}
			text(c, lx + 14, ly, names[k]);
			ly += lh;
		}
	}
	#undef YPIX

}
