#ifndef ZSLICE_H
#define ZSLICE_H

/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * A checkpoint in a long computation -- a big-number exponentiation, an
 * elliptic-curve ladder -- where it may hand control back.
 *
 * The computation does not know or care: z_slice_point() calls z_slice
 * if someone has set it, and nothing else. An app that runs such work
 * in a coroutine (zco.h) points z_slice at a function that yields once
 * its slice of time is used, so a handshake's arithmetic runs a few
 * milliseconds at a time between the app's messages (sw/apps/web,
 * docs/web_app.md "Nothing waits"). Everything else -- net's SSH, the
 * host tests -- leaves it NULL and pays one load and branch a call.
 *
 * Defined weak, here, so that every program that links code with a
 * checkpoint has the variable without having to link anything else.
 */

__attribute__((weak)) void (*z_slice)(void) = 0;

static inline void z_slice_point(void) {
	if (z_slice) z_slice();
}

// -- running one: z_slice_run() (zslice.c) --
//
// Runs fn(arg) in a coroutine on `stack`, about 10 ms at a time: after
// each slice, pump(ctx) -- the app answers its messages, as it would
// in its main loop -- and on again. False from pump() stops it there:
// the coroutine is abandoned, which is safe for work that holds only
// memory (a render into a buffer of the app's). True if fn returned,
// false if it was stopped or ran off its stack.
//
// For a call that cannot be cut into steps and is not worth a state
// machine of its own: drawing an SVG, say. One at a time; z_slice is
// set while it runs and put back after.
#include <stdbool.h>
#include <stddef.h>
bool z_slice_run(void (*fn)(void *), void *arg, void *stack, size_t bytes,
	bool (*pump)(void *), void *ctx);

#endif
