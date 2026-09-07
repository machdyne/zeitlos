/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * See page.h.
 */

#include <string.h>

#include "page.h"

// Bytes read from the source per parser feed. 1024 rather than the
// transport's 512 because this is card I/O, not network: the read
// callback goes to an SPI SD interface where per-call overhead
// dominates, and html.c is indifferent to the chunk size (its own
// tests feed it at 1, 7, 512 and 4096 and require identical output).
#define PAGE_IO_CHUNK  1024

// -- indexing ------------------------------------------------------
//
// One html_ctx_t is used for every parse in this file, one at a time.
// It is nearly 6KB -- an html_line_t plus the tokenizer's buffers --
// which is more than a 16KB stack should carry, and there is never a
// reason for two to be live at once.
static html_ctx_t ctx;

// Where an index pass deposits what it finds.
static struct {
	page_t		*p;
	uint32_t	budget;			// blocks still allowed this pass
	uint32_t	added;
	uint32_t	last_mark;		// the mark most recently recorded
	uint32_t	last_mark_block;// blocks emitted before that mark
	bool		have_last;
	bool		stop;
} idx;

// Halves the table in place, doubling the stride, when it fills.
//
// Keeping every other entry rather than dropping the tail is what
// makes this safe on a document still being read: the entries that
// survive still span the whole indexed range, so a seek anywhere in
// it still lands on a real checkpoint. Dropping the tail would leave
// the end of a long page with no checkpoints at all, and seeking
// there would replay from the middle of the document every time.
static void ckpt_thin(page_t *p) {

	int w = 0;

	for (int i = 0; i < p->nck; i += 2) p->ck[w++] = p->ck[i];

	p->nck = w;
	p->stride *= 2;

}

static void ckpt_add(page_t *p, uint32_t off, uint32_t block,
	const html_state_t *st) {

	// Block numbers must strictly increase, and enforcing that here
	// is not belt-and-braces -- it is load bearing.
	//
	// A streaming index re-emits the blocks after its last mark on
	// every pass (see the frontier note in page_index_more), so the
	// same block number arrives at this function again and again. The
	// rollback trims checkpoints PAST the new count, which correctly
	// leaves the one AT it in place -- and then the next pass adds a
	// second, identical entry for it.
	//
	// Nothing about that is visibly wrong: ckpt_for() takes the last
	// match, the duplicates agree, and every block still comes back
	// correct. What it does instead is fill the table with copies, so
	// ckpt_thin() runs over and over and the stride climbs. Measured
	// on a real 350KB page: 131 entries for 341 blocks and a stride
	// of 128, meaning every fetch replayed up to 128 blocks instead
	// of 4. That is not a wrong answer, it is a slow one, and on
	// hardware it would have surfaced as "scrolling is sluggish on
	// long pages" with nothing obvious to point at.
	if (p->nck > 0 && p->ck[p->nck - 1].block >= block) return;

	if (p->nck >= PAGE_CKPT_MAX) ckpt_thin(p);

	p->ck[p->nck].off = off;
	p->ck[p->nck].block = block;
	p->ck[p->nck].st = *st;
	p->nck++;

}

// Emit callback used while building the index.
//
// The mark handling here is the contract html.h spells out, and it is
// the thing that is easy to get wrong: the mark visible during an
// emit resumes THAT block, and several consecutive blocks can share
// one mark. So a distinct mark is recorded the first time it is seen,
// paired with the count of blocks emitted BEFORE the block that first
// carried it.
static void idx_emit(void *user, const html_line_t *l) {

	page_t *p = idx.p;
	uint32_t mark;

	(void)user;
	(void)l;

	mark = html_mark_offset(&ctx);

	if (!idx.have_last || mark != idx.last_mark) {
		if (p->nblocks == 0 ||
			(p->nblocks % p->stride) == 0 ||
			p->nck == 0)
			ckpt_add(p, mark, p->nblocks, html_mark_state(&ctx));
		idx.last_mark = mark;
		idx.last_mark_block = p->nblocks;
		idx.have_last = true;
	}

	p->nblocks++;
	idx.added++;

	if (idx.added >= idx.budget) idx.stop = true;

}

void page_init(page_t *p, page_read_fn read, void *user) {
	memset(p, 0, sizeof(*p));
	p->read = read;
	p->user = user;
	p->stride = PAGE_STRIDE_MIN;
}

void page_reset(page_t *p) {

	page_read_fn read = p->read;
	void *user = p->user;

	memset(p, 0, sizeof(*p));
	p->read = read;
	p->user = user;
	p->stride = PAGE_STRIDE_MIN;

}

void page_set_charset(page_t *p, html_charset_t cs) { p->charset = cs; }

void page_set_size(page_t *p, uint32_t size, bool complete) {

	// Only ever grows. A source that reported a smaller size than
	// before would invalidate the index rather than extend it, and
	// nothing legitimately does that -- a reload calls page_reset().
	if (size > p->size) p->size = size;

	if (complete) p->complete = true;

}

uint32_t page_index_more(page_t *p, uint32_t max_blocks) {

	static char buf[PAGE_IO_CHUNK];
	uint32_t off;

	if (p->indexed || !p->read) return 0;
	// Nothing new since the last pass. If more can still arrive, wait
	// for it; if not, fall through so the loop below runs zero times
	// and the flush at the end closes the document.
	if (p->frontier_off >= p->size && !p->complete) return 0;

	idx.p = p;
	idx.budget = max_blocks ? max_blocks : 1;
	idx.added = 0;
	idx.stop = false;
	idx.have_last = false;

	// Resume the parser exactly where the last pass left it.
	//
	// The context is rebuilt from the frontier state rather than kept
	// live between calls, because between two calls the caller may
	// have run page_fetch(), which uses the same context. Sharing one
	// context and reconstructing on demand costs a replay of at most
	// one stride; keeping two would cost 6KB.
	html_init(&ctx, idx_emit, NULL);
	html_set_charset(&ctx, p->charset);
	html_restore(&ctx, &p->frontier_st, p->frontier_off);

	off = p->frontier_off;

	while (off < p->size && !idx.stop) {

		uint32_t want = p->size - off;
		uint32_t got;

		if (want > sizeof(buf)) want = sizeof(buf);

		got = p->read(p->user, off, buf, want);
		if (got == 0) break;

		html_feed(&ctx, buf, got);
		off += got;

	}

	// -- where the next pass resumes --
	//
	// The frontier moves to the parser's most recent MARK, not to the
	// byte offset just consumed. That distinction is the whole of the
	// correctness here and it is not obvious: bytes between the last
	// mark and `off` have been fed, but they belong to a block that
	// has not been emitted yet. Resuming from `off` would skip them
	// and lose a block.
	//
	// Resuming from the mark means the blocks emitted since that mark
	// will be emitted AGAIN next pass, so the count has to be wound
	// back to what it was when the mark was first seen -- and any
	// checkpoint recorded for one of those blocks has to go, or the
	// next pass records a second checkpoint for the same block number
	// and ckpt_for() starts returning whichever it happens to hit
	// first.
	{
		uint32_t mark = html_mark_offset(&ctx);

		if (idx.have_last && mark == idx.last_mark &&
			p->nblocks > idx.last_mark_block) {

			p->nblocks = idx.last_mark_block;

			while (p->nck > 0 && p->ck[p->nck - 1].block > p->nblocks)
				p->nck--;

		}

		if (mark > p->frontier_off) {
			p->frontier_off = mark;
			p->frontier_st = *html_mark_state(&ctx);
		}
		// If the mark did NOT advance, the frontier stays put and the
		// next pass re-reads the same bytes. That happens when a
		// single block spans everything available -- a <pre> holding a
		// whole file, say. It is O(n^2) over the life of such a
		// document and it is left as is: the loop above already feeds
		// to the end of what is available before giving up, so it
		// costs a re-scan per arriving chunk rather than a hang, and
		// pages with no marks for tens of kilobytes are rare enough
		// that fixing it would mean carrying a second parser context
		// (6KB) for all the pages that do not need it.
	}

	if (p->complete && off >= p->size && !p->indexed) {
		// End of a finished document: flush whatever block is still
		// open. A page truncated by a dropped connection still shows
		// what arrived.
		html_finish(&ctx);
		p->indexed = true;
		p->frontier_off = off;
	}

	if (html_title(&ctx)[0] && !p->title[0]) {
		strncpy(p->title, html_title(&ctx), sizeof(p->title) - 1);
		p->title[sizeof(p->title) - 1] = '\0';
	}

	return idx.added;

}

// -- fetching ------------------------------------------------------

static struct {
	html_line_t	*out;
	uint32_t	first;
	uint32_t	count;
	uint32_t	seen;			// blocks passed so far in this replay
	uint32_t	got;
} fet;

static void fetch_emit(void *user, const html_line_t *l) {

	(void)user;

	if (fet.seen >= fet.first && fet.got < fet.count)
		fet.out[fet.got++] = *l;

	fet.seen++;

}

// The checkpoint at or before `block`.
static const page_ckpt_t *ckpt_for(const page_t *p, uint32_t block) {

	const page_ckpt_t *best = NULL;

	for (int i = 0; i < p->nck; i++) {
		if (p->ck[i].block > block) break;
		best = &p->ck[i];
	}

	return best;

}

uint32_t page_fetch(page_t *p, uint32_t first, uint32_t count,
	html_line_t *out) {

	static char buf[PAGE_IO_CHUNK];
	const page_ckpt_t *c;
	uint32_t off;

	if (!p->read || count == 0) return 0;
	if (first >= p->nblocks) return 0;
	if (count > PAGE_FETCH_MAX) count = PAGE_FETCH_MAX;

	c = ckpt_for(p, first);
	if (!c) return 0;

	fet.out = out;
	fet.first = first - c->block;
	fet.count = count;
	fet.seen = 0;
	fet.got = 0;

	html_init(&ctx, fetch_emit, NULL);
	html_set_charset(&ctx, p->charset);
	html_restore(&ctx, &c->st, c->off);

	off = c->off;

	while (off < p->size && fet.got < fet.count) {

		uint32_t want = p->size - off;
		uint32_t got;

		if (want > sizeof(buf)) want = sizeof(buf);

		got = p->read(p->user, off, buf, want);
		if (got == 0) break;

		html_feed(&ctx, buf, got);
		off += got;

	}

	// Only flush at the true end of the document. Flushing early
	// would emit a partial block as though it were complete, and the
	// same block would be emitted again, differently, once the rest
	// arrived.
	if (fet.got < fet.count && off >= p->size && p->complete)
		html_finish(&ctx);

	return fet.got;

}

// -- positions -----------------------------------------------------

// Display lines in one block, at this width. One fetch each; callers
// that need many in a row should be walking, not calling this in a
// loop over the whole document.
static int block_lines(page_t *p, const layout_cfg_t *cfg, uint32_t n) {

	static html_line_t one;

	if (page_fetch(p, n, 1, &one) != 1) return 0;

	return layout_count(&one, cfg);

}

int32_t page_advance(page_t *p, const layout_cfg_t *cfg,
	page_pos_t *pos, int32_t lines) {

	int32_t moved = 0;

	if (p->nblocks == 0) return 0;

	while (lines > 0) {

		int n = block_lines(p, cfg, pos->block);

		if (n <= 0) break;

		if ((int32_t)pos->sub + 1 < n) {
			pos->sub++;
			moved++;
			lines--;
			continue;
		}

		if (pos->block + 1 >= p->nblocks) break;

		pos->block++;
		pos->sub = 0;
		moved++;
		lines--;

	}

	while (lines < 0) {

		if (pos->sub > 0) {
			pos->sub--;
			moved--;
			lines++;
			continue;
		}

		if (pos->block == 0) break;

		pos->block--;
		{
			int n = block_lines(p, cfg, pos->block);
			pos->sub = (uint16_t)(n > 0 ? n - 1 : 0);
		}
		moved--;
		lines++;

	}

	return moved;

}

uint32_t page_lines_before(page_t *p, const layout_cfg_t *cfg,
	const page_pos_t *pos) {

	uint32_t total = 0;
	uint32_t i;

	for (i = 0; i < pos->block && i < p->nblocks; i++)
		total += (uint32_t)block_lines(p, cfg, i);

	return total + pos->sub;

}
