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

	// -- a document job (z_imgjob_start_doc()) --
	uint32_t	*doc;			// the caller's 1bpp document, dithered into
	int			doc_wpl, doc_w, doc_h;
	int			rows_done;		// rows in it so far: 0..h (an interlaced GIF's
								// last pass, a BMP's bottom-up rows: the most
								// any row reached)
	bool		ordered;		// DONE: dithered without error diffusion
								// (zimg.h, was_ordered)
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

/*
 * -- into a document --
 *
 * The same, but the picture is dithered by zimg itself into the
 * caller's 1bpp document (zimg.h's z_img_decode(), in the
 * framebuffer's packing), as sw/apps/view and sw/apps/web show it: no
 * grey image is ever held. `doc` (wpl words a row) is cleared over w x
 * h here, and filled a row at a time; rows_done says how far it has
 * got, so the rows so far can be shown while it runs. When DONE, w x h
 * is what it covers; a picture cut short is DONE too, with what there
 * was and `err` the decoder's code (else Z_IMG_OK).
 *
 * It allocates nothing: `mem` (bytes) holds the decoder's state, the
 * chunk being read and the decoder's stack, which is what is left --
 * Z_IMGJOB_DOC_MEM gives it Z_IMGJOB_STACK, Z_IMGJOB_DOC_MEM_STACK(s)
 * gives it s bytes, and less than Z_IMGJOB_DOC_MIN of stack is refused.
 * On RISC-V the decoders use about 2KB of it at most (PNG; JPEG about
 * 0.5KB), so view and web give it 8KB. So a stopped job holds only the
 * file, which z_imgjob_stop() closes; z_imgjob_take() returns NULL and
 * z_imgjob_end() has nothing to free. Step, stop and progress are as
 * above. False, with state FAILED, if the file cannot be opened or
 * `mem` is too small.
 */
#define Z_IMGJOB_DOC_MEM_STACK(s)	((s) + Z_IMGJOB_CHUNK + sizeof(z_img_t) + 16)
#define Z_IMGJOB_DOC_MEM	Z_IMGJOB_DOC_MEM_STACK(Z_IMGJOB_STACK)
#define Z_IMGJOB_DOC_MIN	(4 * 1024)	// the least stack it accepts

bool z_imgjob_start_doc(z_imgjob_t *j, const char *path, uint32_t *doc,
	int wpl, int w, int h, void *mem, size_t bytes);

#endif
