#ifndef ZCONVERT_H
#define ZCONVERT_H

/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * Converting a file in another process: sw/apps/convert, driven by an
 * app that wants a document, a table or an archive in another form.
 * docs/convert.md.
 *
 * -- why another process --
 *
 * A conversion reads a file that may be large and is certainly not
 * trusted, through code (inflate, XML, RTF) that the calling app does
 * not otherwise carry. Run in the caller it would cost every caller
 * that code and the memory, and it could not be stopped except by
 * the caller polling for it. As its own process it costs the caller
 * nothing, it can be stopped at once by killing it, and if it crashes
 * on a hostile file it takes only itself down.
 *
 * -- the protocol --
 *
 *   caller                          convert
 *   ------                          -------
 *   z_conv_start(): writes a job file to /tmp, sets the launch
 *   argument "-j <job>", runs convert, notes its pid and serial
 *                                   reads and deletes the job; writes
 *                                   <out>.part; renames it to <out>
 *                   <- PROGRESS     tag = 0..1000 of the input read,
 *                                   at most ten times a second
 *                   <- DONE         tag = 0, or a ZF_E_* (negative,
 *                                   as uint32_t); then it exits
 *
 * Cancelling is z_conv_cancel(): the caller kills convert and deletes
 * <out>.part. That is immediate -- nothing waits for convert to notice
 * -- and safe: the fs driver closes a dead process's open files before
 * it serves the next request (docs/fs_driver.md, "Owners that are gone"), so
 * the delete finds the file closed. convert in turn checks, a few
 * times a second, that the caller is still the process it was started
 * by (pid AND serial), and stops and cleans up if not: a conversion
 * nobody is waiting for is not left running.
 *
 * The caller hands each message it reads to z_conv_msg(); a caller
 * that might miss DONE (its convert crashed) learns that from
 * z_conv_poll(), which notices the process gone.
 */

#include <stdint.h>
#include <stdbool.h>

#include "zsubjects.h"
#include "zmsg.h"

#define Z_CONVERT_PROGRESS	(Z_SUBJ_CONVERT + 0)
#define Z_CONVERT_DONE		(Z_SUBJ_CONVERT + 1)
Z_SUBJECTS_IN(Z_SUBJ_CONVERT, Z_CONVERT_DONE);

// The result of a convert that disappeared without saying DONE.
#define Z_CONV_E_GONE	(-100)

#define Z_CONV_PATH_MAX	256

typedef struct {
	uint32_t	pid, serial;
	bool		running;
	int32_t		result;			// valid once running is false
	uint16_t	permille;		// last progress
	char		out[Z_CONV_PATH_MAX];
	char		part[Z_CONV_PATH_MAX + 8];
	char		job[48];
} z_conv_t;

// z_conv_start() flags.
#define Z_CONV_UI	1	// convert shows its own window: progress, Cancel, any error
#define Z_CONV_OPEN	2	// and opens the result in the app for it (`out` unused)

// Starts converting `in` into `out`. `to` names the output type
// ("docx", "md", ...) or is NULL to take it from out's extension; for
// an archive, `out` is the folder to extract into. False if convert
// could not be started (c->result says why: Z_CONV_E_GONE when the
// program is missing, -1 when the job file could not be written).
bool z_conv_start(z_conv_t *c, const char *in, const char *out, const char *to,
	uint32_t flags);

// A message for this conversion? Updates c and returns true if so.
bool z_conv_msg(z_conv_t *c, const z_msg_t *m);

// Notices a convert that has gone without saying DONE. Call now and
// then (a timer tick) while c->running.
void z_conv_poll(z_conv_t *c);

// Stops it now, and removes what it had written.
void z_conv_cancel(z_conv_t *c);

// "cancelled", "the file is damaged", ... for c->result.
const char *z_conv_strerror(int32_t result);

#endif
