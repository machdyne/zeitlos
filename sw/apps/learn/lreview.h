/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * learn -- review: questions come back, spaced out (docs/learn.md,
 * "Review").
 *
 * Every question a learner answers is an item in five Leitner boxes. A
 * question answered right the first time, without a hint, goes up a box
 * and comes back later each time (1, 3, 7, 16, 35 days); one missed, or
 * answered after a hint, goes back to box 1, tomorrow. The learner's
 * review.txt keeps every answer, one line each, added to the end:
 *
 *   20387 r zeit-learn/start/questions#q1a2b3c4 1 01-start/02-questions.md
 *   20388 W ...
 *
 * (the day; r/w right first time or not, in a lesson; R/W in a review;
 * the question; its step; its file in the course). The boxes are worked
 * out from the lines when the learner is opened. With no clock set,
 * each time learn is opened counts as a day (a "D n" line).
 */

#ifndef LREVIEW_H
#define LREVIEW_H

#include <stdint.h>
#include <stdbool.h>

#include "lquiz.h"

#define LR_BOXES 5

typedef struct {
	uint32_t	id;			// offset of "lesson-id#key" in the pool
	uint32_t	file;		// offset of the lesson's file, in the course
	int32_t		due;		// the day it comes back
	uint8_t		box;		// 1..5
	uint8_t		step;
} lr_item_t;

// Reads the current learner's review.txt (ls_current()). `mem` holds
// the items and their names.
bool lr_open(void *mem, uint32_t size);
// today, in days (the clock's, or the count of times opened)
int32_t lr_today(void);

// A question's key: its (id "...") if it has one, else made from what it asks.
void lr_key(const lq_q_t *q, char *out, int cap);

// A question answered: right the first time without a hint or not; in
// a review or a lesson. Written at once; false if it could not be.
bool lr_answered(const char *lesson_id, const char *key, int step, const char *file,
	bool right_first, bool in_review);

int lr_count(void);
const lr_item_t *lr_get(int i);
const char *lr_name(uint32_t off);		// a pool string (an id or a file)
// the box of a question, 0 if it has none
int lr_box_of(const char *lesson_id, const char *key);

// The questions due today, the longest-waiting first and courses taken
// in turn (interleaved); only those of `course` (an id) if not NULL.
// Indices into the items; returns how many (at most max).
int lr_due(int *out, int max, const char *course);
int lr_count_due(const char *course);

// "zeit-learn/start/questions#q1a2b3c4" -> the course id
void lr_course_of(const char *id, char *out, int cap);

#endif
