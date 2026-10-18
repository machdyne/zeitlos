/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * Reading plain text and Markdown.
 *
 * Text: a paragraph per line, so a document written here as text and
 * converted to docx keeps its lines -- blank ones too. The encoding is
 * found from a byte-order mark (UTF-8, UTF-16 LE or BE), else read as
 * UTF-8 with any byte that is not part of a valid character taken as
 * Windows-1252: a file from an old Windows machine reads correctly,
 * and a UTF-8 file is never misread.
 *
 * Markdown: the CommonMark most people write -- ATX (#) and setext
 * (=== / ---) headings, -, * and + lists and numbered ones, nesting by
 * indentation, ``` and ~~~ fences and four-space code blocks, > quotes
 * (as plain paragraphs), **bold**, __bold__, *italic*, _italic_,
 * `code` (as plain text), links and images (as their text),
 * backslash escapes, two trailing spaces as a line break. Lines of a
 * paragraph are joined with a space. Tables, footnotes and HTML blocks
 * come through as text.
 */

#include <string.h>

#include "zf.h"
#include "zf_em.h"

// -- lines, in UTF-8, whatever the file was --

#define LINE_MAX_	4096

typedef struct {
	zf_rd_t		rd;
	int			enc;		// 0 UTF-8/1252, 1 UTF-16LE, 2 UTF-16BE
	char		line[LINE_MAX_ + 16];
	uint32_t	len;
	bool		cont;		// the line was longer than the buffer: more follows
} lines_t;

static lines_t L;

static void lines_init(zf_io_t *in) {
	static zf_src_t src;
	memset(&L, 0, sizeof L);
	zf_src_io(&src, in);
	zf_rd_init(&L.rd, &src);
	int a = zf_rd_peek(&L.rd);
	if (a == 0xEF) {
		zf_rd_getc(&L.rd);
		if (zf_rd_peek(&L.rd) == 0xBB) { zf_rd_getc(&L.rd); if (zf_rd_peek(&L.rd) == 0xBF) zf_rd_getc(&L.rd); }
	} else if (a == 0xFF || a == 0xFE) {
		zf_rd_getc(&L.rd);
		int b = zf_rd_peek(&L.rd);
		if ((a == 0xFF && b == 0xFE) || (a == 0xFE && b == 0xFF)) { zf_rd_getc(&L.rd); L.enc = a == 0xFF ? 1 : 2; }
		else { L.line[0] = 0; /* a lone byte: kept as 1252 by the line reader below */ }
	}
}

static int u16(void) {
	int a = zf_rd_getc(&L.rd), b = zf_rd_getc(&L.rd);
	if (a < 0 || b < 0) return -1;
	return L.enc == 1 ? (a | (b << 8)) : ((a << 8) | b);
}

// The next line (without its end) in L.line, NUL-terminated; false at
// the end of the file. A line longer than the buffer comes in pieces,
// L.cont set on all but the last.
static bool next_line(void) {
	L.len = 0;
	L.cont = false;
	bool any = false;
	for (;;) {
		if (L.len >= LINE_MAX_) { L.cont = true; break; }
		uint32_t cp;
		if (L.enc) {
			int c = u16();
			if (c < 0) break;
			any = true;
			cp = (uint32_t)c;
			if (cp >= 0xD800 && cp < 0xDC00) {
				int c2 = u16();
				cp = (c2 >= 0xDC00 && c2 < 0xE000) ? 0x10000 + ((cp - 0xD800) << 10) + ((uint32_t)c2 - 0xDC00) : 0xFFFD;
			} else if (cp >= 0xDC00 && cp < 0xE000) cp = 0xFFFD;
		} else {
			int c = zf_rd_getc(&L.rd);
			if (c < 0) break;
			any = true;
			if (c < 0x80) cp = (uint32_t)c;
			else {
				int need = (c >= 0xC2 && c <= 0xDF) ? 1 : (c >= 0xE0 && c <= 0xEF) ? 2 : (c >= 0xF0 && c <= 0xF4) ? 3 : 0;
				uint8_t seq[4] = { (uint8_t)c };
				int k = 0;
				while (k < need && (zf_rd_peek(&L.rd) & 0xC0) == 0x80 && zf_rd_peek(&L.rd) >= 0) seq[++k] = (uint8_t)zf_rd_getc(&L.rd);
				if (need && k == need) {
					memcpy(L.line + L.len, seq, (size_t)k + 1);
					L.len += (uint32_t)k + 1;
					continue;
				}
				// not UTF-8: each byte as Windows-1252
				for (int i = 0; i <= k; i++) L.len += (uint32_t)zf_utf8_put(zf_cp1252(seq[i]), L.line + L.len);
				continue;
			}
		}
		if (cp == '\n') break;
		if (cp == '\r') {
			// CR LF, or a lone CR (old Macs); in UTF-16 the LF after a
			// CR is the line end and the CR is dropped
			if (L.enc) continue;
			if (zf_rd_peek(&L.rd) == '\n') zf_rd_getc(&L.rd);
			break;
		}
		if (cp == 0) continue;
		L.len += (uint32_t)zf_utf8_put(cp, L.line + L.len);
	}
	L.line[L.len] = 0;
	return any;
}

int zf_read_txt(zf_io_t *in, zf_doc_t *d) {
	static zf_em_t em;
	lines_init(in);
	zf_em_init(&em, d, false);
	bool pending_cont = false;
	uint32_t n = 0;
	while (next_line()) {
		if (!(++n & 63) && zf_cancel_poll && zf_cancel_poll()) return ZF_E_CANCEL;
		if (!pending_cont) zf_em_para(&em, ZF_P_BODY, 1, true);
		zf_em_text(&em, L.line, L.len, 0);
		pending_cont = L.cont;
		if (!L.cont) zf_em_end_para(&em);
		if (em.err) return em.err;
	}
	if (L.rd.err) return L.rd.err;
	return zf_em_end(&em);
}

// -- Markdown --

typedef struct {
	zf_em_t		em;
	bool		in_para;		// a paragraph's lines are being joined
	bool		fence;
	char		fch;			// '`' or '~'
	int			fn;				// its length
	bool		icode;			// indented code block
	bool		brk_owed;		// the last line ended in two spaces
	int			list_indent[10];
	int			nlists;
	// the paragraph so far, held for a setext underline
	char		para[LINE_MAX_ + 8];
	uint32_t	plen;
	int			pkind, plevel;
} md_t;

static md_t M;

static int ilen(const char *s) { int i = 0; while (s[i] == ' ' || s[i] == '\t') i += 1; return i; }
static int indent_of(const char *s) {
	int w = 0;
	for (; *s == ' ' || *s == '\t'; s++) w += (*s == '\t') ? 4 - (w & 3) : 1;
	return w;
}

static bool punct(char c) {
	return c && strchr("\\`*_{}[]()#+-.!|~<>\"'", c) != NULL;
}

// Inline markup in `s`, emitted as runs.
static void inline_md(const char *s, uint32_t n) {
	zf_em_t *e = &M.em;
	int st = 0;
	uint32_t a = 0, i = 0;
#define FLUSH() do { if (i > a) zf_em_text(e, s + a, i - a, st); } while (0)
	while (i < n) {
		char c = s[i];
		if (c == '\\' && i + 1 < n && punct(s[i + 1])) {
			FLUSH();
			zf_em_text(e, s + i + 1, 1, st);
			i += 2;
			a = i;
			continue;
		}
		if (c == '`') {
			// a code span: up to the same number of backticks
			uint32_t k = i;
			while (k < n && s[k] == '`') k++;
			uint32_t ticks = k - i, j = k;
			bool found = false;
			while (j < n) {
				if (s[j] == '`') {
					uint32_t m = j;
					while (m < n && s[m] == '`') m++;
					if (m - j == ticks) { found = true; break; }
					j = m;
				} else j++;
			}
			if (found) {
				FLUSH();
				uint32_t b0 = k, b1 = j;
				if (b1 > b0 + 1 && s[b0] == ' ' && s[b1 - 1] == ' ') { b0++; b1--; }
				zf_em_text(e, s + b0, b1 - b0, st);
				i = j + ticks;
				a = i;
				continue;
			}
			i = k;
			continue;
		}
		if (c == '*' || c == '_') {
			uint32_t k = i;
			while (k < n && s[k] == c) k++;
			uint32_t run = k - i;
			bool prev_ws = i == 0 || s[i - 1] == ' ' || s[i - 1] == '\t';
			bool next_ws = k >= n || s[k] == ' ' || s[k] == '\t';
			bool prev_al = i > 0 && ((s[i - 1] | 32) >= 'a' && (s[i - 1] | 32) <= 'z');
			bool next_al = k < n && ((s[k] | 32) >= 'a' && (s[k] | 32) <= 'z');
			bool can_open = !next_ws && !(c == '_' && prev_al);
			bool can_close = !prev_ws && !(c == '_' && next_al);
			if (run > 3 || (!can_open && !can_close)) { i = k; continue; }
			int bit = run == 1 ? ZF_S_ITAL : run == 2 ? ZF_S_BOLD : (ZF_S_BOLD | ZF_S_ITAL);
			bool closing = can_close && (st & bit) == bit;
			if (!closing && !can_open) { i = k; continue; }
			if (!closing) {
				// only open if a closer exists later on the line
				bool later = false;
				for (uint32_t j = k + 1; j + run <= n; j++) {
					uint32_t m = 0;
					while (m < run && s[j + m] == c) m++;
					if (m == run && s[j - 1] != ' ' && (j + m >= n || s[j + m] != c)) { later = true; break; }
				}
				if (!later) { i = k; continue; }
			}
			FLUSH();
			if (closing) st &= ~bit; else st |= bit;
			i = k;
			a = i;
			continue;
		}
		if ((c == '!' && i + 1 < n && s[i + 1] == '[') || c == '[') {
			// [text](url) or ![alt](url): the text
			uint32_t o = i + (c == '!' ? 2 : 1), j = o;
			int depth = 1;
			while (j < n && depth) { if (s[j] == '[') depth++; else if (s[j] == ']') depth--; if (depth) j++; }
			if (j < n && j + 1 < n && s[j + 1] == '(') {
				uint32_t m = j + 2;
				while (m < n && s[m] != ')') m++;
				if (m < n) {
					FLUSH();
					inline_md(s + o, j - o);
					i = m + 1;
					a = i;
					continue;
				}
			}
			i++;
			continue;
		}
		if (c == '<') {
			// <http://...> autolinks: the address
			uint32_t j = i + 1;
			while (j < n && s[j] != '>' && s[j] != ' ') j++;
			if (j < n && s[j] == '>' && j > i + 1 && memchr(s + i + 1, ':', j - i - 1)) {
				FLUSH();
				zf_em_text(e, s + i + 1, j - i - 1, st);
				i = j + 1;
				a = i;
				continue;
			}
		}
		i++;
	}
	FLUSH();
#undef FLUSH
}

static void md_flush(void) {
	if (!M.in_para) return;
	zf_em_para(&M.em, M.pkind, M.plevel, true);
	// the joined lines, with breaks where they were owed (marked \n)
	uint32_t a = 0;
	for (uint32_t i = 0; i <= M.plen; i++) {
		if (i < M.plen && M.para[i] != '\n') continue;
		inline_md(M.para + a, i - a);
		if (i < M.plen) zf_em_brk(&M.em);
		a = i + 1;
	}
	zf_em_end_para(&M.em);
	M.in_para = false;
	M.plen = 0;
}

static void para_add(const char *s, uint32_t n, int kind, int level) {
	if (!M.in_para) {
		M.in_para = true;
		M.plen = 0;
		M.pkind = kind;
		M.plevel = level;
	} else if (M.plen < LINE_MAX_) {
		M.para[M.plen++] = M.brk_owed ? '\n' : ' ';
	}
	M.brk_owed = false;
	// two trailing spaces, or a backslash: a hard break after this line
	if (n >= 2 && s[n - 1] == ' ' && s[n - 2] == ' ') M.brk_owed = true;
	else if (n >= 1 && s[n - 1] == '\\') { M.brk_owed = true; n--; }
	while (n && (s[n - 1] == ' ' || s[n - 1] == '\t')) n--;
	if (M.plen + n > LINE_MAX_) {
		// a paragraph longer than the buffer: write what is held first
		md_flush();
		M.in_para = true;
		M.pkind = kind == ZF_P_ITEM ? ZF_P_BODY : kind;
		M.plevel = level;
		M.plen = 0;
		if (n > LINE_MAX_) n = LINE_MAX_;
	}
	memcpy(M.para + M.plen, s, n);
	M.plen += n;
}

static void pre_line(const char *s, uint32_t n) {
	zf_em_t *e = &M.em;
	if (!zf_em_in_para(e) || e->kind != ZF_P_PRE) zf_em_para(e, ZF_P_PRE, 1, true);
	else zf_em_brk(e);
	zf_em_raw(e, s, n, 0);
}

static void pre_end(void) {
	if (zf_em_in_para(&M.em) && M.em.kind == ZF_P_PRE) zf_em_end_para(&M.em);
}


// "- x", "* x", "+ x", "1. x", "1) x": the marker's length, or 0
static int list_marker(const char *s) {
	if ((s[0] == '-' || s[0] == '*' || s[0] == '+') && (s[1] == ' ' || s[1] == '\t' || !s[1])) return 1;
	int i = 0;
	while (s[i] >= '0' && s[i] <= '9' && i < 9) i++;
	if (i && (s[i] == '.' || s[i] == ')') && (s[i + 1] == ' ' || s[i + 1] == '\t' || !s[i + 1])) return i + 1;
	return 0;
}

static bool rule(const char *s, char ch) {
	int k = 0;
	for (; *s; s++) {
		if (*s == ch) k++;
		else if (*s != ' ' && *s != '\t') return false;
	}
	return k >= 3;
}

int zf_read_md(zf_io_t *in, zf_doc_t *d) {
	lines_init(in);
	memset(&M, 0, sizeof M);
	zf_em_init(&M.em, d, false);
	uint32_t n = 0;
	while (next_line()) {
		if (!(++n & 63) && zf_cancel_poll && zf_cancel_poll()) return ZF_E_CANCEL;
		if (M.em.err) return M.em.err;
		char *s = L.line;
		int ind = indent_of(s);
		char *t = s + ilen(s);

		if (M.fence) {
			int k = 0;
			while (t[k] == M.fch) k++;
			if (k >= M.fn && !t[k + ilen(t + k)]) { M.fence = false; pre_end(); continue; }
			pre_line(s, L.len);
			continue;
		}
		if ((t[0] == '`' && t[1] == '`' && t[2] == '`') || (t[0] == '~' && t[1] == '~' && t[2] == '~')) {
			md_flush();
			M.fence = true;
			M.fch = t[0];
			M.fn = 0;
			while (t[M.fn] == M.fch) M.fn++;
			pre_end();
			continue;
		}
		if (!*t) {
			md_flush();
			if (M.icode) { M.icode = false; pre_end(); }
			continue;
		}
		if (ind >= 4 && !M.in_para && !M.nlists) {
			// indented code
			M.icode = true;
			const char *c = s;
			int w = 0;
			while (w < 4 && (*c == ' ' || *c == '\t')) { w += (*c == '\t') ? 4 - (w & 3) : 1; c++; }
			pre_line(c, (uint32_t)strlen(c));
			continue;
		}
		if (M.icode) { M.icode = false; pre_end(); }

		// setext underline
		if (M.in_para && M.pkind == ZF_P_BODY && (rule(t, '=') || (rule(t, '-') && !strchr(t, ' ')))) {
			M.pkind = ZF_P_HEAD;
			M.plevel = t[0] == '=' ? 1 : 2;
			md_flush();
			continue;
		}
		if (rule(t, '-') || rule(t, '*') || rule(t, '_')) { md_flush(); M.nlists = 0; continue; }

		if (t[0] == '#') {
			int k = 0;
			while (t[k] == '#') k++;
			if (k <= 6 && (t[k] == ' ' || t[k] == '\t' || !t[k])) {
				md_flush();
				M.nlists = 0;
				char *h = t + k + ilen(t + k);
				uint32_t hl = (uint32_t)strlen(h);
				// closing #s
				while (hl && (h[hl - 1] == ' ')) hl--;
				uint32_t z = hl;
				while (z && h[z - 1] == '#') z--;
				if (z < hl && (z == 0 || h[z - 1] == ' ')) { hl = z; while (hl && h[hl - 1] == ' ') hl--; }
				para_add(h, hl, ZF_P_HEAD, k);
				md_flush();
				continue;
			}
		}
		if (t[0] == '>') {
			char *q = t;
			while (*q == '>') { q++; if (*q == ' ') q++; }
			if (!*q) { md_flush(); continue; }
			if (M.in_para && M.pkind != ZF_P_BODY) md_flush();
			para_add(q, (uint32_t)strlen(q), ZF_P_BODY, 1);
			continue;
		}
		int mk = list_marker(t);
		if (mk) {
			md_flush();
			// nesting by where the marker sits
			while (M.nlists && M.list_indent[M.nlists - 1] > ind) M.nlists--;
			if (!M.nlists || M.list_indent[M.nlists - 1] < ind) {
				if (M.nlists < 9) M.list_indent[M.nlists++] = ind;
			}
			char *b = t + mk;
			b += ilen(b);
			para_add(b, (uint32_t)strlen(b), ZF_P_ITEM, M.nlists ? M.nlists : 1);
			continue;
		}
		// a continuation line of a list item, or a new paragraph
		if (!M.in_para && M.nlists && ind < 2) M.nlists = 0;
		para_add(t, (uint32_t)strlen(t), M.in_para ? M.pkind : ZF_P_BODY, M.in_para ? M.plevel : 1);
	}
	md_flush();
	pre_end();
	if (L.rd.err) return L.rd.err;
	return zf_em_end(&M.em);
}
