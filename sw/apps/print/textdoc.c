/*
 * print -- plain text laid out on pages. See textdoc.h.
 *
 * Each line of the file is a paragraph: it is word-wrapped to the text
 * width (zc_text_wrap()), its leading spaces kept as an indent that the
 * lines it wraps onto keep too, so an indented list or a quoted block
 * still reads as one. A tab moves to the next multiple of eight
 * characters, a form feed starts a new page, and an empty line is an
 * empty line. Each page has its number at the foot.
 *
 * The face is a Hershey stroke font (docs/fonts.md: "text that will be
 * printed belongs in a Hershey face"): sharp at any resolution, and
 * named in the job like any other run of text, so a PDF carries it as
 * a font. It is proportional, so columns lined up with spaces in the
 * file do not line up on paper; the alternative, a bitmap font
 * enlarged, prints blocky.
 */

#include "textdoc.h"

#include <string.h>

#include "../../common/zpaper.h"
#include "../../common/zpage.h"
#include "../../common/zcfont.h"
#include "../../common/zpool.h"
#include "res.h"

#define MARGIN_MM10   200	// 2 cm all round
#define FOOT_MM10     100	// the page number, 1 cm above the bottom edge
#define TAB           8
#define WRAP_LINES    128	// display lines per zc_text_wrap() call

// The pages as written, after the job's own bytes.
typedef struct {
	uint8_t		*b;
	uint32_t	n, cap;
	uint32_t	skip;		// the writer's own header, not wanted
	bool		failed;
} mbuf_t;

static bool mput(void *ctx, const void *d, uint32_t len) {
	mbuf_t *m = ctx;
	const uint8_t *p = d;
	if (m->failed) return false;
	if (m->skip) {
		uint32_t k = len < m->skip ? len : m->skip;
		m->skip -= k;
		p += k;
		len -= k;
	}
	if (m->n + len > m->cap) {
		uint32_t cap = m->cap * 2;
		uint8_t *nb;
		while (cap < m->n + len) cap *= 2;
		nb = pool_realloc(m->b, cap);
		if (!nb) { m->failed = true; return false; }
		m->b = nb;
		m->cap = cap;
	}
	memcpy(m->b + m->n, p, len);
	m->n += len;
	return true;
}

static int to_int(const char *s, int def) {
	int v = 0;
	bool any = false;
	while (*s >= '0' && *s <= '9') { v = v * 10 + (*s++ - '0'); any = true; if (v > 1000) break; }
	return any ? v : def;
}

static void say(char *err, int n, const char *s) {
	int i;
	for (i = 0; i < n - 1 && s[i]; i++) err[i] = s[i];
	if (n > 0) err[i] = 0;
}

// -- the layout --

typedef struct {
	zp_writer_t		w;
	zpage_t			pg;
	zc_style_t		st, foot;
	zc_paint_t		ink;
	zc_target_t		*t;
	zc_xf_t			xf;
	zc_fix			left, right, top, bottom, foot_y;
	zc_fix			asc, line;
	zc_fix			y;			// the next line's baseline
	int				page;		// pages begun
} lay_t;

static zc_fix mm10(const lay_t *L, int v, int along_mm10, int along_units) {
	(void)L;
	return (zc_fix)(((int64_t)v * along_units * ZC_ONE) / along_mm10);
}

static void page_end(lay_t *L) {
	char num[12];
	int k = 0, v = L->page;
	char rev[12];
	int r = 0;
	if (!L->t) return;
	do { rev[r++] = (char)('0' + v % 10); v /= 10; } while (v && r < 10);
	while (r) num[k++] = rev[--r];
	num[k] = 0;
	{
		zc_fix w = zc_text_width(&L->foot, num, (uint32_t)k);
		zc_text_draw(L->t, &L->xf, &L->foot, (L->left + L->right) / 2 - w / 2,
			L->foot_y, num, (uint32_t)k, &L->ink);
	}
	zp_page_end(&L->w);
	L->t = NULL;
}

static void page_begin(lay_t *L) {
	page_end(L);
	L->t = zp_page_begin(&L->w, &L->pg, &L->xf);
	L->page++;
	L->y = L->top + L->asc;
}

// One display line of text at indent x, on this page or the next.
static void put_line(lay_t *L, zc_fix x, const char *s, uint32_t len) {
	if (!L->t || L->y > L->bottom) page_begin(L);
	if (len) zc_text_draw(L->t, &L->xf, &L->st, L->left + x, L->y, s, len, &L->ink);
	L->y += L->line;
}

// A paragraph: one line of the file, tabs already expanded.
static void put_para(lay_t *L, const char *s, uint32_t len) {

	static zc_line_t lines[WRAP_LINES];
	uint32_t lead = 0;
	zc_fix indent, width;

	while (lead < len && s[lead] == ' ') lead++;
	if (lead == len) { put_line(L, 0, s, 0); return; }

	indent = (zc_fix)lead * zc_text_advance(&L->st, ' ');
	width = L->right - L->left - indent;
	// an indent that leaves less than a quarter of the line: kept to that
	if (width < (L->right - L->left) / 4) {
		width = (L->right - L->left) / 4;
		indent = L->right - L->left - width;
	}

	s += lead;
	len -= lead;

	while (len) {
		bool more = false;
		int n = zc_text_wrap(&L->st, s, len, width, lines, WRAP_LINES, &more);
		uint32_t used = 0;
		int i;
		if (n <= 0) break;
		for (i = 0; i < n; i++) {
			put_line(L, indent, s + lines[i].start, lines[i].len);
			used = lines[i].start + lines[i].len;
		}
		if (!more) break;
		while (used < len && s[used] == ' ') used++;
		s += used;
		len -= used;
	}

}

bool textdoc_build(const uint8_t *job, uint32_t len, const char *text, uint32_t tlen,
	const char *default_paper, uint8_t **out, uint32_t *olen, char *err, int errn) {

	static lay_t L;
	mbuf_t m;
	char v[32], paper[16];
	const zc_font_t *font = NULL;
	int wmm, hmm, pt;
	char *line;
	uint32_t cap, i;

	memset(&L, 0, sizeof(L));

	// -- the page --
	paper[0] = 0;
	if (zp_job_get(job, len, "paper", v, sizeof(v)) && v[0] && strcmp(v, "page"))
		say(paper, sizeof(paper), v);
	else if (default_paper && default_paper[0])
		say(paper, sizeof(paper), default_paper);
	if (!paper[0] || !zpage_set(&L.pg, paper, false) || !zpage_physical(&L.pg, &wmm, &hmm)) {
		zpage_set(&L.pg, "letter", false);
		zpage_physical(&L.pg, &wmm, &hmm);
	}

	// -- the face --
	if (!zp_job_get(job, len, "text-font", v, sizeof(v)) || !v[0]) say(v, sizeof(v), "sans");
	if (!res_font(NULL, v, &font) && !res_font(NULL, "sans", &font)) font = res_fallback();
	if (!font) { say(err, errn, "No font to print the text with"); return false; }
	pt = zp_job_get(job, len, "text-size", v, sizeof(v)) ? to_int(v, 10) : 10;
	if (pt < 6) pt = 6;
	if (pt > 24) pt = 24;

	// page units per tenth of a millimetre: the grid is the paper's
	// shape, gw across wmm and gh down hmm
	L.left = mm10(&L, MARGIN_MM10, wmm, L.pg.gw);
	L.right = (zc_fix)L.pg.gw * ZC_ONE - L.left;
	L.top = mm10(&L, MARGIN_MM10, hmm, L.pg.gh);
	L.foot_y = (zc_fix)L.pg.gh * ZC_ONE - mm10(&L, FOOT_MM10, hmm, L.pg.gh);

	memset(&L.st, 0, sizeof(L.st));
	L.st.font = font;
	L.st.size = mm10(&L, pt * 3528 / 1000, hmm, L.pg.gh);	// 1 pt = 0.3528 mm
	L.foot = L.st;
	L.foot.size = L.st.size * 9 / 10;
	zc_text_metrics(&L.st, &L.asc, NULL, &L.line);
	L.bottom = (zc_fix)L.pg.gh * ZC_ONE - mm10(&L, MARGIN_MM10, hmm, L.pg.gh);
	L.ink = zc_black();

	// -- the output: the job's own records, then the pages --
	m.cap = len + 64 * 1024;
	m.b = pool_alloc(m.cap);
	if (!m.b) { say(err, errn, "Not enough memory for the text"); return false; }
	memcpy(m.b, job, len);
	m.n = len;
	m.skip = 8;			// "ZPP1" and the version: the job has them
	m.failed = false;
	zp_writer_init(&L.w, mput, &m);

	// -- the text, a line at a time, tabs expanded --
	cap = 1024;
	line = pool_alloc(cap);
	if (!line) { pool_free(m.b); say(err, errn, "Not enough memory for the text"); return false; }

	i = 0;
	while (i < tlen && !m.failed) {
		uint32_t e = i, n = 0, col = 0;
		while (e < tlen && text[e] != '\n' && text[e] != '\f') e++;
		for (; i < e; i++) {
			char c = text[i];
			uint32_t need = n + TAB + 1;
			if (need > cap) {
				char *nl = pool_realloc(line, cap * 2 > need ? cap * 2 : need);
				if (!nl) { m.failed = true; break; }
				line = nl;
				cap = cap * 2 > need ? cap * 2 : need;
			}
			if (c == '\r') continue;
			if (c == '\t') {
				do { line[n++] = ' '; col++; } while (col % TAB);
				continue;
			}
			line[n++] = c;
			// a column per character, not per byte: UTF-8 continuation
			// bytes do not move it
			if (((uint8_t)c & 0xC0) != 0x80) col++;
		}
		if (m.failed) break;
		put_para(&L, line, n);
		if (i < tlen && text[i] == '\f') {
			// the next line starts a page, unless this one already did
			if (L.t && L.y > L.top + L.asc) L.y = L.bottom + 1;
		}
		i++;		// past the newline or form feed
	}

	if (!L.t) page_begin(&L);		// an empty file: one blank page
	page_end(&L);
	pool_free(line);

	if (m.failed || L.w.failed) {
		pool_free(m.b);
		say(err, errn, "Not enough memory to lay out the text");
		return false;
	}

	*out = m.b;
	*olen = m.n;
	return true;

}
