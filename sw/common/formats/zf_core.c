/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * The pieces every format shares: errors, output, a buffered reader,
 * CRC-32, UTF-8, Windows-1252 and character entities. See zf.h.
 */

#include <string.h>

#include "zf.h"

bool (*zf_cancel_poll)(void);
void (*zf_progress)(uint32_t permille);
uint8_t *zf_heap;
uint32_t zf_heap_size;

const char *zf_strerror(int e) {
	switch (e) {
	case ZF_OK:			return "done";
	case ZF_E_IO:		return "a read or write failed";
	case ZF_E_FORMAT:	return "not a file of that kind";
	case ZF_E_CORRUPT:	return "the file is damaged";
	case ZF_E_UNSUPP:	return "the file uses something this cannot read";
	case ZF_E_LIMIT:	return "too large";
	case ZF_E_NOMEM:	return "out of memory";
	case ZF_E_PATH:		return "an archive path outside the folder";
	case ZF_E_CANCEL:	return "cancelled";
	default:			return "failed";
	}
}

// -- output --

int32_t zf_write(zf_io_t *o, const void *buf, uint32_t n) {
	if (!n) return 0;
	if (o->written + n > ZF_OUT_MAX || o->written + n < o->written) return ZF_E_LIMIT;
	int32_t r = o->write(o, buf, n);
	if (r != (int32_t)n) return ZF_E_IO;
	o->written += n;
	return r;
}

int32_t zf_puts(zf_io_t *o, const char *s) {
	return zf_write(o, s, (uint32_t)strlen(s));
}

// -- the buffered reader --

void zf_rd_init(zf_rd_t *r, zf_src_t *src) {
	r->src = src;
	r->pos = r->len = 0;
	r->eof = false;
	r->err = 0;
	r->consumed = 0;
}

static bool rd_fill(zf_rd_t *r) {
	if (r->eof) return false;
	if (zf_cancel_poll && zf_cancel_poll()) { r->err = ZF_E_CANCEL; r->eof = true; return false; }
	int32_t n = r->src->read(r->src, r->buf, ZF_RD_BUF);
	if (n < 0) { r->err = n; r->eof = true; return false; }
	if (n == 0) { r->eof = true; return false; }
	r->pos = 0;
	r->len = (uint32_t)n;
	return true;
}

int zf_rd_getc(zf_rd_t *r) {
	if (r->pos >= r->len && !rd_fill(r)) return -1;
	r->consumed++;
	return r->buf[r->pos++];
}

int zf_rd_peek(zf_rd_t *r) {
	if (r->pos >= r->len && !rd_fill(r)) return -1;
	return r->buf[r->pos];
}

static int32_t io_src_read(zf_src_t *s, uint8_t *buf, uint32_t n) {
	zf_io_t *io = (zf_io_t *)s->ctx;
	int32_t r = io->read(io, buf, n);
	if (r > 0 && zf_progress && io->size)
		zf_progress((uint32_t)((uint64_t)io->pos * 1000u / io->size));
	return r;
}

void zf_src_io(zf_src_t *s, zf_io_t *io) {
	s->ctx = io;
	s->read = io_src_read;
}

// -- CRC-32 (IEEE, as zip and gzip use), a nibble at a time: a 64-byte
// table rather than 1KB, at about half the speed, which is still far
// faster than the card or the inflater feeding it.

static const uint32_t crc_nib[16] = {
	0x00000000, 0x1DB71064, 0x3B6E20C8, 0x26D930AC,
	0x76DC4190, 0x6B6B51F4, 0x4DB26158, 0x5005713C,
	0xEDB88320, 0xF00F9344, 0xD6D6A3E8, 0xCB61B38C,
	0x9B64C2B0, 0x86D3D2D4, 0xA00AE278, 0xBDBDF21C,
};

uint32_t zf_crc32(uint32_t crc, const void *p, uint32_t n) {
	const uint8_t *b = (const uint8_t *)p;
	crc = ~crc;
	while (n--) {
		crc ^= *b++;
		crc = (crc >> 4) ^ crc_nib[crc & 15];
		crc = (crc >> 4) ^ crc_nib[crc & 15];
	}
	return ~crc;
}

// -- characters --

int zf_utf8_put(uint32_t cp, char *o) {
	if (cp >= 0xD800 && cp < 0xE000) cp = 0xFFFD;
	if (cp > 0x10FFFF) cp = 0xFFFD;
	if (cp < 0x80) { o[0] = (char)cp; return 1; }
	if (cp < 0x800) { o[0] = (char)(0xC0 | (cp >> 6)); o[1] = (char)(0x80 | (cp & 63)); return 2; }
	if (cp < 0x10000) {
		o[0] = (char)(0xE0 | (cp >> 12)); o[1] = (char)(0x80 | ((cp >> 6) & 63));
		o[2] = (char)(0x80 | (cp & 63));
		return 3;
	}
	o[0] = (char)(0xF0 | (cp >> 18)); o[1] = (char)(0x80 | ((cp >> 12) & 63));
	o[2] = (char)(0x80 | ((cp >> 6) & 63)); o[3] = (char)(0x80 | (cp & 63));
	return 4;
}

static const uint16_t cp1252_hi[32] = {
	0x20AC, 0x81,   0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021,
	0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0x8D,   0x017D, 0x8F,
	0x90,   0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014,
	0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0x9D,   0x017E, 0x0178,
};

uint32_t zf_cp1252(uint8_t b) {
	return (b >= 0x80 && b < 0xA0) ? cp1252_hi[b - 0x80] : b;
}

// The entities that turn up in documents. HTML has some 2,000; past
// these, an unknown one is left as written, which reads better than a
// box.
static const struct { const char *n; uint16_t cp; } ents[] = {
	{ "amp", '&' }, { "lt", '<' }, { "gt", '>' }, { "quot", '"' }, { "apos", '\'' },
	{ "nbsp", 0xA0 }, { "shy", 0xAD }, { "copy", 0xA9 }, { "reg", 0xAE },
	{ "trade", 0x2122 }, { "hellip", 0x2026 }, { "mdash", 0x2014 }, { "ndash", 0x2013 },
	{ "lsquo", 0x2018 }, { "rsquo", 0x2019 }, { "sbquo", 0x201A }, { "ldquo", 0x201C },
	{ "rdquo", 0x201D }, { "bdquo", 0x201E }, { "bull", 0x2022 }, { "middot", 0xB7 },
	{ "euro", 0x20AC }, { "pound", 0xA3 }, { "yen", 0xA5 }, { "cent", 0xA2 },
	{ "sect", 0xA7 }, { "para", 0xB6 }, { "deg", 0xB0 }, { "plusmn", 0xB1 },
	{ "times", 0xD7 }, { "divide", 0xF7 }, { "laquo", 0xAB }, { "raquo", 0xBB },
	{ "iexcl", 0xA1 }, { "iquest", 0xBF }, { "frac12", 0xBD }, { "frac14", 0xBC },
	{ "frac34", 0xBE }, { "sup2", 0xB2 }, { "sup3", 0xB3 }, { "micro", 0xB5 },
	{ "szlig", 0xDF }, { "thinsp", 0x2009 }, { "ensp", 0x2002 }, { "emsp", 0x2003 },
	{ "lsaquo", 0x2039 }, { "rsaquo", 0x203A }, { "dagger", 0x2020 }, { "Dagger", 0x2021 },
	{ "permil", 0x2030 }, { "prime", 0x2032 }, { "Prime", 0x2033 }, { "larr", 0x2190 },
	{ "rarr", 0x2192 }, { "uarr", 0x2191 }, { "darr", 0x2193 }, { "minus", 0x2212 },
	{ "le", 0x2264 }, { "ge", 0x2265 }, { "ne", 0x2260 }, { "infin", 0x221E },
	{ "OElig", 0x152 }, { "oelig", 0x153 }, { "Scaron", 0x160 }, { "scaron", 0x161 },
	{ "Yuml", 0x178 }, { "fnof", 0x192 }, { "circ", 0x2C6 }, { "tilde", 0x2DC },
};

// The Latin-1 letters by their HTML names.
static const struct { const char *n; uint8_t cp; } l1_names[] = {
	{ "Agrave", 0xC0 }, { "Aacute", 0xC1 }, { "Acirc", 0xC2 },
	{ "Atilde", 0xC3 }, { "Auml", 0xC4 }, { "Aring", 0xC5 }, { "AElig", 0xC6 },
	{ "Ccedil", 0xC7 }, { "Egrave", 0xC8 }, { "Eacute", 0xC9 },
	{ "Ecirc", 0xCA }, { "Euml", 0xCB }, { "Igrave", 0xCC }, { "Iacute", 0xCD },
	{ "Icirc", 0xCE }, { "Iuml", 0xCF }, { "ETH", 0xD0 }, { "Ntilde", 0xD1 },
	{ "Ograve", 0xD2 }, { "Oacute", 0xD3 }, { "Ocirc", 0xD4 },
	{ "Otilde", 0xD5 }, { "Ouml", 0xD6 }, { "times", 0xD7 }, { "Oslash", 0xD8 },
	{ "Ugrave", 0xD9 }, { "Uacute", 0xDA }, { "Ucirc", 0xDB }, { "Uuml", 0xDC },
	{ "Yacute", 0xDD }, { "THORN", 0xDE }, { "szlig", 0xDF },
	{ "agrave", 0xE0 }, { "aacute", 0xE1 }, { "acirc", 0xE2 },
	{ "atilde", 0xE3 }, { "auml", 0xE4 }, { "aring", 0xE5 }, { "aelig", 0xE6 },
	{ "ccedil", 0xE7 }, { "egrave", 0xE8 }, { "eacute", 0xE9 },
	{ "ecirc", 0xEA }, { "euml", 0xEB }, { "igrave", 0xEC }, { "iacute", 0xED },
	{ "icirc", 0xEE }, { "iuml", 0xEF }, { "eth", 0xF0 }, { "ntilde", 0xF1 },
	{ "ograve", 0xF2 }, { "oacute", 0xF3 }, { "ocirc", 0xF4 },
	{ "otilde", 0xF5 }, { "ouml", 0xF6 }, { "divide", 0xF7 },
	{ "oslash", 0xF8 }, { "ugrave", 0xF9 }, { "uacute", 0xFA },
	{ "ucirc", 0xFB }, { "uuml", 0xFC }, { "yacute", 0xFD }, { "thorn", 0xFE },
	{ "yuml", 0xFF },
};

static uint32_t latin1_named(const char *e) {
	for (unsigned i = 0; i < sizeof l1_names / sizeof l1_names[0]; i++)
		if (!strcmp(l1_names[i].n, e)) return l1_names[i].cp;
	return 0;
}

uint32_t zf_entity(const char *e) {
	if (e[0] == '#') {
		uint32_t v = 0;
		const char *p = e + 1;
		bool hex = (*p == 'x' || *p == 'X');
		if (hex) p++;
		if (!*p) return 0;
		for (; *p; p++) {
			int d;
			if (*p >= '0' && *p <= '9') d = *p - '0';
			else if (hex && *p >= 'a' && *p <= 'f') d = *p - 'a' + 10;
			else if (hex && *p >= 'A' && *p <= 'F') d = *p - 'A' + 10;
			else return 0;
			v = v * (hex ? 16 : 10) + (uint32_t)d;
			if (v > 0x10FFFF) return 0xFFFD;
		}
		// &#150; and friends: numbered as Windows-1252, as browsers read them
		if (v >= 0x80 && v < 0xA0) v = zf_cp1252((uint8_t)v);
		return v ? v : 0xFFFD;
	}
	for (unsigned i = 0; i < sizeof ents / sizeof ents[0]; i++)
		if (!strcmp(ents[i].n, e)) return ents[i].cp;
	return latin1_named(e);
}

bool zf_ieq(const char *a, const char *b) {
	for (;; a++, b++) {
		char x = *a, y = *b;
		if (x >= 'A' && x <= 'Z') x = (char)(x + 32);
		if (y >= 'A' && y <= 'Z') y = (char)(y + 32);
		if (x != y) return false;
		if (!x) return true;
	}
}

bool zf_ieq_n(const char *a, const char *b, int n) {
	for (int i = 0; i < n; i++) {
		char x = a[i], y = b[i];
		if (x >= 'A' && x <= 'Z') x = (char)(x + 32);
		if (y >= 'A' && y <= 'Z') y = (char)(y + 32);
		if (x != y) return false;
		if (!x) return true;
	}
	return true;
}

const char *zf_ext(const char *path) {
	const char *dot = NULL;
	for (const char *p = path; *p; p++) {
		if (*p == '/') dot = NULL;
		else if (*p == '.') dot = p;
	}
	return (dot && dot[1]) ? dot + 1 : "";
}
