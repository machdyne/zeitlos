#ifndef WEB_IMGDEC_H
#define WEB_IMGDEC_H

/*
 * Zeitlos -- sw/apps/web
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * imgdec -- a picture that has arrived in its spool file, decoded into
 * its placeholder box a slice at a time from the main loop, so the page
 * keeps answering and Esc / Stop ends it at once (docs/responsive.md,
 * docs/web_app.md "Pictures").
 *
 * sw/common/zimgjob.h does the work: it reads the spool a chunk a step
 * and runs zimg's decoder in a coroutine that dithers into the box's
 * bitmap. What this adds is the spool: however the decode ends --
 * done, failed, stopped, or dropped because the page changed -- the
 * spool's handle is closed and the file removed, so nothing is left
 * open on the RAM disk. It allocates nothing; its memory is its own
 * (static), and only one picture is decoded at a time.
 */

#include <stdint.h>
#include <stdbool.h>

#include "../../common/zimgjob.h"

// The decoder's stack: zimg uses about 2KB of it at most on the board
// (PNG; JPEG about 0.5KB).
#define IMGDEC_STACK (8 * 1024)

typedef struct {
	z_imgjob_t	job;
	bool		on;				// a decode is under way
	char		spool[200];		// removed when it ends
} imgdec_t;

// Starts decoding the picture in `spool` into `bits` (wpl words a row),
// at most w x h, cleared first. False, with the spool removed and
// d->job.err set, when it cannot start.
bool imgdec_start(imgdec_t *d, const char *spool, uint32_t *bits, int wpl,
	int w, int h);

// About `ticks` of work. Z_IMGJOB_RUNNING while there is more; any
// other state means it is over, the spool closed and removed. DONE:
// d->job.w x d->job.h is in the bitmap (a picture cut short is DONE
// too, with d->job.err its decoder's code).
int imgdec_step(imgdec_t *d, uint32_t ticks);

// 0..1000.
uint16_t imgdec_permille(const imgdec_t *d);

// Stops it now: the spool closed and removed. Nothing if none runs.
void imgdec_stop(imgdec_t *d);

static inline bool imgdec_busy(const imgdec_t *d) { return d->on; }

#endif
