/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * learn -- S-expressions read as data (docs/learn_format.md, "Blocks").
 *
 * The data blocks of a lesson (`check`, `cards`, `example`, ...) are
 * read by this reader and never evaluated, as slide reads a deck: a
 * question is a question, and learncheck can verify it without running
 * anything. Nodes live in an arena the caller gives; nothing is freed
 * one by one.
 */

#ifndef LSX_H
#define LSX_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

typedef struct {
	uint8_t		*mem;
	size_t		size, used;
	bool		oom;
} lx_arena_t;

void lx_arena_init(lx_arena_t *a, void *mem, size_t size);
void *lx_alloc(lx_arena_t *a, size_t n);		// zeroed, 8-aligned; NULL + oom when full
char *lx_strdup(lx_arena_t *a, const char *s, size_t n);
static inline void lx_arena_reset(lx_arena_t *a) { a->used = 0; a->oom = false; }

enum {
	LX_LIST = 0,
	LX_SYM,		// a bare word: right, wrong, ask, #t, #f
	LX_STR,		// "text", with \" \\ \n escapes
	LX_NUM,		// 12, -3, 0.25, 1e-3 (text kept as written, value in num)
};

typedef struct lx_node lx_node_t;
struct lx_node {
	uint8_t		kind;
	uint16_t	line;		// in the lesson file
	const char	*text;		// SYM, STR, NUM: the text (NUM as written)
	double		num;		// NUM
	lx_node_t	*head;		// LIST: first element
	lx_node_t	*next;		// the next element of the enclosing list
};

// Reads every top-level form of `s` into a list (kind LX_LIST, the forms
// as its elements). `line0` is the line number of the block's first line
// in its file. NULL on a syntax error, with "line N: why" in err.
lx_node_t *lx_read(lx_arena_t *a, const char *s, size_t len, int line0,
	char *err, int errcap);

// -- looking at forms --
int lx_len(const lx_node_t *list);
lx_node_t *lx_nth(const lx_node_t *list, int n);
bool lx_is_sym(const lx_node_t *n, const char *name);
bool lx_is_str(const lx_node_t *n);
// a list whose first element is the symbol `name`
bool lx_is_form(const lx_node_t *n, const char *name);
// the name of a form (its first symbol), or NULL
const char *lx_form_name(const lx_node_t *n);
// the first sub-form of `form` named `name`, or NULL. `form` is a form
// (name ...), searched after its name, or a block's list of forms
lx_node_t *lx_find(const lx_node_t *form, const char *name);
// the n-th sub-form named `name` (0 first)
lx_node_t *lx_find_nth(const lx_node_t *form, const char *name, int nth);
int lx_count(const lx_node_t *form, const char *name);
// the first argument of (name ARG ...) inside form, as text/number
const char *lx_str_of(const lx_node_t *form, const char *name);
bool lx_num_of(const lx_node_t *form, const char *name, double *out);
bool lx_flag(const lx_node_t *form, const char *name);	// (name) or (name #t)

#endif
