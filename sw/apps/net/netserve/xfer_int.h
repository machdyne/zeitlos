#ifndef XFER_INT_H
#define XFER_INT_H
/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * Inside xfer.h: what sftp.c and scp.c share. Each one's state begins
 * with an xfer_t, and the generic calls go through its `step` and
 * `free` hooks.
 */
#include "xfer.h"
#include "nsfs.h"

struct xfer {
	uint32_t (*step)(xfer_t *x, int budget);
	void (*abort)(xfer_t *x);

	xfer_log_fn log;
	void *user;
	uint32_t flags;					// XFER_*
	char home[XFER_PATH];

	// input: a ring, so a large WRITE can stream through it
	uint8_t in[XFER_IN];
	uint32_t in_head, in_len;
	bool eof;						// the client will send no more

	uint8_t out[XFER_OUT];
	uint32_t out_len;

	bool done;
	uint32_t status;
};

void xfer_base_init(xfer_t *x, const char *home, uint32_t flags, xfer_log_fn log, void *user);
void xfer_logf(xfer_t *x, const char *fmt, ...);

// The input ring. peek copies up to n bytes from the front without
// taking them; take drops n.
uint32_t xfer_in_peek(const xfer_t *x, uint8_t *dst, uint32_t n);
void xfer_in_take(xfer_t *x, uint32_t n);
// The front run of contiguous bytes, for streaming without a copy.
const uint8_t *xfer_in_front(const xfer_t *x, uint32_t *n);

static inline uint32_t xfer_out_room(const xfer_t *x) { return XFER_OUT - x->out_len; }
// Appends; false (nothing appended) if it does not fit.
bool xfer_emit(xfer_t *x, const void *d, uint32_t n);

#endif
