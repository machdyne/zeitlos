#ifndef SMALLWEB_H
#define SMALLWEB_H

/*
 * Zeitlos -- sw/apps/web
 *
 * Gopher (RFC 1436) and Gemini: the requests, Gemini's response
 * header, and the conversion of what comes back into HTML for the
 * browser's own engine (html.c, layout.c). docs/gopher_gemini.md.
 *
 * -- why convert to HTML --
 *
 * Everything that is hard about showing a document -- wrapping, links
 * and selecting them, scrolling, the index that lets a long page be
 * read without holding it (page.c) -- already exists for HTML. A
 * Gemini page or a Gopher menu turned into a few plain tags goes
 * through all of it unchanged, and a link in one is followed by the
 * same code as a link in a web page. The alternative, a second
 * renderer, would be a second place for every one of those bugs.
 *
 * The conversion streams: bytes in, HTML out, a line at a time, so a
 * page of any size needs one line of memory. Like url.c and html.c
 * this file has no Zeitlos dependencies; tests/test_smallweb.c runs it
 * on the build machine.
 */

#include <stdint.h>
#include <stdbool.h>

#include "url.h"

// Longest line held for conversion. A longer one is converted in
// pieces -- harmless for text, which wraps anyway. Gemini's own limit
// for the response header is 1024 bytes of URL/meta.
#define SW_LINE_MAX  1100
#define SW_META_MAX  1025

typedef void (*sw_out_fn)(void *user, const char *data, uint32_t len);

// What the body is being turned into.
typedef enum {
	SW_BODY_GEMTEXT,		// text/gemini -> HTML
	SW_BODY_MENU,			// a Gopher menu -> HTML in one <pre>
	SW_BODY_TEXT,			// any other text -> escaped, in <pre>
	SW_BODY_RAW,			// already HTML: passed through
	SW_BODY_DISCARD,		// nothing to show (an error, an image, ...)
} sw_body_t;

typedef struct {

	bool		gemini;
	bool		header_done;	// Gemini: the status line has arrived
	int			status;			// Gemini: the two-digit status, or -1
	char		meta[SW_META_MAX];	// Gemini: what follows it
	char		gopher_type;	// Gopher: the item type requested

	sw_body_t	body;
	bool		started;		// the HTML preamble has been written
	bool		pre;			// gemtext: inside a ``` block
	bool		list;			// gemtext: inside a <ul>
	bool		para;			// gemtext: inside a <p> of text lines
	bool		menu_done;		// gopher: the "." line has been seen

	char		line[SW_LINE_MAX];
	uint32_t	llen;

	sw_out_fn	out;
	void		*user;

} sw_ctx_t;

// The request to send, into `out`. Returns its length, or 0 if it does
// not fit or `u` is neither gopher nor gemini.
//
//   gemini: the whole URL, without fragment, then CRLF
//   gopher: the selector (from the path, after its type character),
//           then a TAB and the search terms if there is a query, CRLF
uint32_t sw_request(const url_t *u, char *out, uint32_t cap);

// The Gopher item type a URL asks for: the first character of the
// path after '/', or '1' (a menu) for the root.
char sw_gopher_type(const url_t *u);

// Starts a response for `u`. `out` receives HTML.
void sw_begin(sw_ctx_t *c, const url_t *u, sw_out_fn out, void *user);

// Bytes from the server, in any size of piece.
void sw_feed(sw_ctx_t *c, const char *data, uint32_t len);

// The server closed the connection: flush the last line and close any
// open element. Neither protocol has a length, so this is the only
// end there is.
void sw_end(sw_ctx_t *c);

// A Gemini status class: 1 input, 2 success, 3 redirect, 4/5 failure,
// 6 client certificate. 0 before the header has arrived.
static inline int sw_status_class(const sw_ctx_t *c) {
	return (c->header_done && c->status >= 10) ? c->status / 10 : 0;
}

#endif
