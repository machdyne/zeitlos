#ifndef ZPRINT_H
#define ZPRINT_H

/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * Printing from an app: describe the pages (zpaper.h), and the print
 * service (sw/apps/print) lays them on sheets and sends them to the
 * printer, or writes a PDF or PostScript file. docs/print.md.
 *
 *   z_print_t pr;
 *   if (z_print_begin(&pr, "My deck")) {
 *       zp_job(&pr.w, "nup", "4");             // options, before pages
 *       for (each page) {
 *           zc_xf_t xf;
 *           zc_target_t *t = zp_page_begin(&pr.w, &shape, &xf);
 *           draw_page(t, &xf);                 // the window's own code
 *           zp_page_end(&pr.w);
 *       }
 *       z_print_end(&pr);                       // print takes it from here
 *   }
 *
 * -- the protocol --
 *
 * As convert's (zconvert.h): the job is a file in /tmp; print is run
 * with "-j <job> -c <pid>:<serial>", shows its own window (what it is
 * doing, Cancel, any error), deletes the job when it has read it, and
 * tells the caller how far it has got and how it ended:
 *
 *                   <- PROGRESS     tag = 0..1000
 *                   <- DONE         tag = 0, or a Z_PRINT_E_* (negative,
 *                                   as uint32_t); then it exits
 *
 * An app need not listen: print's window says everything. One that
 * does hands each message to z_print_msg().
 */

#include <stdint.h>
#include <stdbool.h>

#include "zsubjects.h"
#include "zmsg.h"
#include "zpaper.h"

#define Z_PRINT_PROGRESS	(Z_SUBJ_PRINT + 0)
#define Z_PRINT_DONE		(Z_SUBJ_PRINT + 1)
Z_SUBJECTS_IN(Z_SUBJ_PRINT, Z_PRINT_DONE);

#define Z_PRINT_E_FAILED	(-1)	// the printer or the file: see print's window
#define Z_PRINT_E_CANCELLED	(-2)
#define Z_PRINT_E_GONE		(-100)	// print is not on the card, or stopped

typedef struct {
	zp_writer_t	w;				// what the pages are written with
	int			fh;				// the job file being written
	uint8_t		buf[2048];		// written in chunks of this
	int			n;
	bool		failed;
	char		job[48];		// its path, /tmp/prNNNN.zpp
	uint32_t	pid, serial;	// print, once started
	bool		running;
	int32_t		result;
	uint16_t	permille;
} z_print_t;

// Starts a job: a file in /tmp, its title the first option. False if
// the file cannot be made.
bool z_print_begin(z_print_t *p, const char *title);

// Finishes the job and starts print on it. False (and the job removed)
// if writing failed or print could not be started (p->result says
// which: Z_PRINT_E_FAILED, Z_PRINT_E_GONE).
bool z_print_end(z_print_t *p);

// Abandons a job not yet ended.
void z_print_abort(z_print_t *p);

// A message for this job? Updates p and returns true if so.
bool z_print_msg(z_print_t *p, const z_msg_t *m);

// Notices a print that has gone without saying DONE.
void z_print_poll(z_print_t *p);

#endif
