/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * learn -- text laid out: a block's inline Markdown (lformat.h) broken
 * into lines of styled runs -- bold, italic, code, links, formulas set
 * by lmathdraw, Japanese in its own font and broken between
 * characters -- then drawn.
 */

#ifndef LTEXT_H
#define LTEXT_H

#include "lformat.h"
#include "ldraw.h"

enum { LT_TEXT = 0, LT_MATH, LT_CODE };

typedef struct {
	uint8_t		kind;
	uint8_t		bold, italic, link, cjk;
	int16_t		x;			// from the line's left
	int16_t		w;
	const char	*s;
	int			len;
	int16_t		link_id;	// which link (index into lt_layout_t.links), -1 if none
} lt_run_t;

typedef struct {
	int			first, count;	// runs
	int16_t		asc, desc, w;
} lt_line_t;

#define LT_MAX_RUNS 640
#define LT_MAX_LINES 96
#define LT_MAX_LINKS 16

typedef struct {
	lt_run_t	runs[LT_MAX_RUNS];
	int			nruns;
	lt_line_t	lines[LT_MAX_LINES];
	int			nlines;
	char		links[LT_MAX_LINKS][96];	// targets
	int			nlinks;
	int			height;					// all lines, with their leading
	int			width;					// the widest line
	// set before laying out
	const zc_font_t	*font, *bold_font;	// the face (and its bold) of this text
	int			size;					// px; formulas are set at this size
	int			lead;					// extra pixels between lines
} lt_layout_t;

// Lays out `text` (inline Markdown) in width w, with lt->font etc. set.
// `a` holds the spans while laying out (and formulas while drawing).
void lt_layout(lt_layout_t *lt, lx_arena_t *a, const char *text, int w);
// Draws a layout with its top-left at (x, y).
void lt_draw(const lt_layout_t *lt, ld_page_t *p, lx_arena_t *a, int x, int y, int c);
// The link (index into lt->links) at (px, py) relative to the layout's top-left, or -1.
int lt_link_at(const lt_layout_t *lt, lx_arena_t *a, int px, int py);

// Plain text: what speech reads -- markup removed, formulas as written.
int lt_plain(lx_arena_t *a, const char *text, char *out, int cap);

#endif
