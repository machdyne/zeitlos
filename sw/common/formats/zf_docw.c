/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * The document writers: plain text, Markdown, HTML, RTF, docx and
 * odt, all from the one stream of paragraphs and runs (zf.h).
 *
 *   txt    a line per paragraph; a blank line before a heading; list
 *          items as "- ", indented two spaces a level
 *   md     CommonMark: # headings, - lists, **bold**, *italic*, fenced
 *          code for preformatted runs, a blank line between blocks
 *   html   one self-contained page, UTF-8
 *   rtf    RTF 1.x, ANSI with \uN for everything past ASCII -- what
 *          WordPad, Word, TextEdit and LibreOffice all read
 *   docx   WordprocessingML: document, styles (Title, Heading1-6,
 *          a monospace style) and numbering (one bullet list, nine
 *          levels), in a stored zip
 *   odt    OpenDocument text: content with automatic styles for bold
 *          and italic and a bullet list style, in a stored zip whose
 *          first entry is the uncompressed mimetype, as the standard
 *          requires
 */

#include <string.h>
#include <stdio.h>

#include "zf.h"
#include "zf_zip.h"

typedef struct {
	zf_dw_kind_t	kind;
	zf_io_t			*out;
	zw_t			zw;
	bool			zip;
	int				pkind, plevel;
	bool			in_para, any, in_pre;
	int				style;			// what the open run has
	int				lists;			// list levels open (html, odt)
	bool			item_open;		// odt: a list item's paragraph is open
	char			ws[16];			// md: spaces held back from the end of a styled run
	int				nws;
	int				err;
} dw_t;

_Static_assert(sizeof(dw_t) <= ZF_DOCW_STATE, "ZF_DOCW_STATE too small");

static dw_t *W(zf_doc_t *d) { return (dw_t *)d->ctx; }

static int put(dw_t *w, const char *s, uint32_t n) {
	if (w->err) return w->err;
	int r = w->zip ? zw_write(&w->zw, s, n) : zf_write(w->out, s, n);
	if (r < 0) { w->err = r; return r; }
	if (zf_cancel_poll && zf_cancel_poll()) { w->err = ZF_E_CANCEL; return w->err; }
	return 0;
}
static int puts_(dw_t *w, const char *s) { return put(w, s, (uint32_t)strlen(s)); }

// XML and HTML: & < > escaped, and the C0 controls XML forbids dropped.
static int put_xml(dw_t *w, const char *s, uint32_t n) {
	uint32_t a = 0;
	for (uint32_t i = 0; i < n; i++) {
		const char *rep = NULL;
		unsigned char c = (unsigned char)s[i];
		if (c == '&') rep = "&amp;";
		else if (c == '<') rep = "&lt;";
		else if (c == '>') rep = "&gt;";
		else if (c < 0x20 && c != '\t' && c != '\n') rep = "";
		if (!rep) continue;
		if (i > a && put(w, s + a, i - a)) return w->err;
		if (*rep && puts_(w, rep)) return w->err;
		a = i + 1;
	}
	return n > a ? put(w, s + a, n - a) : 0;
}

static uint32_t utf8_next(const char **p, const char *end) {
	const unsigned char *s = (const unsigned char *)*p;
	uint32_t c = *s++;
	int k = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : c >= 0xC0 ? 1 : 0;
	if (c >= 0x80 && !k) { *p = (const char *)s; return 0xFFFD; }
	c &= (k == 3) ? 7 : (k == 2) ? 15 : (k == 1) ? 31 : 0x7F;
	while (k-- && (const char *)s < end && (*s & 0xC0) == 0x80) c = (c << 6) | (*s++ & 63);
	*p = (const char *)s;
	return c;
}

// -- text and Markdown --

static int md_style(dw_t *w, int want) {
	// close what is open, then open what is wanted: ** outside *
	if (w->style == want) return 0;
	if (w->style & ZF_S_ITAL) puts_(w, "*");
	if (w->style & ZF_S_BOLD) puts_(w, "**");
	if (want & ZF_S_BOLD) puts_(w, "**");
	if (want & ZF_S_ITAL) puts_(w, "*");
	w->style = want;
	return w->err;
}

// -- RTF --

static int put_rtf(dw_t *w, const char *s, uint32_t n) {
	const char *p = s, *end = s + n;
	char buf[24];
	while (p < end) {
		unsigned char c = (unsigned char)*p;
		if (c < 0x80) {
			p++;
			if (c == '\\' || c == '{' || c == '}') { buf[0] = '\\'; buf[1] = (char)c; put(w, buf, 2); }
			else if (c == '\t') puts_(w, "\\tab ");
			else if (c == '\n') puts_(w, "\\line ");
			else if (c >= 0x20) put(w, (char *)&c, 1);
			continue;
		}
		uint32_t cp = utf8_next(&p, end);
		// \uN takes a signed 16-bit number; past the BMP, a surrogate pair
		if (cp > 0xFFFF) {
			cp -= 0x10000;
			snprintf(buf, sizeof buf, "\\u%d?\\u%d?", (int)(int16_t)(0xD800 + (cp >> 10)),
				(int)(int16_t)(0xDC00 + (cp & 0x3FF)));
		} else
			snprintf(buf, sizeof buf, "\\u%d?", (int)(int16_t)cp);
		puts_(w, buf);
	}
	return w->err;
}

// -- odt: spaces --

// A run of spaces in ODF collapses to one unless written as <text:s>.
static int put_odt_text(dw_t *w, const char *s, uint32_t n) {
	uint32_t a = 0;
	for (uint32_t i = 0; i < n; i++) {
		char c = s[i];
		if (c == '\t' || c == '\n' || (c == ' ' && i + 1 < n && s[i + 1] == ' ')) {
			if (i > a) put_xml(w, s + a, i - a);
			if (c == '\t') puts_(w, "<text:tab/>");
			else if (c == '\n') puts_(w, "<text:line-break/>");
			else {
				uint32_t k = i;
				while (k < n && s[k] == ' ') k++;
				char b[48];
				snprintf(b, sizeof b, " <text:s text:c=\"%u\"/>", (unsigned)(k - i - 1));
				puts_(w, b);
				i = k - 1;
			}
			a = i + 1;
		}
	}
	return n > a ? put_xml(w, s + a, n - a) : 0;
}

// -- the fixed parts of docx and odt --

static const char docx_types[] =
	"<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"
	"<Types xmlns=\"http://schemas.openxmlformats.org/package/2006/content-types\">"
	"<Default Extension=\"rels\" ContentType=\"application/vnd.openxmlformats-package.relationships+xml\"/>"
	"<Default Extension=\"xml\" ContentType=\"application/xml\"/>"
	"<Override PartName=\"/word/document.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.wordprocessingml.document.main+xml\"/>"
	"<Override PartName=\"/word/styles.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.wordprocessingml.styles+xml\"/>"
	"<Override PartName=\"/word/numbering.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.wordprocessingml.numbering+xml\"/>"
	"</Types>";

static const char docx_rels[] =
	"<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"
	"<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">"
	"<Relationship Id=\"rId1\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/officeDocument\" Target=\"word/document.xml\"/>"
	"</Relationships>";

static const char docx_doc_rels[] =
	"<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"
	"<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">"
	"<Relationship Id=\"rId1\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/styles\" Target=\"styles.xml\"/>"
	"<Relationship Id=\"rId2\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/numbering\" Target=\"numbering.xml\"/>"
	"</Relationships>";

#define W_NS "xmlns:w=\"http://schemas.openxmlformats.org/wordprocessingml/2006/main\""

static int docx_styles(dw_t *w) {
	char b[512];
	puts_(w, "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n<w:styles " W_NS ">"
		"<w:docDefaults><w:rPrDefault><w:rPr><w:sz w:val=\"22\"/></w:rPr></w:rPrDefault>"
		"<w:pPrDefault><w:pPr><w:spacing w:after=\"120\"/></w:pPr></w:pPrDefault></w:docDefaults>"
		"<w:style w:type=\"paragraph\" w:default=\"1\" w:styleId=\"Normal\"><w:name w:val=\"Normal\"/></w:style>"
		"<w:style w:type=\"paragraph\" w:styleId=\"Title\"><w:name w:val=\"Title\"/><w:basedOn w:val=\"Normal\"/>"
		"<w:rPr><w:b/><w:sz w:val=\"48\"/></w:rPr></w:style>"
		"<w:style w:type=\"paragraph\" w:styleId=\"ListParagraph\"><w:name w:val=\"List Paragraph\"/><w:basedOn w:val=\"Normal\"/></w:style>"
		"<w:style w:type=\"paragraph\" w:styleId=\"PlainText\"><w:name w:val=\"Plain Text\"/><w:basedOn w:val=\"Normal\"/>"
		"<w:pPr><w:spacing w:after=\"0\"/></w:pPr><w:rPr><w:rFonts w:ascii=\"Courier New\" w:hAnsi=\"Courier New\"/><w:sz w:val=\"20\"/></w:rPr></w:style>");
	static const int sz[6] = { 36, 30, 26, 24, 22, 22 };
	for (int i = 1; i <= 6; i++) {
		snprintf(b, sizeof b, "<w:style w:type=\"paragraph\" w:styleId=\"Heading%d\"><w:name w:val=\"heading %d\"/>"
			"<w:basedOn w:val=\"Normal\"/><w:next w:val=\"Normal\"/><w:pPr><w:keepNext/><w:spacing w:before=\"240\"/>"
			"<w:outlineLvl w:val=\"%d\"/></w:pPr><w:rPr><w:b/><w:sz w:val=\"%d\"/></w:rPr></w:style>",
			i, i, i - 1, sz[i - 1]);
		puts_(w, b);
	}
	return puts_(w, "</w:styles>");
}

static int docx_numbering(dw_t *w) {
	char b[384];
	puts_(w, "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n<w:numbering " W_NS ">"
		"<w:abstractNum w:abstractNumId=\"0\"><w:multiLevelType w:val=\"hybridMultilevel\"/>");
	for (int l = 0; l < 9; l++) {
		snprintf(b, sizeof b, "<w:lvl w:ilvl=\"%d\"><w:start w:val=\"1\"/><w:numFmt w:val=\"bullet\"/>"
			"<w:lvlText w:val=\"%s\"/><w:lvlJc w:val=\"left\"/><w:pPr><w:ind w:left=\"%d\" w:hanging=\"360\"/></w:pPr></w:lvl>",
			l, (l & 1) ? "\xE2\x97\xA6" : "\xE2\x80\xA2", 720 * (l + 1));
		puts_(w, b);
	}
	return puts_(w, "</w:abstractNum><w:num w:numId=\"1\"><w:abstractNumId w:val=\"0\"/></w:num></w:numbering>");
}

static const char odt_manifest[] =
	"<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
	"<manifest:manifest xmlns:manifest=\"urn:oasis:names:tc:opendocument:xmlns:manifest:1.0\" manifest:version=\"1.2\">"
	"<manifest:file-entry manifest:full-path=\"/\" manifest:media-type=\"application/vnd.oasis.opendocument.text\"/>"
	"<manifest:file-entry manifest:full-path=\"content.xml\" manifest:media-type=\"text/xml\"/>"
	"</manifest:manifest>";

static const char odt_head[] =
	"<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
	"<office:document-content xmlns:office=\"urn:oasis:names:tc:opendocument:xmlns:office:1.0\""
	" xmlns:style=\"urn:oasis:names:tc:opendocument:xmlns:style:1.0\""
	" xmlns:text=\"urn:oasis:names:tc:opendocument:xmlns:text:1.0\""
	" xmlns:fo=\"urn:oasis:names:tc:opendocument:xmlns:xsl-fo-compatible:1.0\""
	" office:version=\"1.2\"><office:automatic-styles>"
	"<style:style style:name=\"T1\" style:family=\"text\"><style:text-properties fo:font-weight=\"bold\"/></style:style>"
	"<style:style style:name=\"T2\" style:family=\"text\"><style:text-properties fo:font-style=\"italic\"/></style:style>"
	"<style:style style:name=\"T3\" style:family=\"text\"><style:text-properties fo:font-weight=\"bold\" fo:font-style=\"italic\"/></style:style>"
	"<style:style style:name=\"Pre\" style:family=\"paragraph\"><style:text-properties style:font-name=\"Courier New\" fo:font-family=\"'Courier New'\"/></style:style>"
	"<style:style style:name=\"Ttl\" style:family=\"paragraph\"><style:text-properties fo:font-size=\"24pt\" fo:font-weight=\"bold\"/></style:style>"
	"<text:list-style style:name=\"L1\">";

// -- the writer --

static int dw_begin(zf_doc_t *d, const char *title) {
	dw_t *w = W(d);
	char b[256];
	switch (w->kind) {
	case ZF_DW_HTML:
		puts_(w, "<!DOCTYPE html>\n<html>\n<head>\n<meta charset=\"utf-8\">\n<title>");
		if (title) put_xml(w, title, (uint32_t)strlen(title));
		puts_(w, "</title>\n</head>\n<body>\n");
		break;
	case ZF_DW_RTF:
		puts_(w, "{\\rtf1\\ansi\\ansicpg1252\\deff0\\uc1{\\fonttbl{\\f0\\fswiss Helvetica;}{\\f1\\fmodern Courier New;}}\n"
			"\\f0\\fs22\n");
		break;
	case ZF_DW_DOCX:
		zw_file(&w->zw, "[Content_Types].xml", docx_types);
		zw_file(&w->zw, "_rels/.rels", docx_rels);
		zw_file(&w->zw, "word/_rels/document.xml.rels", docx_doc_rels);
		zw_begin(&w->zw, "word/styles.xml");
		docx_styles(w);
		zw_end(&w->zw);
		zw_begin(&w->zw, "word/numbering.xml");
		docx_numbering(w);
		zw_end(&w->zw);
		zw_begin(&w->zw, "word/document.xml");
		puts_(w, "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n<w:document " W_NS "><w:body>");
		break;
	case ZF_DW_ODT:
		zw_file(&w->zw, "mimetype", "application/vnd.oasis.opendocument.text");
		zw_file(&w->zw, "META-INF/manifest.xml", odt_manifest);
		zw_begin(&w->zw, "content.xml");
		puts_(w, odt_head);
		for (int l = 1; l <= 10; l++) {
			snprintf(b, sizeof b, "<text:list-level-style-bullet text:level=\"%d\" text:bullet-char=\"%s\">"
				"<style:list-level-properties text:space-before=\"%d.%dcm\" text:min-label-width=\"0.6cm\"/>"
				"</text:list-level-style-bullet>", l, (l & 1) ? "\xE2\x80\xA2" : "\xE2\x97\xA6",
				(l - 1) * 6 / 10, (l - 1) * 6 % 10);
			puts_(w, b);
		}
		puts_(w, "</text:list-style></office:automatic-styles><office:body><office:text>");
		break;
	default:
		break;
	}
	(void)title;
	return w->err;
}

// Lists in html and odt are nested elements: open or close them to
// reach `want` levels.
static void lists_to(dw_t *w, int want) {
	while (w->lists > want) {
		if (w->kind == ZF_DW_HTML) puts_(w, "</ul>\n");
		else { if (w->item_open) { puts_(w, "</text:list-item>"); } puts_(w, "</text:list>"); w->item_open = w->lists > 1; }
		w->lists--;
	}
	while (w->lists < want) {
		if (w->kind == ZF_DW_HTML) puts_(w, "<ul>\n");
		else {
			if (w->lists == 0) puts_(w, "<text:list text:style-name=\"L1\">");
			else {
				if (!w->item_open) puts_(w, "<text:list-item>");
				puts_(w, "<text:list>");
			}
			w->item_open = false;
		}
		w->lists++;
	}
}

static int dw_para(zf_doc_t *d, int kind, int level) {
	dw_t *w = W(d);
	char b[160];
	if (level < 1) level = 1;
	if (kind == ZF_P_HEAD && level > 6) level = 6;
	if (kind == ZF_P_ITEM && level > 9) level = 9;
	bool first = !w->any;
	w->any = true;
	w->pkind = kind;
	w->plevel = level;
	w->in_para = true;
	w->style = 0;

	int want_lists = (kind == ZF_P_ITEM) ? level : 0;

	switch (w->kind) {
	case ZF_DW_TXT:
		if (w->in_pre && kind != ZF_P_PRE) w->in_pre = false;
		if (kind == ZF_P_HEAD && !first) puts_(w, "\n");
		if (kind == ZF_P_ITEM) {
			for (int i = 1; i < level; i++) puts_(w, "  ");
			puts_(w, "- ");
		}
		break;
	case ZF_DW_MD:
		if (w->in_pre && kind != ZF_P_PRE) { puts_(w, "```\n"); w->in_pre = false; }
		if (!first && !(kind == ZF_P_PRE && w->in_pre) &&
			!(kind == ZF_P_ITEM && w->lists)) puts_(w, "\n");
		w->lists = (kind == ZF_P_ITEM);
		if (kind == ZF_P_PRE && !w->in_pre) { puts_(w, "```\n"); w->in_pre = true; }
		if (kind == ZF_P_HEAD) { for (int i = 0; i < level; i++) puts_(w, "#"); puts_(w, " "); }
		if (kind == ZF_P_ITEM) { for (int i = 1; i < level; i++) puts_(w, "  "); puts_(w, "- "); }
		break;
	case ZF_DW_HTML:
		lists_to(w, want_lists);
		if (w->in_pre && kind != ZF_P_PRE) { puts_(w, "</pre>\n"); w->in_pre = false; }
		if (kind == ZF_P_HEAD) { snprintf(b, sizeof b, "<h%d>", level); puts_(w, b); }
		else if (kind == ZF_P_ITEM) puts_(w, "<li>");
		else if (kind == ZF_P_PRE) { if (!w->in_pre) puts_(w, "<pre>"); w->in_pre = true; }
		else puts_(w, "<p>");
		break;
	case ZF_DW_RTF:
		puts_(w, "\\pard\\plain\\f0\\fs22 ");
		if (kind == ZF_P_HEAD) {
			static const int sz[6] = { 36, 30, 26, 24, 22, 22 };
			snprintf(b, sizeof b, "\\sb240\\keepn\\outlinelevel%d\\b\\fs%d ", level - 1, sz[level - 1]);
			puts_(w, b);
		} else if (kind == ZF_P_ITEM) {
			snprintf(b, sizeof b, "\\li%d\\fi-360 \\bullet\\tab ", 360 * level);
			puts_(w, b);
		} else if (kind == ZF_P_PRE) puts_(w, "\\f1\\fs20 ");
		else puts_(w, "\\sa120 ");
		break;
	case ZF_DW_DOCX:
		puts_(w, "<w:p>");
		if (kind == ZF_P_HEAD) { snprintf(b, sizeof b, "<w:pPr><w:pStyle w:val=\"Heading%d\"/></w:pPr>", level); puts_(w, b); }
		else if (kind == ZF_P_ITEM) {
			snprintf(b, sizeof b, "<w:pPr><w:pStyle w:val=\"ListParagraph\"/><w:numPr><w:ilvl w:val=\"%d\"/>"
				"<w:numId w:val=\"1\"/></w:numPr></w:pPr>", level - 1);
			puts_(w, b);
		} else if (kind == ZF_P_PRE) puts_(w, "<w:pPr><w:pStyle w:val=\"PlainText\"/></w:pPr>");
		break;
	case ZF_DW_ODT:
		lists_to(w, want_lists);
		if (kind == ZF_P_HEAD) { snprintf(b, sizeof b, "<text:h text:outline-level=\"%d\">", level); puts_(w, b); }
		else if (kind == ZF_P_ITEM) {
			if (w->item_open) puts_(w, "</text:list-item>");
			puts_(w, "<text:list-item><text:p>");
			w->item_open = true;
		} else if (kind == ZF_P_PRE) puts_(w, "<text:p text:style-name=\"Pre\">");
		else puts_(w, "<text:p>");
		break;
	}
	return w->err;
}

static int dw_text(zf_doc_t *d, const char *s, uint32_t n, int style) {
	dw_t *w = W(d);
	if (!n) return 0;
	if (!w->in_para) dw_para(d, ZF_P_BODY, 1);
	if (w->pkind == ZF_P_PRE) style = 0;
	switch (w->kind) {
	case ZF_DW_TXT:
		return put(w, s, n);
	case ZF_DW_MD: {
		if (w->pkind == ZF_P_PRE) return put(w, s, n);
		// spaces go outside the markers: "a **b** c", never "a** b **c"
		if (style != w->style) md_style(w, w->style & style);
		if (w->nws) { put(w, w->ws, (uint32_t)w->nws); w->nws = 0; }
		while (n && (*s == ' ' || *s == '\t')) { put(w, s, 1); s++; n--; }
		if (!n) return w->err;
		md_style(w, style);
		uint32_t t = 0;
		while (style && t < n && t < sizeof w->ws && (s[n - 1 - t] == ' ' || s[n - 1 - t] == '\t')) t++;
		if (t == n) t = 0;
		put(w, s, n - t);
		memcpy(w->ws, s + n - t, t);
		w->nws = (int)t;
		return w->err;
	}
	case ZF_DW_HTML:
		if (style != w->style) {
			if (w->style & ZF_S_ITAL) puts_(w, "</i>");
			if (w->style & ZF_S_BOLD) puts_(w, "</b>");
			if (style & ZF_S_BOLD) puts_(w, "<b>");
			if (style & ZF_S_ITAL) puts_(w, "<i>");
			w->style = style;
		}
		return put_xml(w, s, n);
	case ZF_DW_RTF:
		if (style != w->style) {
			puts_(w, (style & ZF_S_BOLD) ? "\\b " : (w->pkind == ZF_P_HEAD ? "\\b " : "\\b0 "));
			puts_(w, (style & ZF_S_ITAL) ? "\\i " : "\\i0 ");
			w->style = style;
		}
		return put_rtf(w, s, n);
	case ZF_DW_DOCX: {
		puts_(w, "<w:r>");
		if (style) {
			puts_(w, "<w:rPr>");
			if (style & ZF_S_BOLD) puts_(w, "<w:b/>");
			if (style & ZF_S_ITAL) puts_(w, "<w:i/>");
			puts_(w, "</w:rPr>");
		}
		// tabs are their own element; the rest is <w:t>, spaces kept
		uint32_t a = 0;
		for (uint32_t i = 0; i <= n; i++) {
			if (i < n && s[i] != '\t' && s[i] != '\n') continue;
			if (i > a) { puts_(w, "<w:t xml:space=\"preserve\">"); put_xml(w, s + a, i - a); puts_(w, "</w:t>"); }
			if (i < n) puts_(w, s[i] == '\t' ? "<w:tab/>" : "<w:br/>");
			a = i + 1;
		}
		return puts_(w, "</w:r>");
	}
	case ZF_DW_ODT: {
		static const char *ts[4] = { NULL, "T1", "T2", "T3" };
		if (style) { puts_(w, "<text:span text:style-name=\""); puts_(w, ts[style & 3]); puts_(w, "\">"); }
		put_odt_text(w, s, n);
		if (style) puts_(w, "</text:span>");
		return w->err;
	}
	}
	return w->err;
}

static int dw_brk(zf_doc_t *d) {
	dw_t *w = W(d);
	if (!w->in_para) dw_para(d, ZF_P_BODY, 1);
	switch (w->kind) {
	case ZF_DW_TXT:		return puts_(w, "\n");
	case ZF_DW_MD:		md_style(w, 0); w->nws = 0; return puts_(w, w->pkind == ZF_P_PRE ? "\n" : "  \n");
	case ZF_DW_HTML:	return puts_(w, w->pkind == ZF_P_PRE ? "\n" : "<br>");
	case ZF_DW_RTF:		return puts_(w, "\\line ");
	case ZF_DW_DOCX:	return puts_(w, "<w:r><w:br/></w:r>");
	case ZF_DW_ODT:		return puts_(w, "<text:line-break/>");
	}
	return 0;
}

static int dw_end_para(zf_doc_t *d) {
	dw_t *w = W(d);
	char b[16];
	if (!w->in_para) return 0;
	w->in_para = false;
	switch (w->kind) {
	case ZF_DW_TXT:
		return puts_(w, "\n");
	case ZF_DW_MD:
		md_style(w, 0);
		w->nws = 0;
		return puts_(w, "\n");
	case ZF_DW_HTML:
		if (w->style & ZF_S_ITAL) puts_(w, "</i>");
		if (w->style & ZF_S_BOLD) puts_(w, "</b>");
		w->style = 0;
		if (w->pkind == ZF_P_HEAD) { snprintf(b, sizeof b, "</h%d>\n", w->plevel); return puts_(w, b); }
		if (w->pkind == ZF_P_ITEM) return puts_(w, "</li>\n");
		if (w->pkind == ZF_P_PRE) return puts_(w, "\n");
		return puts_(w, "</p>\n");
	case ZF_DW_RTF:
		return puts_(w, "\\par\n");
	case ZF_DW_DOCX:
		return puts_(w, "</w:p>");
	case ZF_DW_ODT:
		if (w->pkind == ZF_P_HEAD) return puts_(w, "</text:h>");
		return puts_(w, "</text:p>");
	}
	return 0;
}

static int dw_end(zf_doc_t *d) {
	dw_t *w = W(d);
	if (w->in_para) dw_end_para(d);
	switch (w->kind) {
	case ZF_DW_MD:
		if (w->in_pre) puts_(w, "```\n");
		break;
	case ZF_DW_HTML:
		lists_to(w, 0);
		if (w->in_pre) puts_(w, "</pre>\n");
		puts_(w, "</body>\n</html>\n");
		break;
	case ZF_DW_RTF:
		puts_(w, "}\n");
		break;
	case ZF_DW_DOCX:
		puts_(w, "<w:sectPr><w:pgSz w:w=\"11906\" w:h=\"16838\"/><w:pgMar w:top=\"1440\" w:right=\"1440\""
			" w:bottom=\"1440\" w:left=\"1440\" w:header=\"708\" w:footer=\"708\" w:gutter=\"0\"/></w:sectPr>"
			"</w:body></w:document>");
		if (!w->err) w->err = zw_end(&w->zw);
		if (!w->err) w->err = zw_finish(&w->zw);
		break;
	case ZF_DW_ODT:
		lists_to(w, 0);
		puts_(w, "</office:text></office:body></office:document-content>");
		if (!w->err) w->err = zw_end(&w->zw);
		if (!w->err) w->err = zw_finish(&w->zw);
		break;
	default:
		break;
	}
	return w->err;
}

int zf_docw_open(zf_doc_t *d, void *state, zf_dw_kind_t kind, zf_io_t *out) {
	dw_t *w = (dw_t *)state;
	memset(w, 0, sizeof(*w));
	w->kind = kind;
	w->out = out;
	w->zip = (kind == ZF_DW_DOCX || kind == ZF_DW_ODT);
	if (w->zip) zw_init(&w->zw, out);
	d->ctx = w;
	d->begin = dw_begin;
	d->para = dw_para;
	d->text = dw_text;
	d->brk = dw_brk;
	d->end_para = dw_end_para;
	d->end = dw_end;
	return ZF_OK;
}
