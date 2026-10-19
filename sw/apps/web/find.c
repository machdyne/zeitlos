/*
 * Zeitlos -- sw/apps/web
 *
 * See find.h.
 */

#include <string.h>

#include "find.h"

static char lower(char c) {
	return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
}

int32_t find_text(const char *hay, uint32_t len, uint32_t from,
	const char *needle, uint32_t nlen) {

	uint32_t i, j;
	char first;

	if (nlen == 0 || from > len || len - from < nlen) return -1;

	first = lower(needle[0]);

	for (i = from; i + nlen <= len; i++) {
		if (lower(hay[i]) != first) continue;
		for (j = 1; j < nlen; j++)
			if (lower(hay[i + j]) != lower(needle[j])) break;
		if (j == nlen) return (int32_t)i;
	}

	return -1;

}

bool find_begin(find_t *f, const char *needle, uint32_t block, uint32_t off) {

	uint32_t n = needle ? (uint32_t)strlen(needle) : 0;

	if (n == 0) return false;
	if (n > FIND_MAX - 1) n = FIND_MAX - 1;

	memmove(f->needle, needle, n);	// may be f->needle itself
	f->needle[n] = 0;
	f->nlen = n;

	f->start_block = f->block = block;
	f->start_off = f->off = off;
	f->wrapped = false;
	f->result = FIND_RUN;
	return true;

}

bool find_again(find_t *f) {

	if (f->result != FIND_FOUND || f->nlen == 0) return false;

	f->start_block = f->block = f->found_block;
	f->start_off = f->off = f->found_off + 1;
	f->wrapped = false;
	f->result = FIND_RUN;
	return true;

}

bool find_feed(find_t *f, uint32_t block, const char *text, uint32_t len) {

	uint32_t from = (block == f->block) ? f->off : 0;
	int32_t at;

	if (f->result != FIND_RUN) return true;

	// Round once and past the starting block: everything has been seen.
	if (f->wrapped && block > f->start_block) {
		f->result = FIND_NONE;
		return true;
	}

	at = find_text(text, len, from, f->needle, f->nlen);

	// Back at the starting block after a wrap, only what lies before
	// the start is new. The rest was searched on the way down.
	if (f->wrapped && block == f->start_block) {
		if (at >= 0 && (uint32_t)at >= f->start_off) at = -1;
		if (at < 0) { f->result = FIND_NONE; return true; }
	}

	if (at >= 0) {
		f->result = FIND_FOUND;
		f->found_block = block;
		f->found_off = (uint32_t)at;
		return true;
	}

	f->block = block + 1;
	f->off = 0;
	return false;

}

bool find_wrap(find_t *f) {

	// Started at the very top, or wrapped already: nothing is left.
	if (f->wrapped || (f->start_block == 0 && f->start_off == 0)) {
		f->result = FIND_NONE;
		return false;
	}

	f->wrapped = true;
	f->block = 0;
	f->off = 0;
	return true;

}
