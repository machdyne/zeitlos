#ifndef PRINT_RASTER_H
#define PRINT_RASTER_H

/*
 * print -- a sheet as grey lines: its pages played onto a band of the
 * sheet at a time (zpaper.h's player, zcanvas underneath), with cut
 * marks and frames, and each line handed on (to the PWG encoder). A
 * Letter sheet at 300 dpi is 2550 x 3300; a band of 64 lines is 160KB,
 * and the sheet never exists whole. docs/print.md, "Raster".
 */

#include <stdint.h>
#include <stdbool.h>

#include "job.h"

typedef bool (*raster_line_fn)(void *ctx, const uint8_t *row, int y);
// Between bands: false stops the sheet (cancelled).
typedef bool (*raster_tick_fn)(void *ctx, int y, int h);

typedef struct {
	const job_t		*job;
	zp_play_t		*play;
	uint8_t			*band;		// ((sheet.w + 3) & ~3) * rows bytes, word-aligned
	int				rows;
	zpage_sheet_t	sheet;
	raster_tick_fn	tick;
	void			*tick_ctx;
} raster_t;

bool raster_sheet(raster_t *r, int s, raster_line_fn line, void *ctx);

#endif
