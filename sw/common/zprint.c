/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * zprint -- printing from an app. See zprint.h and docs/print.md.
 */

#include "zprint.h"

#include <string.h>

#include "zeitlos.h"
#include "zfsapp.h"
#include "zwin.h"
#include "zpaths.h"
#include "zcfg.h"

static int cat(char *d, int n, int cap, const char *s) {
	while (*s && n < cap - 1) d[n++] = *s++;
	d[n] = 0;
	return n;
}

static int cat_hex(char *d, int n, int cap, uint32_t v, int digits) {
	static const char hx[] = "0123456789abcdef";
	int i;
	for (i = digits - 1; i >= 0 && n < cap - 1; i--) d[n++] = hx[(v >> (4 * i)) & 15];
	d[n] = 0;
	return n;
}

static int cat_dec(char *d, int n, int cap, uint32_t v) {
	char t[12];
	int k = 0;
	do { t[k++] = (char)('0' + v % 10); v /= 10; } while (v);
	while (k && n < cap - 1) d[n++] = t[--k];
	d[n] = 0;
	return n;
}

static bool flush(z_print_t *p) {
	if (p->n && !p->failed) {
		if (fs_write_chunk(p->fh, p->buf, p->n) != p->n) p->failed = true;
	}
	p->n = 0;
	return !p->failed;
}

static bool to_job(void *ctx, const void *d, uint32_t len) {
	z_print_t *p = (z_print_t *)ctx;
	const uint8_t *s = (const uint8_t *)d;
	while (len && !p->failed) {
		uint32_t k = (uint32_t)sizeof(p->buf) - (uint32_t)p->n;
		if (k > len) k = len;
		memcpy(p->buf + p->n, s, k);
		p->n += (int)k;
		s += k;
		len -= k;
		if (p->n == (int)sizeof(p->buf)) flush(p);
	}
	return !p->failed;
}

bool z_print_begin(z_print_t *p, const char *title) {
	int n;
	memset(p, 0, sizeof(*p));
	n = cat(p->job, 0, sizeof(p->job), Z_DIR_TMP "/pr");
	n = cat_hex(p->job, n, sizeof(p->job), z_getpid() & 0xFF, 2);
	n = cat_hex(p->job, n, sizeof(p->job), z_uptime_ticks() & 0xFFFFFF, 6);
	cat(p->job, n, sizeof(p->job), ".zpp");
	p->fh = fs_open_write(p->job);
	if (p->fh < 0) { p->result = Z_PRINT_E_FAILED; return false; }
	zp_writer_init(&p->w, to_job, p);
	if (title && title[0]) zp_job(&p->w, "title", title);
	return !p->w.failed;
}

void z_print_abort(z_print_t *p) {
	if (p->fh >= 0) fs_close_handle(p->fh);
	p->fh = -1;
	fs_unlink(p->job);
}

bool z_print_end(z_print_t *p) {
	char arg[96];
	int n;
	if (p->w.in_page) zp_page_end(&p->w);
	flush(p);
	if (fs_close_handle(p->fh) < 0) p->failed = true;
	p->fh = -1;
	if (p->failed || p->w.failed) {
		fs_unlink(p->job);
		p->result = Z_PRINT_E_FAILED;
		return false;
	}
	n = cat(arg, 0, sizeof(arg), "-j ");
	n = cat(arg, n, sizeof(arg), p->job);
	n = cat(arg, n, sizeof(arg), " -c ");
	n = cat_dec(arg, n, sizeof(arg), z_getpid());
	n = cat(arg, n, sizeof(arg), ":");
	cat_dec(arg, n, sizeof(arg), z_proc_serial(z_getpid()));
	z_launch_arg_set(arg);
	p->pid = z_proc_run("print");
	if (!p->pid) {
		fs_unlink(p->job);
		p->result = Z_PRINT_E_GONE;
		return false;
	}
	p->serial = z_proc_serial(p->pid);
	p->running = true;
	return true;
}

bool z_print_msg(z_print_t *p, const z_msg_t *m) {
	if (!p->running || m->from != p->pid) return false;
	if (m->subject == Z_PRINT_PROGRESS) {
		p->permille = (uint16_t)(m->tag > 1000 ? 1000 : m->tag);
		return true;
	}
	if (m->subject == Z_PRINT_DONE) {
		p->result = (int32_t)m->tag;
		p->running = false;
		return true;
	}
	return false;
}

void z_print_poll(z_print_t *p) {
	if (!p->running) return;
	if (z_proc_serial(p->pid) != p->serial) {
		p->running = false;
		p->result = Z_PRINT_E_GONE;
	}
}

// -- the print dialog's answer --

void z_print_opts_init(z_print_opts_t *o) {
	memset(o, 0, sizeof(*o));
	z_cfg_get("apps.print.printer", o->printer, sizeof(o->printer));
	z_cfg_get("apps.print.paper", o->paper_default, sizeof(o->paper_default));
	o->dest = o->printer[0] ? Z_PRINT_TO_PRINTER : Z_PRINT_TO_PDF;
	o->pages = Z_PRINT_PAGES_ALL;
	o->copies = 1;
	o->nup = 1;
}

void z_print_opts_job(const z_print_opts_t *o, zp_writer_t *w) {
	char v[24 + sizeof(o->file)];
	int n;
	if (o->dest != Z_PRINT_TO_PRINTER && o->file[0]) {
		n = cat(v, 0, sizeof(v), o->dest == Z_PRINT_TO_PDF ? "pdf:" : "ps:");
		cat(v, n, sizeof(v), o->file);
		zp_job(w, "dest", v);
	}
	if (o->pages == Z_PRINT_PAGES_CURRENT && o->current > 0) {
		cat_dec(v, 0, sizeof(v), (uint32_t)o->current);
		zp_job(w, "pages", v);
	} else if (o->pages == Z_PRINT_PAGES_RANGE && o->first > 0) {
		n = cat_dec(v, 0, sizeof(v), (uint32_t)o->first);
		if (o->last != o->first) {
			n = cat(v, n, sizeof(v), "-");
			if (o->last > 0) cat_dec(v, n, sizeof(v), (uint32_t)o->last);
		}
		zp_job(w, "pages", v);
	}
	if (o->copies > 1) {
		cat_dec(v, 0, sizeof(v), (uint32_t)o->copies);
		zp_job(w, "copies", v);
	}
	if (o->paper[0]) zp_job(w, "paper", o->paper);
	if (o->nup > 1) {
		cat_dec(v, 0, sizeof(v), (uint32_t)o->nup);
		zp_job(w, "nup", v);
	}
	if (o->dest == Z_PRINT_TO_PRINTER) {
		if (o->tray[0]) zp_job(w, "media-source", o->tray);
		if (o->quality[0]) zp_job(w, "quality", o->quality);
	}
}

bool z_print_text_job(z_print_t *p, const char *path) {
	return zp_job(&p->w, "text", path);
}
