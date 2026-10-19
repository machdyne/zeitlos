/*
 * print -- a job from start to finish. See run.h.
 */

#include "run.h"
#include "raster.h"
#include "res.h"

#include <string.h>

#include "../../common/zpool.h"

#define RUN_PTS   8192
#define RUN_SUBS  1024
#define BAND_ROWS 64

static zc_pt_t *r_pts, *r_wpts;
static uint16_t *r_subs, *r_wsubs;
static zc_path_t r_path, r_work;
static bool r_ready;

void run_init(void) {
	if (r_ready) return;
	r_pts = pool_alloc(sizeof(zc_pt_t) * RUN_PTS);
	r_wpts = pool_alloc(sizeof(zc_pt_t) * RUN_PTS * 2);
	r_subs = pool_alloc(sizeof(uint16_t) * RUN_SUBS);
	r_wsubs = pool_alloc(sizeof(uint16_t) * RUN_SUBS * 2);
	if (!r_pts || !r_wpts || !r_subs || !r_wsubs) return;
	zc_path_init(&r_path, r_pts, RUN_PTS, r_subs, RUN_SUBS);
	zc_path_init(&r_work, r_wpts, RUN_PTS * 2, r_wsubs, RUN_SUBS * 2);
	r_ready = true;
}

void run_say(char *d, int cap, const char *a, const char *b) {
	int n = 0, i;
	for (i = 0; a && a[i] && n < cap - 1; i++) d[n++] = a[i];
	for (i = 0; b && b[i] && n < cap - 1; i++) d[n++] = b[i];
	d[n] = 0;
}

static void say(run_t *r, const char *a, const char *b) {
	run_say(r->err, (int)sizeof(r->err), a, b);
}

// -- raster ----------------------------------------------------------------

typedef struct {
	run_t		*r;
	pwg_t		*p;
	int			sheet, sheets;
} line_ctx_t;

static bool to_pwg(void *ctx, const uint8_t *row, int y) {
	(void)y;
	return pwg_line(((line_ctx_t *)ctx)->p, row);
}

static bool band_tick(void *ctx, int y, int h) {
	line_ctx_t *lc = ctx;
	// the band is about to be drawn over: the held line is copied out
	pwg_keep(lc->p);
	if (!lc->r->tick) return true;
	return lc->r->tick(lc->r->ctx, lc->sheet, lc->sheets, y, h);
}

bool run_raster(run_t *r, const job_t *j, out_t *o, int dpi, int space,
	const int margins[4]) {
	zp_play_t pl;
	raster_t ra;
	const zpage_size_t *paper;
	int s, ns = job_sheets(j), c, wmm, hmm, maxw = 0;
	char media[64];
	zpage_sheet_t sh;
	uint8_t *band = NULL, *prev = NULL, *enc = NULL;
	pwg_t pw;
	line_ctx_t lc;
	bool ok = false;

	run_init();
	if (!r_ready) { say(r, "out of memory", NULL); return false; }
	memset(&pl, 0, sizeof(pl));
	pl.font = res_font;
	pl.image = res_image;
	pl.fallback = res_fallback();
	pl.path = &r_path;
	pl.work = &r_work;

	// the widest sheet decides the buffers
	for (s = 0; s < ns; s++) {
		job_sheet_at(j, s, dpi, NULL, margins, &sh, &paper, &wmm, &hmm);
		if (sh.w > maxw) maxw = sh.w;
	}
	band = pool_alloc((size_t)((maxw + 3) & ~3) * BAND_ROWS);
	prev = pool_alloc((size_t)maxw);
	enc = pool_alloc(pwg_enc_bytes(maxw, 3));
	if (!band || !prev || !enc) { say(r, "out of memory for the band", NULL); goto done; }
	if (!pwg_file(o)) goto io;

	lc.r = r;
	lc.p = &pw;
	lc.sheets = ns * j->copies;
	for (c = 0; c < j->copies; c++) {
		for (s = 0; s < ns; s++) {
			pwg_page_t pg;
			job_sheet_at(j, s, dpi, NULL, margins, &sh, &paper, &wmm, &hmm);
			if (paper) {
				job_media_name(paper, media, sizeof(media));
			} else {
				zpage_size_t cs;
				cs.name = "custom";
				cs.w = (uint16_t)wmm;
				cs.h = (uint16_t)hmm;
				job_media_name(&cs, media, sizeof(media));
			}
			memset(&pg, 0, sizeof(pg));
			pg.w = sh.w;
			pg.h = sh.h;
			pg.dpi = dpi;
			pg.space = space;
			pg.page = c * ns + s + 1;
			pg.pages = ns * j->copies;
			pg.media = media;
			pg.media_type = j->media_type[0] ? j->media_type : NULL;
			if (!pwg_page(&pw, o, &pg, prev, enc)) goto io;
			memset(&ra, 0, sizeof(ra));
			ra.job = j;
			ra.play = &pl;
			ra.band = band;
			ra.rows = BAND_ROWS;
			ra.sheet = sh;
			ra.tick = band_tick;
			ra.tick_ctx = &lc;
			lc.sheet = c * ns + s;
			if (!raster_sheet(&ra, s, to_pwg, &lc)) {
				if (o->failed) goto io;
				say(r, "cancelled", NULL);
				goto done;
			}
			if (!pwg_end(&pw)) goto io;
		}
	}
	ok = true;
	goto done;
io:
	if (!r->err[0]) say(r, "the printer stopped taking the job", NULL);
done:
	r->skipped += pl.skipped;
	r->bytes = o->total;
	pool_free(band);
	pool_free(prev);
	pool_free(enc);
	return ok;
}

// -- vectors ------------------------------------------------------------------

bool run_vec(run_t *r, const job_t *j, out_t *o, int fmt) {
	vec_t *v = pool_zalloc(sizeof(vec_t));
	vfont_t *fonts = pool_zalloc(sizeof(vfont_t) * VEC_FONTS);
	int xcap = 4096 + job_sheets(j) * 64;
	uint32_t *xref = pool_zalloc(sizeof(uint32_t) * (size_t)xcap);
	int *kids = pool_zalloc(sizeof(int) * (size_t)(job_sheets(j) + 1));
	int s, ns = job_sheets(j);
	bool ok = false;
	if (!v || !fonts || !xref || !kids) { say(r, "out of memory", NULL); goto done; }
	v->fmt = fmt;
	v->o = o;
	v->job = j;
	v->font = res_font;
	v->image = res_image;
	v->fallback = res_fallback();
	v->xref = xref;
	v->xref_cap = xcap;
	v->kids = kids;
	v->fonts = fonts;
	if (!vec_begin(v)) goto io;
	for (s = 0; s < ns; s++) {
		if (r->tick && !r->tick(r->ctx, s, ns, 0, 1)) { say(r, "cancelled", NULL); goto done; }
		if (!vec_sheet(v, s)) goto io;
	}
	if (!vec_end(v)) goto io;
	ok = true;
	goto done;
io:
	say(r, "could not write", NULL);
done:
	if (v) r->skipped += v->skipped;
	r->bytes = o->total;
	pool_free(v);
	pool_free(fonts);
	pool_free(xref);
	pool_free(kids);
	return ok;
}

// -- IPP --------------------------------------------------------------------------

static char uri_buf[200];

static const char *uri_of(const ipp_url_t *u) {
	// the printer-uri: ipp://host:port/path
	int n = 0, i;
	char p[8];
	const char *pre = "ipp://";
	for (i = 0; pre[i]; i++) uri_buf[n++] = pre[i];
	for (i = 0; u->host[i] && n < 120; i++) uri_buf[n++] = u->host[i];
	uri_buf[n++] = ':';
	fmt_int(p, u->port);
	for (i = 0; p[i]; i++) uri_buf[n++] = p[i];
	for (i = 0; u->path[i] && n < (int)sizeof(uri_buf) - 1; i++) uri_buf[n++] = u->path[i];
	uri_buf[n] = 0;
	return uri_buf;
}

static bool exchange_end(run_t *r, http_t *h, ipp_info_t *info) {
	int status = 0, n;
	uint8_t *body = pool_alloc(65536);
	memset(info, 0, sizeof(*info));
	if (!body) { say(r, "out of memory", NULL); return false; }
	n = http_response(h, &status, body, 65536);
	if (n < 0 || status == 0) { pool_free(body); say(r, "no answer from the printer", NULL); return false; }
	if (status != 200) {
		char b[8];
		fmt_int(b, status);
		pool_free(body);
		say(r, "the printer said HTTP ", b);
		return false;
	}
	if (!ipp_parse(body, n, info)) { pool_free(body); say(r, "the printer's answer is not IPP", NULL); return false; }
	pool_free(body);
	if (info->status >= 0x0100) {
		say(r, ipp_status_name(info->status), info->message[0] ? ": " : NULL);
		if (info->message[0]) {
			int l = (int)strlen(r->err), i;
			for (i = 0; info->message[i] && l < (int)sizeof(r->err) - 1; i++) r->err[l++] = info->message[i];
			r->err[l] = 0;
		}
		return false;
	}
	return true;
}

bool run_probe(run_t *r, transport_t *t, const ipp_url_t *u, ipp_info_t *info) {
	http_t *h = pool_zalloc(sizeof(http_t));
	uint8_t req[1024];
	uint32_t n = ipp_get_attributes(req, sizeof(req), uri_of(u));
	bool ok;
	memset(info, 0, sizeof(*info));
	if (!h) { say(r, "out of memory", NULL); return false; }
	if (!t->connect(t->ctx, u, r->err, sizeof(r->err))) { pool_free(h); return false; }
	h->sock = t->sock;
	h->read = t->read;
	h->rctx = t->ctx;
	ok = http_post_begin(h, u, "application/ipp") && out_put(http_body(h), req, n) && http_post_end(h);
	if (!ok) say(r, "could not send to the printer", NULL);
	else ok = exchange_end(r, h, info);
	t->close(t->ctx);
	pool_free(h);
	return ok;
}

static int pick_dpi(const ipp_info_t *info) {
	int i, best = 0;
	if (!info->ndpi) return 300;
	for (i = 0; i < info->ndpi; i++) {
		int d = info->dpi[i];
		if (d == 300) return 300;
		// the nearest, preferring more over less
		if (!best || (d > 300 && (best < 300 || d < best)) || (best < 300 && d > best)) best = d;
	}
	return best;
}

#define BUSY_WAIT_MS	5000
#define BUSY_TRIES		360		// half an hour of a printer at work

static bool attempt(run_t *r, transport_t *t, const ipp_url_t *u, const job_t *j,
	const char *prefer, int *busy);

bool run_ipp(run_t *r, transport_t *t, const ipp_url_t *u, const job_t *j,
	const char *prefer) {
	int tries;
	for (tries = 0; tries < BUSY_TRIES; tries++) {
		int busy = 0;
		r->err[0] = 0;
		if (attempt(r, t, u, j, prefer, &busy)) return true;
		if (!busy || !r->wait) return false;
		if (!r->wait(r->ctx, BUSY_WAIT_MS, busy == 1 ? "the printer is busy" : "the printer is not ready"))
			{ say(r, "cancelled", NULL); return false; }
	}
	return false;
}

// One try. *busy: 1 when the printer is printing something else (try
// again later), 2 when it says it cannot take jobs for now.
static bool attempt(run_t *r, transport_t *t, const ipp_url_t *u, const job_t *j,
	const char *prefer, int *busy) {
	ipp_info_t info;
	ipp_job_t ij;
	http_t *h;
	int dpi, space, margins[4], fmt = -1, wmm, hmm;
	const zpage_size_t *paper;
	zpage_sheet_t sh;
	char media[64];
	bool ok;

	if (!run_probe(r, t, u, &info)) return false;
	if (info.state == 4 && r->wait) {
		// printing something else: wait rather than be refused after the
		// whole job has been made and sent
		*busy = 1;
		say(r, "the printer is busy", NULL);
		return false;
	}
	// PDF where the printer takes it: a page is a few hundred thousand
	// instructions here, against tens of millions to rasterize it, and
	// the printer draws it at its own resolution. Raster otherwise;
	// "pwg" (or "raster") and "ps" ask for those.
	if (!prefer) prefer = "auto";
	if ((!strcmp(prefer, "pwg") || !strcmp(prefer, "raster")) && info.pwg) fmt = -1;
	else if (!strcmp(prefer, "ps") && info.ps) fmt = VEC_PS;
	else if (info.pdf && strcmp(prefer, "pwg") && strcmp(prefer, "raster")) fmt = VEC_PDF;
	else if (info.pwg) fmt = -1;
	else if (info.ps) fmt = VEC_PS;
	else if (info.pdf) fmt = VEC_PDF;
	else { say(r, "the printer takes no format this can make (PWG raster, PDF, PostScript)", NULL); return false; }
	r->format = fmt == VEC_PDF ? "PDF" : fmt == VEC_PS ? "PostScript" : "PWG raster";
	r->dpi = fmt < 0 ? pick_dpi(&info) : 0;
	dpi = pick_dpi(&info);
	space = info.sgray8 || !info.srgb8 ? PWG_SGRAY8 : PWG_SRGB8;
	// the printer's margins, hundredths of mm -> tenths
	margins[0] = (info.margin[0] + 9) / 10;
	margins[1] = (info.margin[1] + 9) / 10;
	margins[2] = (info.margin[2] + 9) / 10;
	margins[3] = (info.margin[3] + 9) / 10;

	job_sheet_at(j, 0, dpi, NULL, margins, &sh, &paper, &wmm, &hmm);
	if (paper) {
		job_media_name(paper, media, sizeof(media));
	} else {
		zpage_size_t cs;
		cs.name = "custom";
		cs.w = (uint16_t)wmm;
		cs.h = (uint16_t)hmm;
		job_media_name(&cs, media, sizeof(media));
	}

	h = pool_zalloc(sizeof(http_t));
	if (!h) { say(r, "out of memory", NULL); return false; }
	if (!t->connect(t->ctx, u, r->err, sizeof(r->err))) { pool_free(h); return false; }
	h->sock = t->sock;
	h->read = t->read;
	h->rctx = t->ctx;

	memset(&ij, 0, sizeof(ij));
	ij.uri = uri_of(u);
	ij.user = "zeitlos";
	ij.job_name = j->title;
	ij.format = fmt == VEC_PDF ? "application/pdf" : fmt == VEC_PS ? "application/postscript" : "image/pwg-raster";
	ij.media = media;
	ij.media_w = wmm * 10;
	ij.media_h = hmm * 10;
	ij.media_type = j->media_type;
	ij.media_source = j->media_source;
	ij.quality = !strcmp(j->quality, "draft") ? 3 : !strcmp(j->quality, "high") ? 5 :
		!strcmp(j->quality, "normal") ? 4 : 0;
	// a raster carries its copies, and PostScript says them itself
	ij.copies = fmt == VEC_PDF ? j->copies : 1;
	ij.mono = true;

	ok = http_post_begin(h, u, "application/ipp") && ipp_print_job(http_body(h), &ij);
	if (ok) {
		if (fmt >= 0) ok = run_vec(r, j, http_body(h), fmt);
		else ok = run_raster(r, j, http_body(h), dpi, space, margins);
	}
	// whatever happened, the printer's answer says the most
	if (ok) ok = http_post_end(h);
	{
		ipp_info_t ans;
		char keep[96];
		memcpy(keep, r->err, sizeof(keep));
		r->err[0] = 0;
		if (!exchange_end(r, h, &ans)) {
			if (ans.status == 0x0507) *busy = 1;
			else if (ans.status == 0x0505 || ans.status == 0x0506 || ans.status == 0x0502) *busy = 2;
			if (!r->err[0] || (keep[0] && !strcmp(r->err, "no answer from the printer")))
				memcpy(r->err, keep, sizeof(keep));
			ok = false;
		} else if (!ok) {
			memcpy(r->err, keep, sizeof(keep));
		}
	}
	r->bytes = h->body.total;
	t->close(t->ctx);
	pool_free(h);
	return ok;
}

bool run_socket(run_t *r, transport_t *t, const ipp_url_t *u, const job_t *j) {
	bool ok;
	if (!t->connect(t->ctx, u, r->err, sizeof(r->err))) return false;
	ok = run_vec(r, j, t->sock, VEC_PS);
	t->close(t->ctx);
	return ok;
}
