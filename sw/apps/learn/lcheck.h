/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * learn -- checking courses (docs/learn_format.md, "Checks"; the tool is
 * tools/learn/learncheck).
 *
 * Every rule the format states and a program can verify is here: the
 * structure, every question having a checkable right answer and every
 * wrong answer its feedback, answers COMPUTED from their formulas,
 * claims in (true ...) and (false ...) evaluated, links and pictures
 * present, text the fonts can draw, steps short enough to teach, and
 * the review a course's class requires before it is released. The same
 * code runs on the build machine and (later) on Zeitlos.
 */

#ifndef LCHECK_H
#define LCHECK_H

#include <stdbool.h>

#include "lformat.h"

enum { LC_ERROR = 0, LC_WARN, LC_NOTE };

typedef struct lc_ctx lc_ctx_t;
struct lc_ctx {
	const lf_fs_t	*fs;
	bool			release;	// apply the release rules (class B needs its review, ...)
	// a problem: file is a path, line 0 for the file as a whole
	void			(*report)(lc_ctx_t *c, int level, const char *file, int line, const char *msg);
	void			*user;
	// courses known to the check (for `needs` and links between courses); may be NULL
	const char		*const *known_courses;
	int				nknown;
	// counts, filled in
	int				errors, warnings;
	int				lessons, steps, checks, cards, figures, words, minutes;
	// memory for one lesson at a time (>= 256 KB is plenty)
	void			*mem;
	unsigned long	memsize;
	// runs a step's figures (its figure blocks joined), if given: the
	// course's folder for its lib/*.scm; reports through report() and
	// returns the number of errors. NULL: brackets and strings only.
	int				(*run_figure)(lc_ctx_t *c, const char *course_path, const char *src,
						const char *file, int line);
};

// Checks one course folder. Returns the number of errors found in it.
int lc_check_course(lc_ctx_t *c, const char *path);

// Flesch-Kincaid grade of some English prose (words, sentences, syllables
// estimated); exposed for the tests.
double lc_reading_grade(const char *text, int *words_out);

// Can the system's fonts draw this UTF-8 character? (Latin-9 everywhere,
// Japanese where the course is lang = ja.)
bool lc_drawable(unsigned long cp, bool japanese);

#endif
