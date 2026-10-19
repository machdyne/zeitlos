/*
 * print -- sheets in bands. See raster.h.
 */

#include "raster.h"

#include <string.h>

#define MAX_MARKS (JOB_MAX_NUP * 8)

bool raster_sheet(raster_t *r, int s, raster_line_fn line, void *ctx) {
	const zpage_sheet_t *sh = &r->sheet;
	int page[JOB_MAX_NUP];
	zpage_cell_t cell[JOB_MAX_NUP];
	static int marks[MAX_MARKS][4];
	int n = job_sheet(r->job, s, sh, page, cell);
	int nm = 0, y0, i;
	// a hairline: a quarter of a point, never under one dot
	int hair = sh->dpi / 288 > 0 ? sh->dpi / 288 : 1;
	zc_paint_t white = zc_white(), black = zc_black(), grey = zc_shade(10);
	// rows a whole number of words apart: every line then starts on a
	// word, which is what the PWG coder's word compares need
	int stride = (sh->w + 3) & ~3;

	if (r->job->cut && n > 0)
		nm = zpage_cut_marks(sh, cell, n, zpage_mm10_to_px(50, sh->dpi), marks, MAX_MARKS);

	for (y0 = 0; y0 < sh->h; y0 += r->rows) {
		zc_target_t t;
		int rows = sh->h - y0 < r->rows ? sh->h - y0 : r->rows;
		zc_target_gray(&t, r->band, stride, sh->w, rows);
		zc_target_origin(&t, 0, y0);
		zc_clear(&t, &white);

		for (i = 0; i < n; i++) {
			const zpage_cell_t *c = &cell[i];
			if (c->y >= y0 + rows || c->y + c->h <= y0) continue;
			zc_clip_reset(&t);
			zc_clip(&t, c->x, c->y, c->x + c->w, c->y + c->h);
			zp_play_page(r->play, r->job->data + r->job->start[page[i]],
				r->job->bytes[page[i]], &t, &c->xf);
			if (r->job->frame) {
				zc_clip_reset(&t);
				zc_fill_rect_dev(&t, c->x - hair, c->y - hair, c->w + 2 * hair, hair, &grey);
				zc_fill_rect_dev(&t, c->x - hair, c->y + c->h, c->w + 2 * hair, hair, &grey);
				zc_fill_rect_dev(&t, c->x - hair, c->y, hair, c->h, &grey);
				zc_fill_rect_dev(&t, c->x + c->w, c->y, hair, c->h, &grey);
			}
		}
		zc_clip_reset(&t);
		for (i = 0; i < nm; i++) {
			int x0 = marks[i][0], ya = marks[i][1], x1 = marks[i][2], yb = marks[i][3];
			if (x0 == x1) {
				int a = ya < yb ? ya : yb, b = ya < yb ? yb : ya;
				zc_fill_rect_dev(&t, x0 - hair / 2, a, hair, b - a, &black);
			} else {
				int a = x0 < x1 ? x0 : x1, b = x0 < x1 ? x1 : x0;
				zc_fill_rect_dev(&t, a, ya - hair / 2, b - a, hair, &black);
			}
		}

		for (i = 0; i < rows; i++)
			if (!line(ctx, r->band + (size_t)i * stride, y0 + i)) return false;
		if (r->tick && !r->tick(r->tick_ctx, y0 + rows, sh->h)) return false;
	}
	return true;
}
