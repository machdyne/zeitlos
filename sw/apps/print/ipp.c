/*
 * print -- IPP over HTTP. See ipp.h.
 */

#include "ipp.h"

#include <string.h>

// -- addresses -----------------------------------------------------------

static bool prefix(const char *s, const char *p) {
	while (*p) {
		char a = *s++, b = *p++;
		if (a >= 'A' && a <= 'Z') a = (char)(a + 32);
		if (a != b) return false;
	}
	return true;
}

bool ipp_url(const char *s, ipp_url_t *u) {
	const char *h, *e;
	int i;
	memset(u, 0, sizeof(*u));
	u->kind = URL_IPP;
	u->port = 631;
	if (prefix(s, "ipp://")) h = s + 6;
	else if (prefix(s, "http://")) { h = s + 7; u->kind = URL_HTTP; }
	else if (prefix(s, "socket://")) { h = s + 9; u->kind = URL_SOCKET; u->port = 9100; }
	else if (strstr(s, "://")) return false;
	else h = s;
	for (e = h; *e && *e != ':' && *e != '/'; e++) ;
	if (e == h || e - h >= (int)sizeof(u->host)) return false;
	for (i = 0; h + i < e; i++) u->host[i] = h[i];
	u->host[i] = 0;
	if (*e == ':') {
		int p = 0;
		e++;
		if (*e < '0' || *e > '9') return false;
		while (*e >= '0' && *e <= '9') { p = p * 10 + (*e++ - '0'); if (p > 65535) return false; }
		u->port = p;
	}
	if (*e == '/') {
		for (i = 0; e[i] && i < (int)sizeof(u->path) - 1; i++) u->path[i] = e[i];
		u->path[i] = 0;
	} else if (*e) {
		return false;
	} else if (u->kind != URL_SOCKET) {
		memcpy(u->path, "/ipp/print", 11);
	}
	return true;
}

// -- HTTP -------------------------------------------------------------------

static bool sock_str(out_t *o, const char *s) { return out_str(o, s); }

static void hex_len(char *b, uint32_t n) {
	static const char d[] = "0123456789abcdef";
	char t[9];
	int k = 0, i = 0;
	do { t[k++] = d[n & 15]; n >>= 4; } while (n);
	while (k) b[i++] = t[--k];
	b[i] = 0;
}

static bool flush_chunk(http_t *h) {
	char len[12];
	if (!h->n) return true;
	hex_len(len, (uint32_t)h->n);
	if (!sock_str(h->sock, len) || !sock_str(h->sock, "\r\n") ||
		!out_put(h->sock, h->buf, (uint32_t)h->n) || !sock_str(h->sock, "\r\n")) return false;
	h->n = 0;
	return true;
}

static bool body_write(out_t *o, const void *d, uint32_t n) {
	http_t *h = (http_t *)o->ctx;
	const uint8_t *p = (const uint8_t *)d;
	while (n) {
		uint32_t k = (uint32_t)sizeof(h->buf) - (uint32_t)h->n;
		if (k > n) k = n;
		memcpy(h->buf + h->n, p, k);
		h->n += (int)k;
		p += k;
		n -= k;
		if (h->n == (int)sizeof(h->buf) && !flush_chunk(h)) return false;
	}
	return true;
}

bool http_post_begin(http_t *h, const ipp_url_t *u, const char *ctype) {
	char port[8];
	h->n = 0;
	h->rlen = h->rpos = 0;
	h->body.write = body_write;
	h->body.ctx = h;
	h->body.total = 0;
	h->body.failed = false;
	fmt_int(port, u->port);
	sock_str(h->sock, "POST ");
	sock_str(h->sock, u->path[0] ? u->path : "/");
	sock_str(h->sock, " HTTP/1.1\r\nHost: ");
	sock_str(h->sock, u->host);
	sock_str(h->sock, ":");
	sock_str(h->sock, port);
	sock_str(h->sock, "\r\nUser-Agent: Zeitlos print\r\nContent-Type: ");
	sock_str(h->sock, ctype);
	return sock_str(h->sock, "\r\nTransfer-Encoding: chunked\r\n\r\n");
}

out_t *http_body(http_t *h) { return &h->body; }

bool http_post_end(http_t *h) {
	if (h->body.failed || !flush_chunk(h)) return false;
	return sock_str(h->sock, "0\r\n\r\n");
}

static int rd_byte(http_t *h) {
	if (h->rpos >= h->rlen) {
		int n = h->read(h->rctx, h->rbuf, (int)sizeof(h->rbuf));
		if (n <= 0) return -1;
		h->rlen = n;
		h->rpos = 0;
	}
	return h->rbuf[h->rpos++];
}

// A header line, without its CRLF; false at the end of the stream.
static bool rd_line(http_t *h, char *b, int max) {
	int n = 0, c;
	for (;;) {
		c = rd_byte(h);
		if (c < 0) { b[n] = 0; return n > 0; }
		if (c == '\n') break;
		if (c != '\r' && n < max - 1) b[n++] = (char)c;
	}
	b[n] = 0;
	return true;
}

static long parse_num(const char *s, int base) {
	long v = 0;
	while (*s == ' ') s++;
	for (;; s++) {
		int d;
		if (*s >= '0' && *s <= '9') d = *s - '0';
		else if (base == 16 && *s >= 'a' && *s <= 'f') d = *s - 'a' + 10;
		else if (base == 16 && *s >= 'A' && *s <= 'F') d = *s - 'A' + 10;
		else break;
		v = v * base + d;
		if (v > 0x7FFFFFFF) return -1;
	}
	return v;
}

int http_response(http_t *h, int *status, uint8_t *body, int max) {
	char line[256];
	long clen = -1;
	bool chunked = false;
	int n = 0;
	*status = 0;
	// skip any 100 Continue
	for (;;) {
		if (!rd_line(h, line, sizeof(line))) return -1;
		if (strncmp(line, "HTTP/1.", 7)) return -1;
		*status = (int)parse_num(line + 8, 10);
		if (*status != 100) break;
		while (rd_line(h, line, sizeof(line)) && line[0]) ;
	}
	while (rd_line(h, line, sizeof(line)) && line[0]) {
		if (prefix(line, "content-length:")) clen = parse_num(line + 15, 10);
		else if (prefix(line, "transfer-encoding:") && strstr(line, "hunked")) chunked = true;
	}
	if (chunked) {
		for (;;) {
			long k;
			if (!rd_line(h, line, sizeof(line))) break;
			k = parse_num(line, 16);
			if (k <= 0) break;
			while (k-- > 0) {
				int c = rd_byte(h);
				if (c < 0) return n;
				if (n < max) body[n++] = (uint8_t)c;
			}
			rd_line(h, line, sizeof(line));
		}
		return n;
	}
	while (clen < 0 || n < clen) {
		int c = rd_byte(h);
		if (c < 0) break;
		if (n < max) body[n++] = (uint8_t)c;
		else if (clen < 0) break;
	}
	return n;
}

// -- IPP encoding --------------------------------------------------------------

enum {
	T_OP = 0x01, T_JOB = 0x02, T_END = 0x03, T_PRINTER = 0x04,
	V_INT = 0x21, V_BOOL = 0x22, V_ENUM = 0x23, V_RES = 0x32, V_RANGE = 0x33,
	V_TEXT = 0x41, V_NAME = 0x42, V_KEYWORD = 0x44, V_URI = 0x45,
	V_CHARSET = 0x47, V_LANG = 0x48, V_MIME = 0x49,
};

typedef struct { uint8_t *b; uint32_t n, max; out_t *o; bool bad; } enc_t;

static void e_bytes(enc_t *e, const void *d, uint32_t n) {
	if (e->o) { if (!out_put(e->o, d, n)) e->bad = true; return; }
	if (e->n + n > e->max) { e->bad = true; return; }
	memcpy(e->b + e->n, d, n);
	e->n += n;
}
static void e8(enc_t *e, uint32_t v) { uint8_t b = (uint8_t)v; e_bytes(e, &b, 1); }
static void e16(enc_t *e, uint32_t v) { uint8_t b[2] = { (uint8_t)(v >> 8), (uint8_t)v }; e_bytes(e, b, 2); }
static void e32(enc_t *e, uint32_t v) { e16(e, v >> 16); e16(e, v); }

static void a_str(enc_t *e, int tag, const char *name, const char *val) {
	uint32_t nl = name ? (uint32_t)strlen(name) : 0, vl = (uint32_t)strlen(val);
	e8(e, (uint32_t)tag);
	e16(e, nl);
	if (nl) e_bytes(e, name, nl);
	e16(e, vl);
	e_bytes(e, val, vl);
}

static void a_int(enc_t *e, int tag, const char *name, int32_t v) {
	uint32_t nl = (uint32_t)strlen(name);
	e8(e, (uint32_t)tag);
	e16(e, nl);
	e_bytes(e, name, nl);
	e16(e, 4);
	e32(e, (uint32_t)v);
}

static void op_head(enc_t *e, int op, const char *uri) {
	e8(e, 2); e8(e, 0);			// IPP/2.0
	e16(e, (uint32_t)op);
	e32(e, 1);					// request-id
	e8(e, T_OP);
	a_str(e, V_CHARSET, "attributes-charset", "utf-8");
	a_str(e, V_LANG, "attributes-natural-language", "en");
	a_str(e, V_URI, "printer-uri", uri);
}

uint32_t ipp_get_attributes(uint8_t *buf, uint32_t max, const char *uri) {
	static const char *const want[] = {
		"printer-make-and-model", "printer-state", "printer-state-reasons",
		"document-format-supported", "pwg-raster-document-resolution-supported",
		"pwg-raster-document-type-supported", "media-default", "media-ready",
		"media-source-supported", "media-col-ready",
		"media-left-margin-supported", "media-top-margin-supported",
		"media-right-margin-supported", "media-bottom-margin-supported",
	};
	enc_t e = { buf, 0, max, NULL, false };
	unsigned i;
	op_head(&e, 0x000B, uri);
	a_str(&e, V_NAME, "requesting-user-name", "zeitlos");
	for (i = 0; i < sizeof(want) / sizeof(want[0]); i++)
		a_str(&e, V_KEYWORD, i ? NULL : "requested-attributes", want[i]);
	e8(&e, T_END);
	return e.bad ? 0 : e.n;
}

bool ipp_print_job(out_t *o, const ipp_job_t *j) {
	enc_t e = { NULL, 0, 0, o, false };
	op_head(&e, 0x0002, j->uri);
	a_str(&e, V_NAME, "requesting-user-name", j->user ? j->user : "zeitlos");
	a_str(&e, V_NAME, "job-name", j->job_name && j->job_name[0] ? j->job_name : "Zeitlos");
	a_str(&e, V_MIME, "document-format", j->format);
	e8(&e, T_JOB);
	if (j->copies > 1) a_int(&e, V_INT, "copies", j->copies);
	if (j->mono) a_str(&e, V_KEYWORD, "print-color-mode", "monochrome");
	if (j->quality >= 3 && j->quality <= 5) a_int(&e, V_ENUM, "print-quality", j->quality);
	if (j->media && j->media[0] && !(j->media_type && j->media_type[0]) &&
		!(j->media_source && j->media_source[0])) {
		a_str(&e, V_KEYWORD, "media", j->media);
	} else if (j->media_w > 0 && j->media_h > 0) {
		// media-col, for a paper type or tray: the size as dimensions
		// (hundredths of a millimetre), which every IPP Everywhere
		// printer takes
		e8(&e, 0x34); e16(&e, 9); e_bytes(&e, "media-col", 9); e16(&e, 0);
		e8(&e, 0x4A); e16(&e, 0); e16(&e, 10); e_bytes(&e, "media-size", 10);
		e8(&e, 0x34); e16(&e, 0); e16(&e, 0);
		e8(&e, 0x4A); e16(&e, 0); e16(&e, 11); e_bytes(&e, "x-dimension", 11);
		e8(&e, V_INT); e16(&e, 0); e16(&e, 4); e32(&e, (uint32_t)j->media_w);
		e8(&e, 0x4A); e16(&e, 0); e16(&e, 11); e_bytes(&e, "y-dimension", 11);
		e8(&e, V_INT); e16(&e, 0); e16(&e, 4); e32(&e, (uint32_t)j->media_h);
		e8(&e, 0x37); e16(&e, 0); e16(&e, 0);
		if (j->media_type && j->media_type[0]) {
			e8(&e, 0x4A); e16(&e, 0); e16(&e, 10); e_bytes(&e, "media-type", 10);
			a_str(&e, V_KEYWORD, NULL, j->media_type);
		}
		if (j->media_source && j->media_source[0]) {
			e8(&e, 0x4A); e16(&e, 0); e16(&e, 12); e_bytes(&e, "media-source", 12);
			a_str(&e, V_KEYWORD, NULL, j->media_source);
		}
		e8(&e, 0x37); e16(&e, 0); e16(&e, 0);
	}
	e8(&e, T_END);
	return !e.bad;
}

// -- IPP decoding ------------------------------------------------------------------

static uint32_t rd32(const uint8_t *p) {
	return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

static void scopy(char *d, int dn, const uint8_t *s, int n) {
	int i;
	if (n > dn - 1) n = dn - 1;
	for (i = 0; i < n; i++) d[i] = (char)s[i];
	d[n] = 0;
}

static bool is(const char *a, const char *b) { return !strcmp(a, b); }

bool ipp_parse(const uint8_t *d, int n, ipp_info_t *info) {
	int p = 8;
	char name[64] = "", member[32] = "";
	int depth = 0;
	bool in_ready = false, in_size = false;
	memset(info, 0, sizeof(*info));
	if (n < 9) return false;
	info->status = (d[2] << 8) | d[3];
	while (p < n) {
		int tag = d[p++], nl, vl;
		const uint8_t *v;
		if (tag == T_END) break;
		if (tag < 0x10) continue;		// a group: nothing to keep
		if (p + 2 > n) return false;
		nl = (d[p] << 8) | d[p + 1];
		p += 2;
		if (p + nl + 2 > n) return false;
		if (nl) scopy(name, sizeof(name), d + p, nl);
		p += nl;
		vl = (d[p] << 8) | d[p + 1];
		p += 2;
		if (p + vl > n) return false;
		v = d + p;
		p += vl;
		// collections: media-col-ready is read (what is in which tray),
		// the rest skipped whole
		if (tag == 0x34) {
			if (!depth) {
				in_ready = is(name, "media-col-ready") && info->ntray < 6;
				if (in_ready) memset(&info->tray[info->ntray], 0, sizeof(info->tray[0]));
				member[0] = 0;
			} else if (depth == 1) {
				in_size = is(member, "media-size");
			}
			depth++;
			continue;
		}
		if (tag == 0x37) {
			if (depth) depth--;
			if (depth == 1) in_size = false;
			if (!depth && in_ready) {
				if (info->tray[info->ntray].w || info->tray[info->ntray].source[0]) info->ntray++;
				in_ready = false;
			}
			continue;
		}
		if (depth) {
			if (!in_ready) continue;
			if (tag == 0x4A) { scopy(member, sizeof(member), v, vl); continue; }
			if (depth == 2 && in_size && tag == V_INT && vl == 4) {
				if (is(member, "x-dimension")) info->tray[info->ntray].w = (int)rd32(v);
				else if (is(member, "y-dimension")) info->tray[info->ntray].h = (int)rd32(v);
			} else if (depth == 1 && is(member, "media-source")) {
				scopy(info->tray[info->ntray].source, sizeof(info->tray[0].source), v, vl);
			} else if (depth == 1 && is(member, "media-type")) {
				scopy(info->tray[info->ntray].type, sizeof(info->tray[0].type), v, vl);
			}
			continue;
		}

		if (is(name, "status-message") && (tag == V_TEXT || tag == 0x35)) {
			scopy(info->message, sizeof(info->message), v, vl);
		} else if (is(name, "printer-make-and-model")) {
			scopy(info->make, sizeof(info->make), v, vl);
		} else if (is(name, "printer-state") && vl == 4) {
			info->state = (int)rd32(v);
		} else if (is(name, "printer-state-reasons")) {
			int l = (int)strlen(info->state_reasons);
			if (l && l < (int)sizeof(info->state_reasons) - 2) info->state_reasons[l++] = ' ';
			scopy(info->state_reasons + l, (int)sizeof(info->state_reasons) - l, v, vl);
		} else if (is(name, "document-format-supported")) {
			char f[48];
			scopy(f, sizeof(f), v, vl);
			if (is(f, "image/pwg-raster")) info->pwg = true;
			else if (is(f, "application/pdf")) info->pdf = true;
			else if (is(f, "application/postscript")) info->ps = true;
			else if (is(f, "image/urf")) info->urf = true;
			else if (is(f, "image/jpeg")) info->jpeg = true;
		} else if (is(name, "pwg-raster-document-type-supported")) {
			char f[24];
			scopy(f, sizeof(f), v, vl);
			if (is(f, "sgray_8")) info->sgray8 = true;
			else if (is(f, "srgb_8")) info->srgb8 = true;
		} else if (is(name, "pwg-raster-document-resolution-supported") && tag == V_RES && vl == 9) {
			if (info->ndpi < 4) info->dpi[info->ndpi++] = (int)rd32(v);
		} else if (is(name, "media-default")) {
			scopy(info->media_default, sizeof(info->media_default), v, vl);
		} else if (is(name, "media-source-supported")) {
			if (info->nsources < 6) scopy(info->sources[info->nsources++], 16, v, vl);
		} else if (is(name, "media-ready")) {
			if (info->nready < 4) scopy(info->media_ready[info->nready++], 48, v, vl);
		} else if (vl == 4 && (tag == V_INT) && !strncmp(name, "media-", 6) &&
			strstr(name, "-margin-supported")) {
			int k = strstr(name, "left") ? 0 : strstr(name, "top") ? 1 : strstr(name, "right") ? 2 : 3;
			int m = (int)rd32(v);
			// the largest: 0 is there for borderless paper only, and a
			// margin wider than needed loses less than one too narrow
			if (m > info->margin[k]) info->margin[k] = m;
			info->has_margins = true;
		} else if (is(name, "job-id") && vl == 4) {
			info->job_id = (int)rd32(v);
		} else if (is(name, "job-state") && vl == 4) {
			info->job_state = (int)rd32(v);
		}
	}
	return true;
}

const char *ipp_status_name(int s) {
	switch (s) {
	case 0x0000: return "successful-ok";
	case 0x0001: return "successful-ok-ignored-or-substituted-attributes";
	case 0x0002: return "successful-ok-conflicting-attributes";
	case 0x0400: return "client-error-bad-request";
	case 0x0401: return "client-error-forbidden";
	case 0x0402: return "client-error-not-authenticated";
	case 0x0403: return "client-error-not-authorized";
	case 0x0404: return "client-error-not-possible";
	case 0x0405: return "client-error-timeout";
	case 0x0406: return "client-error-not-found";
	case 0x040A: return "client-error-document-format-not-supported";
	case 0x040B: return "client-error-attributes-or-values-not-supported";
	case 0x0500: return "server-error-internal-error";
	case 0x0501: return "server-error-operation-not-supported";
	case 0x0502: return "server-error-service-unavailable";
	case 0x0503: return "server-error-version-not-supported";
	case 0x0504: return "server-error-device-error";
	case 0x0505: return "server-error-temporary-error";
	case 0x0506: return "server-error-not-accepting-jobs";
	case 0x0507: return "server-error-busy";
	default: return s < 0x0100 ? "successful" : s < 0x0500 ? "client-error" : "server-error";
	}
}
