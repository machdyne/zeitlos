#ifndef FIND_H
#define FIND_H

/*
 * Zeitlos -- sw/apps/web
 *
 * Find in page: the search, without the window. docs/web_app.md,
 * "Find in page".
 *
 * The page is never held in memory, so the search walks the document
 * block by block (page_scan(), page.h), a bounded number per pass of
 * the main loop, and feeds each block's text to find_feed(). This file
 * decides what a match is and where the walk goes next -- including
 * the wrap from the end back to the top, and when a wrapped search has
 * come round to where it began and must stop. No Zeitlos dependencies;
 * tests/test_find.c runs it on the build machine, against plain
 * strings and against real pages through page.c.
 *
 * Matching is case-insensitive ASCII. Block text is already folded to
 * ASCII (uni.h), so that is all there is to fold. A match does not
 * span two blocks: a block is a paragraph, a list item, a cell row.
 */

#include <stdint.h>
#include <stdbool.h>

#define FIND_MAX  64			// longest needle, including the NUL

typedef enum {
	FIND_IDLE = 0,
	FIND_RUN,				// walking; feed it blocks from f->block
	FIND_FOUND,				// found_block / found_off
	FIND_NONE,				// the whole document, no match
} find_result_t;

typedef struct {

	char		needle[FIND_MAX];
	uint32_t	nlen;

	uint32_t	start_block;	// where this search began
	uint32_t	start_off;
	uint32_t	block;			// the next block to look at,
	uint32_t	off;			// and where in it
	bool		wrapped;		// has gone past the end once

	find_result_t	result;
	uint32_t	found_block;
	uint32_t	found_off;

} find_t;

// First case-insensitive match of `needle` (length `nlen`) in
// `hay[from, len)`, or -1.
int32_t find_text(const char *hay, uint32_t len, uint32_t from,
	const char *needle, uint32_t nlen);

// Starts a search for `needle` at (block, off). False -- and nothing
// started -- for an empty needle. A longer one is cut to FIND_MAX - 1.
bool find_begin(find_t *f, const char *needle, uint32_t block, uint32_t off);

// The next match after the last one found, wrapping at the end.
// Requires a previous FIND_FOUND; false otherwise.
bool find_again(find_t *f);

// One block, in order from f->block. True when the walk is over: the
// result is FIND_FOUND or FIND_NONE.
bool find_feed(find_t *f, uint32_t block, const char *text, uint32_t len);

// The walk reached the end of the document. Wraps to the top and
// returns true, or -- when that would only repeat what has been
// searched -- sets FIND_NONE and returns false.
bool find_wrap(find_t *f);

#endif
