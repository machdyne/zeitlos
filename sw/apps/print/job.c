/*
 * print -- a job and its sheets. See job.h.
 */

#include "job.h"

#include <string.h>

static int to_int(const char *s, int def) {
	int v = 0;
	bool any = false;
	while (*s == ' ') s++;
	while (*s >= '0' && *s <= '9') { v = v * 10 + (*s++ - '0'); any = true; if (v > 1000000) break; }
	return any ? v : def;
}

static void copy(char *d, int n, const char *s) {
	int i;
	for (i = 0; i < n - 1 && s[i]; i++) d[i] = s[i];
	d[i] = 0;
}

bool job_open(job_t *j, const uint8_t *data, uint32_t len, uint32_t *start,
	uint32_t *bytes) {
	char v[200];
	memset(j, 0, sizeof(*j));
	j->data = data;
	j->len = len;
	j->start = start;
	j->bytes = bytes;
	j->npages = zp_pages(data, len, start, bytes, JOB_MAX_PAGES);
	if (j->npages > JOB_MAX_PAGES) j->npages = JOB_MAX_PAGES;
	if (j->npages <= 0) return false;

	zp_job_get(data, len, "title", j->title, sizeof(j->title));
	zp_job_get(data, len, "dest", j->dest, sizeof(j->dest));
	zp_job_get(data, len, "paper", j->paper, sizeof(j->paper));
	zp_job_get(data, len, "quality", j->quality, sizeof(j->quality));
	zp_job_get(data, len, "media-type", j->media_type, sizeof(j->media_type));
	zp_job_get(data, len, "media-source", j->media_source, sizeof(j->media_source));

	j->copies = zp_job_get(data, len, "copies", v, sizeof(v)) ? to_int(v, 1) : 1;
	if (j->copies < 1) j->copies = 1;
	if (j->copies > 99) j->copies = 99;

	j->nup = zp_job_get(data, len, "nup", v, sizeof(v)) ? to_int(v, 1) : 1;
	if (j->nup < 1) j->nup = 1;
	if (j->nup > JOB_MAX_NUP) j->nup = JOB_MAX_NUP;

	j->mode = ZPAGE_FIT;
	if (zp_job_get(data, len, "size", v, sizeof(v)) && !strcmp(v, "actual")) j->mode = ZPAGE_ACTUAL;
	j->cut = zp_job_get(data, len, "cut", v, sizeof(v)) && v[0] == '1';
	j->frame = zp_job_get(data, len, "frame", v, sizeof(v)) && v[0] == '1';
	// a gutter to cut along when there are several, none for one
	j->gap_mm10 = zp_job_get(data, len, "gap", v, sizeof(v)) ? to_int(v, 0) : (j->nup > 1 ? 80 : 0);
	j->margin_mm10 = zp_job_get(data, len, "margin", v, sizeof(v)) ? to_int(v, 50) : 50;

	if (zp_job_get(data, len, "pages", v, sizeof(v)) && v[0]) {
		const char *dash = strchr(v, '-');
		j->from = to_int(v, 0);
		j->to = dash ? to_int(dash + 1, j->npages) : j->from;
	}
	return true;
}

void job_range(const job_t *j, int *first, int *count) {
	int a = j->from > 0 ? j->from : 1, b = j->to > 0 ? j->to : j->npages;
	if (b > j->npages) b = j->npages;
	if (a < 1) a = 1;
	*first = a - 1;
	*count = b >= a ? b - a + 1 : 0;
}

int job_sheets(const job_t *j) {
	int first, count;
	job_range(j, &first, &count);
	return (count + j->nup - 1) / j->nup;
}

bool job_page(const job_t *j, int index, zpage_t *pg) {
	zp_reader_t r;
	zp_rec_t rec;
	zp_page_t pi;
	if (index < 0 || index >= j->npages) return false;
	r.data = j->data + j->start[index];
	r.len = j->bytes[index];
	r.pos = 0;
	r.bad = false;
	if (!zp_next(&r, &rec) || !zp_page_info(&rec, &pi)) return false;
	zp_page_zpage(&pi, pg);
	return true;
}

const zpage_size_t *job_paper(const job_t *j, const char *fallback) {
	const zpage_size_t *p = NULL;
	zpage_t pg;
	if (j->paper[0]) p = zpage_find(j->paper);
	if (p && p->kind == ZPAGE_PAPER) return p;
	if (job_page(j, 0, &pg) && pg.size.kind == ZPAGE_PAPER) {
		p = zpage_find(pg.size.name);
		if (p) return p;
	}
	p = fallback ? zpage_find(fallback) : NULL;
	if (p && p->kind == ZPAGE_PAPER) return p;
	p = j->default_paper[0] ? zpage_find(j->default_paper) : NULL;
	if (p && p->kind == ZPAGE_PAPER) return p;
	return zpage_find("letter");
}

int job_sheet(const job_t *j, int s, const zpage_sheet_t *sh, int *page,
	zpage_cell_t *cell) {
	int first, count, i, n = 0;
	job_range(j, &first, &count);
	for (i = 0; i < j->nup; i++) {
		int k = s * j->nup + i;
		zpage_t pg;
		if (k >= count) break;
		if (!job_page(j, first + k, &pg)) continue;
		if (!zpage_place(&pg, sh, j->nup, i, j->mode, j->gap_mm10, &cell[n])) {
			// too large for its real size: fitted instead
			if (!zpage_place(&pg, sh, j->nup, i, ZPAGE_FIT, j->gap_mm10, &cell[n])) continue;
		}
		page[n++] = first + k;
	}
	return n;
}

void job_sheet_geometry(const job_t *j, const zpage_size_t *paper, int dpi,
	const int printer_mm10[4], zpage_sheet_t *sh) {
	int m[4], i;
	zpage_sheet(sh, paper, dpi, j->margin_mm10);
	for (i = 0; i < 4; i++) {
		int p = printer_mm10 ? printer_mm10[i] : 0;
		m[i] = zpage_mm10_to_px(p > j->margin_mm10 ? p : j->margin_mm10, dpi);
	}
	sh->left = m[0];
	sh->top = m[1];
	sh->right = m[2];
	sh->bottom = m[3];
}

void job_sheet_at(const job_t *j, int s, int dpi, const char *fallback,
	const int printer_mm10[4], zpage_sheet_t *sh, const zpage_size_t **paper,
	int *w_mm10, int *h_mm10) {
	if (!strcmp(j->paper, "page")) {
		int first, count, w = 0, h = 0;
		zpage_t pg;
		job_range(j, &first, &count);
		if (job_page(j, first + s * j->nup, &pg) && !zpage_physical(&pg, &w, &h)) {
			// a screen page: ten inches along its long side
			if (pg.gw >= pg.gh) { w = 2540; h = (int)((int64_t)2540 * pg.gh / pg.gw); }
			else { h = 2540; w = (int)((int64_t)2540 * pg.gw / pg.gh); }
		}
		if (w <= 0 || h <= 0) { w = 2159; h = 2794; }
		memset(sh, 0, sizeof(*sh));
		sh->dpi = dpi;
		sh->w = zpage_mm10_to_px(w, dpi);
		sh->h = zpage_mm10_to_px(h, dpi);
		*paper = NULL;
		*w_mm10 = w;
		*h_mm10 = h;
		return;
	}
	*paper = job_paper(j, fallback);
	job_sheet_geometry(j, *paper, dpi, printer_mm10, sh);
	*w_mm10 = (*paper)->w < (*paper)->h ? (*paper)->w : (*paper)->h;
	*h_mm10 = (*paper)->w < (*paper)->h ? (*paper)->h : (*paper)->w;
}

void job_media_name(const zpage_size_t *paper, char *out, int n) {
	static const struct { const char *z, *pwg; } names[] = {
		{ "letter", "na_letter_8.5x11in" },
		{ "legal",  "na_legal_8.5x14in" },
		{ "a4",     "iso_a4_210x297mm" },
		{ "a5",     "iso_a5_148x210mm" },
		{ "a6",     "iso_a6_105x148mm" },
		{ "4x6",    "na_index-4x6_4x6in" },
		{ "5x7",    "na_5x7_5x7in" },
		{ "3x5",    "na_index-3x5_3x5in" },
	};
	unsigned i;
	for (i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
		if (paper->name && !strcmp(paper->name, names[i].z)) {
			copy(out, n, names[i].pwg);
			return;
		}
	}
	// PWG 5101.1 self-describing: custom_<name>_<w>x<h>mm
	{
		char b[48];
		int k = 0, v, d;
		const char *pre = "custom_zeitlos_";
		int dims[2] = { (paper->w + 5) / 10, (paper->h + 5) / 10 };
		for (d = 0; pre[d]; d++) b[k++] = pre[d];
		for (d = 0; d < 2; d++) {
			char t[8];
			int m = 0;
			v = dims[d];
			do { t[m++] = (char)('0' + v % 10); v /= 10; } while (v);
			while (m) b[k++] = t[--m];
			if (!d) b[k++] = 'x';
		}
		b[k++] = 'm';
		b[k++] = 'm';
		b[k] = 0;
		copy(out, n, b);
	}
}
