#ifndef ZIMGJOB_H
#define ZIMGJOB_H

/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * zimgjob -- a picture decoded to grey without stopping the app: read
 * and decoded a slice at a time from the main loop, with its progress
 * known and a stop that takes effect at once. docs/responsive.md.
 *
 *   z_imgjob_t *j = ...;
 *   z_imgjob_start(j, "/media/images/squirrel.jpg", my_alloc, my_free);
 *   in the main loop, when there is no message waiting:
 *       if (z_imgjob_step(j, Z_TICK_HZ / 50) != Z_IMGJOB_RUNNING) ...done
 *       show z_imgjob_permille(j)
 *   to stop:   z_imgjob_stop(j)          (immediately)
 *   the result: j->gray, j->w x j->h (the job's until taken: z_imgjob_take())
 *
 * The file is read here, in the main loop, a chunk at a step; the
 * decoder (zimg.h's z_img_decode_gray(), unchanged) runs as a
 * coroutine (zco.h) that waits for each chunk and hands control back
 * every few rows. zimg decodes one picture at a time in a process, so
 * an app runs one job at a time, and decodes nothing else meanwhile.
 */

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#include "zco.h"
#include "zimg.h"

enum {
	Z_IMGJOB_IDLE = 0,
	Z_IMGJOB_RUNNING,
	Z_IMGJOB_DONE,
	Z_IMGJOB_FAILED,		// not found, not a picture, damaged, no memory
	Z_IMGJOB_STOPPED,		// z_imgjob_stop()
};

#define Z_IMGJOB_STACK	(16 * 1024)	// the decoder's: zimg needs ~3KB
#define Z_IMGJOB_CHUNK	2048

typedef struct {
	int			state;
	int			err;			// FAILED: a Z_IMG_E_* code, or Z_IMGJOB_E_*
	char		path[200];

	uint8_t		*gray;			// DONE: w x h, 0 black .. 255 white
	int			w, h;

	// -- inside --
	void		*(*alloc)(size_t);
	void		(*release)(void *);
	int			fh;
	uint32_t	size, got;		// the file, and how much is read
	uint8_t		*in;			// Z_IMGJOB_CHUNK
	int			in_len, in_pos;
	bool		eof, oom;
	z_img_t		*im;
	z_img_fmt_t	fmt;
	zco_t		co;
	void		*stack;
	int			rows;			// rows since the decoder last handed back
} z_imgjob_t;

#define Z_IMGJOB_E_OPEN		(-100)	// cannot open the file
#define Z_IMGJOB_E_MEM		(-101)	// no memory for it

// Starts a job (opens the file). False, with state FAILED, if it cannot.
bool z_imgjob_start(z_imgjob_t *j, const char *path, void *(*alloc)(size_t),
	void (*release)(void *));

// Works on it for about `ticks` (zsoc.h's Z_TICK_HZ a second), then
// returns its state: RUNNING while there is more.
int z_imgjob_step(z_imgjob_t *j, uint32_t ticks);

// 0..1000: how much of the file has been read and decoded.
uint16_t z_imgjob_permille(const z_imgjob_t *j);

// Stops it now: the file closed, its memory freed. State STOPPED.
void z_imgjob_stop(z_imgjob_t *j);

// The decoded grey, now the caller's (freed with the job's `release`);
// the job forgets it. NULL unless DONE.
uint8_t *z_imgjob_take(z_imgjob_t *j);

// Frees what a finished job still holds (a result not taken).
void z_imgjob_end(z_imgjob_t *j);

#endif
