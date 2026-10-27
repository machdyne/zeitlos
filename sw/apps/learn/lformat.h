/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * learn -- courses and lessons as files (docs/learn_format.md).
 *
 * A course is a folder: course.cfg, then parts and units as numbered
 * folders, lessons as numbered Markdown files. A lesson is Markdown for
 * words, with fenced blocks for everything else: data blocks (read as
 * S-expressions, never evaluated) and figure blocks (Scheme). This file
 * reads both into a model the app draws and learncheck checks; the same
 * code runs on the build machine and on Zeitlos, through lf_fs_t.
 */

#ifndef LFORMAT_H
#define LFORMAT_H

#include <stdint.h>
#include <stdbool.h>

#include "lsx.h"

#define LF_FORMAT_VERSION	1
#define LF_MAX_LESSON		(48 * 1024)	// bytes of one lesson file
#define LF_PATH				160
#define LF_NAME				48

// -- the file system, as the format needs it --

typedef struct {
	// calls fn for each entry of dir (not . or ..); false if dir cannot be read
	bool	(*list)(void *ctx, const char *dir,
				void (*fn)(void *arg, const char *name, bool is_dir), void *arg);
	// reads at most cap bytes; returns the length, -1 if it cannot be read
	long	(*read)(void *ctx, const char *path, char *buf, long cap);
	void	*ctx;
} lf_fs_t;

// -- a lesson --

enum {
	LB_PARA = 0,	// a paragraph (inline Markdown in text)
	LB_HEAD,		// ## and ### inside a lesson (level 2, 3)
	LB_ITEM,		// a list item: level = depth (1..), num = its number or 0 for a bullet
	LB_QUOTE,		// > text
	LB_TABLE,		// a table: text = the rows as written, one per line
	LB_IMAGE,		// ![alt](src): text = alt, src
	LB_MATH,		// $$ formula $$ on a line of its own
	LB_CODE,		// a fenced block that is not one of learn's: shown as written
	LB_DATA,		// a data block: name, body, forms
	LB_FIGURE,		// a figure block: body is Scheme
};

typedef struct {
	uint8_t		kind;
	uint8_t		level;
	uint16_t	num;
	uint16_t	line;		// first line in the file
	const char	*text;		// inline Markdown, or the code / body as written
	const char	*name;		// fence name (data, figure, code's info string)
	const char	*src;		// image
	lx_node_t	*forms;		// LB_DATA: the forms, parsed
} lf_block_t;

#define LF_MAX_BLOCKS 160
#define LF_MAX_STEPS 32

typedef struct {
	int			first, count;	// blocks
} lf_step_t;

typedef struct {
	const char	*title;			// the # heading
	uint16_t	title_line;
	lx_node_t	*meta;			// the `lesson` block's forms, or NULL
	lf_block_t	blocks[LF_MAX_BLOCKS];
	int			nblocks;
	lf_step_t	steps[LF_MAX_STEPS];
	int			nsteps;
} lf_lesson_t;

// Data blocks learn knows; any other fence is code, shown as written.
bool lf_is_data_block(const char *name);

// Parses one lesson file. False, with "line N: why" in err, if the
// file cannot be a lesson (a data block that does not read, a fence not
// closed, too many blocks); the finer rules are learncheck's.
bool lf_lesson_parse(lx_arena_t *a, const char *text, long len, lf_lesson_t *out,
	char *err, int errcap);

// -- inline Markdown --

enum { LS_TEXT = 0, LS_BOLD, LS_ITALIC, LS_CODE, LS_MATH, LS_LINK };

typedef struct {
	uint8_t		kind;
	uint8_t		bold, italic;	// in force for TEXT (nesting ** inside *)
	const char	*s;				// not NUL-terminated
	int			len;
	const char	*target;		// LINK: where to, NUL-terminated (in the arena)
} lf_span_t;

// Splits inline Markdown into spans; returns how many (at most max).
// Unclosed markers are text.
int lf_inline(lx_arena_t *a, const char *s, int len, lf_span_t *out, int max);

// -- a course --

typedef struct {
	char		dir[LF_NAME];	// folder name, as on disk (05-comparing)
	char		id[LF_NAME];	// without its number (comparing): what progress records
	char		title[80];		// the first # of its index.md, or from the name
	int			part;			// index into parts, -1 if the course has none
	int			first_lesson, nlessons;
} lf_unit_t;

typedef struct {
	char		dir[LF_NAME];
	char		id[LF_NAME];
	char		title[80];
	int			first_unit, nunits;
} lf_part_t;

typedef struct {
	char		file[LF_NAME];	// 02-same-denominator.md
	char		id[LF_NAME];	// same-denominator
	char		title[80];		// filled when the lesson is first read; else from the name
	int			unit;
} lf_lref_t;

#define LF_MAX_PARTS 24
#define LF_MAX_UNITS 160
#define LF_MAX_LESSONS 1200
#define LF_MAX_LIST 8			// repeated keys: author, reviewer, needs, source

// what course.cfg says: all the catalogue needs
typedef struct {
	char		path[LF_PATH];	// the course's folder
	char		id[LF_NAME];
	char		title[80];
	char		level[16];		// K, 1..12, ms, hs, col, life, zeit, lang
	char		area[16];		// school, college, life, lang, zeit
	char		subject[32];
	char		classes[8];		// A B C D, as letters
	char		ages[16];
	char		lang[8];
	char		license[24];
	char		summary[160];
	char		standards[64];
	int			version;
	int			format;			// LF_FORMAT_VERSION it was written for
	char		authors[LF_MAX_LIST][80];
	int			nauthors;
	char		reviewers[LF_MAX_LIST][120];
	int			nreviewers;
	char		needs[LF_MAX_LIST][LF_NAME];
	int			nneeds;
	bool		parent_opt_in;
	// problems found while loading (unknown keys, an empty unit...):
	// learncheck reports them, the app ignores them
	char		warn[4][96];
	int			nwarn;
} lf_info_t;

typedef struct {
	lf_info_t	m;				// course.cfg
	// the tree
	lf_part_t	parts[LF_MAX_PARTS];
	int			nparts;
	lf_unit_t	units[LF_MAX_UNITS];
	int			nunits;
	lf_lref_t	lessons[LF_MAX_LESSONS];
	int			nlessons;
} lf_course_t;

// Reads only course.cfg (for the catalogue). False with a reason if
// there is none, or it has no id.
bool lf_info_load(const lf_fs_t *fs, const char *path, lf_info_t *m, char *err, int errcap);

// Loads course.cfg and the folder tree under `path`. Does not read
// lessons. False with a reason in err if there is no usable course.cfg.
bool lf_course_load(const lf_fs_t *fs, const char *path, lf_course_t *c,
	char *err, int errcap);

// "math-3/fractions/comparing/same-denominator": the lesson's stable id,
// the course id and the ids of its part (if any), unit and lesson.
void lf_lesson_id(const lf_course_t *c, int lesson, char *out, int cap);
// The file path of a lesson.
void lf_lesson_path(const lf_course_t *c, int lesson, char *out, int cap);

// strips a "05-" prefix: "05-comparing" -> "comparing"; ".md" too
void lf_strip_num(const char *name, char *out, int cap);
// "05-comparing" -> "Comparing"
void lf_title_of(const char *name, char *out, int cap);

#endif
