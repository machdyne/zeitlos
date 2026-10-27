/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * learn -- questions: a check block's form as a question, and the
 * judging of an answer (docs/learn_format.md, "check"). Deterministic:
 * the same answer to the same question is always judged the same way,
 * so learn is never confidently wrong about what it marked.
 */

#ifndef LQUIZ_H
#define LQUIZ_H

#include <stdint.h>
#include <stdbool.h>

#include "lsx.h"

enum { LQ_CHOICE = 0, LQ_NUMBER, LQ_FRACTION, LQ_TEXT, LQ_ORDER, LQ_MATCH, LQ_POINT, LQ_UNKNOWN };

enum {
	LQ_RIGHT = 0,
	LQ_WRONG,
	LQ_NEARLY,		// a text answer right but for its accents; a number in the wrong form
	LQ_UNREADABLE,	// not an answer at all ("abc" for a number): not counted
};

#define LQ_MAX_OPT 8
#define LQ_MAX_ACCEPT 8
#define LQ_MAX_WRONGIF 8
#define LQ_MAX_ASK 8

typedef struct {
	const char	*text;		// a string, or the formula of a (math ...)
	bool		math;
} lq_part_t;

typedef struct {
	const char	*text;
	bool		right;
	const char	*feedback;	// wrong answers
} lq_opt_t;

typedef struct {
	const lx_node_t	*form;
	uint8_t		kind;
	lq_part_t	ask[LQ_MAX_ASK];
	int			nask;
	const char	*hints[3];
	int			nhints;
	const char	*explain;
	const char	*id;			// (id "...") or NULL
	// choice, order, match
	lq_opt_t	opt[LQ_MAX_OPT];
	int			nopt;
	int			order[LQ_MAX_OPT];	// display order (shuffled)
	bool		several;
	// number
	double		answer, within;
	const char	*unit;
	// fraction
	long		fnum, fden;
	bool		equivalent, simplest;
	// text
	const char	*accept[LQ_MAX_ACCEPT];
	int			naccept;
	bool		case_matters, accents_nearly;
	// wrong-if: the answer as written, and what to say
	const char	*wrong_if[LQ_MAX_WRONGIF];
	const char	*wrong_fb[LQ_MAX_WRONGIF];
	int			nwrong_if;
} lq_q_t;

// Reads a check form. `seed` shuffles the choices (the same seed, the same order).
bool lq_load(lq_q_t *q, const lx_node_t *form, uint32_t seed);

typedef struct {
	int			verdict;		// LQ_RIGHT ...
	const char	*feedback;		// what to say (may be NULL: then a plain "Not quite.")
	char		said[64];		// for a typed answer: how it was read, or why not
} lq_result_t;

// A choice: `chosen` is a bit mask of the options in display order.
lq_result_t lq_judge_choice(const lq_q_t *q, uint32_t chosen);
// A typed answer (number, fraction, text).
lq_result_t lq_judge_typed(const lq_q_t *q, const char *answer);
// Order: perm[i] is the option shown at position i (its index in opt).
lq_result_t lq_judge_order(const lq_q_t *q, const int *perm, int n);

// The right answer, written out (for "the answer was ...").
void lq_answer_text(const lq_q_t *q, char *out, int cap);

// Text compared as learn compares typed words: trimmed, spaces folded,
// case folded unless case matters; *nearly set when only accents differ.
bool lq_text_equal(const char *typed, const char *want, bool case_matters, bool *nearly);

#endif
