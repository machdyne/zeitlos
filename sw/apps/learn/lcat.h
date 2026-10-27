/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * learn -- the catalogue: every course learn can find, from each
 * course's course.cfg. Courses are looked for in the edu packs
 * (/opt/ark/edu*: edu-zeitlos, edu, edu-k8, ...) and in a folder of
 * courses on a stick (/usb/edu).
 * The scan reads one course a step, so the window answers throughout.
 */

#ifndef LCAT_H
#define LCAT_H

#include "lformat.h"

#define LK_MAX 400

enum { LK_ALL = 0, LK_SCHOOL, LK_COLLEGE, LK_LIFE, LK_LANG, LK_ZEIT, LK_AREAS };

typedef struct {
	lf_info_t	*c[LK_MAX];		// sorted: area, level, title
	int			n;
	// the scan
	char		pending[LK_MAX][LF_PATH];
	int			npending, next;
	bool		scanning;
	void		*(*alloc)(unsigned long);	// where entries live
} lk_t;

extern lk_t LK;

void lk_begin(void *(*alloc)(unsigned long));
// Reads the next course; false when the scan is done (then the list is sorted).
bool lk_step(void);
int lk_area_of(const lf_info_t *m);		// LK_SCHOOL ...
const char *lk_area_name(int area);
// "Grade 3", "Kindergarten", "High school", "College", "Life", ...
void lk_level_name(const lf_info_t *m, char *out, int cap);
// the course with this id, or NULL
const lf_info_t *lk_find(const char *id);
// how far down the school levels (K=0 .. 12, ms=7, hs=10, col=13, others 20)
int lk_level_rank(const char *level);

#endif
