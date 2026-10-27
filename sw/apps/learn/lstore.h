/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * learn -- learners and their records (docs/learn.md, "Learners").
 *
 * A learner is a name and a folder, on the machine (/data/learn/learners/
 * NAME) or on a USB stick (/usb/learn/NAME): no account, no password.
 * The folder holds plain text a person can read without learn:
 *
 *   learner.cfg    name = ..., and the learner's settings
 *   progress.txt   one line per thing done, added to the end:
 *                  2026-10-26T09:12 done math-3/fractions/comparing/same-denominator 4/5 840
 *                  2026-10-26T09:30 at math-3/fractions/comparing/on-a-number-line 2
 *   reports.txt    mistakes the learner reported, one per line
 */

#ifndef LSTORE_H
#define LSTORE_H

#include <stdint.h>
#include <stdbool.h>

#include "lplat.h"

#define LS_NAME 32
#define LS_MAX_LEARNERS 24

enum { LS_ON_MACHINE = 0, LS_ON_STICK };

typedef struct {
	char		name[LS_NAME];
	char		dir[96];
	uint8_t		where;
} ls_who_t;

// The learners there are: on the machine, then on a stick if one is in.
int ls_list(ls_who_t *out, int max);
// A new learner on the machine (or the stick). False if the name is taken or unusable.
bool ls_create(const char *name, int where, ls_who_t *out);
// A name learn can use as a folder: letters, digits, spaces, - and _ (1..31 bytes)
bool ls_name_ok(const char *name);

// -- the records of the learner in use --

typedef struct {
	uint8_t		done;		// finished at least once
	uint8_t		right, total;	// the best finish
	uint8_t		step;		// where they left off (an `at` line), 0 none
	uint32_t	when;		// the last time (seconds), 0 unknown
} ls_rec_t;

// Opens a learner: reads progress.txt into memory. `mem` holds the
// records (a few tens of bytes a lesson).
bool ls_open(const ls_who_t *who, void *mem, uint32_t memsize);
const ls_who_t *ls_current(void);
// the record of a lesson (by id), or NULL if nothing is recorded
const ls_rec_t *ls_get(const char *lesson_id);
// the lesson last worked on in a course (by course id), "" if none
const char *ls_last_in(const char *course_id);
// the most recent lesson worked on in any course, "" if none
const char *ls_last(void);
// the courses the learner has worked in, the most recent first
int ls_ncourses(void);
const char *ls_course(int i);
// how many lessons whose id starts with `prefix` (a course, a unit) are done
int ls_count_done(const char *prefix);

// A lesson finished: right of total checks answered right the first
// time, in `seconds`. Written to progress.txt at once.
bool ls_done(const char *lesson_id, int right, int total, uint32_t seconds);
// Left a lesson at a step (0-based): written so the learner can go on.
bool ls_at(const char *lesson_id, int step);
// A mistake reported in a lesson, in the learner's words.
bool ls_report(const char *course_id, int version, const char *lesson_id, int step, const char *words);

// "2026-10-26T09:12", or "-" when the clock is not set
void ls_stamp(uint32_t t, char *out, int cap);

#endif
