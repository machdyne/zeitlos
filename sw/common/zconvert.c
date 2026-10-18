/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * The caller's side of sw/apps/convert. See zconvert.h.
 */

#include <string.h>

#include "zeitlos.h"
#include "zfsapp.h"
#include "zwin.h"
#include "zpaths.h"
#include "zconvert.h"

// No printf family here: an app that links this should not pay for
// one (files does not otherwise have it).
static int cat(char *d, int n, int cap, const char *s) {
	while (s && *s && n < cap - 1) d[n++] = *s++;
	d[n] = 0;
	return n;
}

static int cat_num(char *d, int n, int cap, uint32_t v, unsigned base) {
	char t[12];
	int k = 0;
	do { t[k++] = "0123456789abcdef"[v % base]; v /= base; } while (v && k < 12);
	while (k && n < cap - 1) d[n++] = t[--k];
	d[n] = 0;
	return n;
}

bool z_conv_start(z_conv_t *c, const char *in, const char *out, const char *to,
	uint32_t flags) {
	static char job[3 * Z_CONV_PATH_MAX + 96];
	int n;
	memset(c, 0, sizeof(*c));
	cat(c->out, 0, sizeof c->out, out);
	n = cat(c->part, 0, sizeof c->part, c->out);
	cat(c->part, n, sizeof c->part, ".part");
	uint32_t me = z_getpid();
	// a name unique to this caller and moment
	n = cat(c->job, 0, sizeof c->job, Z_DIR_TMP "/cv");
	n = cat_num(c->job, n, sizeof c->job, me & 0xFF, 16);
	n = cat_num(c->job, n, sizeof c->job, z_uptime_ticks() & 0xFFFFFF, 16);
	cat(c->job, n, sizeof c->job, ".job");

	n = cat(job, 0, sizeof job, "in=");
	n = cat(job, n, sizeof job, in);
	n = cat(job, n, sizeof job, "\nout=");
	n = cat(job, n, sizeof job, out);
	n = cat(job, n, sizeof job, "\nto=");
	n = cat(job, n, sizeof job, to);
	n = cat(job, n, sizeof job, "\ncaller=");
	n = cat_num(job, n, sizeof job, me, 10);
	n = cat(job, n, sizeof job, ":");
	n = cat_num(job, n, sizeof job, z_proc_serial(me), 10);
	n = cat(job, n, sizeof job, "\n");
	if (flags & Z_CONV_UI) n = cat(job, n, sizeof job, "ui=1\n");
	if (flags & Z_CONV_OPEN) n = cat(job, n, sizeof job, "open=1\n");
	if (n >= (int)sizeof job - 1 || fs_write_file(c->job, job, n) != n) {
		c->result = -1;
		return false;
	}
	char arg[64];
	n = cat(arg, 0, sizeof arg, "-j ");
	cat(arg, n, sizeof arg, c->job);
	z_launch_arg_set(arg);
	c->pid = z_proc_run("convert");
	if (!c->pid) {
		fs_unlink(c->job);
		c->result = Z_CONV_E_GONE;
		return false;
	}
	c->serial = z_proc_serial(c->pid);
	c->running = true;
	return true;
}

bool z_conv_msg(z_conv_t *c, const z_msg_t *m) {
	if (!c->running || m->from != c->pid) return false;
	if (m->subject == Z_CONVERT_PROGRESS) {
		c->permille = (uint16_t)(m->tag > 1000 ? 1000 : m->tag);
		return true;
	}
	if (m->subject == Z_CONVERT_DONE) {
		c->result = (int32_t)m->tag;
		c->running = false;
		if (!c->result) c->permille = 1000;
		return true;
	}
	return false;
}

void z_conv_poll(z_conv_t *c) {
	if (!c->running) return;
	if (z_proc_serial(c->pid) == c->serial) return;
	// gone without a word: crashed, or killed by someone else
	c->running = false;
	c->result = Z_CONV_E_GONE;
	fs_unlink(c->part);
	fs_unlink(c->job);
}

void z_conv_cancel(z_conv_t *c) {
	if (c->running && z_proc_serial(c->pid) == c->serial) z_proc_kill(c->pid);
	c->running = false;
	c->result = -8;				// ZF_E_CANCEL
	fs_unlink(c->part);
	fs_unlink(c->job);
}

const char *z_conv_strerror(int32_t r) {
	switch (r) {
	case 0:				return "done";
	case -1:			return "a read or write failed";
	case -2:			return "not a file of that kind";
	case -3:			return "the file is damaged";
	case -4:			return "the file uses something this cannot read";
	case -5:			return "too large";
	case -6:			return "out of memory";
	case -7:			return "an archive path outside the folder";
	case -8:			return "cancelled";
	case Z_CONV_E_GONE:	return "the converter stopped";
	default:			return "failed";
	}
}
