/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * Reading RTF -- what WordPad, TextEdit and Word save, and what this
 * system's own writer (zf_docw.c) produces.
 *
 * A tokenizer over groups, control words and text. Kept: paragraphs
 * (\par), line breaks (\line), tabs, bold and italic, headings (by
 * \outlinelevelN, or a stylesheet entry named "heading N"), list items
 * (\ls / \ilvl, or a paragraph that opens with \bullet), and
 * preformatted text (a paragraph in a font of the modern -- fixed
 * pitch -- family). Characters: the ANSI code page (Windows-1252 is
 * assumed: it is what \ansicpg1252 says and what Mac RTF's \mac
 * mostly agrees with for text), \'hh, and \uN with its \ucN
 * fallback skipped. Left out: every destination that is not body text
 * -- font and colour tables, the stylesheet once read, info, pictures,
 * objects, headers, footers, footnotes, field instructions (the field's
 * result is kept) and anything marked {\* ...} that is not understood.
 * Table cells come through as tab-separated paragraphs, one per row.
 */

#include <string.h>

#include "zf.h"
#include "zf_em.h"

#define DEPTH_MAX	64

typedef struct {
	uint8_t	bold, ital, skip, uc, font;
	uint8_t	in_style;		// inside a stylesheet entry
	uint8_t	in_fonts;
} grp_t;

typedef struct {
	zf_rd_t		rd;
	zf_em_t		em;
	grp_t		g[DEPTH_MAX];
	int			sp;				// g[sp] is the current group
	int			deep;			// groups past DEPTH_MAX, all skipped
	int			skip_chars;		// \uN fallback still to drop
	uint64_t	modern_fonts;	// bit per font number < 64
	int			cur_font;		// in the font table
	// the paragraph being built
	int			pkind, plevel;
	bool		pstarted;		// text has been written in it
	int			li;				// \li, twips
	int			ilvl;
	bool		ls;
	int			style_no;
	// stylesheet: style number -> heading level (0 none)
	uint8_t		style_head[256];
	int			sheet_style;	// the entry being read
	char		sname[48];
	int			snlen;
	uint16_t	hi_surrogate;
	bool		bullet_tab;		// the \tab after a hand-made bullet is not text
	uint32_t	ticks;
} rtf_t;

static rtf_t R;

static grp_t *G(void) { return &R.g[R.sp]; }

static int style_of(void) {
	return (G()->bold ? ZF_S_BOLD : 0) | (G()->ital ? ZF_S_ITAL : 0);
}

// Text reached: open the paragraph with what is known of it.
static void para_start(void) {
	if (R.pstarted) return;
	R.pstarted = true;
	int kind = R.pkind, level = R.plevel;
	if (kind == ZF_P_BODY && R.ls) { kind = ZF_P_ITEM; level = R.ilvl + 1; }
	if (kind == ZF_P_BODY && R.style_no >= 0 && R.style_no < 256 && R.style_head[R.style_no]) {
		kind = ZF_P_HEAD;
		level = R.style_head[R.style_no];
	}
	if (kind == ZF_P_BODY && G()->font < 64 && (R.modern_fonts >> G()->font) & 1) kind = ZF_P_PRE;
	if (level < 1) level = 1;
	zf_em_para(&R.em, kind, level, true);
}

static void out_text(const char *s, uint32_t n) {
	if (G()->skip) return;
	if (R.skip_chars > 0) { R.skip_chars--; return; }	// n is always one char here
	if (R.bullet_tab) {
		R.bullet_tab = false;
		if (n == 1 && (*s == '\t' || *s == ' ')) return;
	}
	para_start();
	zf_em_text(&R.em, s, n, style_of());
}

static void out_cp(uint32_t cp) {
	char b[4];
	int k = zf_utf8_put(cp, b);
	out_text(b, (uint32_t)k);
}

static void par_end(void) {
	if (G()->skip) return;
	if (!R.pstarted) para_start();
	zf_em_end_para(&R.em);
	R.pstarted = false;
}

static void pard(void) {
	R.bullet_tab = false;
	R.pkind = ZF_P_BODY;
	R.plevel = 1;
	R.li = 0;
	R.ilvl = 0;
	R.ls = false;
	R.style_no = -1;
}

// Destinations whose content is not body text.
static bool skip_dest(const char *w) {
	static const char *const d[] = {
		"fonttbl", "colortbl", "stylesheet", "info", "pict", "object", "header", "footer",
		"headerl", "headerr", "headerf", "footerl", "footerr", "footerf", "footnote",
		"fldinst", "listtable", "listoverridetable", "rsidtbl", "generator", "themedata",
		"colorschememapping", "datastore", "latentstyles", "xmlnstbl", "pntext", "listtext",
		"pntxta", "pntxtb", "revtbl", "annotation", "atnid", "atnauthor", "bkmkstart", "bkmkend",
		"nonshppict", "shp", "shpinst", "sp", "mmathPr", "wgrffmtfilter", "pgdsctbl", "filetbl",
		"title", "author", "operator", "company", "keywords", "comment", "doccomm", "userprops",
		"xe", "tc", "txe", NULL };
	for (int i = 0; d[i]; i++) if (!strcmp(w, d[i])) return true;
	return false;
}

static void word(const char *w, bool has, int32_t v, bool star) {
	grp_t *g = G();

	// tables read before the body is reached
	if (!strcmp(w, "fonttbl")) { g->in_fonts = 1; g->skip = 1; return; }
	if (g->in_fonts) {
		if (!strcmp(w, "f") && has) R.cur_font = v;
		else if (!strcmp(w, "fmodern") && R.cur_font >= 0 && R.cur_font < 64) R.modern_fonts |= 1ull << R.cur_font;
		return;
	}
	if (!strcmp(w, "stylesheet")) { g->skip = 1; g->in_style = 1; return; }
	if (g->in_style) {
		if (!strcmp(w, "s") && has) { R.sheet_style = v; R.snlen = 0; }
		return;
	}

	if (g->skip) return;
	if (star || skip_dest(w)) { g->skip = 1; return; }

	// a control word stands for one character of a \u fallback
	if (R.skip_chars > 0 && strcmp(w, "u")) { R.skip_chars--; return; }

	switch (w[0]) {
	case 'b':
		if (!strcmp(w, "b")) { g->bold = !has || v != 0; return; }
		if (!strcmp(w, "bullet")) {
			if (!R.pstarted && R.pkind == ZF_P_BODY) {
				// a hand-made list item: the bullet and the tab after it
				R.pkind = ZF_P_ITEM;
				R.plevel = R.li > 360 ? R.li / 360 : 1;
				para_start();
				R.skip_chars = 0;
				R.bullet_tab = true;
				return;
			}
			out_cp(0x2022);
			return;
		}
		break;
	case 'c':
		if (!strcmp(w, "cell")) { out_text("\t", 1); return; }
		break;
	case 'e':
		if (!strcmp(w, "emdash")) { out_cp(0x2014); return; }
		if (!strcmp(w, "endash")) { out_cp(0x2013); return; }
		if (!strcmp(w, "emspace") || !strcmp(w, "enspace")) { out_text(" ", 1); return; }
		break;
	case 'i':
		if (!strcmp(w, "i")) { g->ital = !has || v != 0; return; }
		if (!strcmp(w, "ilvl")) { R.ilvl = v < 0 ? 0 : v > 8 ? 8 : v; return; }
		break;
	case 'f':
		if (!strcmp(w, "f") && has) { g->font = (uint8_t)(v < 0 || v > 255 ? 255 : v); return; }
		break;
	case 'l':
		if (!strcmp(w, "line")) { if (!g->skip) { para_start(); zf_em_brk(&R.em); } return; }
		if (!strcmp(w, "ls")) { R.ls = true; return; }
		if (!strcmp(w, "li")) { R.li = v; return; }
		if (!strcmp(w, "lquote")) { out_cp(0x2018); return; }
		if (!strcmp(w, "ldblquote")) { out_cp(0x201C); return; }
		break;
	case 'o':
		if (!strcmp(w, "outlinelevel") && v >= 0 && v < 9) { R.pkind = ZF_P_HEAD; R.plevel = v + 1; return; }
		break;
	case 'p':
		if (!strcmp(w, "par")) { par_end(); return; }
		if (!strcmp(w, "pard")) { pard(); return; }
		if (!strcmp(w, "plain")) { g->bold = g->ital = 0; return; }
		if (!strcmp(w, "page")) return;
		break;
	case 'r':
		if (!strcmp(w, "row")) { par_end(); return; }
		if (!strcmp(w, "rquote")) { out_cp(0x2019); return; }
		if (!strcmp(w, "rdblquote")) { out_cp(0x201D); return; }
		break;
	case 's':
		if (!strcmp(w, "s") && has) { R.style_no = v; return; }
		if (!strcmp(w, "sect")) { par_end(); return; }
		break;
	case 't':
		if (!strcmp(w, "tab")) { if (R.bullet_tab) R.bullet_tab = false; else out_text("\t", 1); return; }
		break;
	case 'u':
		if (!strcmp(w, "uc")) { g->uc = (uint8_t)(v < 0 ? 0 : v > 8 ? 8 : v); return; }
		if (!strcmp(w, "u") && has) {
			uint32_t cp = (uint32_t)(v < 0 ? v + 65536 : v) & 0xFFFF;
			int skip = g->uc;
			if (cp >= 0xD800 && cp < 0xDC00) { R.hi_surrogate = (uint16_t)cp; R.skip_chars = 0; }
			else {
				if (cp >= 0xDC00 && cp < 0xE000) {
					cp = R.hi_surrogate ? 0x10000 + ((uint32_t)(R.hi_surrogate - 0xD800) << 10) + (cp - 0xDC00) : 0xFFFD;
					R.hi_surrogate = 0;
				}
				R.skip_chars = 0;
				out_cp(cp);
			}
			R.skip_chars = skip;
			return;
		}
		break;
	}
}

int zf_read_rtf(zf_io_t *in, zf_doc_t *d) {
	static zf_src_t src;
	memset(&R, 0, sizeof R);
	zf_src_io(&src, in);
	zf_rd_init(&R.rd, &src);
	zf_em_init(&R.em, d, false);
	R.g[0].uc = 1;
	R.cur_font = -1;
	R.sheet_style = -1;
	pard();

	// "{\rtf"
	char head[6];
	for (int i = 0; i < 5; i++) { int c = zf_rd_getc(&R.rd); if (c < 0) return ZF_E_FORMAT; head[i] = (char)c; }
	head[5] = 0;
	if (strcmp(head, "{\\rtf")) return ZF_E_FORMAT;
	while (zf_rd_peek(&R.rd) >= '0' && zf_rd_peek(&R.rd) <= '9') zf_rd_getc(&R.rd);	// \rtf1
	if (zf_rd_peek(&R.rd) == ' ') zf_rd_getc(&R.rd);
	R.sp = 1;
	R.g[1] = R.g[0];

	for (;;) {
		if (!(++R.ticks & 1023) && zf_cancel_poll && zf_cancel_poll()) return ZF_E_CANCEL;
		if (R.em.err) return R.em.err;
		int c = zf_rd_getc(&R.rd);
		if (c < 0) break;
		if (c == '{') {
			if (R.sp + 1 >= DEPTH_MAX) { R.deep++; G()->skip = 1; continue; }
			R.g[R.sp + 1] = R.g[R.sp];
			R.sp++;
			// "{\*\" marks an ignorable destination
			if (zf_rd_peek(&R.rd) == '\\') {
				zf_rd_getc(&R.rd);
				if (zf_rd_peek(&R.rd) == '*') {
					zf_rd_getc(&R.rd);
					// fldrslt-like destinations are not starred; anything starred
					// is skipped, except the list text Word marks this way, which
					// is skipped anyway
					G()->skip = 1;
					continue;
				}
				c = '\\';
				goto backslash;
			}
			continue;
		}
		if (c == '}') {
			if (R.deep) { R.deep--; continue; }
			// leaving a stylesheet entry: its name may say "heading N"
			if (G()->in_style && R.sp > 1 && R.g[R.sp - 1].in_style) {
				R.sname[R.snlen] = 0;
				if (R.sheet_style >= 0 && zf_ieq_n(R.sname, "heading", 7)) {
					const char *p = R.sname + 7;
					while (*p == ' ') p++;
					if (*p >= '1' && *p <= '9' && R.sheet_style < 256) R.style_head[R.sheet_style] = (uint8_t)(*p - '0');
				}
				R.sheet_style = -1;
				R.snlen = 0;
			}
			if (G()->in_fonts) R.cur_font = -1;
			if (R.sp > 1) R.sp--;
			else break;			// the document's closing brace
			continue;
		}
		if (c == '\\') {
backslash:
			c = zf_rd_getc(&R.rd);
			if (c < 0) break;
			if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')) {
				char w[33];
				int n = 0;
				w[n++] = (char)c;
				while (n < 32) {
					int p = zf_rd_peek(&R.rd);
					if (!((p >= 'a' && p <= 'z') || (p >= 'A' && p <= 'Z'))) break;
					w[n++] = (char)zf_rd_getc(&R.rd);
				}
				w[n] = 0;
				bool has = false, neg = false;
				int32_t v = 0;
				if (zf_rd_peek(&R.rd) == '-') { zf_rd_getc(&R.rd); neg = true; }
				while (zf_rd_peek(&R.rd) >= '0' && zf_rd_peek(&R.rd) <= '9') {
					int dgt = zf_rd_getc(&R.rd) - '0';
					if (v < 100000000) v = v * 10 + dgt;
					has = true;
				}
				if (neg) v = -v;
				if (zf_rd_peek(&R.rd) == ' ') zf_rd_getc(&R.rd);
				if (!strcmp(w, "bin") && has) {
					for (int32_t i = 0; i < v && zf_rd_getc(&R.rd) >= 0; i++) ;
					continue;
				}
				word(w, has, v, false);
				continue;
			}
			if (c == '\'') {
				int h1 = zf_rd_getc(&R.rd), h2 = zf_rd_getc(&R.rd);
				int v = 0;
				for (int k = 0; k < 2; k++) {
					int h = k ? h2 : h1;
					v <<= 4;
					if (h >= '0' && h <= '9') v |= h - '0';
					else if (h >= 'a' && h <= 'f') v |= h - 'a' + 10;
					else if (h >= 'A' && h <= 'F') v |= h - 'A' + 10;
				}
				if (G()->in_style) { if (R.snlen < 47) R.sname[R.snlen++] = (char)v; continue; }
				if (G()->skip) continue;
				if (R.skip_chars > 0) { R.skip_chars--; continue; }
				out_cp(zf_cp1252((uint8_t)v));
				continue;
			}
			if (G()->skip) continue;
			switch (c) {
			case '\\': case '{': case '}': { char ch = (char)c; out_text(&ch, 1); break; }
			case '~': out_cp(0xA0); break;
			case '_': out_cp(0x2011); break;
			case '-': break;				// optional hyphen
			case '\n': case '\r': par_end(); break;	// \<newline> is \par
			case '*': G()->skip = 1; break;
			default: break;
			}
			continue;
		}
		if (c == '\r' || c == '\n') continue;
		if (G()->in_style) {
			if (c != ';' && R.snlen < 47) R.sname[R.snlen++] = (char)c;
			continue;
		}
		if (G()->skip) continue;
		if (c >= 0x80) { out_cp(zf_cp1252((uint8_t)c)); continue; }
		char ch = (char)c;
		out_text(&ch, 1);
	}
	if (R.pstarted) zf_em_end_para(&R.em);
	if (R.rd.err) return R.rd.err;
	return zf_em_end(&R.em);
}
