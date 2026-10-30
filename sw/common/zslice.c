/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * zslice -- a long call run a slice at a time. See zslice.h.
 */

#include "zslice.h"
#include "zco.h"
#include "zeitlos.h"
#include "zsoc.h"			// Z_TICK_HZ

static zco_t run_co;
static uint32_t run_t0;

static void run_yield(void) {
	if (zco_current() != &run_co) return;
	if (z_uptime_ticks() - run_t0 < Z_TICK_HZ / 100) return;
	zco_yield();
}

bool z_slice_run(void (*fn)(void *), void *arg, void *stack, size_t bytes,
	bool (*pump)(void *), void *ctx) {

	void (*was)(void) = z_slice;
	bool ok = true;

	z_slice = run_yield;
	zco_init(&run_co, fn, arg, stack, bytes);
	for (;;) {
		run_t0 = z_uptime_ticks();
		if (!zco_resume(&run_co)) { ok = !run_co.overflow; break; }
		if (!pump(ctx)) { zco_abandon(&run_co); ok = false; break; }
	}
	z_slice = was;
	return ok;

}
