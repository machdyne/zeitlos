#ifndef PRINT_RUN_H
#define PRINT_RUN_H

/*
 * print -- a job from start to finish: rendered as the destination
 * wants it and sent as it is made, never held whole. The same on the
 * board and in the host tests; only the transport differs (net's raw
 * socket, or a POSIX one). docs/print.md, "Inside".
 */

#include <stdint.h>
#include <stdbool.h>

#include "out.h"
#include "job.h"
#include "ipp.h"
#include "pwg.h"
#include "vec.h"

typedef struct {
	// Connects to u->host:u->port; false (with a reason) if it cannot.
	bool		(*connect)(void *ctx, const ipp_url_t *u, char *err, int n);
	void		(*close)(void *ctx);
	out_t		*sock;			// writes, once connected
	net_read_fn	read;			// reads, once connected
	void		*ctx;
} transport_t;

typedef struct {
	// Now and then while it works: sheet (0-based) of sheets, and how
	// far down it, in lines. False cancels.
	bool		(*tick)(void *ctx, int sheet, int sheets, int y, int h);
	// Waits ms, saying why ("the printer is busy"); false cancels. NULL:
	// a busy printer is an error.
	bool		(*wait)(void *ctx, int ms, const char *why);
	void		*ctx;
	uint32_t	skipped;		// things not drawn (fonts, pictures)
	uint32_t	bytes;			// sent or written
	char		err[96];		// why it failed
	const char	*format;		// what went to the printer: "PDF" ...
	int			dpi;			// at what resolution (raster)
} run_t;

// a then b into d (cap bytes), cut short if need be: messages, without
// printf
void run_say(char *d, int cap, const char *a, const char *b);

// Memory: band, paths and the vector writer's tables come from the pool
// (zpool.h); run_init() sizes the paths.
void run_init(void);

// To a file (or any out_t): PWG raster, PDF or PostScript.
bool run_raster(run_t *r, const job_t *j, out_t *o, int dpi, int space,
	const int margins_mm10[4]);
bool run_vec(run_t *r, const job_t *j, out_t *o, int fmt);

// Asks an IPP printer what it takes.
bool run_probe(run_t *r, transport_t *t, const ipp_url_t *u, ipp_info_t *info);

// Prints over IPP: probes, picks the format -- PDF if the printer takes
// it, else PWG raster (sgray_8, or srgb_8; 300 dpi or the nearest it
// has), else PostScript -- and the margins, and sends the job as it is
// made. `prefer` "pwg" (or "raster"), "pdf" or "ps" asks for that one
// where the printer takes it; NULL or "auto" is the order above.
bool run_ipp(run_t *r, transport_t *t, const ipp_url_t *u, const job_t *j,
	const char *prefer);

// PostScript to a socket printer (port 9100, a laser).
bool run_socket(run_t *r, transport_t *t, const ipp_url_t *u, const job_t *j);

#endif
