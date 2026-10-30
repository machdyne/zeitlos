#ifndef PRINT_TEXTDOC_H
#define PRINT_TEXTDOC_H

/*
 * print -- plain text laid out on pages (docs/print.md, "Printing
 * text").
 *
 * An app that prints text need not draw pages itself: it writes the
 * text to a file and names it in the job, "text=/tmp/...", with no
 * pages. print reads it, lays it out here -- the paper, margins, a
 * Hershey face, word wrap, page numbers -- and prints the result like
 * any other job. So a small app (text is in flash) prints without
 * linking the page renderer: the renderer is print's.
 */

#include <stdint.h>
#include <stdbool.h>

// The job's options and the text's pages, as a new job in *out (pool
// memory): `job` (the options, no pages) followed by the pages.
// `default_paper` for a job that names none ("" for Letter). False
// with a reason in err.
//
// The job's own options for the text, all optional:
//   text-font   a Hershey face: sans (default), serif
//   text-size   points, 6 .. 24 (10)
bool textdoc_build(const uint8_t *job, uint32_t len, const char *text, uint32_t tlen,
	const char *default_paper, uint8_t **out, uint32_t *olen, char *err, int errn);

#endif
