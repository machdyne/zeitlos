/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * zpage -- see zpage.h.
 */

#include "zpage.h"

#include <string.h>

static const zpage_size_t sizes[] = {
	{ "classic", "Classic card (512x342)", 512, 342, ZPAGE_RATIO },
	{ "4:3",     "Screen 4:3",             4,   3,   ZPAGE_RATIO },
	{ "16:9",    "Screen 16:9",            16,  9,   ZPAGE_RATIO },
	{ "16:10",   "Screen 16:10",           16,  10,  ZPAGE_RATIO },
	{ "square",  "Square",                 1,   1,   ZPAGE_RATIO },
	{ "letter",  "US Letter",              2159, 2794, ZPAGE_PAPER },
	{ "legal",   "US Legal",               2159, 3556, ZPAGE_PAPER },
	{ "a4",      "A4",                     2100, 2970, ZPAGE_PAPER },
	{ "a5",      "A5",                     1480, 2100, ZPAGE_PAPER },
	{ "a6",      "A6",                     1050, 1480, ZPAGE_PAPER },
	{ "4x6",     "4 x 6 in (postcard)",    1016, 1524, ZPAGE_PAPER },
	{ "5x7",     "5 x 7 in",               1270, 1778, ZPAGE_PAPER },
	{ "3x5",     "3 x 5 in (index card)",  762,  1270, ZPAGE_PAPER },
};

#define NSIZES ((int)(sizeof(sizes) / sizeof(sizes[0])))

int zpage_count(void) { return NSIZES; }

const zpage_size_t *zpage_at(int i) {
	return (i >= 0 && i < NSIZES) ? &sizes[i] : NULL;
}

const zpage_size_t *zpage_find(const char *name) {
	int i;
	if (!name) return NULL;
	for (i = 0; i < NSIZES; i++) {
		const char *a = sizes[i].name, *b = name;
		while (*a && (*a == *b || (*b >= 'A' && *b <= 'Z' && *a == *b + 32))) { a++; b++; }
		if (!*a && !*b) return &sizes[i];
	}
	return NULL;
}

static void derive_grid(zpage_t *pg) {
	int lo = pg->size.w < pg->size.h ? pg->size.w : pg->size.h;
	int hi = pg->size.w < pg->size.h ? pg->size.h : pg->size.w;
	if (hi <= 0) { pg->gw = ZPAGE_GRID_LONG; pg->gh = 342; return; }
	if (pg->landscape) {
		pg->gw = ZPAGE_GRID_LONG;
		pg->gh = (uint16_t)((ZPAGE_GRID_LONG * lo + hi / 2) / hi);
	} else {
		pg->gh = ZPAGE_GRID_PORTRAIT;
		pg->gw = (uint16_t)((ZPAGE_GRID_PORTRAIT * lo + hi / 2) / hi);
	}
	if (pg->gw < 1) pg->gw = 1;
	if (pg->gh < 1) pg->gh = 1;
}

bool zpage_set(zpage_t *pg, const char *name, bool landscape) {
	const zpage_size_t *s = zpage_find(name);
	if (!s) return false;
	memset(pg, 0, sizeof(*pg));
	pg->size = *s;
	pg->landscape = landscape;
	derive_grid(pg);
	return true;
}

bool zpage_custom(zpage_t *pg, int kind, int w, int h, bool landscape) {
	if (w <= 0 || h <= 0 || w > 60000 || h > 60000) return false;
	memset(pg, 0, sizeof(*pg));
	pg->size.name = "custom";
	pg->size.label = "Custom";
	pg->size.kind = (uint8_t)kind;
	if (kind == ZPAGE_PAPER) {
		pg->size.w = (uint16_t)(w < h ? w : h);
		pg->size.h = (uint16_t)(w < h ? h : w);
	} else {
		pg->size.w = (uint16_t)(w < h ? h : w);
		pg->size.h = (uint16_t)(w < h ? w : h);
	}
	pg->landscape = landscape;
	derive_grid(pg);
	return true;
}

bool zpage_physical(const zpage_t *pg, int *w_mm10, int *h_mm10) {
	int lo, hi;
	if (pg->size.kind != ZPAGE_PAPER) return false;
	lo = pg->size.w < pg->size.h ? pg->size.w : pg->size.h;
	hi = pg->size.w < pg->size.h ? pg->size.h : pg->size.w;
	*w_mm10 = pg->landscape ? hi : lo;
	*h_mm10 = pg->landscape ? lo : hi;
	return true;
}

void zpage_sheet(zpage_sheet_t *sh, const zpage_size_t *paper, int dpi,
	int margin_mm10) {
	int m = zpage_mm10_to_px(margin_mm10, dpi);
	memset(sh, 0, sizeof(*sh));
	sh->dpi = dpi;
	sh->w = zpage_mm10_to_px(paper->w < paper->h ? paper->w : paper->h, dpi);
	sh->h = zpage_mm10_to_px(paper->w < paper->h ? paper->h : paper->w, dpi);
	sh->left = sh->top = sh->right = sh->bottom = m;
}

// Fills `out` for a card of cw x ch device pixels (already turned if
// `turned`), its top-left at (x, y).
static void make_cell(const zpage_t *pg, int x, int y, int64_t s16, bool turned,
	zpage_cell_t *out) {
	int gw = pg->gw, gh = pg->gh;
	int cw = (int)(((int64_t)(turned ? gh : gw) * s16 + 32768) >> 16);
	int ch = (int)(((int64_t)(turned ? gw : gh) * s16 + 32768) >> 16);
	out->x = x;
	out->y = y;
	out->w = cw;
	out->h = ch;
	out->turned = turned;
	if (turned) out->xf = zc_xf_rot90((int32_t)s16, (zc_fix)(x + cw) * ZC_ONE, (zc_fix)y * ZC_ONE);
	else out->xf = zc_xf_scale((int32_t)s16, (zc_fix)x * ZC_ONE, (zc_fix)y * ZC_ONE);
}

bool zpage_place(const zpage_t *pg, const zpage_sheet_t *sh, int nup, int index,
	int mode, int gap_mm10, zpage_cell_t *out) {
	int ax = sh->left, ay = sh->top;
	int aw = sh->w - sh->left - sh->right, ah = sh->h - sh->top - sh->bottom;
	int gap = zpage_mm10_to_px(gap_mm10, sh->dpi);
	int best_cols = 0, best_rows = 0;
	bool best_turn = false;
	int64_t best = 0, actual = 0;
	int cols;
	int pw, ph;

	if (nup < 1 || index < 0 || index >= nup || aw <= 0 || ah <= 0) return false;
	if (nup == 1) gap = 0;

	// actual size: the page's paper at the sheet's resolution
	if (mode == ZPAGE_ACTUAL && zpage_physical(pg, &pw, &ph)) {
		int dw = zpage_mm10_to_px(pw, sh->dpi);
		actual = ((int64_t)dw << 16) / pg->gw;
		if (nup == 1) {
			// centred on the sheet itself: a page the size of the sheet
			// covers it edge to edge
			bool turned = false;
			int cw = dw, ch = zpage_mm10_to_px(ph, sh->dpi);
			if (cw > sh->w || ch > sh->h) {
				if (ch <= sh->w && cw <= sh->h) turned = true;
				else actual = 0;
			}
			if (actual) {
				int tw = turned ? ch : cw, th = turned ? cw : ch;
				make_cell(pg, (sh->w - tw) / 2, (sh->h - th) / 2, actual, turned, out);
				return true;
			}
		}
	}

	for (cols = 1; cols <= nup; cols++) {
		int rows = (nup + cols - 1) / cols;
		int t;
		if ((cols - 1) * rows >= nup) continue;	// a column would be empty
		for (t = 0; t < 2; t++) {
			int cw = t ? pg->gh : pg->gw, ch = t ? pg->gw : pg->gh;
			int cellw = (aw - gap * (cols - 1)) / cols;
			int cellh = (ah - gap * (rows - 1)) / rows;
			int64_t sx, sy, s;
			if (cellw <= 0 || cellh <= 0) continue;
			sx = ((int64_t)cellw << 16) / cw;
			sy = ((int64_t)cellh << 16) / ch;
			s = sx < sy ? sx : sy;
			if (actual) {
				if (actual > s) continue;	// does not fit at its real size
				s = actual;
			}
			// several to a sheet stay upright (handouts are read in a
			// stack, not turned one by one) unless upright cannot fit
			if (t && nup > 1 && best > 0 && !best_turn) continue;
			if (s > best || (t == 0 && best_turn && nup > 1)) {
				best = s;
				best_cols = cols;
				best_rows = rows;
				best_turn = t != 0;
			}
		}
	}
	if (best <= 0) return false;

	{
		int cw = (int)(((int64_t)(best_turn ? pg->gh : pg->gw) * best + 32768) >> 16);
		int ch = (int)(((int64_t)(best_turn ? pg->gw : pg->gh) * best + 32768) >> 16);
		int bw = cw * best_cols + gap * (best_cols - 1);
		int bh = ch * best_rows + gap * (best_rows - 1);
		int col = index % best_cols, row = index / best_cols;
		int x = ax + (aw - bw) / 2 + col * (cw + gap);
		int y = ay + (ah - bh) / 2 + row * (ch + gap);
		make_cell(pg, x, y, best, best_turn, out);
	}
	return true;
}

// Does the segment touch a card's interior? A mark is never drawn over a
// card (in a narrow gutter the marks of neighbours would).
static bool over_card(const zpage_cell_t *cells, int n, const int *s) {
	int i;
	int x0 = s[0] < s[2] ? s[0] : s[2], x1 = s[0] < s[2] ? s[2] : s[0];
	int y0 = s[1] < s[3] ? s[1] : s[3], y1 = s[1] < s[3] ? s[3] : s[1];
	for (i = 0; i < n; i++) {
		const zpage_cell_t *z = &cells[i];
		if (x1 >= z->x && x0 < z->x + z->w && y1 >= z->y && y0 < z->y + z->h)
			return true;
	}
	return false;
}

int zpage_cut_marks(const zpage_sheet_t *sh, const zpage_cell_t *cells, int n,
	int len_px, int (*seg)[4], int max) {
	int i, k = 0, c;
	int off = len_px / 4 + 1;
	for (i = 0; i < n; i++) {
		const zpage_cell_t *z = &cells[i];
		int xs[2] = { z->x, z->x + z->w }, ys[2] = { z->y, z->y + z->h };
		for (c = 0; c < 4; c++) {
			int x = xs[c & 1], y = ys[c >> 1];
			int dx = (c & 1) ? 1 : -1, dy = (c >> 1) ? 1 : -1;
			int s[2][4] = {
				{ x + dx * off, y, x + dx * (off + len_px), y },
				{ x, y + dy * off, x, y + dy * (off + len_px) },
			};
			int j;
			for (j = 0; j < 2 && k < max; j++) {
				int q;
				for (q = 0; q < 4; q++) {
					int lim = (q & 1) ? sh->h - 1 : sh->w - 1;
					if (s[j][q] < 0) s[j][q] = 0;
					if (s[j][q] > lim) s[j][q] = lim;
				}
				if (over_card(cells, n, s[j])) continue;
				memcpy(seg[k++], s[j], sizeof(s[j]));
			}
		}
	}
	return k;
}
