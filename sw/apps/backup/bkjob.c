/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * bk_*() a slice at a time. See bkjob.h.
 */

#include <string.h>
#include <stdio.h>

#include "../../common/zco.h"

#include "bkjob.h"

static bk_io_t base;            // the files, clock, owner
static bk_io_t io;              // the same, handing back after each call
static zco_t co;
static void *stack;
static size_t stack_bytes;
static bkjob_t job;
static bool running, cancel;

// -- handing back ------------------------------------------------------------

static void hand_back(void) {
	if (zco_current() == &co) zco_yield();
}

#define WRAP0(ret, name, params, args) \
	static ret y_##name params { ret r = base.name args; hand_back(); return r; }

WRAP0(int, open_read, (void *c, const char *p), (c, p))
WRAP0(int, open_write, (void *c, const char *p), (c, p))
WRAP0(int, read, (void *c, int h, void *b, int n), (c, h, b, n))
WRAP0(int, write, (void *c, int h, const void *b, int n), (c, h, b, n))
WRAP0(int, seek, (void *c, int h, uint32_t off), (c, h, off))
WRAP0(int, sync, (void *c, int h), (c, h))
WRAP0(int, close, (void *c, int h), (c, h))
WRAP0(int, stat, (void *c, const char *p, bk_stat_t *st), (c, p, st))
WRAP0(int, unlink, (void *c, const char *p), (c, p))
WRAP0(int, mkdir, (void *c, const char *p), (c, p))
WRAP0(int, rename, (void *c, const char *a, const char *b), (c, a, b))
WRAP0(int, free_kb, (void *c, const char *p, uint32_t *kb), (c, p, kb))

// A listing comes a page at a time (zfsc.c's READDIR): handing back
// after each entry puts the next page's request in a slice of its own.
typedef struct { int (*fn)(void *, const char *, const bk_stat_t *); void *arg; } list_t;
static int list_entry(void *a, const char *name, const bk_stat_t *st) {
	list_t *l = a;
	int r = l->fn(l->arg, name, st);
	hand_back();
	return r;
}
static int y_list(void *c, const char *p,
		int (*fn)(void *arg, const char *name, const bk_stat_t *st), void *arg) {
	list_t l = { fn, arg };
	int r = base.list(c, p, list_entry, &l);
	hand_back();
	return r;
}

static void y_out(void *c, const char *t) {
	(void)c;
	// whole lines, the last few kept; a line longer than the window's is cut
	static char part[BKJOB_LINE_MAX];
	static int np;
	for (; *t; t++) {
		if (*t == '\r') continue;
		if (*t != '\n') { if (np < BKJOB_LINE_MAX - 1) part[np++] = *t; continue; }
		part[np] = 0;
		snprintf(job.lines[job.nlines % BKJOB_LINES], BKJOB_LINE_MAX, "%s", part);
		job.nlines++;
		job.said = true;
		np = 0;
	}
}

static void y_progress(void *c, const bk_progress_t *p) {
	(void)c;
	job.prog = *p;
	if (p->path) snprintf(job.path_now, sizeof job.path_now, "%s", p->path);
	job.prog.path = job.path_now;
}

static bool y_stop(void *c) { (void)c; return cancel; }
static void y_yield(void *c) { (void)c; hand_back(); }

// -- the call ---------------------------------------------------------------

static void run(void *arg) {
	(void)arg;
	bkjob_t *j = &job;
	const char *b = j->backup[0] ? j->backup : NULL;
	switch (j->kind) {
	case BKJOB_BACKUP:  j->rc = bk_backup(&io, &j->o, &j->r); break;
	case BKJOB_SUMMARY: j->rc = bk_glance(&io, &j->o, &j->sum); break;
	case BKJOB_LIST:    j->rc = bk_list(&io, &j->o, j->list_fn, j->arg); break;
	case BKJOB_FILES:   j->rc = bk_files(&io, &j->o, b, j->path[0] ? j->path : "/", j->file_fn, j->arg); break;
	case BKJOB_HISTORY: j->rc = bk_history(&io, &j->o, j->path, j->hist_fn, j->arg); break;
	case BKJOB_RESTORE: j->rc = bk_restore(&io, &j->o, b, j->path, j->out[0] ? j->out : NULL, j->replace, &j->r); break;
	case BKJOB_CHECK:   j->rc = bk_check(&io, &j->o, j->all, &j->r); break;
	case BKJOB_PIN:     j->rc = bk_pin(&io, &j->o, b, j->pin); break;
	default:            j->rc = BK_USAGE; break;
	}
}

void bkjob_init(const bk_io_t *b, void *st, size_t bytes) {
	base = *b;
	io = *b;
	io.open_read = y_open_read; io.open_write = y_open_write;
	io.read = y_read; io.write = y_write; io.seek = y_seek;
	io.sync = y_sync; io.close = y_close; io.stat = y_stat;
	io.list = y_list; io.unlink = y_unlink; io.mkdir = y_mkdir;
	io.rename = y_rename; io.free_kb = y_free_kb;
	io.out = y_out; io.progress = y_progress; io.stop = y_stop; io.yield = y_yield;
	stack = st;
	stack_bytes = bytes;
}

bool bkjob_start(const bkjob_t *j) {
	if (running) return false;
	job = *j;
	job.rc = BK_FAILED;
	memset(&job.r, 0, sizeof job.r);
	memset(&job.prog, 0, sizeof job.prog);
	job.nlines = 0;
	job.said = false;
	job.slices = 0;
	job.path_now[0] = 0;
	cancel = false;
	zco_init(&co, run, NULL, stack, stack_bytes);
	running = true;
	return true;
}

bool bkjob_step(void) {
	if (!running) return false;
	job.slices++;
	bool more = zco_resume(&co);
	if (!more) {
		running = false;
		job.stack_used = (uint32_t)zco_stack_used(&co);
		if (co.overflow) {
			// ran off its stack: what it wrote below is not to be trusted
			// either, but the drive is: nothing it holds is half written
			// in a way the next backup does not clean up (bk.h)
			job.rc = BK_FAILED;
			y_out(NULL, "backup: out of stack (a bug: please report it); stopped\n");
		}
	}
	return more;
}

bool bkjob_running(void) { return running; }
void bkjob_cancel(void) { if (running) cancel = true; }
bool bkjob_cancelling(void) { return running && cancel; }
bkjob_t *bkjob_current(void) { return &job; }

const char *bkjob_last_line(void) {
	job.said = false;
	return job.nlines ? job.lines[(job.nlines - 1) % BKJOB_LINES] : "";
}
