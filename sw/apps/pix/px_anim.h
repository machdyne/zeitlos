#ifndef PX_ANIM_H
#define PX_ANIM_H

/*
 * pix -- animations: which cells, in which order, how fast. See
 * docs/pix_app.md, "Animation", and docs/zsp.md.
 *
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * A sprite sheet's frames are its cells. An animation is a line of the
 * sheet,
 *
 *   anim run 80 mouse0 mouse1 mouse2 mouse1
 *
 * its name, the milliseconds each frame shows, and the cells by name
 * (one may come more than once). The lines live where the sheet keeps
 * every line it does not edit itself (px_doc_t.extra), so they are
 * written back where they were; this file reads and changes them
 * there. Pure C, no hardware: the host tests use it as it is.
 */

#include "px_doc.h"

#define PX_ANIM_FRAMES  64      /* frames an animation may have */
#define PX_ANIM_MS_MIN  10
#define PX_ANIM_MS_MAX  10000
#define PX_ANIM_DEFAULT_MS 100  /* a sheet with no line for the cell */

typedef struct {
	char name[PX_NAME_MAX];
	int  ms;                    /* each frame's time */
	int  n;                     /* frames */
	int  cell[PX_ANIM_FRAMES];  /* the cells; -1 for a name the sheet lacks */
} px_anim_t;

/* How many anim lines the document has. */
int px_anim_count(const px_doc_t *d);

/* The k-th, its names found among the cells. False if there is none. */
bool px_anim_get(const px_doc_t *d, int k, px_anim_t *a);

/* What F5 plays for cell `cell`: the first animation it is a frame of;
 * or, with none, every cell of its size, in order, at the default
 * time (name ""). Returns the line's number, or -1 for the default.
 * *at is where in it the cell is. */
int px_anim_for(const px_doc_t *d, int cell, px_anim_t *a, int *at);

/* The frames before and after `cell` in what F5 would play (-1 where
 * there is none: a single frame, say): what onion skin shows. */
void px_anim_neighbours(const px_doc_t *d, int cell, int *prev, int *next);

/* An animation from its words, "run 80 mouse0 mouse1": it replaces the
 * line of that name, or is added. Just a name deletes that line. The
 * cells must exist. False, and *err says why, if it is not one. */
bool px_anim_set(px_doc_t *d, const char *words, const char **err);

/* The line for animation k as words (no "anim "): for editing. */
bool px_anim_words(const px_doc_t *d, int k, char *out, int len);

/* A cell was renamed: every animation says the new name. */
void px_anim_rename(px_doc_t *d, const char *was, const char *now);

/* A cell was deleted: it leaves every animation (an animation left
 * with no frames goes too). */
void px_anim_forget(px_doc_t *d, const char *name);

/* Cell `after`'s copy `added` goes right after it in every animation
 * that shows `after` (once per showing). */
void px_anim_insert_after(px_doc_t *d, const char *after, const char *added);

/* A name for the next frame after `name`: its number one up (walk1 ->
 * walk2, run -> run2), the first not taken. */
void px_anim_next_name(const px_doc_t *d, const char *name, char *out);

#endif
