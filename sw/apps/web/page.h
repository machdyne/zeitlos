#ifndef PAGE_H
#define PAGE_H

/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * The document model: a checkpoint index over a spooled page, and
 * the arithmetic for moving a viewport around inside it.
 *
 * -- why this is not in web.c --
 *
 * Because it is the part most likely to be subtly wrong, and it does
 * not have to touch the machine to work.
 *
 * Bytes come from a CALLBACK, not from a file. sw/apps/web hands it
 * one that reads the spool file on the card; tests/test_page.c hands
 * it one that reads a buffer in memory. So every position invariant
 * -- scroll down N lines and back up N and land in the same place,
 * the index agrees with a parse from the start, a re-index at a new
 * width does not move the viewport -- gets checked on the build
 * machine, thousands of times, with a debugger.
 *
 * sw/apps/read's own history is the argument for this: its scrolling
 * is the part that took the longest to get right, and the symptoms
 * ("scrolling is slightly wrong") were close to undebuggable on
 * hardware.
 *
 * -- the index --
 *
 * html.c is a streaming parser whose state is a flat POD, which means
 * a (byte offset, html_state_t) pair is a resume point: seek there,
 * restore, and replay to reproduce any part of the document. This
 * file keeps a sparse table of those, so any screen can be produced
 * without holding the document in memory. Same mechanism sw/apps/read
 * uses for Markdown, and the same reason -- a Wikipedia article is
 * 350KB and the kernel pool is 1MB.
 *
 * The index is built INCREMENTALLY, a bounded number of blocks per
 * call, so the window keeps repainting while a long page is still
 * being scanned.
 *
 * -- the scrollbar is over blocks, not lines --
 *
 * A scrollbar over display lines needs the document's total line
 * count, which depends on the window width, which means every resize
 * re-counts every block in the document. `read` can afford the line
 * view because a Markdown line is a source line; here a block is a
 * paragraph and its line count is a layout question.
 *
 * So the scrollbar is over BLOCKS and the viewport position is
 * (block, display line within block). Scrolling stays smooth --
 * that is what the sub-position is for -- and nothing has to be
 * recounted when the window changes size. The cost is that the thumb
 * is slightly non-linear on a page with paragraphs of wildly
 * different lengths, which is not something anyone can see.
 */

#include <stdint.h>
#include <stdbool.h>

#include "html.h"
#include "layout.h"

// Checkpoints held. The stride doubles when this fills, so a document
// of any size is covered -- at the cost of more replay per seek.
//
// 256 at a starting stride of 4 covers 1024 blocks before the first
// doubling, which is a long article. A 350KB page of dense markup
// runs to roughly 3000 blocks and settles at a stride of 16.
#define PAGE_CKPT_MAX     256
#define PAGE_STRIDE_MIN   4

// Blocks a single page_fetch() can return. Sized for the deepest
// screen at the smallest font plus slack: 480px of 9px lines is 53.
#define PAGE_FETCH_MAX    64

typedef struct {
	uint32_t		off;		// byte offset to resume at
	uint32_t		block;		// blocks emitted before this point
	html_state_t	st;
} page_ckpt_t;

// A viewport position: which block is at the top of the screen, and
// how many of its display lines are scrolled off above.
typedef struct {
	uint32_t	block;
	uint16_t	sub;
} page_pos_t;

// Returns bytes actually read, which may be short at end of data.
typedef uint32_t (*page_read_fn)(void *user, uint32_t off,
	char *buf, uint32_t len);

typedef struct {

	page_read_fn	read;
	void			*user;

	// Bytes available to read. Grows while a page is still arriving;
	// `complete` says whether more can appear.
	uint32_t		size;
	bool			complete;

	html_charset_t	charset;

	page_ckpt_t		ck[PAGE_CKPT_MAX];
	int				nck;
	uint32_t		stride;

	// How far the index has been built. Everything before this is
	// known; everything after has not been looked at.
	uint32_t		frontier_off;
	uint32_t		frontier_block;
	html_state_t	frontier_st;
	bool			indexed;		// frontier reached the end

	uint32_t		nblocks;

	char			title[HTML_TITLE_MAX];

} page_t;

// `read` is called with (user, offset, buf, len).
void page_init(page_t *p, page_read_fn read, void *user);

// Throws away the index and starts over -- a new document, or a
// reload. Does not clear the read callback.
void page_reset(page_t *p);

// Must be called before indexing, and only then: the charset comes
// from the transport (http.h) and cannot change once bytes have been
// decoded.
void page_set_charset(page_t *p, html_charset_t cs);

// Tells the model how many bytes are now readable, and whether the
// document is finished. Safe to call repeatedly as a page streams in.
void page_set_size(page_t *p, uint32_t size, bool complete);

// Extends the index by at most `max_blocks` blocks. Returns the
// number added. Call from the idle path until page_indexed() is true.
//
// Bounded on purpose: indexing a 350KB page in one go is hundreds of
// milliseconds during which the window does not repaint and wm starts
// reporting a missed redraw ack.
uint32_t page_index_more(page_t *p, uint32_t max_blocks);

static inline bool page_indexed(const page_t *p) { return p->indexed; }
static inline uint32_t page_blocks(const page_t *p) { return p->nblocks; }
static inline const char *page_title(const page_t *p) { return p->title; }

// Fills `out` with up to `count` consecutive blocks starting at
// `first`. Returns how many were written, which is fewer at the end
// of what has been indexed.
//
// One seek and one replay for the whole run, so a screen costs one
// call rather than one per block.
uint32_t page_fetch(page_t *p, uint32_t first, uint32_t count,
	html_line_t *out);

// Moves `pos` by `lines` display lines, positive for down. Returns
// the number of lines actually moved, which is smaller at either end
// of the document.
//
// Clamped so that the position never lands past the last block. It is
// NOT clamped to keep the last screen full -- a document shorter than
// the window scrolls to its end and stops, rather than refusing to
// scroll at all.
int32_t page_advance(page_t *p, const layout_cfg_t *cfg,
	page_pos_t *pos, int32_t lines);

// Total display lines from the start of the document to `pos`. Used
// only where an absolute answer is genuinely needed; it walks every
// block before `pos`, so it is not something to call per frame.
uint32_t page_lines_before(page_t *p, const layout_cfg_t *cfg,
	const page_pos_t *pos);

#endif
