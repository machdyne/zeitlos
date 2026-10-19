/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * zimgjob -- see zimgjob.h.
 */

#include "zimgjob.h"

#include <string.h>

#include "zeitlos.h"
#include "zfsapp.h"

// Rows the decoder does before it hands back, so the main loop sees the
// clock: a 640-pixel grey row is a few thousand instructions.
#define ROWS_A_TURN 4

// -- inside the coroutine --------------------------------------------------

static int job_read(void *ctx, uint8_t *buf, int len) {
	z_imgjob_t *j = ctx;
	int n = 0;
	while (n < len) {
		int k;
		if (j->in_pos == j->in_len) {
			if (j->eof) break;
			zco_yield();			// the main loop reads the next chunk
			continue;
		}
		k = j->in_len - j->in_pos;
		if (k > len - n) k = len - n;
		memcpy(buf + n, j->in + j->in_pos, (size_t)k);
		j->in_pos += k;
		n += k;
	}
	return n;
}

static void job_row(void *user, const uint8_t *g, int y, int w) {
	z_imgjob_t *j = user;
	if (!j->gray && !j->oom) {
		size_t bytes = (size_t)j->im->out_w * (size_t)j->im->out_h;
		j->w = j->im->out_w;
		j->h = j->im->out_h;
		j->gray = bytes ? j->alloc(bytes) : NULL;
		if (!j->gray) j->oom = true;
		else memset(j->gray, 255, bytes);
	}
	if (j->gray && y >= 0 && y < j->h && w <= j->w)
		memcpy(j->gray + (size_t)y * j->w, g, (size_t)w);
	if (++j->rows >= ROWS_A_TURN) {
		j->rows = 0;
		zco_yield();
	}
}

static void job_body(void *arg) {
	z_imgjob_t *j = arg;
	int rv = z_img_decode_gray(j->im, j->fmt, job_row, j);
	if (j->oom) { j->state = Z_IMGJOB_FAILED; j->err = Z_IMGJOB_E_MEM; return; }
	// a picture cut short still shows what there was
	if (rv != Z_IMG_OK && !j->gray) { j->state = Z_IMGJOB_FAILED; j->err = rv; return; }
	j->state = Z_IMGJOB_DONE;
}

// -- the main loop's side ------------------------------------------------------

static void release_all(z_imgjob_t *j, bool keep_gray) {
	if (j->fh >= 0) fs_close_handle(j->fh);
	j->fh = -1;
	if (j->stack) j->release(j->stack);
	if (j->in) j->release(j->in);
	if (j->im) j->release(j->im);
	j->stack = NULL;
	j->in = NULL;
	j->im = NULL;
	if (!keep_gray && j->gray) { j->release(j->gray); j->gray = NULL; }
}

static bool fail(z_imgjob_t *j, int err) {
	j->state = Z_IMGJOB_FAILED;
	j->err = err;
	release_all(j, false);
	return false;
}

static void refill(z_imgjob_t *j) {
	int n;
	if (j->eof || j->in_pos < j->in_len) return;
	n = fs_read_chunk(j->fh, j->in, Z_IMGJOB_CHUNK);
	if (n <= 0) { j->eof = true; j->in_len = j->in_pos = 0; return; }
	j->in_len = n;
	j->in_pos = 0;
	j->got += (uint32_t)n;
}

bool z_imgjob_start(z_imgjob_t *j, const char *path, void *(*alloc)(size_t),
	void (*release)(void *)) {
	int i, n;
	memset(j, 0, sizeof(*j));
	j->fh = -1;
	j->alloc = alloc;
	j->release = release;
	for (i = 0; i < (int)sizeof(j->path) - 1 && path[i]; i++) j->path[i] = path[i];
	j->state = Z_IMGJOB_RUNNING;
	n = fs_size((char *)path);
	j->size = n > 0 ? (uint32_t)n : 0;
	j->fh = fs_open_read(path);
	if (j->fh < 0) return fail(j, Z_IMGJOB_E_OPEN);
	j->in = alloc(Z_IMGJOB_CHUNK);
	j->im = alloc(sizeof(z_img_t));
	j->stack = alloc(Z_IMGJOB_STACK);
	if (!j->in || !j->im || !j->stack) return fail(j, Z_IMGJOB_E_MEM);
	// the first chunk says what the file is
	refill(j);
	j->fmt = z_img_sniff(j->in, j->in_len < 64 ? j->in_len : 64);
	memset(j->im, 0, sizeof(z_img_t));
	j->im->read = job_read;
	j->im->ctx = j;
	j->im->doc_w = Z_IMG_MAX_W;
	j->im->doc_h = Z_IMG_MAX_H;
	zco_init(&j->co, job_body, j, j->stack, Z_IMGJOB_STACK);
	return true;
}

int z_imgjob_step(z_imgjob_t *j, uint32_t ticks) {
	uint32_t t0 = z_uptime_ticks();
	if (j->state != Z_IMGJOB_RUNNING) return j->state;
	do {
		refill(j);
		if (!zco_resume(&j->co)) {
			// the decoder is finished, one way or another
			if (j->state == Z_IMGJOB_RUNNING) j->state = Z_IMGJOB_FAILED;
			release_all(j, j->state == Z_IMGJOB_DONE);
			return j->state;
		}
	} while (z_uptime_ticks() - t0 < ticks);
	return j->state;
}

uint16_t z_imgjob_permille(const z_imgjob_t *j) {
	uint32_t pm;
	if (j->state == Z_IMGJOB_DONE) return 1000;
	if (!j->size) return 0;
	// what is read is a step ahead of what is decoded: count it half
	// way through the chunk in hand
	pm = (uint32_t)(((uint64_t)(j->got - (uint32_t)(j->in_len - j->in_pos)) * 1000) / j->size);
	return (uint16_t)(pm > 999 ? 999 : pm);
}

void z_imgjob_stop(z_imgjob_t *j) {
	if (j->state != Z_IMGJOB_RUNNING) return;
	zco_abandon(&j->co);
	release_all(j, false);
	j->state = Z_IMGJOB_STOPPED;
}

uint8_t *z_imgjob_take(z_imgjob_t *j) {
	uint8_t *g;
	if (j->state != Z_IMGJOB_DONE) return NULL;
	g = j->gray;
	j->gray = NULL;
	return g;
}

void z_imgjob_end(z_imgjob_t *j) {
	if (j->state == Z_IMGJOB_RUNNING) z_imgjob_stop(j);
	if (j->gray && j->release) j->release(j->gray);
	j->gray = NULL;
}
