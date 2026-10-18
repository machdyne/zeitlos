/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * Reading docx (WordprocessingML) and odt (OpenDocument text).
 *
 * Both are a zip holding XML; both are read for what a plain document
 * keeps -- paragraphs, headings, list items, preformatted text, bold,
 * italic, line breaks, tabs. Tables come through as their cells'
 * paragraphs, in order; images, footnotes, comments, headers and
 * footers, tracked deletions and field codes are left out.
 *
 * docx: word/document.xml (found through _rels/.rels), with
 * word/styles.xml read first so a paragraph styled "Heading 2" -- or
 * any style with an outline level, or based on one -- is a heading.
 * A paragraph with numbering properties is a list item.
 *
 * odt: content.xml, whose automatic styles say which spans are bold or
 * italic; styles.xml's named styles are matched by their names
 * ("Heading_20_2", "Preformatted_20_Text", "Strong_20_Emphasis").
 */

#include <string.h>

#include "zf.h"
#include "zf_zip.h"
#include "zf_xml.h"
#include "zf_em.h"

#define ze zz_ent
#define ent_open zz_ent_open
#define ent_close zz_ent_close

static bool val_true(const zx_t *x) {
	const char *v = zx_attr_local(x, "val");
	return !v || !(strcmp(v, "0") == 0 || strcmp(v, "false") == 0 || strcmp(v, "off") == 0 || strcmp(v, "none") == 0);
}

static int atoi_(const char *s) {
	int v = 0;
	if (!s) return -1;
	while (*s >= '0' && *s <= '9') v = v * 10 + (*s++ - '0');
	return v;
}

static bool contains_ci(const char *s, const char *w) {
	int n = (int)strlen(w);
	for (; *s; s++) if (zf_ieq_n(s, w, n)) return true;
	return false;
}

static bool check_cancel(void) {
	return zf_cancel_poll && zf_cancel_poll();
}

// -- styles, shared by both --

#define NSTYLE		128

typedef struct {
	char		id[32];
	char		base[32];
	int8_t		kind;		// -1 unknown, else ZF_P_*
	int8_t		level;
	uint8_t		rstyle;		// ZF_S_* for character styles
} st_t;

static st_t st[NSTYLE];
static int nst;

static st_t *st_find(const char *id) {
	for (int i = 0; i < nst; i++) if (!strcmp(st[i].id, id)) return &st[i];
	return NULL;
}

static void copy(char *d, const char *s, int cap) {
	int i = 0;
	if (s) for (; s[i] && i < cap - 1; i++) d[i] = s[i];
	d[i] = 0;
}

// A style's kind from its name, when it says nothing else.
static void st_by_name(st_t *s, const char *name) {
	if (!name) return;
	// "heading 1", "Heading_20_1", "Heading1"
	if (zf_ieq_n(name, "heading", 7)) {
		const char *p = name + 7;
		while (*p && (*p < '1' || *p > '9')) p++;
		if (*p) { s->kind = ZF_P_HEAD; s->level = (int8_t)(*p - '0'); return; }
		s->kind = ZF_P_HEAD;
		s->level = 1;
		return;
	}
	if (zf_ieq(name, "title")) { s->kind = ZF_P_HEAD; s->level = 1; return; }
	if (zf_ieq(name, "subtitle")) { s->kind = ZF_P_HEAD; s->level = 2; return; }
	if (contains_ci(name, "preformatted") || contains_ci(name, "plain text") || contains_ci(name, "plain_20_text") ||
		contains_ci(name, "code") || contains_ci(name, "source_20_text") || contains_ci(name, "macro"))
		{ s->kind = ZF_P_PRE; return; }
	if (contains_ci(name, "list bullet") || contains_ci(name, "list number") || contains_ci(name, "list_20_bullet") ||
		contains_ci(name, "listbullet") || contains_ci(name, "listnumber")) {
		// "List Bullet 2": the second level
		const char *p = name + strlen(name);
		s->kind = ZF_P_ITEM;
		s->level = (p > name && p[-1] >= '2' && p[-1] <= '9') ? (int8_t)(p[-1] - '0') : 1;
		return;
	}
	if (contains_ci(name, "strong")) s->rstyle |= ZF_S_BOLD;
	if (contains_ci(name, "emphasis")) s->rstyle |= ZF_S_ITAL;
}

// Kinds inherited through basedOn, a few steps deep.
static void st_resolve(void) {
	for (int pass = 0; pass < 4; pass++)
		for (int i = 0; i < nst; i++) {
			if (st[i].kind >= 0 || !st[i].base[0]) continue;
			st_t *b = st_find(st[i].base);
			if (b && b->kind >= 0) { st[i].kind = b->kind; st[i].level = b->level; }
		}
}

// -- docx --

static void docx_styles(zz_t *z) {
	nst = 0;
	if (ent_open(z, "word/styles.xml") < 0) { ent_close(); return; }
	zx_t *x = &ze.x;
	st_t *cur = NULL;
	bool in_rpr = false;
	for (;;) {
		zx_tok_t t = zx_next(x);
		if (t == ZX_EOF || t == ZX_ERR) break;
		const char *n = zx_local(x->name);
		if (t == ZX_START) {
			if (!strcmp(n, "style")) {
				cur = NULL;
				const char *id = zx_attr_local(x, "styleId");
				if (id && nst < NSTYLE) {
					cur = &st[nst++];
					memset(cur, 0, sizeof(*cur));
					cur->kind = -1;
					copy(cur->id, id, sizeof cur->id);
				}
			} else if (cur && !strcmp(n, "name")) {
				st_by_name(cur, zx_attr_local(x, "val"));
			} else if (cur && !strcmp(n, "basedOn")) {
				copy(cur->base, zx_attr_local(x, "val"), sizeof cur->base);
			} else if (cur && !strcmp(n, "outlineLvl")) {
				int l = atoi_(zx_attr_local(x, "val"));
				if (l >= 0 && l < 9) { cur->kind = ZF_P_HEAD; cur->level = (int8_t)(l + 1); }
			} else if (cur && !strcmp(n, "numId") && !in_rpr) {
				if (atoi_(zx_attr_local(x, "val")) > 0 && cur->kind != ZF_P_HEAD) {
					if (cur->kind != ZF_P_ITEM) cur->level = 1;
					cur->kind = ZF_P_ITEM;
				}
			} else if (cur && !strcmp(n, "ilvl") && !in_rpr) {
				int l = atoi_(zx_attr_local(x, "val"));
				if (l >= 0 && l < 9) cur->level = (int8_t)(l + 1);
			} else if (cur && !strcmp(n, "rPr")) in_rpr = true;
			else if (cur && in_rpr && !strcmp(n, "b") && val_true(x)) cur->rstyle |= ZF_S_BOLD;
			else if (cur && in_rpr && !strcmp(n, "i") && val_true(x)) cur->rstyle |= ZF_S_ITAL;
		} else if (t == ZX_END) {
			if (!strcmp(n, "rPr")) in_rpr = false;
			else if (!strcmp(n, "style")) cur = NULL;
		}
	}
	ent_close();
	st_resolve();
}

// The main part's name, from the package relationships.
static void docx_main(zz_t *z, char *out, int cap) {
	copy(out, "word/document.xml", cap);
	if (ent_open(z, "_rels/.rels") < 0) { ent_close(); return; }
	zx_t *x = &ze.x;
	for (;;) {
		zx_tok_t t = zx_next(x);
		if (t == ZX_EOF || t == ZX_ERR) break;
		if (t != ZX_START || strcmp(zx_local(x->name), "Relationship")) continue;
		const char *ty = zx_attr(x, "Type"), *tg = zx_attr(x, "Target");
		int tl = ty ? (int)strlen(ty) : 0;
		if (tg && tl > 15 && !strcmp(ty + tl - 15, "/officeDocument")) {
			if (*tg == '/') tg++;
			copy(out, tg, cap);
			break;
		}
	}
	ent_close();
}

int zf_read_docx(zf_io_t *in, zf_doc_t *d) {
	static zz_t z;
	static zf_em_t em;
	char main[ZF_PATH_MAX];
	int r = zz_open(&z, in);
	if (r < 0) return r;
	docx_styles(&z);
	docx_main(&z, main, sizeof main);
	if ((r = ent_open(&z, main)) < 0) { ent_close(); return r; }

	zf_em_init(&em, d, false);
	zx_t *x = &ze.x;
	int depth_skip = 0;			// inside something left out
	bool in_ppr = false, in_rpr = false, in_t = false;
	int pkind = ZF_P_BODY, plevel = 1;
	bool para = false, para_opened = false;
	int pnest = 0;
	int rstyle = 0;
	int ilvl = 0;
	bool numpr = false;
	uint32_t ticks = 0;

	for (;;) {
		zx_tok_t t = zx_next(x);
		if (t == ZX_EOF) break;
		if (t == ZX_ERR) { r = ze.rd.err ? ze.rd.err : ZF_E_CORRUPT; break; }
		if (!(++ticks & 255) && check_cancel()) { r = ZF_E_CANCEL; break; }
		const char *n = zx_local(x->name);

		if (depth_skip) {
			if (t == ZX_START) depth_skip++;
			else if (t == ZX_END) depth_skip--;
			continue;
		}

		if (t == ZX_START) {
			// left out: deleted text, field instructions, the second
			// rendering of alternate content, drawings, footnote marks
			if (!strcmp(n, "del") || !strcmp(n, "instrText") || !strcmp(n, "delText") ||
				!strcmp(n, "Fallback") || !strcmp(n, "drawing") || !strcmp(n, "pict") ||
				!strcmp(n, "object") || !strcmp(n, "sectPr")) {
				depth_skip = 1;
				continue;
			}
			if (!strcmp(n, "p")) {
				// a paragraph inside a paragraph (a text box): end the outer
				zf_em_end_para(&em);
				para = true;
				para_opened = false;
				pnest++;
				pkind = ZF_P_BODY;
				plevel = 1;
				numpr = false;
				ilvl = 0;
				if (x->empty) { zf_em_para(&em, ZF_P_BODY, 1, true); para_opened = true; }
			} else if (!strcmp(n, "pPr")) in_ppr = !x->empty;
			else if (in_ppr && !in_rpr && !strcmp(n, "pStyle")) {
				const char *v = zx_attr_local(x, "val");
				st_t *s = v ? st_find(v) : NULL;
				if (s && s->kind >= 0) { pkind = s->kind; plevel = s->level; }
				else if (!s && v) { st_t tmp; memset(&tmp, 0, sizeof tmp); tmp.kind = -1; st_by_name(&tmp, v);
					if (tmp.kind >= 0) { pkind = tmp.kind; plevel = tmp.level; } }
			} else if (in_ppr && !in_rpr && !strcmp(n, "outlineLvl")) {
				int l = atoi_(zx_attr_local(x, "val"));
				if (l >= 0 && l < 9) { pkind = ZF_P_HEAD; plevel = l + 1; }
			} else if (in_ppr && !strcmp(n, "numPr")) numpr = true;
			else if (in_ppr && !strcmp(n, "ilvl")) { ilvl = atoi_(zx_attr_local(x, "val")); if (ilvl < 0) ilvl = 0; }
			else if (in_ppr && !strcmp(n, "numId")) { if (atoi_(zx_attr_local(x, "val")) == 0) numpr = false; }
			else if (!strcmp(n, "rPr")) in_rpr = !x->empty;
			else if (!strcmp(n, "r")) rstyle = 0;
			else if (in_rpr && !in_ppr) {
				if (!strcmp(n, "b")) { if (val_true(x)) rstyle |= ZF_S_BOLD; else rstyle &= ~ZF_S_BOLD; }
				else if (!strcmp(n, "i")) { if (val_true(x)) rstyle |= ZF_S_ITAL; else rstyle &= ~ZF_S_ITAL; }
				else if (!strcmp(n, "rStyle")) {
					const char *v = zx_attr_local(x, "val");
					st_t *s = v ? st_find(v) : NULL;
					if (s) rstyle |= s->rstyle;
				}
			} else if (!strcmp(n, "t")) in_t = !x->empty;
			else if (!strcmp(n, "tab") && !in_ppr) { if (para) zf_em_raw(&em, "\t", 1, rstyle); }
			else if ((!strcmp(n, "br") || !strcmp(n, "cr")) && !in_ppr) {
				const char *ty = zx_attr_local(x, "type");
				if (para && !(ty && !strcmp(ty, "page"))) zf_em_brk(&em);
			} else if (!strcmp(n, "noBreakHyphen")) { if (para) zf_em_raw(&em, "-", 1, rstyle); }
			else if (!strcmp(n, "tc")) { /* a table cell: its paragraphs follow */ }
		} else if (t == ZX_END) {
			if (!strcmp(n, "pPr")) {
				in_ppr = false;
				if (numpr && pkind != ZF_P_HEAD) { pkind = ZF_P_ITEM; plevel = ilvl + 1; }
				if (pkind == ZF_P_ITEM && plevel < 1) plevel = 1;
				zf_em_para(&em, pkind, plevel, true);
				para_opened = true;
			} else if (!strcmp(n, "rPr")) in_rpr = false;
			else if (!strcmp(n, "t")) in_t = false;
			else if (!strcmp(n, "p") && para) {
				if (!para_opened) zf_em_para(&em, ZF_P_BODY, 1, true);
				zf_em_end_para(&em);
				// the rest of an outer paragraph, after a text box in it
				if (pnest) pnest--;
				para = pnest > 0;
				para_opened = false;
				pkind = ZF_P_BODY;
				plevel = 1;
			}
		} else if (t == ZX_TEXT && in_t && para) {
			if (!para_opened) { zf_em_para(&em, pkind, plevel, true); para_opened = true; }
			zf_em_text(&em, x->text, x->tlen, rstyle);
		}
		if (em.err) { r = em.err; break; }
	}
	int e2 = ent_close();
	if (r >= 0 && e2 < 0) r = e2;
	if (r < 0) return r;
	return zf_em_end(&em);
}

// -- odt --

static void odt_styles(zz_t *z, const char *entry, bool add) {
	if (!add) nst = 0;
	if (ent_open(z, entry) < 0) { ent_close(); return; }
	zx_t *x = &ze.x;
	st_t *cur = NULL;
	for (;;) {
		zx_tok_t t = zx_next(x);
		if (t == ZX_EOF || t == ZX_ERR) break;
		const char *n = zx_local(x->name);
		if (t == ZX_START) {
			if (!strcmp(n, "style") && zx_attr(x, "style:name")) {
				cur = NULL;
				if (nst < NSTYLE) {
					cur = &st[nst++];
					memset(cur, 0, sizeof(*cur));
					cur->kind = -1;
					const char *nm = zx_attr(x, "style:name");
					copy(cur->id, nm, sizeof cur->id);
					copy(cur->base, zx_attr(x, "style:parent-style-name"), sizeof cur->base);
					const char *dn = zx_attr(x, "style:display-name");
					st_by_name(cur, dn ? dn : nm);
					const char *ol = zx_attr(x, "style:default-outline-level");
					if (ol && atoi_(ol) > 0) { cur->kind = ZF_P_HEAD; cur->level = (int8_t)atoi_(ol); }
				}
				if (x->empty) cur = NULL;
			} else if (cur && !strcmp(n, "text-properties")) {
				const char *w = zx_attr(x, "fo:font-weight"), *s = zx_attr(x, "fo:font-style");
				if (w && (!strcmp(w, "bold") || atoi_(w) >= 600)) cur->rstyle |= ZF_S_BOLD;
				if (s && (!strcmp(s, "italic") || !strcmp(s, "oblique"))) cur->rstyle |= ZF_S_ITAL;
				const char *f = zx_attr(x, "style:font-name");
				if (f && (contains_ci(f, "courier") || contains_ci(f, "mono")) && cur->kind < 0) cur->kind = ZF_P_PRE;
			}
		} else if (t == ZX_END && !strcmp(n, "style")) cur = NULL;
		if (t == ZX_START && !strcmp(n, "body")) break;		// content.xml: styles end here
	}
	ent_close();
	st_resolve();
}

int zf_read_odt(zf_io_t *in, zf_doc_t *d) {
	static zz_t z;
	static zf_em_t em;
	int r = zz_open(&z, in);
	if (r < 0) return r;
	odt_styles(&z, "styles.xml", false);
	odt_styles(&z, "content.xml", true);
	if ((r = ent_open(&z, "content.xml")) < 0) { ent_close(); return r; }

	zf_em_init(&em, d, true);
	zx_t *x = &ze.x;
	int depth_skip = 0;
	int lists = 0;
	int pdepth = 0;					// text:p / text:h nesting
	int sstack[16], sp = 0;			// span styles
	int style = 0;
	bool in_body = false;
	uint32_t ticks = 0;

	for (;;) {
		zx_tok_t t = zx_next(x);
		if (t == ZX_EOF) break;
		if (t == ZX_ERR) { r = ze.rd.err ? ze.rd.err : ZF_E_CORRUPT; break; }
		if (!(++ticks & 255) && check_cancel()) { r = ZF_E_CANCEL; break; }
		const char *n = zx_local(x->name);

		if (depth_skip) {
			if (t == ZX_START) depth_skip++;
			else if (t == ZX_END) depth_skip--;
			continue;
		}
		if (!in_body) {
			if (t == ZX_START && !strcmp(x->name, "office:text")) in_body = true;
			continue;
		}

		if (t == ZX_START) {
			if (!strcmp(n, "note") || !strcmp(n, "annotation") || !strcmp(n, "tracked-changes") ||
				!strcmp(n, "sequence-decls") || !strcmp(n, "image") || !strcmp(n, "bookmark-ref")) {
				depth_skip = 1;
				continue;
			}
			if (!strcmp(n, "list")) lists++;
			else if (!strcmp(n, "p") || !strcmp(n, "h")) {
				int kind = ZF_P_BODY, level = 1;
				const char *sn = zx_attr(x, "text:style-name");
				st_t *s = sn ? st_find(sn) : NULL;
				if (s && s->kind >= 0) { kind = s->kind; level = s->level; }
				if (!strcmp(n, "h")) {
					kind = ZF_P_HEAD;
					level = atoi_(zx_attr(x, "text:outline-level"));
					if (level < 1) level = 1;
				} else if (lists && kind != ZF_P_HEAD) { kind = ZF_P_ITEM; level = lists; }
				if (level > 9) level = 9;
				zf_em_para(&em, kind, level, true);
				em.collapse = (kind != ZF_P_PRE);
				style = s ? s->rstyle : 0;
				sp = 0;
				pdepth++;		// an empty one's END follows too
			} else if (!strcmp(n, "span")) {
				if (sp < 16) sstack[sp] = style;
				sp++;
				const char *sn = zx_attr(x, "text:style-name");
				st_t *s = sn ? st_find(sn) : NULL;
				if (s) style |= s->rstyle;
			} else if (pdepth && !strcmp(n, "s")) {
				int c = atoi_(zx_attr(x, "text:c"));
				if (c < 1) c = 1;
				if (c > 256) c = 256;
				while (c--) zf_em_raw(&em, " ", 1, style);
			} else if (pdepth && !strcmp(n, "tab")) zf_em_raw(&em, "\t", 1, style);
			else if (pdepth && !strcmp(n, "line-break")) zf_em_brk(&em);
		} else if (t == ZX_END) {
			if (!strcmp(n, "list")) { if (lists) lists--; }
			else if (!strcmp(n, "p") || !strcmp(n, "h")) { if (pdepth) { pdepth--; zf_em_end_para(&em); } }
			else if (!strcmp(n, "span")) { if (sp) { sp--; if (sp < 16) style = sstack[sp]; } }
		} else if (t == ZX_TEXT && pdepth) {
			zf_em_text(&em, x->text, x->tlen, style);
		}
		if (em.err) { r = em.err; break; }
	}
	int e2 = ent_close();
	if (r >= 0 && e2 < 0) r = e2;
	if (r < 0) return r;
	return zf_em_end(&em);
}
