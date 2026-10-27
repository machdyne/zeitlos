/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * learn -- formulas typeset (docs/learn_format.md, "Formulas"): stacked
 * fractions, powers, subscripts, roots, Greek letters by name, in the
 * Hershey serif faces at any size, so the same formula reads in a line
 * of text, on a line of its own and on paper.
 */

#ifndef LMATHDRAW_H
#define LMATHDRAW_H

#include "lmath.h"
#include "ldraw.h"

typedef struct {
	int		w, asc, desc;	// pixels: width, above and below the baseline
} lm_box_t;

// Measures n set at `size` px (the em of the surrounding text).
lm_box_t lm_measure(const lm_node_t *n, int size);
// Draws n with its baseline's left end at (x, base). Returns its box.
lm_box_t lm_draw(ld_page_t *p, const lm_node_t *n, int size, int x, int base, int c);

// Parse and set a formula given as text; a formula that does not parse is
// drawn as its text (learncheck has said so already).
lm_box_t lm_draw_str(ld_page_t *p, lx_arena_t *a, const char *s, int len, int size,
	int x, int base, int c, bool measure_only);

#endif
