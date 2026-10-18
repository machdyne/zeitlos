/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * What a file is, and the whole of a conversion.
 */

#include <string.h>

#include "zf.h"
#include "zf_zip.h"

static const struct { const char *ext; zf_type_t t; } exts[] = {
	{ "txt", ZF_T_TXT }, { "text", ZF_T_TXT }, { "log", ZF_T_TXT },
	{ "md", ZF_T_MD }, { "markdown", ZF_T_MD },
	{ "html", ZF_T_HTML }, { "htm", ZF_T_HTML }, { "xhtml", ZF_T_HTML },
	{ "rtf", ZF_T_RTF },
	{ "docx", ZF_T_DOCX }, { "docm", ZF_T_DOCX },
	{ "odt", ZF_T_ODT },
	{ "epub", ZF_T_EPUB },
	{ "csv", ZF_T_CSV },
	{ "tsv", ZF_T_TSV }, { "tab", ZF_T_TSV },
	{ "zss", ZF_T_ZSS },
	{ "xlsx", ZF_T_XLSX }, { "xlsm", ZF_T_XLSX },
	{ "ods", ZF_T_ODS },
	{ "zip", ZF_T_ZIP },
	{ "gz", ZF_T_GZ },
	{ "tar", ZF_T_TAR },
	{ "tgz", ZF_T_TGZ },
	{ NULL, ZF_T_UNKNOWN },
};

static const char *const names[] = {
	"unknown", "text", "markdown", "html", "rtf", "docx", "odt", "epub",
	"csv", "tsv", "zss", "xlsx", "ods", "zip", "gz", "tar", "tar.gz",
};

const char *zf_type_name(zf_type_t t) {
	return (unsigned)t < sizeof names / sizeof names[0] ? names[t] : "unknown";
}

zf_type_t zf_type_from_name(const char *name) {
	if (zf_ieq(name, "tar.gz")) return ZF_T_TGZ;
	if (zf_ieq(name, "text")) return ZF_T_TXT;
	if (zf_ieq(name, "markdown")) return ZF_T_MD;
	for (int i = 0; exts[i].ext; i++) if (zf_ieq(name, exts[i].ext)) return exts[i].t;
	return ZF_T_UNKNOWN;
}

bool zf_type_is_doc(zf_type_t t) { return t >= ZF_T_TXT && t <= ZF_T_EPUB; }
bool zf_type_is_tab(zf_type_t t) { return t >= ZF_T_CSV && t <= ZF_T_ODS; }
bool zf_type_is_arch(zf_type_t t) { return t >= ZF_T_ZIP && t <= ZF_T_TGZ; }

static zf_type_t by_name(const char *name) {
	if (!name) return ZF_T_UNKNOWN;
	int n = (int)strlen(name);
	if (n > 7 && zf_ieq(name + n - 7, ".tar.gz")) return ZF_T_TGZ;
	const char *e = zf_ext(name);
	if (!e || !*e) return ZF_T_UNKNOWN;
	for (int i = 0; exts[i].ext; i++) if (zf_ieq(e, exts[i].ext)) return exts[i].t;
	return ZF_T_UNKNOWN;
}

// A zip's kind, from what is inside it.
static zf_type_t zip_kind(zf_io_t *in) {
	static zz_t z;
	static zz_entry_t e;
	if (zz_open(&z, in) < 0) return ZF_T_UNKNOWN;
	if (zz_find(&z, "word/document.xml", &e) == 1) return ZF_T_DOCX;
	if (zz_find(&z, "xl/workbook.xml", &e) == 1) return ZF_T_XLSX;
	if (zz_find(&z, "mimetype", &e) == 1 && e.usize < 100) {
		zf_type_t t = ZF_T_ZIP;
		if (zz_ent_open(&z, "mimetype") == ZF_OK) {
			char m[100];
			int n = 0, c;
			while (n < 99 && (c = zf_rd_getc(&zz_ent.rd)) >= 0) m[n++] = (char)c;
			m[n] = 0;
			if (!strcmp(m, "application/vnd.oasis.opendocument.text")) t = ZF_T_ODT;
			else if (!strcmp(m, "application/vnd.oasis.opendocument.spreadsheet")) t = ZF_T_ODS;
			else if (!strcmp(m, "application/epub+zip")) t = ZF_T_EPUB;
		}
		zz_ent_close();
		return t;
	}
	if (zz_find(&z, "META-INF/container.xml", &e) == 1) return ZF_T_EPUB;
	return ZF_T_ZIP;
}

zf_type_t zf_detect(const char *name, zf_io_t *in) {
	zf_type_t t = by_name(name);
	if (!in || !in->seek) return t;
	uint8_t h[512];
	memset(h, 0, sizeof h);
	if (!in->seek(in, 0)) return t;
	int32_t n = in->read(in, h, sizeof h);
	in->seek(in, 0);
	if (n < 4) return t;

	if (h[0] == 'P' && h[1] == 'K' && h[2] == 3 && h[3] == 4) {
		zf_type_t k = zip_kind(in);
		in->seek(in, 0);
		return k == ZF_T_UNKNOWN ? t : k;
	}
	if (h[0] == 0x1F && h[1] == 0x8B) return t == ZF_T_TGZ ? ZF_T_TGZ : ZF_T_GZ;
	if (n >= 262 && !memcmp(h + 257, "ustar", 5)) return ZF_T_TAR;
	if (!memcmp(h, "{\\rtf", 5)) return ZF_T_RTF;
	// a file whose name says text-ish is believed; otherwise, HTML is
	// recognised by its opening
	if (t == ZF_T_UNKNOWN) {
		int i = 0;
		if (h[0] == 0xEF && h[1] == 0xBB && h[2] == 0xBF) i = 3;
		while (i < n && (h[i] == ' ' || h[i] == '\t' || h[i] == '\r' || h[i] == '\n')) i++;
		if (i + 14 < n && (zf_ieq_n((char *)h + i, "<!doctype html", 14) || zf_ieq_n((char *)h + i, "<html", 5)))
			return ZF_T_HTML;
	}
	return t;
}

// -- the conversion --

static zf_dw_kind_t dw_kind(zf_type_t t) {
	switch (t) {
	case ZF_T_MD:	return ZF_DW_MD;
	case ZF_T_HTML:	return ZF_DW_HTML;
	case ZF_T_RTF:	return ZF_DW_RTF;
	case ZF_T_DOCX:	return ZF_DW_DOCX;
	case ZF_T_ODT:	return ZF_DW_ODT;
	default:		return ZF_DW_TXT;
	}
}

static zf_tw_kind_t tw_kind(zf_type_t t) {
	switch (t) {
	case ZF_T_TSV:	return ZF_TW_TSV;
	case ZF_T_ZSS:	return ZF_TW_ZSS;
	case ZF_T_XLSX:	return ZF_TW_XLSX;
	case ZF_T_ODS:	return ZF_TW_ODS;
	default:		return ZF_TW_CSV;
	}
}

bool zf_can_convert(zf_type_t from, zf_type_t to) {
	if (zf_type_is_doc(from)) return zf_type_is_doc(to) && to != ZF_T_EPUB;
	if (zf_type_is_tab(from)) return zf_type_is_tab(to);
	if (from == ZF_T_GZ) return to == ZF_T_UNKNOWN || !zf_type_is_arch(to);	// one file out
	if (zf_type_is_arch(from)) return to == ZF_T_UNKNOWN;	// into a folder
	return false;
}

int zf_convert(zf_io_t *in, zf_type_t from, zf_io_t *out, zf_type_t to, zf_dir_t *dir) {
	static uint8_t state[ZF_TABW_STATE > ZF_DOCW_STATE ? ZF_TABW_STATE : ZF_DOCW_STATE]
		__attribute__((aligned(8)));
	if (!zf_can_convert(from, to)) return ZF_E_UNSUPP;
	if (in->seek && !in->seek(in, 0)) return ZF_E_IO;
	zf_tab_dropped = 0;

	if (zf_type_is_doc(from)) {
		zf_doc_t d;
		zf_docw_open(&d, state, dw_kind(to), out);
		switch (from) {
		case ZF_T_TXT:	return zf_read_txt(in, &d);
		case ZF_T_MD:	return zf_read_md(in, &d);
		case ZF_T_HTML:	return zf_read_html(in, &d);
		case ZF_T_RTF:	return zf_read_rtf(in, &d);
		case ZF_T_DOCX:	return zf_read_docx(in, &d);
		case ZF_T_ODT:	return zf_read_odt(in, &d);
		case ZF_T_EPUB:	return zf_read_epub(in, &d);
		default:		return ZF_E_UNSUPP;
		}
	}
	if (zf_type_is_tab(from)) {
		zf_tab_t t;
		zf_tabw_open(&t, state, tw_kind(to), out);
		switch (from) {
		case ZF_T_CSV:	return zf_read_csv(in, &t, ',');
		case ZF_T_TSV:	return zf_read_csv(in, &t, '\t');
		case ZF_T_ZSS:	return zf_read_zss(in, &t);
		case ZF_T_XLSX:	return zf_read_xlsx(in, &t);
		case ZF_T_ODS:	return zf_read_ods(in, &t);
		default:		return ZF_E_UNSUPP;
		}
	}
	switch (from) {
	case ZF_T_GZ:	return out ? zf_extract_gz(in, out) : ZF_E_UNSUPP;
	case ZF_T_ZIP:	return dir ? zf_extract_zip(in, dir) : ZF_E_UNSUPP;
	case ZF_T_TGZ:	return dir ? zf_extract_tgz(in, dir) : ZF_E_UNSUPP;
	case ZF_T_TAR: {
		static zf_src_t s;
		zf_src_io(&s, in);
		return dir ? zf_extract_tar(&s, dir) : ZF_E_UNSUPP;
	}
	default:		return ZF_E_UNSUPP;
	}
}
