#ifndef SLIDE_PRINTJOB_H
#define SLIDE_PRINTJOB_H

/*
 * slide -- a deck as a print job: each card drawn by the renderer the
 * editor uses, onto zpaper's recording target, as pages for the print
 * service (sw/common/zpaper.h, zprint.h, docs/print.md).
 * docs/slide_app.md, "Printing".
 */

#include <stdbool.h>
#include "../../common/zpaper.h"
#include "deck.h"

// Cards first .. first+count-1 (to the last with count < 0), as pages
// on w. False if a write failed.
bool printjob_pages(const deck_t *d, int first, int count, zp_writer_t *w);

// The print settings: a deck's (print "...") words and a /print line's,
// read into job options. Words that are settings (4up, cut, paper a4
// ...) are also written back to `keep` (the deck's, without the one-off
// words: pdf, ps, this, a card range). `cur` is the card shown, for
// "this". Returns false with a message for a word it does not know.
typedef struct {
	int		nup, copies;
	bool	cut, frame, actual;
	char	paper[16];
	char	quality[8];
	char	tray[16];		// IPP media-source: "main", "rear" ... ("" the printer's)
	char	type[24];		// IPP media-type: "labels", "photographic" ...
	int		dest;			// PJ_PRINTER, PJ_PDF, PJ_PS
	int		first, count;	// cards; count < 0 all
} pj_settings_t;

enum { PJ_PRINTER = 0, PJ_PDF = 1, PJ_PS = 2, PJ_NONE = 3 };	// NONE: "setup", only settings

bool printjob_parse(pj_settings_t *s, const char *deck_words, const char *line,
	int cur, int ncard, char *keep, int keep_n, char *err, int err_n);

// The settings as job options on w (before the first page).
void printjob_options(const pj_settings_t *s, zp_writer_t *w);

#endif
