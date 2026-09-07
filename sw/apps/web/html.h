#ifndef HTML_H
#define HTML_H

/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * An HTML reader: bytes in, described lines out. No drawing, no
 * files, no windows, no Zeitlos types -- so it compiles for a host
 * and runs against real pages (tests/test_html.c), which is the only
 * way to have any confidence in a parser whose input is written by
 * everyone.
 *
 * -- this is deliberately the same shape as sw/apps/read's md.h --
 *
 * Not by coincidence and not to be tidy. `read` already solved the
 * problem this app has: render a document too large to hold in
 * memory, scroll it backwards as well as forwards, and do it on a
 * machine with no MMU and a 1MB kernel pool. Its answer is a SPARSE
 * INDEX of checkpoints, each recording a byte offset plus the parser
 * state that went with it, so any screen can be produced by seeking
 * to the nearest checkpoint, restoring the state, and replaying
 * forward.
 *
 * That works only if the parser state is small and trivially
 * copyable. md_state_t is five bools. html_state_t below is an
 * element stack and a few counters -- larger, but still a flat POD
 * with no pointers, for exactly the same reason.
 *
 * html_line_t is field-for-field compatible in spirit with
 * md_line_t so that layout.c can wrap, style and draw either. It is
 * a separate type rather than a shared one because coupling this app
 * to another app's header would make both harder to change; the
 * cost is one conversion function if `read` ever wants this layout
 * engine.
 *
 * -- push, not pull --
 *
 * md.h takes one source line and returns one described line, because
 * Markdown is line oriented. HTML is not: a paragraph can be one
 * line or ten thousand, and a tag can straddle any byte boundary. So
 * this is a PUSH parser -- html_feed() takes whatever bytes arrived,
 * and calls the emit callback whenever a complete block has been
 * assembled.
 *
 * That difference is invisible to the caller's index, which is what
 * matters: html_mark() reports a (offset, state) pair that is always
 * a legal resume point, and replaying from one reproduces byte for
 * byte what a parse from the start would have produced. That
 * property is checked directly -- tests/test_html.c's `resume` mode
 * parses a document whole, then re-parses it from every mark, and
 * requires the output to match.
 *
 * -- what a "block" is here --
 *
 * One paragraph, one heading, one list item, one table row, one line
 * of a <pre>. Roughly: the unit that gets its own vertical space.
 * Inline markup inside a block becomes spans (below), not separate
 * blocks.
 *
 * -- what is NOT implemented, and why --
 *
 *   - **No DOM.** There is no tree, so nothing can be queried,
 *     restyled or mutated after the fact. An element stack is enough
 *     to lay out a document once, and a tree of a Wikipedia article
 *     is tens of thousands of nodes on a machine with a 1MB pool.
 *
 *   - **No CSS.** Not parsed, not applied, not fetched. A page whose
 *     layout depends entirely on CSS renders as its source order,
 *     which for a document-shaped page is right and for an
 *     application-shaped page is a mess. `<style>` contents are
 *     dropped as non-text.
 *
 *   - **No JavaScript, ever.** See docs/web_app.md. `<script>`
 *     contents are dropped as non-text, exactly like `<style>`.
 *
 *   - **No HTML5 tree-construction error recovery.** The real
 *     specification's "adoption agency algorithm" and its table
 *     foster-parenting exist to make broken markup produce the same
 *     tree in every browser. With no tree there is nothing to agree
 *     about: a stray `</b>` closes a `<b>` if one is open and is
 *     ignored otherwise, and misnested tags produce slightly wrong
 *     emphasis rather than a different document.
 *
 *   - **No emphasis as a visual style.** One weight per font
 *     (sw/common/zfont_data.c), and underline already means "link".
 *     `<b>`/`<i>`/`<strong>`/`<em>` are structural no-ops: their text
 *     is kept, their distinction is not. Same decision md.h made, for
 *     the same reason.
 *
 *   - **Forms are text.** `<input>` renders as a placeholder box and
 *     nothing submits. GET forms arrive in Phase 4.
 */

#include <stdint.h>
#include <stdbool.h>

// Longest assembled block. A block longer than this is TRUNCATED and
// flagged, not wrapped into the next one -- splitting it would
// desynchronise nothing (there is no line numbering here) but would
// silently double a paragraph's apparent length, and a reader cannot
// tell that from the real thing.
//
// 2048 rather than md.h's 1024: Markdown in this tree is hand
// written and hard wrapped, while a Wikipedia lead paragraph with
// its citation markers stripped routinely runs past 1200 characters.
#define HTML_LINE_MAX   2048

// Styled runs recorded per block. Excess renders as plain text --
// degraded, never wrong. Higher than md.h's 24 because a paragraph of
// an encyclopedia article is mostly links.
#define HTML_MAX_SPANS  48

// Link targets recorded per block, and the longest one kept. A block
// with more links than this keeps the text and drops the targets of
// the excess, which draws them as ordinary words.
#define HTML_MAX_LINKS  16
#define HTML_LINK_MAX   256

// Deepest element nesting tracked. Beyond this, further open tags are
// counted but not pushed, so the stack stays consistent and a
// pathological page degrades instead of corrupting.
//
// 48 sounds generous and is not: a Wikipedia table cell sits around
// 25 elements deep before any of the article's own markup starts.
#define HTML_STACK_MAX  48

// Longest <title> kept, for the window title bar.
#define HTML_TITLE_MAX  128

typedef enum {
	// A paragraph break: vertical space, no text. Distinct from "no
	// output at all", which this parser simply does not emit.
	HTML_BLANK = 0,
	HTML_PARA,
	HTML_HEADING,	// level in html_line_t.level, 1-6
	HTML_PRE,		// preformatted: never wrapped, never styled
	HTML_QUOTE,
	HTML_LIST,		// bullet or ordered; marker already resolved
	HTML_RULE,
	HTML_TABLE,		// one row, cells already joined -- see html.c

	// An image, drawn as a box rather than fetched. `text` is the
	// caption -- the alt text, or the filename from the src when
	// there is no alt -- and img_w/img_h are the declared size if the
	// markup gave one.
	//
	// A box rather than inline "[alt]" text because a page of
	// diagrams reads as a page of diagrams, and because the reader
	// can see there IS an image rather than a stray bracket. What it
	// deliberately does not do is fetch anything: decoding needs
	// inflate and a PNG reader, which is Phase 5.
	HTML_IMAGE,
} html_kind_t;

typedef enum {
	HTML_SPAN_CODE = 1,		// <code>/<kbd>/<samp>/<tt> -- drawn inverse
	HTML_SPAN_LINK,			// <a href> -- drawn underlined
} html_span_kind_t;

typedef struct {
	uint16_t	start;		// offset into html_line_t.text
	uint16_t	len;
	uint8_t		kind;		// html_span_kind_t
	uint8_t		link;		// index into html_line_t.links, for _LINK
} html_span_t;

typedef struct {

	html_kind_t	kind;

	// HTML_HEADING: 1-6. HTML_LIST: nesting depth, 0 for top level.
	// HTML_QUOTE: blockquote depth, 1 or more.
	uint8_t		level;

	// HTML_LIST: the marker to draw, already resolved -- a bullet for
	// unordered, the literal number text for ordered. The renderer
	// draws this in the hanging indent rather than re-deriving it,
	// because only the parser knows the ordered-list counter.
	char		marker[8];

	// The block's text, entity-decoded, folded to ASCII (uni.h),
	// whitespace collapsed, NUL terminated. This is what gets wrapped
	// and drawn.
	char		text[HTML_LINE_MAX];
	uint16_t	len;

	// HTML_IMAGE: the declared size in CSS pixels, or 0 when the
	// markup did not say.
	//
	// Most of the modern web sizes images in CSS rather than in
	// attributes, so 0 is the common case and the renderer picks a
	// default. When they ARE given they are worth honouring: a page
	// of thumbnails and a page of banners then look different, which
	// is most of what a placeholder is for.
	uint16_t	img_w;
	uint16_t	img_h;

	// Set when the block hit HTML_LINE_MAX and lost its tail. The
	// renderer marks it, so a reader knows the sentence really does
	// stop there.
	bool		truncated;

	// This block continues the previous one rather than starting a new
	// thought -- it exists because of a <br>, not because of a block
	// element. The layout engine omits the inter-block gap for it.
	//
	// Without this every <br> in an address block or a poem would open
	// a full paragraph gap, and <br> is how most of the web still does
	// a line break.
	bool		tight;

	html_span_t	spans[HTML_MAX_SPANS];
	uint8_t		nspans;

	char		links[HTML_MAX_LINKS][HTML_LINK_MAX];
	uint8_t		nlinks;

} html_line_t;

// Parser state carried between blocks.
//
// Flat, no pointers, trivially copyable -- see this file's header on
// why that is the whole point. sw/apps/web stores one of these
// alongside every index checkpoint.
//
// The TOKENIZER's own state is deliberately absent. A mark is only
// ever taken at a point where the tokenizer is between tokens (see
// html_mark()), so there is nothing to save; recording it would make
// this struct bigger and invite marks to be taken where resuming
// would not actually work.
typedef struct {

	uint8_t		depth;						// elements on the stack
	uint8_t		stack[HTML_STACK_MAX];		// tag ids, outermost first

	// How many opens were dropped because the stack was full. A close
	// consumes one of these before touching the stack, which is what
	// keeps the stack aligned with reality on a pathological page.
	uint16_t	overflow;

	uint8_t		list_depth;					// open <ul>/<ol> count
	uint8_t		list_ordered;				// bitmask, one bit per depth
	uint16_t	list_index[8];				// <ol> counters, per depth

	uint8_t		quote_depth;				// open <blockquote> count
	uint8_t		drop_depth;					// inside <script>/<style>/...
	uint8_t		pre_depth;					// inside <pre>
	uint8_t		flags;						// HTML_ST_* below

} html_state_t;

#define HTML_ST_IN_TITLE   0x01
#define HTML_ST_IN_TABLE   0x02
#define HTML_ST_SEEN_BODY  0x04

typedef enum {
	// The default and the overwhelmingly common case.
	HTML_CS_UTF8 = 0,
	// Windows-1252, which is what "iso-8859-1" means in practice --
	// every browser has treated the two as the same thing since the
	// 1990s, and a page that declares Latin-1 and then uses 0x92 for
	// a curly apostrophe is extremely common. Treating them
	// separately would render those as nothing at all.
	HTML_CS_CP1252,
} html_charset_t;

typedef void (*html_emit_fn)(void *user, const html_line_t *line);

typedef struct {

	html_state_t	st;

	html_emit_fn	emit;
	void			*user;

	html_charset_t	charset;

	// -- tokenizer --
	uint8_t			ts;						// internal state enum
	uint8_t			tflags;
	uint16_t		tag_len;
	char			tag[32];				// current tag name
	uint16_t		attr_len;
	char			attr[24];				// current attribute name
	uint16_t		val_len;
	char			val[HTML_LINK_MAX];		// current attribute value
	char			quote;					// which quote closes val
	uint16_t		ent_len;
	char			ent[16];				// pending &entity;
	uint8_t			raw_match;				// progress matching </script
	uint8_t			raw_tag;				// which tag TS_RAW is inside

	// Attribute values collected while a tag is being read, applied
	// when the '>' arrives. Separate fields rather than a generic
	// key/value list because exactly six attributes are acted on and a
	// list would be a hash table for six entries.
	char			pend_href[HTML_LINK_MAX];
	// This block is an image, so flush_block() must not overwrite its
	// kind from the open element stack -- an <img> inside a <p> is
	// still an image.
	bool			img_pending;

	char			pend_src[HTML_LINK_MAX];
	uint16_t		pend_w, pend_h;
	char			pend_alt[128];
	bool			pend_alt_set;
	char			pend_charset[32];
	char			pend_content[64];
	char			pend_type[16];
	char			pend_value[32];

	// The href of the anchor currently open, kept separately from the
	// line's own link table so that an anchor straddling a block
	// boundary can reopen in the next block.
	char			cur_href[HTML_LINK_MAX];
	uint8_t			anchor_depth;

	// Set once any text has been decoded: a <meta charset> after that
	// point cannot retroactively change bytes already folded.
	bool			charset_locked;

	// Pending `tight` for the NEXT block -- set by <br>, consumed by
	// the following flush.
	bool			line_tight;

	// UTF-8 continuation carried across a feed() boundary.
	char			pend[4];
	uint8_t			pend_len;

	// -- block under construction --
	html_line_t		line;
	bool			have_text;				// any non-space in `line` yet
	bool			pending_space;			// collapsed run seen, not yet
											// emitted (see html.c)
	uint8_t			cur_kind;
	uint8_t			cur_level;

	// The link currently open, if any: index into line.links.
	int8_t			cur_link;
	uint16_t		span_start;
	uint8_t			span_kind;

	// -- position --
	uint32_t		off;					// bytes fed so far
	uint32_t		mark_off;
	html_state_t	mark_st;

	char			title[HTML_TITLE_MAX];
	uint16_t		title_len;

} html_ctx_t;

// `emit` is called once per assembled block, with a pointer valid
// only for the duration of the call -- the same borrowed-data
// convention message payloads use (docs/messaging.md). Copy what you
// need.
void html_init(html_ctx_t *ctx, html_emit_fn emit, void *user);

// Must be called before any html_feed(). Comes from the HTTP
// Content-Type header (http.h), which wins over any `<meta charset>`
// in the document -- the transport is the authority per the HTML
// specification, and it is also the only one available before the
// first byte is parsed.
void html_set_charset(html_ctx_t *ctx, html_charset_t cs);

// Resume at a previously reported mark. `st` is what html_mark_state()
// gave; the caller is responsible for having seeked its byte source to
// the matching html_mark_offset(). Resets the tokenizer and the block
// under construction, which is safe precisely because a mark is only
// ever taken where both were already clean.
void html_restore(html_ctx_t *ctx, const html_state_t *st, uint32_t off);

void html_feed(html_ctx_t *ctx, const char *data, uint32_t len);

// End of document: flushes whatever block is still open. A page that
// ends mid-paragraph -- a truncated download, a server that hung up --
// still shows what arrived.
void html_finish(html_ctx_t *ctx);

// The most recent legal resume point: a byte offset, and the state to
// restore before feeding from it.
//
// THE MARK VISIBLE DURING AN EMIT CALLBACK IS THE RESUME POINT FOR
// THAT SAME BLOCK, not for the block after it -- and SEVERAL
// CONSECUTIVE BLOCKS CAN SHARE ONE MARK, in which case it resumes at
// the first of them.
//
// Both halves of that matter. Blocks are not always ended by a tag: a
// newline inside <pre> ends one, and so does <br>, and neither
// produces a new mark. So an index cannot assume one mark per block.
// What it does instead is record each DISTINCT mark the first time it
// is seen, together with the number of blocks emitted before the
// block that first carried it:
//
//     if (html_mark_offset(ctx) != last_recorded) {
//         record(html_mark_offset(ctx), html_mark_state(ctx), nblocks);
//         last_recorded = html_mark_offset(ctx);
//     }
//     nblocks++;
//
// Getting this wrong costs one duplicated or one missing block at
// every checkpoint, which on screen looks like a scrollbar that does
// not quite line up rather than like a parser bug.
// tests/test_html.c checks it directly.
//
// A mark is only taken where nothing about the block under
// construction is carried in the context rather than in
// html_state_t -- see html.c. On any real page they are still
// frequent, because every close of a block element produces one.
// Both are zero until the first one occurs, which is itself a legal
// resume point (the start of the document).
uint32_t html_mark_offset(const html_ctx_t *ctx);
const html_state_t *html_mark_state(const html_ctx_t *ctx);

// The document's <title>, or "" if it had none. Valid once the head
// has been parsed, which for every real page is within the first few
// hundred bytes.
const char *html_title(const html_ctx_t *ctx);

#endif
