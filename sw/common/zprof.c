/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * zprof -- storage for the phase timers. See zprof.h.
 *
 * There is deliberately no report function here. Formatting the
 * numbers means printing them, and printing means deciding between
 * printf and a direct UART write -- a decision that belongs to the
 * app, not to a module every app might link. sw/apps/view formats its
 * own report through the UART precisely so that enabling the
 * profiler does not drag ~100KB of stdio into a binary that had
 * carefully avoided it (docs/app_runtime.md).
 */

#include <string.h>

#include "zprof.h"

#if Z_PROF

z_prof_slot_t z_prof_slot[Z_PROF_MAX] __attribute__((section(".bss")));

void z_prof_reset(void) {
	memset(z_prof_slot, 0, sizeof(z_prof_slot));
}

#endif
