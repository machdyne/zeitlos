/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * Tables: csv, tsv, zss (sw/apps/sheet's own format), xlsx and ods,
 * read and written as a stream of cells in row order.
 *
 * -- formulas --
 *
 * A formula crosses between formats only when the sheet app could
 * evaluate it: + - * / ^ and comparisons, parentheses, numbers,
 * references A1..Z999 and ranges of them, TRUE and FALSE, and the
 * sheet's functions (SUM AVERAGE/AVG MIN MAX COUNT ABS INT ROUND MOD
 * SQRT POWER IF AND OR NOT PMT PV FV NPV VLOOKUP INDEX).
 * Anything else -- other functions, other sheets, text operators,
 * comparisons -- arrives as the value the other program last
 * calculated for it, which xlsx and ods both store. Going out, AVG is
 * written AVERAGE, and ods gets its own reference syntax ([.A1:.B2]).
 *
 * -- the sheet's limits --
 *
 * zss holds columns A-Z, rows 1-999 and 95 bytes a cell; a cell
 * outside that is not written (zf_tab_dropped counts them, so convert
 * can say so), and longer text is cut at a character boundary. Numbers
 * in exponent form (1.5E-7) are written out in full.
 */

#include <string.h>
#include <stdio.h>

#include "zf.h"
#include "zf_zip.h"
#include "zf_xml.h"
#include "../../apps/sheet/sheet_core.h"

uint32_t zf_tab_dropped;

static void copy(char *d, const char *s, int cap) {
	int i = 0;
	if (s) for (; s[i] && i < cap - 1; i++) d[i] = s[i];
	d[i] = 0;
}

static unsigned long strtoul_(const char *s) {
	unsigned long v = 0;
	while (*s == ' ') s++;
	while (*s >= '0' && *s <= '9' && v < 100000000ul) v = v * 10 + (unsigned long)(*s++ - '0');
	return v;
}

static bool cancelled(uint32_t *ticks) {
	return !(++*ticks & 255) && zf_cancel_poll && zf_cancel_poll();
}

// -- numbers --

// "-12.5", "3", ".5", "1e3", "1.5E-07": true if all of `s` is a number.
bool zf_is_num(const char *s) {
	const char *p = s;
	if (*p == '-' || *p == '+') p++;
	int d = 0;
	while (*p >= '0' && *p <= '9') { p++; d++; }
	if (*p == '.') { p++; while (*p >= '0' && *p <= '9') { p++; d++; } }
	if (!d) return false;
	if (*p == 'e' || *p == 'E') {
		p++;
		if (*p == '-' || *p == '+') p++;
		if (*p < '0' || *p > '9') return false;
		while (*p >= '0' && *p <= '9') p++;
	}
	return *p == 0;
}

// A number without an exponent: "1.5E-3" -> "0.0015", "2E5" -> "200000".
static void num_plain(const char *s, char *out, int cap) {
	const char *e = s;
	while (*e && *e != 'e' && *e != 'E') e++;
	if (!*e || cap < 8) { copy(out, s, cap); return; }
	int ex = 0, es = 1;
	const char *p = e + 1;
	if (*p == '-') { es = -1; p++; } else if (*p == '+') p++;
	while (*p >= '0' && *p <= '9') { if (ex < 1000) ex = ex * 10 + (*p - '0'); p++; }
	ex *= es;
	// digits and where the point is
	char dg[64];
	int nd = 0, point = -1;
	bool neg = false;
	for (p = s; p < e; p++) {
		if (*p == '-') neg = true;
		else if (*p == '.') point = nd;
		else if (*p >= '0' && *p <= '9' && nd < 63) dg[nd++] = *p;
	}
	if (point < 0) point = nd;
	point += ex;
	int o = 0;
	if (neg && o < cap - 1) out[o++] = '-';
	if (point <= 0) {
		if (-point > 24) { copy(out, "0", cap); return; }		// below what the sheet holds
		out[o++] = '0';
		out[o++] = '.';
		for (int i = 0; i < -point && o < cap - 2; i++) out[o++] = '0';
		for (int i = 0; i < nd && o < cap - 1; i++) out[o++] = dg[i];
	} else {
		if (point > 30) { copy(out, s, cap); return; }		// too big for anything here
		for (int i = 0; i < point && o < cap - 2; i++) out[o++] = i < nd ? dg[i] : '0';
		if (nd > point) {
			out[o++] = '.';
			for (int i = point; i < nd && o < cap - 1; i++) out[o++] = dg[i];
		}
	}
	out[o] = 0;
}

// -- column names --

static int col_name(uint32_t col, char *out) {
	char t[8];
	int n = 0;
	col++;
	while (col && n < 7) { col--; t[n++] = (char)('A' + col % 26); col /= 26; }
	for (int i = 0; i < n; i++) out[i] = t[n - 1 - i];
	out[n] = 0;
	return n;
}

static int ref_name(uint32_t row, uint32_t col, char *out) {
	int n = col_name(col, out);
	return n + sprintf(out + n, "%u", (unsigned)(row + 1));
}

// "AB12" -> row 11, col 27. Returns the characters used, 0 if none.
static int parse_ref(const char *s, uint32_t *row, uint32_t *col) {
	const char *p = s;
	uint32_t c = 0, r = 0;
	if (*p == '$') p++;
	int nl = 0;
	while (((*p | 32) >= 'a' && (*p | 32) <= 'z') && nl < 4) { c = c * 26 + (uint32_t)((*p | 32) - 'a' + 1); p++; nl++; }
	if (!nl) return 0;
	if (*p == '$') p++;
	int nd = 0;
	while (*p >= '0' && *p <= '9' && nd < 8) { r = r * 10 + (uint32_t)(*p - '0'); p++; nd++; }
	if (!nd || !r) return 0;
	*row = r - 1;
	*col = c - 1;
	return (int)(p - s);
}

// -- formulas --

static const char *const fn_ok[] = { "SUM", "AVERAGE", "AVG", "MIN", "MAX", "COUNT", "ABS", "INT", "ROUND",
	"MOD", "SQRT", "POWER", "IF", "AND", "OR", "NOT", "PMT", "PV", "FV", "NPV", "VLOOKUP", "INDEX", NULL };

// `f` (no leading '=') in the sheet's syntax as "=...", if the sheet
// can evaluate it.
static bool formula_in(const char *f, char *out, int cap) {
	int o = 0;
	out[o++] = '=';
	const char *p = f;
	while (*p == '=') p++;
	while (*p) {
		if (o >= cap - 12) return false;
		char c = *p;
		if (c == ' ' || c == '+' || c == '-' || c == '*' || c == '/' || c == '(' || c == ')' || c == ',' || c == ':' ||
			c == '^' || c == '<' || c == '>' || c == '=') {
			out[o++] = c;
			p++;
			continue;
		}
		if ((c >= '0' && c <= '9') || c == '.') {
			while ((*p >= '0' && *p <= '9') || *p == '.') out[o++] = *p++;
			if (*p == 'e' || *p == 'E' || *p == '%') return false;
			continue;
		}
		if (((c | 32) >= 'a' && (c | 32) <= 'z') || c == '$') {
			const char *q = p;
			while (((*q | 32) >= 'a' && (*q | 32) <= 'z') || *q == '.' || *q == '_') q++;
			if (*q == '(' && c != '$') {
				int n = (int)(q - p);
				bool ok = false;
				for (int i = 0; fn_ok[i]; i++) if ((int)strlen(fn_ok[i]) == n && zf_ieq_n(p, fn_ok[i], n)) ok = true;
				if (!ok) return false;
				for (int i = 0; i < n; i++) out[o++] = (char)(p[i] & ~32);
				p = q;
				continue;
			}
			if (c != '$' && (q - p == 4 || q - p == 5) && (zf_ieq_n(p, "TRUE", 4) || zf_ieq_n(p, "FALSE", 5))) {
				int n = (int)(q - p);
				if ((n == 4 && zf_ieq_n(p, "TRUE", 4)) || (n == 5 && zf_ieq_n(p, "FALSE", 5))) {
					for (int i = 0; i < n; i++) out[o++] = (char)(p[i] & ~32);
					p = q;
					continue;
				}
			}
			uint32_t r, cl;
			int k = parse_ref(p, &r, &cl);
			if (!k || cl >= SHEET_COLS || r >= SHEET_ROWS) return false;
			// as written, $ and all ($ pins a part when the sheet copies it)
			for (int i = 0; i < k; i++) out[o++] = (char)((p[i] >= 'a' && p[i] <= 'z') ? p[i] - 32 : p[i]);
			p += k;
			continue;
		}
		return false;		// strings, &, other sheets ...
	}
	out[o] = 0;
	return o < SHEET_SRC_MAX;
}

// ods: "of:=SUM([.A1:.B3];2)" -> "SUM(A1:B3,2)", or false for a
// reference to another sheet.
static bool ods_formula_plain(const char *f, char *out, int cap) {
	const char *p = f;
	if (!strncmp(p, "of:", 3) || !strncmp(p, "oooc:", 5)) p = strchr(p, ':') + 1;
	while (*p == '=') p++;
	int o = 0;
	while (*p && o < cap - 2) {
		if (*p == '[') {
			p++;
			while (*p && *p != ']') {
				if (*p == '.') { p++; continue; }
				if (*p == '\'' || *p == '$') {
					// '$Sheet2'.A1 -- or a $ that is an absolute marker
					if (*p == '\'') return false;
					const char *q = p + 1;
					while (((*q | 32) >= 'a' && (*q | 32) <= 'z')) q++;
					if (*q == '.') return false;		// $Sheet.A1
				}
				if (((*p | 32) >= 'a' && (*p | 32) <= 'z')) {
					const char *q = p;
					while (((*q | 32) >= 'a' && (*q | 32) <= 'z') || (*q >= '0' && *q <= '9') || *q == '_') q++;
					if (*q == '.') return false;		// Sheet2.A1
				}
				if (o >= cap - 2) return false;
				out[o++] = *p++;
			}
			if (*p == ']') p++;
			continue;
		}
		out[o++] = *p == ';' ? ',' : *p;
		p++;
	}
	out[o] = 0;
	return !*p;
}

// The sheet's "=SUM(A1:A3)" for xlsx ("SUM(A1:A3)") or ods
// ("of:=SUM([.A1:.A3])").
static void formula_out(const char *f, bool ods, char *out, int cap) {
	int o = 0;
	if (*f == '=') f++;
	if (ods) { strcpy(out, "of:="); o = 4; }
	const char *p = f;
	while (*p && o < cap - 16) {
		char c = *p;
		if (((c | 32) >= 'a' && (c | 32) <= 'z') || c == '$') {
			const char *q = p;
			while (((*q | 32) >= 'a' && (*q | 32) <= 'z')) q++;
			if (*q == '(') {
				int n = (int)(q - p);
				if (n == 3 && zf_ieq_n(p, "AVG", 3)) { memcpy(out + o, "AVERAGE", 7); o += 7; }
				else { for (int i = 0; i < n; i++) out[o++] = (char)(p[i] & ~32); }
				p = q;
				continue;
			}
			uint32_t r, cl, r2, c2;
			int k = parse_ref(p, &r, &cl);
			if (!k) { out[o++] = *p++; continue; }
			// references as written, $ and all, upper-cased
			#define COPYREF(q, len) for (int i_ = 0; i_ < (len); i_++) out[o++] = (char)(((q)[i_] >= 'a' && (q)[i_] <= 'z') ? (q)[i_] - 32 : (q)[i_])
			if (ods) {
				out[o++] = '[';
				out[o++] = '.';
				COPYREF(p, k);
				p += k;
				int k2;
				if (*p == ':' && (k2 = parse_ref(p + 1, &r2, &c2)) > 0) {
					out[o++] = ':';
					out[o++] = '.';
					COPYREF(p + 1, k2);
					p += 1 + k2;
				}
				out[o++] = ']';
			} else {
				COPYREF(p, k);
				p += k;
			}
			#undef COPYREF
			continue;
		}
		out[o++] = (ods && c == ',') ? ';' : c;
		p++;
	}
	out[o] = 0;
}

// -- writers --

typedef struct {
	zf_tw_kind_t	kind;
	zf_io_t			*out;
	zw_t			zw;
	bool			zip;
	uint32_t		row, col;		// next expected position
	bool			row_open;
	bool			any;
	int				err;
} tw_t;

_Static_assert(sizeof(tw_t) <= ZF_TABW_STATE, "ZF_TABW_STATE too small");

static int tput(tw_t *w, const char *s, uint32_t n) {
	if (w->err) return w->err;
	int r;
	if (w->zip) r = zw_write(&w->zw, s, n);
	else r = zf_write(w->out, s, n);
	if (r < 0) w->err = r;
	return w->err;
}
static int tputs(tw_t *w, const char *s) { return tput(w, s, (uint32_t)strlen(s)); }

static int txml(tw_t *w, const char *s) {
	const char *a = s;
	for (; *s; s++) {
		const char *rep = NULL;
		unsigned char c = (unsigned char)*s;
		if (c == '&') rep = "&amp;";
		else if (c == '<') rep = "&lt;";
		else if (c == '>') rep = "&gt;";
		else if (c == '"') rep = "&quot;";
		else if (c < 0x20 && c != '\t' && c != '\n') rep = "";
		if (!rep) continue;
		if (s > a) tput(w, a, (uint32_t)(s - a));
		tputs(w, rep);
		a = s + 1;
	}
	if (s > a) tput(w, a, (uint32_t)(s - a));
	return w->err;
}

static const char xlsx_types[] =
	"<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"
	"<Types xmlns=\"http://schemas.openxmlformats.org/package/2006/content-types\">"
	"<Default Extension=\"rels\" ContentType=\"application/vnd.openxmlformats-package.relationships+xml\"/>"
	"<Default Extension=\"xml\" ContentType=\"application/xml\"/>"
	"<Override PartName=\"/xl/workbook.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.spreadsheetml.sheet.main+xml\"/>"
	"<Override PartName=\"/xl/worksheets/sheet1.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.spreadsheetml.worksheet+xml\"/>"
	"<Override PartName=\"/xl/styles.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.spreadsheetml.styles+xml\"/>"
	"</Types>";

static const char xlsx_rels[] =
	"<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"
	"<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">"
	"<Relationship Id=\"rId1\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/officeDocument\" Target=\"xl/workbook.xml\"/>"
	"</Relationships>";

static const char xlsx_workbook[] =
	"<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"
	"<workbook xmlns=\"http://schemas.openxmlformats.org/spreadsheetml/2006/main\""
	" xmlns:r=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships\">"
	"<sheets><sheet name=\"Sheet1\" sheetId=\"1\" r:id=\"rId1\"/></sheets></workbook>";

static const char xlsx_wb_rels[] =
	"<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"
	"<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">"
	"<Relationship Id=\"rId1\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/worksheet\" Target=\"worksheets/sheet1.xml\"/>"
	"<Relationship Id=\"rId2\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/styles\" Target=\"styles.xml\"/>"
	"</Relationships>";

static const char xlsx_styles[] =
	"<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"
	"<styleSheet xmlns=\"http://schemas.openxmlformats.org/spreadsheetml/2006/main\">"
	"<fonts count=\"1\"><font><sz val=\"11\"/><name val=\"Calibri\"/></font></fonts>"
	"<fills count=\"2\"><fill><patternFill patternType=\"none\"/></fill><fill><patternFill patternType=\"gray125\"/></fill></fills>"
	"<borders count=\"1\"><border><left/><right/><top/><bottom/><diagonal/></border></borders>"
	"<cellStyleXfs count=\"1\"><xf numFmtId=\"0\" fontId=\"0\" fillId=\"0\" borderId=\"0\"/></cellStyleXfs>"
	"<cellXfs count=\"1\"><xf numFmtId=\"0\" fontId=\"0\" fillId=\"0\" borderId=\"0\" xfId=\"0\"/></cellXfs>"
	"<cellStyles count=\"1\"><cellStyle name=\"Normal\" xfId=\"0\" builtinId=\"0\"/></cellStyles>"
	"</styleSheet>";

static const char ods_manifest[] =
	"<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
	"<manifest:manifest xmlns:manifest=\"urn:oasis:names:tc:opendocument:xmlns:manifest:1.0\" manifest:version=\"1.2\">"
	"<manifest:file-entry manifest:full-path=\"/\" manifest:media-type=\"application/vnd.oasis.opendocument.spreadsheet\"/>"
	"<manifest:file-entry manifest:full-path=\"content.xml\" manifest:media-type=\"text/xml\"/>"
	"</manifest:manifest>";

static int tw_begin(zf_tab_t *t) {
	tw_t *w = (tw_t *)t->ctx;
	switch (w->kind) {
	case ZF_TW_ZSS:
		tputs(w, "zsheet 1\n");
		break;
	case ZF_TW_XLSX:
		zw_file(&w->zw, "[Content_Types].xml", xlsx_types);
		zw_file(&w->zw, "_rels/.rels", xlsx_rels);
		zw_file(&w->zw, "xl/workbook.xml", xlsx_workbook);
		zw_file(&w->zw, "xl/_rels/workbook.xml.rels", xlsx_wb_rels);
		zw_file(&w->zw, "xl/styles.xml", xlsx_styles);
		zw_begin(&w->zw, "xl/worksheets/sheet1.xml");
		tputs(w, "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"
			"<worksheet xmlns=\"http://schemas.openxmlformats.org/spreadsheetml/2006/main\"><sheetData>");
		break;
	case ZF_TW_ODS:
		zw_file(&w->zw, "mimetype", "application/vnd.oasis.opendocument.spreadsheet");
		zw_file(&w->zw, "META-INF/manifest.xml", ods_manifest);
		zw_begin(&w->zw, "content.xml");
		tputs(w, "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
			"<office:document-content xmlns:office=\"urn:oasis:names:tc:opendocument:xmlns:office:1.0\""
			" xmlns:table=\"urn:oasis:names:tc:opendocument:xmlns:table:1.0\""
			" xmlns:text=\"urn:oasis:names:tc:opendocument:xmlns:text:1.0\""
			" xmlns:of=\"urn:oasis:names:tc:opendocument:xmlns:of:1.2\""
			" office:version=\"1.2\"><office:body><office:spreadsheet>"
			"<table:table table:name=\"Sheet1\">");
		break;
	default:
		break;
	}
	return w->err;
}

// Moves to (row, col): ends rows, writes the empty ones between.
static void tw_goto(tw_t *w, uint32_t row, uint32_t col) {
	char b[96];
	if (w->row_open && row != w->row) {
		switch (w->kind) {
		case ZF_TW_CSV: case ZF_TW_TSV: tputs(w, "\r\n"); break;
		case ZF_TW_XLSX: tputs(w, "</row>"); break;
		case ZF_TW_ODS: tputs(w, "</table:table-row>"); break;
		default: break;
		}
		w->row_open = false;
		w->row++;
		w->col = 0;
	}
	if (!w->row_open) {
		uint32_t gap = row - w->row;
		if (gap) {
			if (w->kind == ZF_TW_CSV || w->kind == ZF_TW_TSV)
				for (uint32_t i = 0; i < gap && !w->err; i++) tputs(w, "\r\n");
			else if (w->kind == ZF_TW_ODS) {
				snprintf(b, sizeof b, "<table:table-row table:number-rows-repeated=\"%u\"><table:table-cell/></table:table-row>", (unsigned)gap);
				tputs(w, b);
			}
		}
		w->row = row;
		w->col = 0;
		w->row_open = true;
		if (w->kind == ZF_TW_XLSX) { snprintf(b, sizeof b, "<row r=\"%u\">", (unsigned)(row + 1)); tputs(w, b); }
		else if (w->kind == ZF_TW_ODS) tputs(w, "<table:table-row>");
	}
	if (col > w->col) {
		uint32_t gap = col - w->col;
		if (w->kind == ZF_TW_ODS) {
			snprintf(b, sizeof b, "<table:table-cell table:number-columns-repeated=\"%u\"/>", (unsigned)gap);
			tputs(w, b);
			w->col = col;
		}
		// csv and tsv: the separators are written with the cell
	}
}

// CSV: quoted when it has to be; TSV: tabs and newlines become spaces.
static void sv_field(tw_t *w, const char *v) {
	if (w->kind == ZF_TW_TSV) {
		const char *a = v;
		for (; *v; v++) {
			if (*v != '\t' && *v != '\n' && *v != '\r') continue;
			if (v > a) tput(w, a, (uint32_t)(v - a));
			tputs(w, " ");
			a = v + 1;
		}
		if (v > a) tput(w, a, (uint32_t)(v - a));
		return;
	}
	bool q = *v == ' ' || strpbrk(v, ",\"\r\n") != NULL;
	if (*v && v[strlen(v) - 1] == ' ') q = true;
	if (!q) { tputs(w, v); return; }
	tputs(w, "\"");
	const char *a = v;
	for (; *v; v++) {
		if (*v != '"') continue;
		tput(w, a, (uint32_t)(v - a + 1));
		tputs(w, "\"");
		a = v + 1;
	}
	if (v > a) tput(w, a, (uint32_t)(v - a));
	tputs(w, "\"");
}

static int tw_cell(zf_tab_t *t, uint32_t row, uint32_t col, int kind, const char *value, const char *formula) {
	tw_t *w = (tw_t *)t->ctx;
	static char b[1024], nv[96], fo[600];
	if (w->err) return w->err;
	if (!value) value = "";
	if (w->any && (row < w->row || (row == w->row && col < w->col))) return ZF_OK;	// out of order: dropped
	if (kind == ZF_C_NUM && !zf_is_num(value)) kind = ZF_C_TEXT;
	if (kind == ZF_C_FORMULA && !formula) kind = zf_is_num(value) ? ZF_C_NUM : ZF_C_TEXT;
	const char *num = value;
	if (kind != ZF_C_TEXT && zf_is_num(value)) { num_plain(value, nv, sizeof nv); num = nv; }

	if (w->kind == ZF_TW_ZSS) {
		if (col >= SHEET_COLS || row >= SHEET_ROWS) { zf_tab_dropped++; return ZF_OK; }
		char ref[8];
		ref_name(row, col, ref);
		const char *src;
		bool force = false;
		if (kind == ZF_C_FORMULA) src = formula;
		else if (kind == ZF_C_NUM) src = num;
		else {
			src = value;
			z_fix_t dummy;
			force = *src == '=' || *src == '\'' || z_fix_parse_all(src, SHEET_DP, &dummy);
		}
		if (!*src) return ZF_OK;
		// one line, at most the sheet's length, cut between characters
		int o = 0, max = SHEET_SRC_MAX - 1 - (force ? 1 : 0);
		if (force) b[o++] = '\'';
		int start = o;
		for (const char *p = src; *p && o - start < max; p++) {
			unsigned char c = (unsigned char)*p;
			if ((c & 0xC0) != 0x80 && o - start + 4 > max) {
				// would a whole character still fit?
				int need = c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : c >= 0xC0 ? 2 : 1;
				if (o - start + need > max) break;
			}
			b[o++] = (c == '\n' || c == '\r' || c == '\t') ? ' ' : (char)c;
		}
		b[o] = 0;
		tputs(w, ref);
		tputs(w, " ");
		tputs(w, b);
		tputs(w, "\n");
		w->any = true;
		return w->err;
	}

	tw_goto(w, row, col);
	w->any = true;

	switch (w->kind) {
	case ZF_TW_CSV: case ZF_TW_TSV:
		// fields 0..col-1 written so far: one separator after each
		// written one, and one per field skipped
		for (uint32_t i = w->col + (w->col ? 0 : 1); i <= col; i++)
			if (i) tputs(w, w->kind == ZF_TW_CSV ? "," : "\t");
		sv_field(w, kind == ZF_C_TEXT ? value : num);
		w->col = col + 1;
		break;
	case ZF_TW_XLSX: {
		char ref[16];
		ref_name(row, col, ref);
		if (kind == ZF_C_TEXT) {
			snprintf(b, sizeof b, "<c r=\"%s\" t=\"inlineStr\"><is><t xml:space=\"preserve\">", ref);
			tputs(w, b);
			txml(w, value);
			tputs(w, "</t></is></c>");
		} else if (kind == ZF_C_NUM) {
			snprintf(b, sizeof b, "<c r=\"%s\"><v>%s</v></c>", ref, num);
			tputs(w, b);
		} else {
			formula_out(formula, false, fo, sizeof fo);
			bool isnum = zf_is_num(value);
			snprintf(b, sizeof b, "<c r=\"%s\"%s><f>", ref, isnum || !*value ? "" : " t=\"str\"");
			tputs(w, b);
			txml(w, fo);
			tputs(w, "</f>");
			if (*value) { tputs(w, "<v>"); txml(w, isnum ? num : value); tputs(w, "</v>"); }
			tputs(w, "</c>");
		}
		w->col = col + 1;
		break;
	}
	case ZF_TW_ODS:
		if (kind == ZF_C_TEXT) {
			tputs(w, "<table:table-cell office:value-type=\"string\"><text:p>");
			txml(w, value);
			tputs(w, "</text:p></table:table-cell>");
		} else {
			tputs(w, "<table:table-cell");
			if (kind == ZF_C_FORMULA) {
				formula_out(formula, true, fo, sizeof fo);
				tputs(w, " table:formula=\"");
				txml(w, fo);
				tputs(w, "\"");
			}
			if (zf_is_num(num)) {
				snprintf(b, sizeof b, " office:value-type=\"float\" office:value=\"%s\"><text:p>%s</text:p></table:table-cell>", num, num);
				tputs(w, b);
			} else {
				tputs(w, " office:value-type=\"string\" office:string-value=\"");
				txml(w, value);
				tputs(w, "\"><text:p>");
				txml(w, value);
				tputs(w, "</text:p></table:table-cell>");
			}
		}
		w->col = col + 1;
		break;
	default:
		break;
	}
	return w->err;
}

static int tw_end(zf_tab_t *t) {
	tw_t *w = (tw_t *)t->ctx;
	switch (w->kind) {
	case ZF_TW_CSV: case ZF_TW_TSV:
		if (w->row_open) tputs(w, "\r\n");
		break;
	case ZF_TW_XLSX:
		if (w->row_open) tputs(w, "</row>");
		tputs(w, "</sheetData></worksheet>");
		if (!w->err) w->err = zw_end(&w->zw);
		if (!w->err) w->err = zw_finish(&w->zw);
		break;
	case ZF_TW_ODS:
		if (w->row_open) tputs(w, "</table:table-row>");
		if (!w->any) tputs(w, "<table:table-row><table:table-cell/></table:table-row>");
		tputs(w, "</table:table></office:spreadsheet></office:body></office:document-content>");
		if (!w->err) w->err = zw_end(&w->zw);
		if (!w->err) w->err = zw_finish(&w->zw);
		break;
	default:
		break;
	}
	return w->err;
}

int zf_tabw_open(zf_tab_t *t, void *state, zf_tw_kind_t kind, zf_io_t *out) {
	tw_t *w = (tw_t *)state;
	memset(w, 0, sizeof(*w));
	w->kind = kind;
	w->out = out;
	w->zip = (kind == ZF_TW_XLSX || kind == ZF_TW_ODS);
	if (w->zip) zw_init(&w->zw, out);
	t->ctx = w;
	t->begin = tw_begin;
	t->cell = tw_cell;
	t->end = tw_end;
	return ZF_OK;
}

// -- csv and tsv --

#define FIELD_MAX	4096

// Bytes that are not UTF-8 as Windows-1252, in place into `out`.
static uint32_t fix_utf8(const uint8_t *s, uint32_t n, char *out, uint32_t cap) {
	uint32_t o = 0;
	for (uint32_t i = 0; i < n && o + 4 < cap; ) {
		uint8_t c = s[i];
		int need = c < 0x80 ? 0 : (c >= 0xC2 && c <= 0xDF) ? 1 : (c >= 0xE0 && c <= 0xEF) ? 2 : (c >= 0xF0 && c <= 0xF4) ? 3 : -1;
		bool ok = need >= 0 && i + (uint32_t)need < n;
		for (int k = 1; ok && k <= need; k++) if ((s[i + (uint32_t)k] & 0xC0) != 0x80) ok = false;
		if (ok) { for (int k = 0; k <= need; k++) out[o++] = (char)s[i++]; }
		else { o += (uint32_t)zf_utf8_put(zf_cp1252(c), out + o); i++; }
	}
	out[o] = 0;
	return o;
}

int zf_read_csv(zf_io_t *in, zf_tab_t *t, char sep) {
	static zf_src_t src;
	static zf_rd_t rd;
	static uint8_t f[FIELD_MAX];
	static char v[FIELD_MAX * 3 / 2 + 8];
	zf_src_io(&src, in);
	zf_rd_init(&rd, &src);
	if (zf_rd_peek(&rd) == 0xEF) { zf_rd_getc(&rd); if (zf_rd_peek(&rd) == 0xBB) zf_rd_getc(&rd); if (zf_rd_peek(&rd) == 0xBF) zf_rd_getc(&rd); }
	int r = t->begin(t);
	if (r < 0) return r;
	uint32_t row = 0, col = 0, n = 0, ticks = 0;
	bool quoted = false, was_quoted = false, any = false;
	for (;;) {
		if (cancelled(&ticks)) return ZF_E_CANCEL;
		int c = zf_rd_getc(&rd);
		if (quoted) {
			if (c < 0) break;
			if (c == '"') {
				if (zf_rd_peek(&rd) == '"') { zf_rd_getc(&rd); if (n < FIELD_MAX) f[n++] = '"'; }
				else quoted = false;
				continue;
			}
			if (n < FIELD_MAX) f[n++] = (uint8_t)c;
			continue;
		}
		if (c == '"' && n == 0 && sep == ',') { quoted = was_quoted = true; any = true; continue; }
		if (c < 0 || c == sep || c == '\n' || c == '\r') {
			if (c < 0 && !any && !n) break;
			// trim a CSV field's spaces only when it was not quoted
			uint32_t m = fix_utf8(f, n, v, sizeof v);
			if (m) {
				int kind = (!was_quoted && zf_is_num(v)) ? ZF_C_NUM : ZF_C_TEXT;
				r = t->cell(t, row, col, kind, v, NULL);
				if (r < 0) return r;
			}
			n = 0;
			was_quoted = false;
			if (c == sep) { col++; any = true; continue; }
			if (c == '\r' && zf_rd_peek(&rd) == '\n') zf_rd_getc(&rd);
			if (c < 0) break;
			row++;
			col = 0;
			any = false;
			continue;
		}
		any = true;
		if (n < FIELD_MAX) f[n++] = (uint8_t)c;
	}
	if (rd.err) return rd.err;
	return t->end(t);
}

// -- zss --

static sheet_t zsheet;

int zf_read_zss(zf_io_t *in, zf_tab_t *t) {
	static zf_src_t src;
	static zf_rd_t rd;
	static char line[256];
	static char val[64];
	zf_src_io(&src, in);
	zf_rd_init(&rd, &src);
	sheet_load_begin(&zsheet);
	uint32_t n = 0, ticks = 0;
	bool first = true;
	for (;;) {
		int c = zf_rd_getc(&rd);
		if (c < 0 || c == '\n') {
			line[n] = 0;
			if (first && n && strncmp(line, "zsheet", 6)) return ZF_E_FORMAT;
			first = false;
			if (n) sheet_load_line(&zsheet, line);
			n = 0;
			if (c < 0) break;
			if (cancelled(&ticks)) return ZF_E_CANCEL;
			continue;
		}
		if (n < sizeof line - 1) line[n++] = (char)c;
	}
	if (rd.err) return rd.err;
	sheet_load_end(&zsheet);
	int r = t->begin(t);
	if (r < 0) return r;
	for (int i = 0; i < zsheet.ncells; i++) {
		if (cancelled(&ticks)) return ZF_E_CANCEL;
		sheet_cell_t *c = &zsheet.cells[i];
		int row = c->key / SHEET_COLS, col = c->key % SHEET_COLS;
		const char *src_ = sheet_src(&zsheet, row, col);
		if (c->kind == SHEET_KIND_TEXT) {
			r = t->cell(t, (uint32_t)row, (uint32_t)col, ZF_C_TEXT, *src_ == '\'' ? src_ + 1 : src_, NULL);
		} else if (c->kind == SHEET_KIND_NUM) {
			z_fix_t v;
			sheet_value(&zsheet, row, col, &v);
			z_fix_format(v, SHEET_DP, Z_FIX_STRIP, val, sizeof val);
			r = t->cell(t, (uint32_t)row, (uint32_t)col, ZF_C_NUM, val, NULL);
		} else {
			z_fix_t v;
			sheet_err_t e = sheet_value(&zsheet, row, col, &v);
			if (e != SHEET_OK) copy(val, sheet_err_text(e), sizeof val);
			else z_fix_format(v, SHEET_DP, Z_FIX_STRIP, val, sizeof val);
			r = t->cell(t, (uint32_t)row, (uint32_t)col, ZF_C_FORMULA, val, src_);
		}
		if (r < 0) return r;
	}
	return t->end(t);
}

// -- xlsx --

// Shared strings in zf_heap: the text from the bottom, an offset per
// string from the top.
static uint32_t ss_count, ss_bot;

static const char *ss_get(uint32_t i) {
	if (i >= ss_count) return "";
	uint32_t *idx = (uint32_t *)(void *)(zf_heap + zf_heap_size) - 1 - i;
	return (const char *)zf_heap + *idx;
}

static int ss_load(zz_t *z, const char *name) {
	ss_count = 0;
	ss_bot = 0;
	int r = zz_ent_open(z, name);
	if (r == ZF_E_FORMAT) { zz_ent_close(); return ZF_OK; }		// no shared strings
	if (r < 0) { zz_ent_close(); return r; }
	zx_t *x = &zz_ent.x;
	bool in_si = false, in_t = false;
	int skip = 0;
	uint32_t ticks = 0;
	uint32_t top = zf_heap_size & ~3u;
	for (;;) {
		zx_tok_t t = zx_next(x);
		if (t == ZX_EOF) break;
		if (t == ZX_ERR) { r = zz_ent.rd.err ? zz_ent.rd.err : ZF_E_CORRUPT; break; }
		if (cancelled(&ticks)) { r = ZF_E_CANCEL; break; }
		const char *n = zx_local(x->name);
		if (skip) { if (t == ZX_START) skip++; else if (t == ZX_END) skip--; continue; }
		if (t == ZX_START) {
			if (!strcmp(n, "rPh") || !strcmp(n, "phoneticPr")) { skip = 1; continue; }
			if (!strcmp(n, "si")) {
				if (top - 4 < ss_bot + 1) { r = ZF_E_NOMEM; break; }
				top -= 4;
				*(uint32_t *)(void *)(zf_heap + top) = ss_bot;
				ss_count++;
				in_si = true;
			} else if (!strcmp(n, "t")) in_t = true;
		} else if (t == ZX_END) {
			if (!strcmp(n, "si")) {
				if (ss_bot + 1 > top) { r = ZF_E_NOMEM; break; }
				zf_heap[ss_bot++] = 0;
				in_si = false;
			} else if (!strcmp(n, "t")) in_t = false;
		} else if (t == ZX_TEXT && in_si && in_t) {
			if (ss_bot + x->tlen + 1 > top) { r = ZF_E_NOMEM; break; }
			memcpy(zf_heap + ss_bot, x->text, x->tlen);
			ss_bot += x->tlen;
		}
	}
	int e2 = zz_ent_close();
	if (r >= 0 && e2 < 0) r = e2;
	return r < 0 ? r : ZF_OK;
}

// The first sheet's entry name, and the shared strings', through the
// workbook and its relationships.
static void xlsx_parts(zz_t *z, char *sheet, char *strings) {
	char rid[32] = "";
	strcpy(sheet, "xl/worksheets/sheet1.xml");
	strcpy(strings, "xl/sharedStrings.xml");
	if (zz_ent_open(z, "xl/workbook.xml") == ZF_OK) {
		for (;;) {
			zx_tok_t t = zx_next(&zz_ent.x);
			if (t == ZX_EOF || t == ZX_ERR) break;
			if (t == ZX_START && !strcmp(zx_local(zz_ent.x.name), "sheet")) {
				const char *s = zx_attr_local(&zz_ent.x, "id");
				copy(rid, s, sizeof rid);
				break;
			}
		}
	}
	zz_ent_close();
	if (zz_ent_open(z, "xl/_rels/workbook.xml.rels") == ZF_OK) {
		for (;;) {
			zx_tok_t t = zx_next(&zz_ent.x);
			if (t == ZX_EOF || t == ZX_ERR) break;
			if (t != ZX_START || strcmp(zx_local(zz_ent.x.name), "Relationship")) continue;
			const char *id = zx_attr(&zz_ent.x, "Id"), *tg = zx_attr(&zz_ent.x, "Target"), *ty = zx_attr(&zz_ent.x, "Type");
			if (!tg || !ty) continue;
			char *dst = NULL;
			int tl = (int)strlen(ty);
			if (id && rid[0] && !strcmp(id, rid)) dst = sheet;
			else if (tl > 14 && !strcmp(ty + tl - 14, "/sharedStrings")) dst = strings;
			if (!dst) continue;
			if (*tg == '/') copy(dst, tg + 1, ZF_PATH_MAX);
			else { strcpy(dst, "xl/"); copy(dst + 3, tg, ZF_PATH_MAX - 3); }
		}
	}
	zz_ent_close();
}

int zf_read_xlsx(zf_io_t *in, zf_tab_t *t) {
	static zz_t z;
	static char sheet[ZF_PATH_MAX], strings[ZF_PATH_MAX];
	static char f[600], v[FIELD_MAX], fsheet[SHEET_SRC_MAX + 16];
	int r = zz_open(&z, in);
	if (r < 0) return r;
	if (!zf_heap || zf_heap_size < 1024) return ZF_E_NOMEM;
	xlsx_parts(&z, sheet, strings);
	if ((r = ss_load(&z, strings)) < 0) return r;
	if ((r = zz_ent_open(&z, sheet)) < 0) { zz_ent_close(); return r; }
	if ((r = t->begin(t)) < 0) { zz_ent_close(); return r; }

	zx_t *x = &zz_ent.x;
	uint32_t row = 0, col = 0, ticks = 0;
	char ctype[12] = "";
	bool in_c = false, in_v = false, in_f = false, in_is_t = false, in_is = false;
	uint32_t fl = 0, vl = 0;
	int skip = 0;
	for (;;) {
		zx_tok_t tk = zx_next(x);
		if (tk == ZX_EOF) break;
		if (tk == ZX_ERR) { r = zz_ent.rd.err ? zz_ent.rd.err : ZF_E_CORRUPT; break; }
		if (cancelled(&ticks)) { r = ZF_E_CANCEL; break; }
		const char *n = zx_local(x->name);
		if (skip) { if (tk == ZX_START) skip++; else if (tk == ZX_END) skip--; continue; }
		if (tk == ZX_START) {
			if (!strcmp(n, "row")) {
				const char *rr = zx_attr(x, "r");
				if (rr) { uint32_t k = (uint32_t)strtoul_(rr); if (k) row = k - 1; }
				col = 0;
			} else if (!strcmp(n, "c")) {
				const char *rr = zx_attr(x, "r");
				uint32_t r2, c2;
				if (rr && parse_ref(rr, &r2, &c2)) { row = r2; col = c2; }
				copy(ctype, zx_attr(x, "t"), sizeof ctype);
				in_c = true;
				fl = vl = 0;
				f[0] = v[0] = 0;
			} else if (in_c && !strcmp(n, "v")) in_v = true;
			else if (in_c && !strcmp(n, "f")) in_f = true;
			else if (in_c && !strcmp(n, "is")) in_is = true;
			else if (in_is && !strcmp(n, "t")) in_is_t = true;
			else if (in_is && !strcmp(n, "rPh")) skip = 1;
		} else if (tk == ZX_END) {
			if (!strcmp(n, "v")) in_v = false;
			else if (!strcmp(n, "f")) in_f = false;
			else if (!strcmp(n, "t")) in_is_t = false;
			else if (!strcmp(n, "is")) in_is = false;
			else if (!strcmp(n, "row")) row++;
			else if (!strcmp(n, "c") && in_c) {
				in_c = false;
				f[fl] = 0;
				v[vl] = 0;
				const char *val = v;
				int kind = ZF_C_NUM;
				if (!strcmp(ctype, "s")) { val = ss_get((uint32_t)strtoul_(v)); kind = ZF_C_TEXT; }
				else if (!strcmp(ctype, "inlineStr") || !strcmp(ctype, "str") || !strcmp(ctype, "e")) kind = ZF_C_TEXT;
				else if (!strcmp(ctype, "b")) { val = (v[0] == '1') ? "TRUE" : "FALSE"; kind = ZF_C_TEXT; }
				const char *form = NULL;
				if (fl && formula_in(f, fsheet, sizeof fsheet)) { form = fsheet; kind = ZF_C_FORMULA; }
				if (*val || form) {
					r = t->cell(t, row, col, kind, val, form);
					if (r < 0) break;
				}
				col++;
			}
		} else if (tk == ZX_TEXT) {
			if (in_f) { uint32_t k = x->tlen; if (fl + k > sizeof f - 1) k = sizeof f - 1 - fl; memcpy(f + fl, x->text, k); fl += k; }
			else if (in_v || in_is_t) { uint32_t k = x->tlen; if (vl + k > sizeof v - 1) k = sizeof v - 1 - vl; memcpy(v + vl, x->text, k); vl += k; }
		}
	}
	int e2 = zz_ent_close();
	if (r >= 0 && e2 < 0) r = e2;
	if (r < 0) return r;
	return t->end(t);
}

// -- ods --

int zf_read_ods(zf_io_t *in, zf_tab_t *t) {
	static zz_t z;
	static char v[FIELD_MAX], val[96], fplain[600], fsheet[SHEET_SRC_MAX + 16];
	static char formula[600];
	int r = zz_open(&z, in);
	if (r < 0) return r;
	if ((r = zz_ent_open(&z, "content.xml")) < 0) { zz_ent_close(); return r; }
	if ((r = t->begin(t)) < 0) { zz_ent_close(); return r; }

	zx_t *x = &zz_ent.x;
	uint32_t row = 0, col = 0, ticks = 0, vl = 0;
	uint32_t rrep = 1, crep = 1;
	int tables = 0;
	bool in_cell = false, row_any = false;
	int in_p = 0;
	bool has_val = false, is_num = false;
	int skip = 0;
	for (;;) {
		zx_tok_t tk = zx_next(x);
		if (tk == ZX_EOF) break;
		if (tk == ZX_ERR) { r = zz_ent.rd.err ? zz_ent.rd.err : ZF_E_CORRUPT; break; }
		if (cancelled(&ticks)) { r = ZF_E_CANCEL; break; }
		const char *n = x->name;
		if (skip) { if (tk == ZX_START) skip++; else if (tk == ZX_END) skip--; continue; }
		if (tk == ZX_START) {
			if (!strcmp(n, "table:table")) { if (++tables > 1) break; }
			if (tables != 1) continue;
			if (!strcmp(n, "office:annotation")) { skip = 1; continue; }
			if (!strcmp(n, "table:table-row")) {
				const char *rr = zx_attr(x, "table:number-rows-repeated");
				rrep = rr ? (uint32_t)strtoul_(rr) : 1;
				if (!rrep) rrep = 1;
				col = 0;
				row_any = false;
			} else if (!strcmp(n, "table:table-cell") || !strcmp(n, "table:covered-table-cell")) {
				const char *cr = zx_attr(x, "table:number-columns-repeated");
				crep = cr ? (uint32_t)strtoul_(cr) : 1;
				if (!crep) crep = 1;
				const char *ty = zx_attr(x, "office:value-type");
				const char *fm = zx_attr(x, "table:formula");
				copy(formula, fm, sizeof formula);
				val[0] = 0;
				has_val = false;
				is_num = false;
				if (ty && (!strcmp(ty, "float") || !strcmp(ty, "percentage") || !strcmp(ty, "currency"))) {
					copy(val, zx_attr(x, "office:value"), sizeof val);
					has_val = is_num = val[0] != 0;
				} else if (ty && !strcmp(ty, "boolean")) {
					const char *b = zx_attr(x, "office:boolean-value");
					copy(val, b && !strcmp(b, "true") ? "TRUE" : "FALSE", sizeof val);
					has_val = true;
				} else if (ty && !strcmp(ty, "date")) {
					copy(val, zx_attr(x, "office:date-value"), sizeof val);
					has_val = val[0] != 0;
				} else if (ty && !strcmp(ty, "time")) {
					copy(val, zx_attr(x, "office:time-value"), sizeof val);
					has_val = val[0] != 0;
				}
				vl = 0;
				v[0] = 0;
				in_cell = true;
			} else if (in_cell && !strcmp(n, "text:p")) {
				if (vl && vl < sizeof v - 1) v[vl++] = ' ';
				in_p++;
			} else if (in_p && !strcmp(n, "text:s")) {
				const char *c = zx_attr(x, "text:c");
				uint32_t k = c ? (uint32_t)strtoul_(c) : 1;
				while (k-- && vl < sizeof v - 1) v[vl++] = ' ';
			} else if (in_p && !strcmp(n, "text:tab")) { if (vl < sizeof v - 1) v[vl++] = '\t'; }
			else if (in_p && !strcmp(n, "text:line-break")) { if (vl < sizeof v - 1) v[vl++] = ' '; }
		} else if (tk == ZX_END) {
			if (tables != 1) continue;
			if (!strcmp(n, "table:table")) break;
			if (!strcmp(n, "text:p")) { if (in_p) in_p--; }
			else if (in_cell && (!strcmp(n, "table:table-cell") || !strcmp(n, "table:covered-table-cell"))) {
				in_cell = false;
				v[vl] = 0;
				const char *text = has_val ? val : v;
				int kind = is_num ? ZF_C_NUM : ZF_C_TEXT;
				const char *form = NULL;
				if (formula[0] && ods_formula_plain(formula, fplain, sizeof fplain) &&
					formula_in(fplain, fsheet, sizeof fsheet)) { form = fsheet; kind = ZF_C_FORMULA; }
				if (*text || form) {
					if (crep > 1024) crep = 1024;
					for (uint32_t k = 0; k < crep && r >= 0; k++)
						r = t->cell(t, row, col + k, kind, text, form);
					if (r < 0) break;
					row_any = true;
				}
				col += crep;
			} else if (!strcmp(n, "table:table-row")) {
				// a row with content repeated: written again, a few times at most
				if (row_any && rrep > 1) {
					// repeated rows with content are rare (a block of equal
					// rows); they would need the row's cells kept to repeat,
					// so only the first is written
				}
				row += rrep;
			}
		} else if (tk == ZX_TEXT && in_p) {
			uint32_t k = x->tlen;
			if (vl + k > sizeof v - 1) k = sizeof v - 1 - vl;
			memcpy(v + vl, x->text, k);
			vl += k;
		}
	}
	int e2 = zz_ent_close();
	if (r >= 0 && e2 < 0) r = e2;
	if (r < 0) return r;
	return t->end(t);
}
