#ifndef PRINT_VEC_H
#define PRINT_VEC_H

/*
 * print -- a job as vectors: PDF (for a file, or a printer that takes
 * it) and PostScript (a laser printer, on port 9100). docs/print.md,
 * "PDF and PostScript".
 *
 * One writer for both: the drawing is the same operators, and the
 * PostScript prolog defines PDF's short names (m l re f S q Q cm ...)
 * so a page's content is word for word the same text. What differs is
 * the container -- objects and a cross-reference table, or a prolog
 * and %%Page comments -- and how fonts, pictures and patterns are
 * carried.
 *
 * Text stays text: each face is made into a Type 3 font from the
 * glyphs a job uses -- a Hershey glyph as its strokes, a bitmap glyph as
 * an image mask -- so a page of text is a few KB and prints at the
 * printer's own resolution. Pictures go in as 8-bit grey, run-length
 * coded (both formats decode that themselves; no deflate here).
 * Patterns are tiling patterns anchored at the page's corner, one bit a
 * page unit, as on the screen.
 *
 * Sheets are laid out at 720 dpi (a tenth of a point) by zpage, and
 * the content is scaled by 0.1 once.
 */

#include <stdint.h>
#include <stdbool.h>

#include "out.h"
#include "job.h"

enum { VEC_PDF = 0, VEC_PS = 1 };

#define VEC_DPI			720
#define VEC_FONTS		48
#define VEC_IMAGES		64
#define VEC_PATTERNS	64		// on one sheet
#define VEC_PAT_ALL		512		// in one file

typedef struct {
	char			name[ZP_FONT_MAX];
	const zc_font_t	*font;
	uint8_t			italic, bold;		// synthetic, as zcfont draws them
	int32_t			pen;				// stroke fonts: pen in font units, 24.8
	int				obj;				// PDF: the font's object
	int				id;					// /F<id>
	int				n;					// codes used
	uint16_t		cp[256];			// code -> resolved codepoint (0: the box)
} vfont_t;

typedef struct {
	int				fmt;
	out_t			*o;
	const job_t		*job;

	bool			(*font)(void *ctx, const char *name, const zc_font_t **out);
	bool			(*image)(void *ctx, const char *path, int contrast, int bright,
						int maxw, int maxh, const uint8_t **pix, int *w, int *h);
	void			*ctx;
	const zc_font_t	*fallback;

	// PDF bookkeeping: object offsets (xref[obj]), capacity xref_cap
	uint32_t		*xref;
	int				xref_cap, nobj;
	int				*kids;				// page objects (job_sheets() of them)
	int				nkids;

	vfont_t			*fonts;				// VEC_FONTS of them, caller's
	int				nfonts;

	struct {
		char		path[ZP_PATH_MAX];
		int16_t		contrast, bright;
		int			obj, id;
	}				img[VEC_IMAGES];
	int				nimg;

	struct {
		uint8_t		pat[8];
		int			cell;
		int			obj, id;
	}				pat[VEC_PATTERNS];
	int				npat, pat_ids;
	int				pat_all[VEC_PAT_ALL];	// PDF: pattern id -> object, all sheets

	int				sheet_no;
	uint32_t		base;				// o->total where the file began
	uint32_t		skipped;			// things it could not carry
} vec_t;

// The caller sets fmt, o, job, the loaders, xref (+cap; PDF), kids (one
// per sheet; PDF) and fonts (VEC_FONTS of them), then:
bool vec_begin(vec_t *v);
bool vec_sheet(vec_t *v, int s);
bool vec_end(vec_t *v);

#endif
