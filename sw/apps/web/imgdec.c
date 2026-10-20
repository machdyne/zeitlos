/*
 * Zeitlos -- sw/apps/web
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * imgdec -- see imgdec.h.
 */

#include "imgdec.h"

#include <string.h>

#include "../../common/zfsapp.h"

// The decoder's stack, its state and the chunk being read.
static uint8_t mem[Z_IMGJOB_DOC_MEM_STACK(IMGDEC_STACK)] __attribute__((aligned(8)));

static void over(imgdec_t *d) {
	z_imgjob_end(&d->job);			// the file is closed by now
	if (d->spool[0]) fs_unlink(d->spool);
	d->spool[0] = 0;
	d->on = false;
}

bool imgdec_start(imgdec_t *d, const char *spool, uint32_t *bits, int wpl,
	int w, int h) {
	int i;
	imgdec_stop(d);
	for (i = 0; i < (int)sizeof(d->spool) - 1 && spool[i]; i++) d->spool[i] = spool[i];
	d->spool[i] = 0;
	d->on = true;
	if (!z_imgjob_start_doc(&d->job, spool, bits, wpl, w, h, mem, sizeof(mem))) {
		over(d);
		return false;
	}
	return true;
}

int imgdec_step(imgdec_t *d, uint32_t ticks) {
	int st;
	if (!d->on) return d->job.state;
	st = z_imgjob_step(&d->job, ticks);
	if (st != Z_IMGJOB_RUNNING) over(d);
	return st;
}

uint16_t imgdec_permille(const imgdec_t *d) {
	return d->on ? z_imgjob_permille(&d->job) : 0;
}

void imgdec_stop(imgdec_t *d) {
	if (!d->on) return;
	z_imgjob_stop(&d->job);
	over(d);
}
