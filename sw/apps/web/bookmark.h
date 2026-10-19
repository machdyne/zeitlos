#ifndef BOOKMARK_H
#define BOOKMARK_H

/*
 * Zeitlos -- sw/apps/web
 *
 * Bookmarks: the file, and the page made from it. docs/web_app.md,
 * "Bookmarks".
 *
 * The file (Z_PATH_WEB_BOOKMARKS) is plain text, one bookmark a line:
 *
 *   URL<TAB>title
 *
 * so it can be read, sorted and edited with anything. A line with no
 * tab is a URL whose title is itself; blank lines and lines starting
 * with '#' are skipped. A trailing CR is ignored.
 *
 * The list is shown as a generated HTML page at the address
 * "about:bookmarks", through the browser's own renderer -- so Tab,
 * Enter, clicking and scrolling all work on it with no code of their
 * own.
 *
 * Everything here works on the file's text in a buffer: reading and
 * writing the card is web.c's, and happens only on Ctrl+D. No Zeitlos
 * dependencies; tests/test_bookmark.c runs it on the build machine.
 */

#include <stdint.h>
#include <stdbool.h>

#define BM_PAGE_URL   "about:bookmarks"

// A formatted line: a URL, a tab, a title, a newline.
#define BM_TITLE_MAX  128
#define BM_LINE_MAX   (1024 + 1 + BM_TITLE_MAX + 2)

typedef void (*bm_out_fn)(void *user, const char *data, uint32_t len);

// The line holding `url` exactly, as [*start, *end) -- *end is past
// its newline. False if there is none.
bool bm_find(const char *file, uint32_t len, const char *url,
	uint32_t *start, uint32_t *end);

// "url<TAB>title\n" into `out`. A tab, CR or newline in the title
// becomes a space, a title longer than BM_TITLE_MAX - 1 is cut, and an
// empty one is the URL. Returns the length, or 0 if the URL is empty,
// contains a tab or a line break, or the line does not fit.
uint32_t bm_format(char *out, uint32_t cap, const char *url,
	const char *title);

// Removes every line holding `url`, in place, and NUL-terminates what
// is left -- `file` must have room for a NUL at `len`, as a buffer from
// fs_mallocfile() does. Returns the new length.
uint32_t bm_remove(char *file, uint32_t len, const char *url);

// The bookmarks page: HTML, in pieces, to `out`. `path` is the file's
// name, said on the page so a reader knows where the list lives.
void bm_html(const char *file, uint32_t len, const char *path,
	bm_out_fn out, void *user);

#endif
