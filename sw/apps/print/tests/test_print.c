/*
 * The print service's parts on the host: addresses, IPP requests and
 * answers, HTTP framing, PWG line coding, job options and sheets,
 * number formatting. `make -C sw/apps/print test` (which also runs
 * tests/run_tests.sh: the whole path against Ghostscript, Poppler,
 * qpdf and ippeveprinter).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../../common/zpool.h"
#include "../out.h"
#include "../ipp.h"
#include "../pwg.h"
#include "../job.h"
#include "../run.h"

static int fails, checks;

#define ck(cond, msg) do { checks++; if (!(cond)) { fails++; \
	printf("FAIL %s:%d %s\n", __FILE__, __LINE__, msg); } } while (0)

typedef struct { uint8_t *p; uint32_t n, cap; } mem_t;

static bool mem_out(out_t *o, const void *d, uint32_t n) {
	mem_t *m = o->ctx;
	if (m->n + n > m->cap) { m->cap = (m->n + n) * 2 + 256; m->p = realloc(m->p, m->cap); }
	memcpy(m->p + m->n, d, n);
	m->n += n;
	return true;
}

static bool mem_wr(void *ctx, const void *d, uint32_t n) {
	out_t o = { mem_out, ctx, 0, false };
	return mem_out(&o, d, n);
}

// -- PWG lines: decoded by the rule, they are what went in --
static int pwg_decode_line(const uint8_t *e, int n, int w, int bpp, uint8_t *out) {
	int i = 0, x = 0;
	while (i < n && x < w) {
		int c = e[i++];
		if (c < 128) {
			int k;
			for (k = 0; k <= c && x < w; k++) out[x++] = e[i];
			i += bpp;
		} else if (c > 128) {
			int k, m = 257 - c;
			for (k = 0; k < m && x < w; k++) { out[x++] = e[i]; i += bpp; }
		} else {
			return -1;		// pwg.c never writes 128
		}
	}
	return (i == n && x == w) ? 0 : -1;
}

static void test_pwg(void) {
	static uint8_t line[3000], enc[6000], dec[3000];
	int t, w, i;
	srand(7);
	for (t = 0; t < 400; t++) {
		int bpp = (t & 1) ? 3 : 1;
		uint32_t n;
		w = 1 + rand() % 2600;
		for (i = 0; i < w; i++) {
			int kind = rand() % 4;
			line[i] = kind == 0 ? 255 : kind == 1 ? (uint8_t)rand() : i ? line[i - 1] : 0;
		}
		n = pwg_encode_line(line, w, bpp, enc);
		ck(n <= pwg_enc_bytes(w, bpp), "encoded line fits its buffer");
		ck(pwg_decode_line(enc, (int)n, w, bpp, dec) == 0 && !memcmp(dec, line, (size_t)w),
			"a line decodes to itself");
	}
	// a white line of a Letter sheet at 300 dpi is a handful of bytes
	memset(line, 255, 2550);
	ck(pwg_encode_line(line, 2550, 1, enc) == 40, "white line: 20 runs of 128");
	// a page: header, repeats, the white rest
	{
		mem_t m = { 0 };
		out_t o = { mem_out, &m, 0, false };
		pwg_t p;
		pwg_page_t pg;
		static uint8_t prev[64], e[256], rows[3][64];
		memset(&pg, 0, sizeof(pg));
		pg.w = 64; pg.h = 600; pg.dpi = 300; pg.media = "na_letter_8.5x11in";
		memset(rows[0], 0, 64);
		memset(rows[1], 0, 64);
		memset(rows[2], 128, 64);
		ck(pwg_file(&o) && pwg_page(&p, &o, &pg, prev, e), "page header");
		for (i = 0; i < 300; i++) pwg_line(&p, rows[i < 299 ? 0 : 2]);
		pwg_keep(&p);
		pwg_line(&p, rows[1]);
		ck(pwg_end(&p), "page end");
		// header + [rep 255: 256 black] + [42 black] + [1 grey] + [1 black] + white rest (299 lines)
		ck(m.n == 4 + 1796 + 3 + 3 + 3 + 3 + 3 * 2, "repeats counted, the rest white");
		ck(!memcmp(m.p, "RaS2PwgRaster", 13), "sync word and class");
		free(m.p);
	}
}

static void test_url(void) {
	ipp_url_t u;
	ck(ipp_url("ipp://192.168.1.50/ipp/print", &u) && u.kind == URL_IPP && u.port == 631 &&
		!strcmp(u.host, "192.168.1.50") && !strcmp(u.path, "/ipp/print"), "ipp url");
	ck(ipp_url("192.168.1.50", &u) && u.port == 631 && !strcmp(u.path, "/ipp/print"), "bare host");
	ck(ipp_url("socket://laser:9100", &u) && u.kind == URL_SOCKET && u.port == 9100 &&
		!strcmp(u.host, "laser"), "socket url");
	ck(ipp_url("socket://laser", &u) && u.port == 9100, "socket default port");
	ck(ipp_url("IPP://Pixma.local:8631/ipp/print", &u) && u.port == 8631, "scheme in capitals");
	ck(!ipp_url("ftp://x/y", &u), "other schemes refused");
	ck(!ipp_url("ipp://:631/", &u) && !ipp_url("ipp://h:99999/", &u) && !ipp_url("ipp://h:x/", &u), "bad host or port");
}

// An IPP answer the way a printer writes one.
typedef struct { uint8_t b[2048]; int n; } ib_t;
static void ib8(ib_t *b, int v) { b->b[b->n++] = (uint8_t)v; }
static void ib16(ib_t *b, int v) { ib8(b, v >> 8); ib8(b, v); }
static void ibattr(ib_t *b, int tag, const char *name, const void *v, int vl) {
	int nl = name ? (int)strlen(name) : 0;
	ib8(b, tag); ib16(b, nl); if (nl) memcpy(b->b + b->n, name, (size_t)nl); b->n += nl;
	ib16(b, vl); memcpy(b->b + b->n, v, (size_t)vl); b->n += vl;
}
static void ibint(ib_t *b, int tag, const char *name, int v) {
	uint8_t x[4] = { (uint8_t)(v >> 24), (uint8_t)(v >> 16), (uint8_t)(v >> 8), (uint8_t)v };
	ibattr(b, tag, name, x, 4);
}

static void test_ipp(void) {
	uint8_t req[1024];
	uint32_t n = ipp_get_attributes(req, sizeof(req), "ipp://p:631/ipp/print");
	ck(n > 100 && req[0] == 2 && req[2] == 0 && req[3] == 0x0B && req[n - 1] == 3, "Get-Printer-Attributes");
	ck(memmem(req, n, "requested-attributes", 20) && memmem(req, n, "media-ready", 11), "asks what it needs");

	{
		ib_t b = { { 0 }, 0 };
		ipp_info_t i;
		uint8_t res[9] = { 0, 0, 1, 44, 0, 0, 1, 44, 3 };
		ib8(&b, 2); ib8(&b, 0); ib16(&b, 0); ib16(&b, 0); ib16(&b, 1);
		ib8(&b, 1);
		ibattr(&b, 0x47, "attributes-charset", "utf-8", 5);
		ib8(&b, 4);
		ibattr(&b, 0x41, "printer-make-and-model", "Canon TS5300 series", 19);
		ibint(&b, 0x23, "printer-state", 3);
		ibattr(&b, 0x44, "printer-state-reasons", "none", 4);
		ibattr(&b, 0x49, "document-format-supported", "application/octet-stream", 24);
		ibattr(&b, 0x49, NULL, "image/urf", 9);
		ibattr(&b, 0x49, NULL, "image/pwg-raster", 16);
		ibattr(&b, 0x44, "pwg-raster-document-type-supported", "srgb_8", 6);
		ibattr(&b, 0x32, "pwg-raster-document-resolution-supported", res, 9);
		res[2] = 2; res[3] = 88; res[6] = 2; res[7] = 88;	// 600
		ibattr(&b, 0x32, NULL, res, 9);
		ibattr(&b, 0x44, "media-ready", "iso_a4_210x297mm", 16);
		ibint(&b, 0x21, "media-left-margin-supported", 0);
		ibint(&b, 0x21, NULL, 340);
		ibint(&b, 0x21, "media-bottom-margin-supported", 500);
		// a collection is skipped whole, attributes in it not mistaken
		ibattr(&b, 0x34, "media-col-ready", "", 0);
		ibattr(&b, 0x4A, NULL, "media-type", 10);
		ibattr(&b, 0x44, NULL, "photographic", 12);
		ibattr(&b, 0x37, NULL, "", 0);
		ib8(&b, 3);
		ck(ipp_parse(b.b, b.n, &i), "answer parses");
		ck(i.status == 0 && !strcmp(i.make, "Canon TS5300 series") && i.state == 3, "make and state");
		ck(i.pwg && i.urf && !i.pdf && !i.ps && i.srgb8 && !i.sgray8, "formats");
		ck(i.ndpi == 2 && i.dpi[0] == 300 && i.dpi[1] == 600, "resolutions");
		ck(i.nready == 1 && !strcmp(i.media_ready[0], "iso_a4_210x297mm"), "media ready");
		ck(i.margin[0] == 340 && i.margin[3] == 500 && i.has_margins, "margins: the largest listed");
		// cut short anywhere: refused or read as far as it goes, never past
		{
			int k;
			for (k = 0; k < b.n; k++) ipp_parse(b.b, k, &i);
			ck(1, "short answers read safely");
		}
	}
	{
		mem_t m = { 0 };
		out_t o = { mem_out, &m, 0, false };
		ipp_job_t j;
		memset(&j, 0, sizeof(j));
		j.uri = "ipp://p:631/ipp/print";
		j.format = "image/pwg-raster";
		j.media = "na_letter_8.5x11in";
		j.media_w = 21590; j.media_h = 27940;
		j.media_source = "rear";
		j.mono = true;
		j.quality = 5;
		ck(ipp_print_job(&o, &j) && m.p[3] == 0x02 && m.p[m.n - 1] == 3, "Print-Job");
		ck(memmem(m.p, m.n, "media-col", 9) && memmem(m.p, m.n, "x-dimension", 11) &&
			memmem(m.p, m.n, "rear", 4) && !memmem(m.p, m.n, "\x44\0\x05media", 8), "media-col with a tray");
		ck(memmem(m.p, m.n, "monochrome", 10) && memmem(m.p, m.n, "print-quality", 13), "colour mode, quality");
		free(m.p);
	}
	ck(!strcmp(ipp_status_name(0x0507), "server-error-busy"), "status names");
}

// -- HTTP: a response read back through a reader that gives a byte at a time --
typedef struct { const char *s; int pos, len; } rd_t;
static int rd_one(void *ctx, uint8_t *buf, int max) {
	rd_t *r = ctx;
	(void)max;
	if (r->pos >= r->len) return 0;
	buf[0] = (uint8_t)r->s[r->pos++];
	return 1;
}

static void test_http(void) {
	http_t *h = calloc(1, sizeof(http_t));
	uint8_t body[64];
	int status, n;
	static const char chunked[] = "HTTP/1.1 100 Continue\r\n\r\nHTTP/1.1 200 OK\r\nContent-Type: application/ipp\r\n"
		"Transfer-Encoding: chunked\r\n\r\n4\r\nabcd\r\n3\r\nefg\r\n0\r\n\r\n";
	static const char plain[] = "HTTP/1.1 400 Bad Request\r\nContent-Length: 5\r\n\r\nhello";
	rd_t r = { chunked, 0, (int)sizeof(chunked) - 1 };
	h->read = rd_one;
	h->rctx = &r;
	n = http_response(h, &status, body, sizeof(body));
	ck(status == 200 && n == 7 && !memcmp(body, "abcdefg", 7), "chunked response, after a 100");
	r.s = plain; r.pos = 0; r.len = (int)sizeof(plain) - 1;
	h->rlen = h->rpos = 0;
	n = http_response(h, &status, body, sizeof(body));
	ck(status == 400 && n == 5, "content-length response");
	// the request: chunked, with its terminator
	{
		mem_t m = { 0 };
		out_t o = { mem_out, &m, 0, false };
		ipp_url_t u;
		static uint8_t big[5000];
		ipp_url("ipp://printer:631/ipp/print", &u);
		memset(h, 0, sizeof(*h));
		h->sock = &o;
		ck(http_post_begin(h, &u, "application/ipp"), "request head");
		out_put(http_body(h), big, sizeof(big));
		ck(http_post_end(h), "request end");
		ck(memmem(m.p, m.n, "POST /ipp/print HTTP/1.1\r\nHost: printer:631\r\n", 44) != NULL, "request line");
		ck(memmem(m.p, m.n, "\r\n800\r\n", 7) && !memcmp(m.p + m.n - 5, "0\r\n\r\n", 5), "chunks of 2048, then 0");
		free(m.p);
	}
	free(h);
}

static void test_job(void) {
	mem_t m = { 0 };
	zp_writer_t w;
	zpage_t pg;
	zc_xf_t xf;
	job_t j;
	static uint32_t st[JOB_MAX_PAGES], by[JOB_MAX_PAGES];
	zpage_sheet_t sh;
	const zpage_size_t *paper;
	int page[JOB_MAX_NUP], i, wm, hm;
	zpage_cell_t cell[JOB_MAX_NUP];
	zp_writer_init(&w, mem_wr, &m);
	zp_job(&w, "nup", "4");
	zp_job(&w, "pages", "2-7");
	zp_job(&w, "copies", "3");
	zp_job(&w, "cut", "1");
	zp_job(&w, "paper", "a4");
	zpage_set(&pg, "16:9", true);
	for (i = 0; i < 9; i++) { zp_page_begin(&w, &pg, &xf); zp_page_end(&w); }
	ck(job_open(&j, m.p, m.n, st, by), "job opens");
	ck(j.npages == 9 && j.nup == 4 && j.copies == 3 && j.cut && j.gap_mm10 == 80, "options");
	{
		int first, count;
		job_range(&j, &first, &count);
		ck(first == 1 && count == 6 && job_sheets(&j) == 2, "range 2-7: six pages on two sheets");
	}
	paper = job_paper(&j, "letter");
	ck(paper && !strcmp(paper->name, "a4"), "paper from the job");
	job_sheet_at(&j, 1, 300, "letter", NULL, &sh, &paper, &wm, &hm);
	ck(job_sheet(&j, 1, &sh, page, cell) == 2 && page[0] == 5 && page[1] == 6, "second sheet: pages 6 and 7");
	ck(cell[0].x >= sh.left && cell[1].x + cell[1].w <= sh.w - sh.right, "inside the margins");
	{
		char name[64];
		zpage_size_t cs = { "custom", "x", 1000, 1500, ZPAGE_PAPER };
		job_media_name(zpage_find("4x6"), name, sizeof(name));
		ck(!strcmp(name, "na_index-4x6_4x6in"), "PWG media name");
		job_media_name(&cs, name, sizeof(name));
		ck(!strcmp(name, "custom_zeitlos_100x150mm"), "custom media name");
	}
	// paper "page": a sheet the size of the page, landscape, no margin
	strcpy(j.paper, "page");
	job_sheet_at(&j, 0, 72, NULL, NULL, &sh, &paper, &wm, &hm);
	ck(!paper && wm == 2540 && sh.w > sh.h && sh.left == 0, "page-sized sheet for a screen page");
	free(m.p);
}

static void test_fmt(void) {
	char b[32];
	fmt_dec(b, 256 * 3 + 128, 8, 2); ck(!strcmp(b, "3.5"), "3.5");
	fmt_dec(b, -256 * 3 - 64, 8, 2); ck(!strcmp(b, "-3.25"), "-3.25");
	fmt_dec(b, 1, 8, 2); ck(!strcmp(b, "0"), "tiny rounds to 0");
	fmt_dec(b, 65536 / 3, 16, 5); ck(!strcmp(b, "0.33333"), "a third");
	fmt_int(b, -2147483647 - 1); ck(!strcmp(b, "-2147483648"), "INT_MIN");
}

int main(int argc, char **argv) {
	static uint8_t mem[8 << 20];
	(void)argc; (void)argv;
	pool_init(mem, sizeof(mem));
	test_pwg();
	test_url();
	test_ipp();
	test_http();
	test_job();
	test_fmt();
	printf("print: %d checks, %d failures\n", checks, fails);
	return fails ? 1 : 0;
}

int fs_open_read(const char *p) { (void)p; return -1; }
int fs_read_chunk(int a, void *b, int c) { (void)a; (void)b; (void)c; return 0; }
int fs_seek(int a, uint32_t b) { (void)a; (void)b; return -1; }
int fs_close_handle(int a) { (void)a; return 0; }
