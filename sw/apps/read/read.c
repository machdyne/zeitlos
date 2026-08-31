/*
 * read -- a document viewer
 *
 * Renders GitHub-flavoured Markdown, from files of any size, with
 * working links.
 *
 *   > run wm
 *   > run read
 *
 * -- files of any size --
 *
 * The document is never held in memory. That is the constraint the
 * whole design follows from: the Timeless Computing book is 85KB in
 * its assembled form, and "any size" means the next one may be worse.
 *
 * Instead there is a SPARSE INDEX built by streaming the file once on
 * open: every IDX_STRIDE source lines, a checkpoint records the byte
 * offset, the line number, and -- crucially -- the parser state
 * (md_state_t, see md.h). To draw a screen starting at source line L,
 * the reader seeks to the nearest checkpoint at or before L, restores
 * that parser state, replays forward to L, and renders from there.
 *
 * The parser state is what makes this work at all. Without it a
 * checkpoint in the middle of a fenced code block would resume
 * thinking it was in prose, and every '#' in the code would become a
 * heading. That is why md.h keeps its state small and copyable.
 *
 * Cost per frame is bounded by the stride plus a screenful, so it does
 * not grow with the document. Memory is the index (IDX_MAX entries)
 * plus one line buffer, and nothing else scales with file size.
 *
 * -- position --
 *
 * A position is (source line, display line within it). Scrolling by
 * whole source lines would jump a whole wrapped paragraph at a time,
 * which is unusable; the sub-line offset makes scrolling smooth while
 * keeping the index in source-line units, which is the only unit that
 * survives a change of window width.
 *
 * -- what it does not do --
 *
 * No editing, no search, no reflow of tables into real columns (see
 * md.c on why), and no bold or italic face -- there is one weight per
 * font. Emphasis markers are stripped so the text reads correctly.
 * Inline code renders inverse and links render underlined, which are
 * the two inline styles a 1bpp display can actually carry.
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#include "../../common/zeitlos.h"
#include "../../common/zsoc.h"	// Z_TICK_HZ, for the scroll settle time
#include "../../common/zwm.h"
#include "../../common/zwin.h"
#include "../../common/zgfx.h"
#include "../../common/zfont.h"
#include "../../common/zkbd.h"
#include "../../common/zwidget.h"
#include "../../common/zflist.h"
#include "../../common/zdialog.h"
#include "../../common/zfsapp.h"
#include "../../common/ztype.h"
#include "md.h"

#define WIN_W   320
#define WIN_H   300

static z_win_t win;

#define MARGIN      3

// Body font and heading font. Both are resident in glyph memory (see
// docs/window_manager.md, "Fonts in glyph memory"), so both draw
// through the hardware blitter.
#define BODY_FONT   (&z_font_5x8)
#define HEAD_FONT   (&z_font_6x12)

// -- the file --

static int fh = -1;
static uint32_t fsize;
static char path[Z_FLIST_PATH_MAX];

// -- sparse index --
//
// IDX_MAX is fixed and the STRIDE grows to fit. A document longer
// than IDX_MAX * stride lines doubles the stride and keeps every
// other entry, which costs a little more replay per frame and no
// memory at all. That is the right trade for a reader that must not
// have a maximum file size.
#define IDX_MAX     384

typedef struct {
	uint32_t	off;		// byte offset of the line
	uint32_t	line;		// source line number
	md_state_t	st;			// parser state entering that line
} idx_t;

static idx_t idx[IDX_MAX];
static int idx_n;
static uint32_t idx_stride;

// -- the index frontier --
//
// The index is built LAZILY, not in one pass at open.
//
// It used to be eager, and on the 85KB assembled book that meant
// reading the whole file over bit-banged SPI before showing anything
// -- several seconds of nothing, every time, to produce checkpoints
// for parts of the document the reader might never visit.
//
// Now the index covers only as far as has actually been needed, and
// extends when something asks to go further. Opening a file reads
// nothing beyond the first screen. Reading straight through costs no
// more than before, spread out. Only an explicit jump into unvisited
// territory -- dragging the scrollbar to the end, or End -- pays for
// the scan, and that scan builds the index as it goes, so it is paid
// once.
static uint32_t frontier_off;
static uint32_t frontier_line;
static md_state_t frontier_st;

// Set once the end has actually been reached. Until then the document
// length is genuinely unknown, which is the honest state to be in --
// see update_scrollbar() for how the scrollbar copes.
static bool eof_seen;
static uint32_t eof_line;

// -- position --

// The source line beginning the block at or before byte `off`.
static uint32_t line_at_offset(uint32_t off);


static uint32_t top_line;
static uint32_t top_sub;

// Byte offset of top_line. Kept because the scrollbar is measured in
// BYTES, not lines: the file size is known the moment the file is
// opened, whereas the line count is not known until the whole thing
// has been read -- which is exactly what lazy indexing exists to
// avoid doing.
static uint32_t top_off;

// Byte offset just past the last block drawn on the current screen.
// (bottom_off - top_off) is one screen's worth of document in bytes,
// which is what the scrollbar needs for its thumb size and for what a
// click in the trough should move.
static uint32_t bottom_off;

// -- layout --

static int view_w, view_h;
static int sb_len;
static z_scrollbar_t sbar;

// -- links on screen --

#define VIS_LINKS   32

typedef struct {
	int16_t		x, y, w, h;
	char		target[MD_LINK_MAX];
} vislink_t;

static vislink_t vlinks[VIS_LINKS];
static int nvlinks;
static int sel_link = -1;

// -- what is currently on screen --
//
// Selection works over what was DRAWN, not over the source. The
// source is not in memory (that is the whole design), and a display
// line is the result of block joining, inline stripping and wrapping
// -- so the only place the rendered text exists is here, recorded as
// it is drawn.
//
// This is also what copy assembles from, which means what lands on
// the clipboard is exactly what the reader could see, with the
// markup already resolved.
#define VIS_LINES   56
#define VIS_CHARS   144

#define VIS_SPANS   8

typedef struct {
	int16_t		x, y, h;
	uint8_t		fw;			// glyph width for this line's font
	uint16_t	len;
	char		text[VIS_CHARS];

	// Styling, in coordinates relative to this display line.
	//
	// Kept so a single row can be repainted without going back to
	// the parser. That is what makes a selection drag cheap: only the
	// rows whose selected state changed are redrawn, and redrawing
	// them from here costs no file access, no parse and no wrap.
	uint8_t		nsp;
	struct {
		uint16_t	s, l;
		uint8_t		k;		// md_span_kind_t
	} sp[VIS_SPANS];

} visline_t;

static visline_t vlines[VIS_LINES];
static int nvlines;

/* -- the step table --
 *
 * One entry per SCROLL STEP on the laid-out screen, in order: every
 * wrapped text sub, and also every blank line and rule, which cost a
 * step (subs_of() returns 1 for them) but produce no vline.
 *
 * scroll_forward() needs it to know how far the pixels must move.
 * That distance cannot be derived from the step count alone -- a
 * blank line costs a step and occupies 5 pixels, a heading costs one
 * and occupies 17 -- and it must not be derived from a second layout
 * pass either, which is what made the first attempt at this slower
 * than what it replaced.
 *
 * step_y is where the block BEGINS, before its space_before. That is
 * the number a fresh layout starting at this step would place at
 * MARGIN, so the shift is simply step_y[k] - MARGIN.
 */
// True when vlines[]/step_y[] describe what is ACTUALLY on the glass.
// The accelerated scroll blits pixels on the strength of that table,
// so anything that changes the screen without going through
// draw_body_from() -- a resize, a new document, a clear during
// loading -- must clear this or the blit moves pixels that are not
// where the table says.
static bool drawn_valid;

#define VIS_STEPS   96

static int16_t  step_y[VIS_STEPS];
static uint32_t step_ln[VIS_STEPS];
static uint8_t  step_sub[VIS_STEPS];
static uint32_t step_off[VIS_STEPS];   // file offset of the step's block
static int nsteps;

/* -- the layout cache --
 *
 * After a scroll, every block still on screen is the same block,
 * wrapped the same way, at a known offset. Re-reading and re-parsing
 * it to rediscover that was 84% of a scroll's cost on hardware -- and
 * on a document with many zero-height blocks (fence closers, front
 * matter) the layout walked 150 blocks to place a 14-block screen,
 * every keypress.
 *
 * So the layout remembers where it STOPPED: the parser state, file
 * offset, source line and y before the first block that did not fit.
 * A forward scroll shifts what it has and resumes from there, reading
 * only the blocks that scrolled in. Nothing above the resume point is
 * touched by the parser again.
 *
 * Only COMPLETE blocks are cached (cache_* counts). The block that was
 * cut short by the bottom of the screen is re-laid-out on resume,
 * because its remaining subs may now fit; its rows stay in vlines[]
 * for selection until then. Decorations need no cache at all -- the
 * blit carries them, and only the strip is redrawn.
 */
static md_state_t end_st;
static uint32_t   end_off, end_line;
static int        end_y;
static int        cache_ns, cache_nv, cache_nl;
static bool       cache_valid;

// Selection over those lines, anchor plus current in (line, column).
// Reading order, like sw/apps/term: partial at each end, whole lines
// between.
static bool sel_on;
static bool sel_dragging;
static bool sel_moved;
static int sel_ar, sel_ac, sel_cr, sel_cc;

// -- navigation history --

#define HIST_MAX    12

typedef struct {
	char		path[Z_FLIST_PATH_MAX];
	uint32_t	line, sub;
} hist_t;

static hist_t hist[HIST_MAX];
static int hist_n;

static z_dialog_ctx_t dlg_ctx;
static char last_dir[Z_FLIST_PATH_MAX] = "/";

static void forward_msg(z_msg_t *msg, void *user);
static void repaint(void);

// ---------------------------------------------------------------
// buffered line reader
// ---------------------------------------------------------------
//
// fs_read_chunk() is a syscall per call, so reading a line at a time
// straight from it would be a syscall per line. This buffers, and
// keeps one line of lookahead for md_parse()'s setext check.

/* One SD read per refill, so this is the granularity every sequential
 * scan through the document pays in. Scrolling reads a screenful of
 * source (~2-3KB) and the seek replay below reads more, so at 512 a
 * single scroll cost a double-figure number of card reads over SPI --
 * which is where a reader on this machine actually spends its time,
 * not in parsing and not in drawing.
 *
 * 2KB costs 1.5KB more RAM and cuts the refills by four. */
#define RBUF 2048

static uint8_t rbuf[RBUF];
static uint32_t rbuf_base;		// file offset of rbuf[0]
static uint32_t rbuf_len, rbuf_pos;

static char peek_line[MD_LINE_MAX];
static bool peek_valid;
static uint32_t peek_off;

/* -- where a repaint's time goes --
 *
 * Build with -DREAD_PROFILE=1 to print a cycle breakdown to the
 * serial console after each body repaint. Off by default; compiles
 * out entirely.
 *
 * This exists to settle one question, because the answer selects
 * between two different optimisations and guessing has already cost
 * more than measuring would have:
 *
 *   PARSE dominates  -> a one-line scroll re-reads and re-parses the
 *                       whole screen to move it up nine pixels. The
 *                       fix is to keep the laid-out screen and shift
 *                       it, parsing only the block that scrolled in.
 *
 *   DRAW dominates   -> the fix is to stop redrawing 25 rows to
 *                       change one: move the pixels with the blitter
 *                       and draw only the exposed strip. (That was
 *                       tried before and did not help -- but it was
 *                       paired with a second layout pass that added
 *                       back more than it saved, and the position
 *                       cache did not exist yet.)
 *
 * Both are worth doing eventually. Only one is worth doing first, and
 * this says which. rdcycle is a real free-running counter here
 * (picorv32, ENABLE_COUNTERS); z_uptime_ticks() at 732Hz cannot
 * resolve a single row.
 */
#ifndef READ_PROFILE
#define READ_PROFILE 0
#endif

#if READ_PROFILE
static inline uint32_t rp_cyc(void) {
	uint32_t v; __asm__ volatile ("rdcycle %0" : "=r"(v)); return v;
}
static uint32_t rp_parse, rp_draw, rp_io, rp_total;
static uint32_t rp_wrap, rp_fill, rp_seek;
static uint32_t rp_rdline, rp_mdparse;
static uint32_t rp_blocks, rp_rows, rp_refills;
static int rp_pending;

/* -- why this formats by hand instead of using printf --
 *
 * read calls printf exactly twice, and neither call has a conversion
 * specifier, so the compiler rewrites both into puts() and the full
 * vfprintf formatter is never referenced. --gc-sections then drops
 * it. Adding ONE "%lu" links the whole formatter back in: read.bin
 * goes from ~50KB to ~150KB, which overruns the space the loader has
 * for it, and the app crashes on start rather than merely being
 * large.
 *
 * That is not a theoretical concern -- it happened twice during this
 * work, and both times the size jump looked like a mystery rather
 * than a consequence of the debug print that caused it. So: build the
 * line by hand, emit it with fputs(), and instrumentation stays free.
 *
 * The same trap applies to any app here whose printf calls are all
 * plain strings. Check the binary size after adding a debug print.
 */
static void rp_u32(char *buf, int *n, uint32_t v) {
	char t[12];
	int i = 0;
	if (!v) { buf[(*n)++] = '0'; return; }
	while (v && i < 12) { t[i++] = (char)('0' + (v % 10)); v /= 10; }
	while (i) buf[(*n)++] = t[--i];
}

static void rp_str(char *buf, int *n, const char *s) {
	while (*s) buf[(*n)++] = *s++;
}

/* Called from the event loop, never from the draw path: a deep call
 * chain plus a formatting buffer is how a 16KB stack gets exhausted. */
static void rp_report(void) {

	static char line[192];
	int n = 0;

	if (!rp_pending) return;
	rp_pending = 0;

	rp_str(line, &n, "read: total ");
	rp_u32(line, &n, rp_total / 1000);
	rp_str(line, &n, "k = seek ");
	rp_u32(line, &n, rp_seek / 1000);
	rp_str(line, &n, "k + parse ");
	rp_u32(line, &n, rp_parse / 1000);
	rp_str(line, &n, "k + wrap ");
	rp_u32(line, &n, rp_wrap / 1000);
	rp_str(line, &n, "k + draw ");
	rp_u32(line, &n, rp_draw / 1000);
	rp_str(line, &n, "k + fill ");
	rp_u32(line, &n, rp_fill / 1000);
	rp_str(line, &n, "k  (rdline ");
	rp_u32(line, &n, rp_rdline / 1000);
	rp_str(line, &n, "k mdparse ");
	rp_u32(line, &n, rp_mdparse / 1000);
	rp_str(line, &n, "k)  [");
	rp_u32(line, &n, rp_blocks);
	rp_str(line, &n, " blk ");
	rp_u32(line, &n, rp_rows);
	rp_str(line, &n, " sub ");
	rp_u32(line, &n, rp_refills);
	rp_str(line, &n, " io ");
	rp_u32(line, &n, rp_io / 1000);
	rp_str(line, &n, "k]");
	line[n] = 0;

	// puts(), not fputs(..., stdout): fputs needs a FILE and drags in
	// picolibc's whole stdio layer with it -- ~40KB, which is the
	// same size trap as the printf formatter this was written to
	// avoid, just via a different symbol. puts is already linked
	// (read's plain-string printfs compile into it) and appends the
	// newline itself.
	puts(line);

	rp_parse = rp_draw = rp_io = rp_total = 0;
	rp_wrap = rp_fill = rp_seek = 0;
	rp_rdline = rp_mdparse = 0;
	rp_blocks = rp_rows = rp_refills = 0;
}
#else
#define rp_report() ((void)0)
#endif

static uint32_t rd_tell(void) {
	return peek_valid ? peek_off : rbuf_base + rbuf_pos;
}

static void rd_seek(uint32_t off) {

	if (fh < 0) return;

	/* Already inside the buffer -- just move the cursor.
	 *
	 * This used to discard unconditionally, which meant every seek
	 * cost a card read even when the target byte was already sitting
	 * in memory. Seeks here are overwhelmingly SHORT and BACKWARD --
	 * to the nearest index checkpoint, or to the top of the screen --
	 * so the target usually is in the buffer. */
	if (rbuf_len && off >= rbuf_base && off < rbuf_base + rbuf_len) {
		rbuf_pos = off - rbuf_base;
		peek_valid = false;
		return;
	}

	fs_seek(fh, off);

	rbuf_base = off;
	rbuf_len = 0;
	rbuf_pos = 0;
	peek_valid = false;

}

static int rd_byte(void) {

	if (rbuf_pos >= rbuf_len) {

		rbuf_base += rbuf_len;
		rbuf_pos = 0;

#if READ_PROFILE
		uint32_t io0 = rp_cyc();
#endif
		int n = fs_read_chunk(fh, rbuf, RBUF);
#if READ_PROFILE
		rp_io += rp_cyc() - io0;
		rp_refills++;
#endif
		rbuf_len = (n > 0) ? (uint32_t)n : 0;

		if (!rbuf_len) return -1;

	}

	return rbuf[rbuf_pos++];

}

// Reads one line into `out`, stripping the newline and any CR.
// Returns false at end of file.
// Reads a line by scanning the BUFFER, not by calling rd_byte() per
// character.
//
// It was per-character, and that cost a function call plus a
// "do I need to refill?" test for every byte of the document. Measured
// on hardware: ~2.4M cycles per repaint inside line reading alone,
// twice what the Markdown parser itself cost. The bytes are already
// sitting in rbuf; finding the newline in them is a scan, and the
// refill test belongs once per buffer rather than once per byte.
//
// Same contract as before: newline consumed and not stored, '\r'
// dropped anywhere in the line, output always NUL-terminated, false
// only when there was nothing left to read at all (a partial last line
// with no trailing newline still returns true).
static bool rd_raw(char *out, int cap) {

	int n = 0;
	bool any = false;

	for (;;) {

		if (rbuf_pos >= rbuf_len) {

			// Exactly rd_byte()'s refill, hoisted out of the loop
			// over characters. rbuf_base must advance by the length
			// CONSUMED, before rbuf_len is replaced.
			rbuf_base += rbuf_len;
			rbuf_pos = 0;

#if READ_PROFILE
			uint32_t io0 = rp_cyc();
#endif
			int r = fs_read_chunk(fh, rbuf, RBUF);
#if READ_PROFILE
			rp_io += rp_cyc() - io0;
			rp_refills++;
#endif
			rbuf_len = (r > 0) ? (uint32_t)r : 0;

			if (!rbuf_len) break;    // end of file
		}

		uint32_t i = rbuf_pos;
		while (i < rbuf_len && rbuf[i] != '\n') i++;

		for (uint32_t k = rbuf_pos; k < i; k++) {
			char ch = (char)rbuf[k];
			if (ch != '\r' && n < cap - 1) out[n++] = ch;
		}

		any = true;

		if (i < rbuf_len) {
			rbuf_pos = i + 1;        // consume the newline
			break;
		}

		// Ran out of buffered bytes before finding one: refill and
		// keep going on the same line.
		rbuf_pos = i;
	}

	out[n] = 0;
	return any || n > 0;

}

static bool rd_line(char *out, int cap) {

	if (peek_valid) {
		int i = 0;
		for (; peek_line[i] && i < cap - 1; i++) out[i] = peek_line[i];
		out[i] = 0;
		peek_valid = false;
		return true;
	}

	return rd_raw(out, cap);

}

// The line after the one rd_line() would return, or NULL at EOF.
static const char *rd_peek(void) {

	if (!peek_valid) {
		peek_off = rbuf_base + rbuf_pos;
		if (!rd_raw(peek_line, MD_LINE_MAX)) return NULL;
		peek_valid = true;
	}

	return peek_line;

}

// ---------------------------------------------------------------
// block reader
// ---------------------------------------------------------------

// Reads one BLOCK -- a source line plus any lines that lazily
// continue it -- and parses it. Returns the number of source lines
// consumed, or 0 at end of file.
//
// A block, not a line, is the unit everything else here works in.
// Generated Markdown is routinely hard wrapped (the book's assembled
// form splits link syntax across lines), so a reader that renders one
// source line at a time shows raw brackets where links should be. See
// md_continues() in md.h.
//
// Positions are therefore always block starts. That is what makes
// scrolling reversible: a block always wraps the same way regardless
// of where it was entered from, which would not be true if a position
// could land in the middle of one.
static uint32_t read_block(md_state_t *st, md_line_t *out) {

	static char line[MD_LINE_MAX];
	static char joined[MD_LINE_MAX];

#if READ_PROFILE
	{ uint32_t r0 = rp_cyc();
	  bool got = rd_line(line, MD_LINE_MAX);
	  rp_rdline += rp_cyc() - r0;
	  if (!got) return 0; }
#else
	if (!rd_line(line, MD_LINE_MAX)) return 0;
#endif

	uint32_t used = 1;

	// Parse once to learn what kind of block this is, then decide
	// whether the following lines belong to it.
	md_state_t probe = *st;

	// Parsed straight into *out rather than into a local and copied.
	// md_line_t is 1956 bytes, so each of those copies was ~500 word
	// moves on both of the paths that return without joining -- which
	// is nearly every block. Only .kind is needed afterwards, and it
	// is taken before the join path overwrites *out below.
#if READ_PROFILE
	uint32_t m0 = rp_cyc();
	const char *pk = rd_peek();
	rp_rdline += rp_cyc() - m0;
	m0 = rp_cyc();
	bool ate = md_parse(&probe, line, pk, out);
	rp_mdparse += rp_cyc() - m0;
#else
	bool ate = md_parse(&probe, line, rd_peek(), out);
#endif

	if (ate) {
		// setext underline
		rd_line(joined, MD_LINE_MAX);
		*st = probe;
		return 2;
	}

	if (!md_continues(st, out->kind, rd_peek())) {
		*st = probe;
		return used;
	}

	// Join. The source line goes in raw, not the parsed text, because
	// inline syntax may straddle the break -- rejoining already
	// stripped text would have lost the very brackets being repaired.
	int n = 0;
	for (int i = 0; line[i] && n < MD_LINE_MAX - 1; i++) joined[n++] = line[i];

	md_kind_t kind = out->kind;

	while (md_continues(st, kind, rd_peek())) {

		if (!rd_line(line, MD_LINE_MAX)) break;

		used++;

		int i = 0;
		while (line[i] == ' ' || line[i] == '\t') i++;

		if (n < MD_LINE_MAX - 1) joined[n++] = ' ';
		for (; line[i] && n < MD_LINE_MAX - 1; i++) joined[n++] = line[i];

	}

	joined[n] = 0;

	md_parse(st, joined, NULL, out);

	return used;

}

// ---------------------------------------------------------------
// index
// ---------------------------------------------------------------

// Halves the index in place and doubles the stride, for a document
// that turned out longer than IDX_MAX * stride lines. Keeping every
// other entry preserves the invariant that entry i covers line
// i * stride.
static void idx_compress(void) {

	for (int i = 0; i * 2 < idx_n; i++) idx[i] = idx[i * 2];

	idx_n = (idx_n + 1) / 2;
	idx_stride *= 2;

}

static bool pos_valid;

// Starts the index. Reads nothing -- see the frontier comment above.
static void index_init(void) {

	idx_n = 0;
	idx_stride = 32;

	// The cached position belongs to the previous document.
	pos_valid = false;

	eof_seen = false;
	eof_line = 0;

	md_state_init(&frontier_st);
	frontier_off = 0;
	frontier_line = 0;

	if (fh < 0) return;

	idx[0].off = 0;
	idx[0].line = 0;
	idx[0].st = frontier_st;
	idx_n = 1;

}

// Extends the index until it covers source line `want_line` AND byte
// offset `want_off`, or the end of the file. Either bound may be 0 to
// mean "no requirement".
//
// This is the only thing that reads unvisited parts of the document,
// and it never re-reads: the frontier only moves forward.
static void extend_index(uint32_t want_line, uint32_t want_off) {

	if (fh < 0 || eof_seen) {
		if (eof_seen) return;
	}

	if (frontier_line >= want_line && frontier_off >= want_off) return;

	rd_seek(frontier_off);

	md_state_t st = frontier_st;
	static md_line_t ml;

	uint32_t ln = frontier_line;
	uint32_t off = frontier_off;

	while (ln < want_line || off < want_off) {

		uint32_t used = read_block(&st, &ml);

		if (!used) {
			eof_seen = true;
			eof_line = ln;
			break;
		}

		ln += used;
		off = rd_tell();

		// Checkpoint at each stride boundary, on a block boundary.
		if (idx_n && ln >= idx[idx_n - 1].line + idx_stride) {

			if (idx_n >= IDX_MAX) idx_compress();

			if (idx_n < IDX_MAX && ln >= idx[idx_n - 1].line + idx_stride) {
				idx[idx_n].off = off;
				idx[idx_n].line = ln;
				idx[idx_n].st = st;
				idx_n++;
			}

		}

	}

	frontier_line = ln;
	frontier_off = off;
	frontier_st = st;

}

// Seeks to `line` and restores the parser state for it.
uint32_t line_at_offset(uint32_t off) {

	extend_index(0, off);

	int e = 0;

	for (int i = 0; i < idx_n; i++)
		if (idx[i].off <= off) e = i; else break;

	rd_seek(idx[e].off);

	md_state_t st = idx[e].st;
	static md_line_t ml;

	uint32_t ln = idx[e].line;
	uint32_t at = idx[e].off;

	while (at < off) {
		uint32_t at_line = ln;
		uint32_t used = read_block(&st, &ml);
		if (!used) return at_line;
		ln += used;
		at = rd_tell();
		if (at >= off) return at_line;
	}

	return ln;

}

// -- the position cache --
//
// Where seek_line() last landed: a checkpoint of exactly the same kind
// as an index entry (offset, line, parser state), but for wherever the
// reader most recently was rather than for a fixed stride.
//
// It exists because the index alone makes scrolling pay for distance
// it has already travelled. A one-line scroll calls seek_line() twice
// -- once in scroll_down() to find the new top, once in
// draw_body_from() to render it -- and each call restarted from the
// nearest checkpoint up to idx_stride (32) source lines back, replaying
// blocks it had parsed moments earlier. On a document read straight
// through, that is the same 32 lines re-parsed on every keypress, and
// each replay is a seek plus card reads.
//
// Replaying forward from here is exactly as correct as replaying from
// an index entry: both are a byte offset with the parser state that
// belongs at it, which is the whole premise the index rests on. The
// only rule is that it can never be used to go BACKWARD, since the
// parser cannot run in reverse -- hence the pos_line <= line test.
static uint32_t   pos_line;
static uint32_t   pos_off;
static md_state_t pos_st;

static void seek_line(uint32_t line, md_state_t *st) {

	// The index may not reach this far yet -- see extend_index().
	extend_index(line, 0);

	int e = 0;

	for (int i = 0; i < idx_n; i++)
		if (idx[i].line <= line) e = i; else break;

	uint32_t   start_line = idx[e].line;
	uint32_t   start_off  = idx[e].off;
	md_state_t start_st   = idx[e].st;

	// Start from the cached position when it is at or before the
	// target AND no earlier than the checkpoint -- i.e. only when it
	// is genuinely closer, so this can never make a seek longer.
	if (pos_valid && pos_line <= line && pos_line >= start_line) {
		start_line = pos_line;
		start_off  = pos_off;
		start_st   = pos_st;
	}

	rd_seek(start_off);
	*st = start_st;

	static md_line_t ml;

	uint32_t ln = start_line;

	while (ln < line) {
		uint32_t used = read_block(st, &ml);
		if (!used) break;
		ln += used;
	}

	// Record where this ended up, EOF included: a short landing is
	// still a valid position, and caching it stops the next call
	// walking the same ground again.
	pos_valid = true;
	pos_line  = ln;
	pos_off   = rd_tell();
	pos_st    = *st;

}

// The block start at or before `line`. Used by scroll_up(), which
// cannot simply decrement -- a block may be several source lines
// long, and stopping inside one would render it differently from
// every other approach to the same place.
//
// Bounded by the index stride, so this does not get slower as the
// document gets longer.
static uint32_t prev_block(uint32_t line) {

	if (!line) return 0;

	extend_index(line, 0);

	int e = 0;

	for (int i = 0; i < idx_n; i++)
		if (idx[i].line < line) e = i; else break;

	rd_seek(idx[e].off);

	md_state_t st = idx[e].st;
	static md_line_t ml;

	uint32_t ln = idx[e].line;
	uint32_t prev = ln;

	while (ln < line) {
		uint32_t used = read_block(&st, &ml);
		if (!used) break;
		prev = ln;
		ln += used;
	}

	return prev;

}

// ---------------------------------------------------------------
// layout of one parsed line
// ---------------------------------------------------------------

static const z_font_t *line_font(const md_line_t *ml) {

	// Headings get the larger font, but only the top two levels.
	// Below that the size difference stops reading as hierarchy and
	// starts reading as noise, and there is no third size to use.
	if (ml->kind == MD_HEADING && ml->level <= 2) return HEAD_FONT;

	return BODY_FONT;

}

// Left inset in pixels, and the hanging indent for wrapped
// continuation lines.
static void line_indent(const md_line_t *ml, int *first, int *rest) {

	int cw = BODY_FONT->w;

	switch (ml->kind) {

		case MD_LIST: {
			int base = (ml->level + 1) * 2 * cw;
			int mlen = (int)strlen(ml->marker) + 1;
			*first = base;
			// Continuation lines align under the text, not under the
			// marker -- a wrapped bullet that starts back at the
			// margin reads as a new item.
			*rest = base + mlen * cw;
			break;
		}

		case MD_QUOTE:
			*first = *rest = 3 * cw;
			break;

		case MD_CODE:
		case MD_TABLE:
			*first = *rest = cw;
			break;

		default:
			*first = *rest = 0;
			break;

	}

}

// Splits `ml` into display lines, filling `starts`/`ends` with
// character ranges. Returns how many.
//
// Code and table lines are NEVER wrapped: both depend on their own
// horizontal layout, and folding them destroys the only thing they
// were carrying. They are clipped instead.
#define MAX_SUB 64

static int wrap_line(const md_line_t *ml, int width,
	uint16_t *starts, uint16_t *ends) {

	if (ml->kind == MD_CODE || ml->kind == MD_TABLE) {
		starts[0] = 0;
		ends[0] = ml->len;
		return 1;
	}

	if (!ml->len) { starts[0] = 0; ends[0] = 0; return 1; }

	const z_font_t *f = line_font(ml);

	int first_in, rest_in;
	line_indent(ml, &first_in, &rest_in);

	int n = 0;
	int i = 0;

	while (i < ml->len && n < MAX_SUB) {

		int avail = width - (n ? rest_in : first_in);
		int cols = avail / f->w;
		if (cols < 1) cols = 1;

		int end = i + cols;

		if (end >= ml->len) {
			starts[n] = (uint16_t)i;
			ends[n] = ml->len;
			n++;
			break;
		}

		// break at the last space in range
		int brk = -1;
		for (int k = end; k > i; k--)
			if (ml->text[k] == ' ') { brk = k; break; }

		if (brk < 0) brk = end;		// a word longer than the line

		starts[n] = (uint16_t)i;
		ends[n] = (uint16_t)brk;
		n++;

		i = brk;
		while (i < ml->len && ml->text[i] == ' ') i++;

	}

	if (!n) { starts[0] = 0; ends[0] = 0; n = 1; }

	return n;

}

// ---------------------------------------------------------------
// drawing
// ---------------------------------------------------------------

static void fill(int cx, int cy, int w, int h, int color) {

	z_clip_t c;
	z_win_content_rect(&win, &c);

	int x0 = c.x0 + cx, y0 = c.y0 + cy;
	int x1 = x0 + w - 1, y1 = y0 + h - 1;

	if (x0 < c.x0) x0 = c.x0;
	if (y0 < c.y0) y0 = c.y0;
	if (x1 > c.x1) x1 = c.x1;
	if (y1 > c.y1) y1 = c.y1;
	if (x1 < x0 || y1 < y0) return;

	z_fb_hw_fill_rect(x0, y0, x1 - x0 + 1, y1 - y0 + 1, color);

}

// The style of character `i` -- which span, if any, covers it.
static const md_span_t *span_at(const md_line_t *ml, int i) {

	for (int s = 0; s < ml->nspans; s++)
		if (i >= ml->spans[s].start &&
			i < ml->spans[s].start + ml->spans[s].len)
			return &ml->spans[s];

	return NULL;

}

// Draws one display line, styled run by run.
static void draw_sub(const md_line_t *ml, int from, int to,
	int cx, int cy, int indent) {

	const z_font_t *f = line_font(ml);

	z_clip_t c;
	z_win_content_rect(&win, &c);

	z_clip_t clip;
	clip.x0 = c.x0 + MARGIN;
	clip.y0 = c.y0 + cy;
	clip.x1 = c.x0 + MARGIN + view_w - 1;
	clip.y1 = clip.y0 + f->h - 1;

	if (clip.x1 > c.x1) clip.x1 = c.x1;
	if (clip.y1 > c.y1) clip.y1 = c.y1;
	if (clip.x1 < clip.x0 || clip.y1 < clip.y0) return;

	int x = c.x0 + cx + indent;
	int y = c.y0 + cy;

	int i = from;

	while (i < to) {

		const md_span_t *sp = span_at(ml, i);

		// how far this run extends
		int end = i;
		while (end < to && span_at(ml, end) == sp) end++;

		char tmp[MD_LINE_MAX];
		int n = 0;

		for (int k = i; k < end && n < (int)sizeof(tmp) - 1; k++)
			tmp[n++] = (ml->text[k] == '\t') ? ' ' : ml->text[k];

		tmp[n] = 0;

		int rw = n * f->w;

		if (sp && sp->kind == MD_SPAN_CODE) {

			// Inverse video -- the one inline style a 1bpp display
			// carries well, and by far the most common construct in
			// the corpus.
			z_fb_draw_text2(x, y, tmp, 0, 1, f, &clip);

		} else {

			z_fb_draw_text(x, y, tmp, 1, f, &clip);

			if (sp && sp->kind == MD_SPAN_LINK) {

				// Underline, one pixel below the glyphs.
				int uy = y + f->h - 1;
				if (uy <= clip.y1)
					z_fb_hw_line(x, uy, x + rw - 2, uy, 1, &clip);

				// Remember it for Tab and for clicks.
				if (nvlinks < VIS_LINKS) {
					vlinks[nvlinks].x = (int16_t)(x - c.x0);
					vlinks[nvlinks].y = (int16_t)(y - c.y0);
					vlinks[nvlinks].w = (int16_t)rw;
					vlinks[nvlinks].h = (int16_t)f->h;
					int t = 0;
					const char *tg = ml->links[sp->link];
					for (; tg[t] && t < MD_LINK_MAX - 1; t++)
						vlinks[nvlinks].target[t] = tg[t];
					vlinks[nvlinks].target[t] = 0;
					nvlinks++;
				}

			}

		}

		x += rw;
		i = end;

	}

}

// Extra vertical space around a block, in pixels.
static int space_before(const md_line_t *ml) {

	if (ml->kind == MD_HEADING) return ml->level <= 2 ? 5 : 3;
	if (ml->kind == MD_RULE) return 3;

	return 0;

}

// -- selection --

static void sel_bounds(int *r0, int *c0, int *r1, int *c1) {

	if (sel_ar < sel_cr || (sel_ar == sel_cr && sel_ac <= sel_cc)) {
		*r0 = sel_ar; *c0 = sel_ac; *r1 = sel_cr; *c1 = sel_cc;
	} else {
		*r0 = sel_cr; *c0 = sel_cc; *r1 = sel_ar; *c1 = sel_ac;
	}

}

static bool sel_active(void) {
	return sel_on && !(sel_ar == sel_cr && sel_ac == sel_cc);
}

// Which display line and column a content-relative point lands on.
// Clamped, so a drag off the bottom selects to the end of the page
// rather than stopping.
static void hit_cell(int cx, int cy, int *row, int *col) {

	int r = 0;

	for (int i = 0; i < nvlines; i++) {
		if (cy >= vlines[i].y) r = i;
		else break;
	}

	if (!nvlines) { *row = 0; *col = 0; return; }

	visline_t *v = &vlines[r];

	int c = v->fw ? (cx - v->x + v->fw / 2) / v->fw : 0;

	if (c < 0) c = 0;
	if (c > v->len) c = v->len;

	*row = r;
	*col = c;

}

// Draws the selection over the already-rendered body.
//
// An overlay rather than a special case inside draw_sub(). Selected
// text is redrawn inverse, which also overrides the inverse already
// used for inline code -- selection winning there is correct, and
// tracking both states through the run splitting would be a lot of
// complexity for a case nobody would notice.
static void draw_selection(void) {

	if (!sel_active()) return;

	int r0, c0, r1, c1;
	sel_bounds(&r0, &c0, &r1, &c1);

	z_clip_t c;
	z_win_content_rect(&win, &c);

	for (int r = r0; r <= r1 && r < nvlines; r++) {

		if (r < 0) continue;

		visline_t *v = &vlines[r];

		int from = (r == r0) ? c0 : 0;
		int to = (r == r1) ? c1 : v->len;

		if (from < 0) from = 0;
		if (to > v->len) to = v->len;
		if (to <= from) continue;

		z_clip_t clip;
		clip.x0 = c.x0 + MARGIN;
		clip.y0 = c.y0 + v->y;
		clip.x1 = c.x0 + MARGIN + view_w - 1;
		clip.y1 = clip.y0 + v->h - 1;

		if (clip.x1 > c.x1) clip.x1 = c.x1;
		if (clip.y1 > c.y1) clip.y1 = c.y1;
		if (clip.x1 < clip.x0 || clip.y1 < clip.y0) continue;

		char tmp[VIS_CHARS];
		int n = 0;

		for (int k = from; k < to && n < VIS_CHARS - 1; k++)
			tmp[n++] = v->text[k];

		tmp[n] = 0;

		const z_font_t *f = (v->fw == HEAD_FONT->w) ? HEAD_FONT : BODY_FONT;

		z_fb_draw_text2(c.x0 + v->x + from * v->fw, c.y0 + v->y,
			tmp, 0, 1, f, &clip);

	}

}

// Repaints one display row from vlines[] -- no file access, no
// parse, no wrap. Used during a selection drag, where only the rows
// whose selected state changed need touching.
//
// Redrawing the whole body per drag sample was the second source of
// flashing in this app, for the same reason the scrollbar was the
// first: the expensive path was being run at pointer rates for a
// change that affects two rows.
static void redraw_row(int r) {

	if (r < 0 || r >= nvlines) return;

	visline_t *v = &vlines[r];

	z_clip_t c;
	z_win_content_rect(&win, &c);

	z_clip_t clip;
	clip.x0 = c.x0 + MARGIN;
	clip.y0 = c.y0 + v->y;
	clip.x1 = c.x0 + MARGIN + view_w - 1;
	clip.y1 = clip.y0 + v->h - 1;

	if (clip.x1 > c.x1) clip.x1 = c.x1;
	if (clip.y1 > c.y1) clip.y1 = c.y1;
	if (clip.x1 < clip.x0 || clip.y1 < clip.y0) return;

	fill(MARGIN, v->y, view_w, v->h, 0);

	const z_font_t *f = (v->fw == HEAD_FONT->w) ? HEAD_FONT : BODY_FONT;

	int i = 0;

	while (i < v->len) {

		// which span, if any, covers this character
		int k = -1;
		for (int j = 0; j < v->nsp; j++)
			if (i >= v->sp[j].s && i < v->sp[j].s + v->sp[j].l) { k = j; break; }

		int end = i + 1;
		while (end < v->len) {
			int k2 = -1;
			for (int j = 0; j < v->nsp; j++)
				if (end >= v->sp[j].s && end < v->sp[j].s + v->sp[j].l) { k2 = j; break; }
			if (k2 != k) break;
			end++;
		}

		char tmp[VIS_CHARS];
		int n = 0;
		for (int q = i; q < end && n < VIS_CHARS - 1; q++) tmp[n++] = v->text[q];
		tmp[n] = 0;

		int x = c.x0 + v->x + i * v->fw;

		if (k >= 0 && v->sp[k].k == MD_SPAN_CODE) {
			z_fb_draw_text2(x, c.y0 + v->y, tmp, 0, 1, f, &clip);
		} else {
			z_fb_draw_text(x, c.y0 + v->y, tmp, 1, f, &clip);
			if (k >= 0 && v->sp[k].k == MD_SPAN_LINK) {
				int uy = c.y0 + v->y + f->h - 1;
				if (uy <= clip.y1)
					z_fb_hw_line(x, uy, x + n * v->fw - 2, uy, 1, &clip);
			}
		}

		i = end;

	}

}

static void sel_clear(void) {

	if (!sel_on) return;

	sel_on = false;
	sel_dragging = false;

}

// Copies the selection, assembled from what is on screen.
//
// Trailing blanks are trimmed per line and rows are joined with
// newlines, the same convention sw/apps/term uses -- the clipboard
// should hold what the reader could see, not the padding that put it
// there.
static void sel_copy(void) {

	if (!sel_active()) return;

	int r0, c0, r1, c1;
	sel_bounds(&r0, &c0, &r1, &c1);

	static char out[Z_WM_CLIP_MAX];
	int n = 0;

	for (int r = r0; r <= r1 && r < nvlines && n < (int)sizeof(out) - 1; r++) {

		if (r < 0) continue;

		visline_t *v = &vlines[r];

		int from = (r == r0) ? c0 : 0;
		int to = (r == r1) ? c1 : v->len;

		if (from < 0) from = 0;
		if (to > v->len) to = v->len;

		int last = from - 1;
		for (int k = from; k < to; k++)
			if (v->text[k] != ' ') last = k;

		for (int k = from; k <= last && n < (int)sizeof(out) - 1; k++)
			out[n++] = v->text[k];

		if (r != r1 && n < (int)sizeof(out) - 1) out[n++] = '\n';

	}

	out[n] = 0;

	z_clip_set(out, n);

}

// Lays out AND draws the body. `thr` splits the two: the whole screen
// is always laid out (vlines[], vlinks[], the step table and
// bottom_off must describe the full screen either way), but only
// pixels at y >= thr are drawn, and only [thr, view_h) is cleared.
//
//   thr = 0          the full redraw this always was
//   0 < thr          just the strip a scroll blit exposed
//
// The rule that keeps a partial draw honest: every y advance happens
// unconditionally, and the gates wrap only the drawing calls. A gated
// `y +=` would make the partial layout disagree with the full one,
// and the seam between moved pixels and drawn pixels would show it.
// The block loop, from an arbitrary starting point. `st` is the parser
// state entering the first block, the file is positioned at it, and
// `line`/`y`/`sub_skip` are where layout begins. Appends to vlines[],
// vlinks[] and the step table -- the caller resets them for a full
// pass and keeps them for a resume.
//
// Records the END STATE for the layout cache: everything up to the
// last block that fit completely, plus where to pick up from.
static void layout_run(md_state_t *st, uint32_t line, int y,
	uint32_t sub_skip, int thr) {

	static md_line_t ml;
	uint16_t starts[MAX_SUB], ends[MAX_SUB];
	uint32_t cur_line = line;

	// Snapshot before each block, so that when a block turns out not
	// to fit the cache can end just before it.
	md_state_t snap_st = *st;
	uint32_t snap_off = rd_tell(), snap_line = cur_line;
	int snap_y = y, snap_nv = nvlines, snap_ns = nsteps, snap_nl = nvlinks;
	bool cut = false;

	while (y < view_h) {

		snap_st = *st;
		snap_off = rd_tell();
		snap_line = cur_line;
		snap_y = y;
		snap_nv = nvlines;
		snap_ns = nsteps;
		snap_nl = nvlinks;


#if READ_PROFILE
		uint32_t pb0 = rp_cyc();
		uint32_t pb_used = read_block(st, &ml);
		rp_parse += rp_cyc() - pb0;
		rp_blocks++;
		if (!pb_used) break;
		uint32_t used = pb_used;
#else
		uint32_t used = read_block(st, &ml);
		if (!used) break;
#endif

		// The block just read starts at blk_line; advance past it
		// here, before any of the continue branches below. y_blk is
		// where it begins, BEFORE space_before -- see the step table.
		uint32_t blk_line = cur_line;
		uint32_t blk_off = snap_off;
		cur_line += used;
		int y_blk = y;

		// Draws nothing and takes no space -- see MD_SKIP in md.h.
		if (ml.kind == MD_SKIP) continue;

		if (ml.kind == MD_BLANK) {
			if (!sub_skip) {
				if (nsteps < VIS_STEPS) {
					step_y[nsteps] = (int16_t)y_blk;
					step_ln[nsteps] = blk_line;
					step_off[nsteps] = blk_off;
					step_sub[nsteps] = 0;
					nsteps++;
				}
				y += BODY_FONT->h / 2 + 1;
			}
			else sub_skip = 0;
			continue;
		}

		if (ml.kind == MD_RULE) {
			if (!sub_skip) {
				if (nsteps < VIS_STEPS) {
					step_y[nsteps] = (int16_t)y_blk;
					step_ln[nsteps] = blk_line;
					step_off[nsteps] = blk_off;
					step_sub[nsteps] = 0;
					nsteps++;
				}
				y += space_before(&ml);
				if (y + 1 >= thr) fill(MARGIN, y + 1, view_w, 1, 1);
				y += 3;
			} else sub_skip = 0;
			continue;
		}

		const z_font_t *f = line_font(&ml);
		int lh = f->h + 1;

		int first_in, rest_in;
		line_indent(&ml, &first_in, &rest_in);

#if READ_PROFILE
		uint32_t w0 = rp_cyc();
		int n = wrap_line(&ml, view_w, starts, ends);
		rp_wrap += rp_cyc() - w0;
#else
		int n = wrap_line(&ml, view_w, starts, ends);
#endif

		if (!sub_skip) y += space_before(&ml);

		for (int s = 0; s < n; s++) {

			if (sub_skip) { sub_skip--; continue; }
			if (y + f->h > view_h) { cut = true; break; }

			if (nsteps < VIS_STEPS) {
				// Sub 0 carries the block's space_before with it;
				// later subs start at their own row, since a layout
				// entered there skips space_before (see sub_skip).
				step_y[nsteps] = (int16_t)(s ? y : y_blk);
				step_ln[nsteps] = blk_line;
					step_off[nsteps] = blk_off;
				step_sub[nsteps] = (uint8_t)s;
				nsteps++;
			}

			int indent = s ? rest_in : first_in;

			// In a partial pass a row may straddle the strip: it did
			// not fit on the old screen and fits now, so the blit
			// filled its band with whatever was above. Clearing it
			// first makes every drawn row self-contained; a full pass
			// (thr == 0) already cleared everything.
			if (thr > 0 && y + f->h > thr && y < thr)
				fill(MARGIN, y, view_w, f->h, 0);

			// The list marker sits in the hanging indent, on the
			// first display line only.
			if (ml.kind == MD_LIST && s == 0 && y + f->h > thr) {
				z_clip_t c;
				z_win_content_rect(&win, &c);
				z_clip_t clip = { c.x0 + MARGIN, c.y0 + y,
					c.x0 + MARGIN + view_w - 1, c.y0 + y + f->h - 1 };
				if (clip.y1 > c.y1) clip.y1 = c.y1;
				z_fb_draw_text(c.x0 + MARGIN + first_in, c.y0 + y,
					ml.marker, 1, f, &clip);
			}

			// Block quotes get a rule down the left, which is the
			// only structural cue available without indentation
			// alone doing all the work.
			if (ml.kind == MD_QUOTE && y + f->h > thr)
				fill(MARGIN, y, 1, f->h, 1);

			int extra = (ml.kind == MD_LIST && s == 0)
				? (int)(strlen(ml.marker) + 1) * f->w : 0;

			// Record what this display line holds, for selection and
			// copy -- see visline_t.
			if (nvlines < VIS_LINES) {
				visline_t *v = &vlines[nvlines];
				v->x = (int16_t)(MARGIN + indent + extra);
				v->y = (int16_t)y;
				v->h = (int16_t)f->h;
				v->fw = (uint8_t)f->w;
				int n2 = 0;
				for (int k = starts[s]; k < ends[s] && n2 < VIS_CHARS - 1; k++)
					v->text[n2++] = (ml.text[k] == '\t') ? ' ' : ml.text[k];
				v->text[n2] = 0;
				v->len = (uint16_t)n2;

				// Clip each span to this display line's range and
				// rebase it, so the row can be redrawn standalone.
				v->nsp = 0;
				for (int k = 0; k < ml.nspans && v->nsp < VIS_SPANS; k++) {
					int a = ml.spans[k].start;
					int b = a + ml.spans[k].len;
					if (a < starts[s]) a = starts[s];
					if (b > ends[s]) b = ends[s];
					if (b <= a) continue;
					v->sp[v->nsp].s = (uint16_t)(a - starts[s]);
					v->sp[v->nsp].l = (uint16_t)(b - a);
					v->sp[v->nsp].k = ml.spans[k].kind;
					v->nsp++;
				}

				nvlines++;
			}

			if (y + f->h > thr) {
#if READ_PROFILE
				uint32_t d0 = rp_cyc();
				draw_sub(&ml, starts[s], ends[s], MARGIN, y, indent + extra);
				rp_draw += rp_cyc() - d0; rp_rows++;
#else
				draw_sub(&ml, starts[s], ends[s], MARGIN, y, indent + extra);
#endif
			}

			y += lh;

		}

		// An h1 gets a rule under it -- at this size a font change
		// alone is not enough to mark a chapter break.
		if (ml.kind == MD_HEADING && ml.level == 1 && y + 2 < view_h) {
			if (y >= thr) fill(MARGIN, y, view_w, 1, 1);
			y += 3;    // never gated -- layout must not diverge
		}

		// Stop at the first block that did not fit. The loop used to
		// carry on reading blocks that could not fit either -- y never
		// decreases, so nothing after a cut ever draws -- which was
		// harmless when layout was a one-shot. It is not harmless with
		// a cache: the snapshot taken at the top of each iteration was
		// overwritten by those later blocks, so the resume point ended
		// up PAST the cut block and everything between, and on the
		// next scroll a heading that should have appeared at the
		// bottom simply never got laid out. The harness stopped here
		// all along, which is why it did not see this.
		if (cut) break;

	}

	// Where the next layout resumes. If a block was cut short, just
	// before it -- it is re-laid-out then, since more of it may fit.
	// Otherwise after the last block, which is where we are.
	if (cut) {
		end_st = snap_st;
		end_off = snap_off;
		end_line = snap_line;
		end_y = snap_y;
		cache_nv = snap_nv;
		cache_ns = snap_ns;
		cache_nl = snap_nl;
	} else {
		end_st = *st;
		end_off = rd_tell();
		end_line = cur_line;
		end_y = y;
		cache_nv = nvlines;
		cache_ns = nsteps;
		cache_nl = nvlinks;
	}
	cache_valid = true;

}

static void draw_body_from(int thr) {

#if READ_PROFILE
	{ uint32_t f0 = rp_cyc();
	if (thr < view_h) fill(0, thr, view_w + MARGIN, view_h - thr, 0);
	rp_fill += rp_cyc() - f0; }
#else
	if (thr < view_h) fill(0, thr, view_w + MARGIN, view_h - thr, 0);
#endif

	nvlinks = 0;
	nvlines = 0;
	nsteps = 0;

	if (fh < 0) return;

	md_state_t st;
#if READ_PROFILE
	{ uint32_t s0 = rp_cyc();
	seek_line(top_line, &st);
	rp_seek += rp_cyc() - s0; }
#else
	seek_line(top_line, &st);
#endif

	top_off = rd_tell();

	md_state_t st_top = st;
	layout_run(&st_top, top_line, MARGIN, top_sub, thr);

	bottom_off = rd_tell();
	if (bottom_off < top_off) bottom_off = top_off;

	draw_selection();

	// Keep the highlighted link in range after a redraw.
	if (sel_link >= nvlinks) sel_link = nvlinks ? 0 : -1;

	// The focused link is boxed rather than merely underlined --
	// underline already means "this is a link", so it cannot also
	// mean "this is the one selected".
	if (sel_link >= 0) {
		z_clip_t c;
		z_win_content_rect(&win, &c);
		vislink_t *v = &vlinks[sel_link];
		z_fb_hw_box(c.x0 + v->x - 1, c.y0 + v->y - 1,
			c.x0 + v->x + v->w, c.y0 + v->y + v->h, 1, &c);
	}

}

// -- deferred body redraw --
//
// Dragging the scrollbar produces a pointer sample every few
// milliseconds, and redrawing the document for each one meant a full
// re-parse and re-render per sample -- visibly a flicker, and most of
// it wasted on positions the reader passed straight through.
//
// So a drag only moves the THUMB, which is cheap and gives immediate
// feedback, and marks the body dirty with a deadline. The body is
// redrawn once the pointer has been still for SETTLE_MS, or
// immediately on release. The reader sees the thumb track their hand
// and the page arrive when they stop.
#define SETTLE_MS   140
#define SETTLE_TICKS ((SETTLE_MS * Z_TICK_HZ) / 1000u)

static bool body_dirty;
static uint32_t body_deadline;

// Set while an Open is in progress, between the dialog closing and
// the new document being rendered.
//
// Dismissing the dialog makes wm repair the region it covered, which
// asks this window to redraw -- and zdialog services that redraw
// itself, before returning the chosen path (see dlg_run()). So the
// natural order is: draw the OLD document to fill the hole, return
// the path, load the new file, draw the NEW document. Two full
// renders, the first of which is about to be thrown away, and
// visibly so on a document of any size.
//
// With this set, that first redraw clears and acks but does not
// render the body. wm gets its ack on time, the area is blank for
// the instant it takes to load, and the text is drawn exactly once.
static bool loading;

// What to say while it loads. Filled in before open_path() is called,
// because that call does not return for as long as the document takes
// to open and nothing else can run in this app meanwhile.
static char loading_msg[80];

// Clears the body and says what is happening.
//
// Without this the window kept showing the PREVIOUS document for the
// whole of the load -- several seconds on a 60KB file -- which reads
// as the app having ignored the click. The old text being both stale
// and convincing is what makes it worse than a blank area: there is
// nothing to distinguish it from the document that was asked for.
//
// Deliberately not a busy cursor: this blocks the app, not the
// system, and the cursor belongs to the window manager.
static void draw_loading(void) {

	z_clip_t c;
	z_win_content_rect(&win, &c);

	fill(0, 0, view_w + MARGIN, view_h, 0);
	drawn_valid = false;
	cache_valid = false;

	if (loading_msg[0])
		z_fb_draw_text((int)c.x0 + MARGIN, (int)c.y0 + MARGIN,
			loading_msg, 1, BODY_FONT, &c);

}

// "Loading <name> ..." with the leading directories dropped -- the
// full path is long enough to run off the window, and the file name
// is the part being waited for.
static void set_loading_msg(const char *p) {

	int last = -1;
	for (int i = 0; p[i]; i++) if (p[i] == '/') last = i;

	const char *base = p + last + 1;

	int n = 0;
	const char *pre = "Loading ";
	while (*pre && n < (int)sizeof(loading_msg) - 6) loading_msg[n++] = *pre++;
	while (*base && n < (int)sizeof(loading_msg) - 5) loading_msg[n++] = *base++;
	loading_msg[n++] = ' ';
	loading_msg[n++] = '.';
	loading_msg[n++] = '.';
	loading_msg[n++] = '.';
	loading_msg[n] = 0;

}

static void defer_body(void) {
	body_dirty = true;
	body_deadline = z_uptime_ticks() + SETTLE_TICKS;
}

static void update_scrollbar(void) {

	// Page size is a real screen's worth of BYTES, measured from the
	// last render -- not a guess, and not 1.
	//
	// It was 1, which broke clicking in the trough: z_scrollbar_mouse()
	// pages by (page - 1), so a click below the thumb moved the value
	// by zero bytes and landed on the same block -- no scroll at all.
	// Clicking above still worked, because the clamp at 0 happened to
	// produce a different position. An asymmetry with one cause.
	//
	// It also makes the thumb the right SIZE: with page = 1 the thumb
	// was always minimum-height regardless of how much of the
	// document fit on screen.
	int32_t span = (bottom_off > top_off) ? (int32_t)(bottom_off - top_off) : 1;

	// Measured in BYTES, not lines.
	//
	// The file size is known the instant the file is opened; the line
	// count is not known until the whole document has been read,
	// which is precisely what lazy indexing exists to avoid. A
	// byte-based scrollbar is therefore correct and complete from the
	// first frame, where a line-based one would have to either lie
	// about the length or read everything to avoid lying.
	//
	// It is also a slightly better proportion indicator: a line of
	// code and a wrapped paragraph occupy very different amounts of
	// screen, and bytes track that more closely than line numbers do.
	z_scrollbar_set_range(&sbar, fsize ? (int32_t)fsize : 1, span);
	z_scrollbar_set_value(&sbar, (int32_t)top_off);
	z_scrollbar_draw(&sbar, true);


	// The table now describes what is on the glass -- set once,
	// after the layout completes, not per block inside the loop.
	drawn_valid = true;

}

// A full repaint, including the window clear. Only for a Z_WM_REDRAW
// or a resize -- anything that may have left the frame or the area
// outside the body dirty.
static void draw_body(void) {

	draw_body_from(0);

}

static void repaint(void) {

	z_win_clear(&win);

	draw_body();
	update_scrollbar();

	body_dirty = false;

}

// Redraws just the document area.
//
// draw_body() fills its own region before drawing, so the window
// clear in repaint() adds nothing here except a visible blank frame
// -- which at scroll rates is exactly the flash it looks like.

static int scroll_down(int n);
static void repaint_body(void);

// -- accelerated forward scroll --
//
// Moves the pixels that survive a scroll instead of redrawing them:
// one VRAM-to-VRAM blit and one strip of new rows, in place of a full
// repaint. Measured, a repaint is ~13.4M cycles on this machine, of
// which drawing is ~7.8M; this removes almost all of that.
//
// Everything here rests on the step table, which says where each
// scroll step BEGINS in the current layout. After k steps the new top
// is old step k, so the pixels must move by step_y[k] - MARGIN.
// Derived from the layout already in hand, with no second layout pass
// -- the first attempt at this measured the shift by laying the new
// screen out twice, and a layout pass costs more than the blit saves.
//
// Preconditions worth stating: a selection highlight or a focused-link
// box is an OVERLAY drawn on top of content, which the blit would
// translate with nothing erasing the original -- the same ghost class
// as the caret in sw/apps/text. Both force the full repaint.
static void scroll_forward(int n) {

	static int16_t  old_step_y[VIS_STEPS];
	static uint32_t old_step_ln[VIS_STEPS];
	static uint8_t  old_step_sub[VIS_STEPS];
	static uint32_t old_step_off[VIS_STEPS];
	int old_nsteps = 0;

#if READ_PROFILE
	uint32_t t_scroll0 = rp_cyc();
#endif

	int fast = drawn_valid && !sel_on && sel_link < 0 && n > 0;

	if (fast) {
		old_nsteps = nsteps;
		if (old_nsteps > VIS_STEPS) old_nsteps = VIS_STEPS;
		for (int i = 0; i < old_nsteps; i++) {
			old_step_y[i] = step_y[i];
			old_step_ln[i] = step_ln[i];
			old_step_sub[i] = step_sub[i];
			old_step_off[i] = step_off[i];
		}
	}

	int k;

	if (fast && n < old_nsteps) {

		// The step table ALREADY says where n steps lands -- it was
		// built by walking these very blocks during the last layout.
		// scroll_down() would re-read and re-parse them to reach the
		// same answer, which on hardware cost more than the drawing
		// this function was written to avoid.
		//
		// Equivalent by construction, and not merely by assumption:
		// the old code called scroll_down() and then CHECKED the
		// result against this table, and that check passed 24,864
		// times across five documents in `make render MODE=seam`
		// before being replaced by the table itself.
		//
		// No clamping needed either: n < old_nsteps means the target
		// is a step that is on screen right now, so it exists.
		sel_clear();
		top_line = old_step_ln[n];
		top_sub  = old_step_sub[n];
		k = n;

	} else {

		k = scroll_down(n);

		if (!fast || k <= 0 || k >= old_nsteps) {
			repaint_body();
			return;
		}

		// Off the table's end: the step count and the layout can
		// legitimately disagree past the bottom of the screen, where
		// a block was cut short by `y + f->h > view_h`.
		if (old_step_ln[k] != top_line || old_step_sub[k] != top_sub) {
			repaint_body();
			return;
		}
	}

	if (k <= 0) {
		repaint_body();
		return;
	}

	int shift = (int)old_step_y[k] - MARGIN;

	if (shift <= 0 || shift >= view_h - MARGIN) {
		repaint_body();
		return;
	}

	{
		z_clip_t c;
		z_win_content_rect(&win, &c);

		// The body band only, below the top margin: that margin is
		// blank and must stay blank, and blitting from c.y0 would
		// drag the first row of text up into it.
		z_fb_hw_scroll((int)c.x0 + MARGIN, (int)c.y0 + MARGIN,
			view_w, view_h - MARGIN, -shift);
	}

	if (cache_valid && k < cache_ns) {

		// -- the layout cache path --
		//
		// Every step being dropped, and the new top, lie within the
		// blocks the last layout completed. So: shift what survived,
		// and resume the parser from where it stopped. Nothing above
		// the resume point is read again.
		int drop_y = (int)old_step_y[k];
		int w;

		w = 0;
		for (int i = 0; i < cache_nv; i++) {
			if (vlines[i].y < drop_y) continue;
			if (w != i) vlines[w] = vlines[i];
			vlines[w].y = (int16_t)(vlines[w].y - shift);
			w++;
		}
		nvlines = w;

		w = 0;
		for (int i = 0; i < cache_nl; i++) {
			if (vlinks[i].y < drop_y) continue;
			if (w != i) vlinks[w] = vlinks[i];
			vlinks[w].y = (int16_t)(vlinks[w].y - shift);
			w++;
		}
		nvlinks = w;

		w = 0;
		for (int i = k; i < cache_ns; i++) {
			step_y[w]   = (int16_t)(old_step_y[i] - shift);
			step_ln[w]  = old_step_ln[i];
			step_sub[w] = old_step_sub[i];
			step_off[w] = old_step_off[i];
			w++;
		}
		nsteps = w;

		top_off = old_step_off[k];

		// Resume. The file is repositioned to the cached offset --
		// usually still inside the read buffer -- and the block
		// loop continues from the cached parser state at the cached
		// y, now shifted. It draws only rows reaching into the
		// strip, exactly as the full pass does.
		// Clear the strip the blit exposed. draw_body_from() does
		// this before its layout_run(); calling layout_run() directly
		// must do it too, or the pre-blit bottom rows stay on the
		// glass underneath the rows drawn into the strip -- which
		// looked like rows drawn on top of each other. The seam
		// harness now models the glass honestly (the strip holds the
		// old rows until something clears it) so this cannot be
		// forgotten silently again.
		fill(0, view_h - shift, view_w + MARGIN, shift, 0);

		md_state_t st = end_st;
		rd_seek(end_off);
		layout_run(&st, end_line, end_y - shift, 0, view_h - shift);

		bottom_off = rd_tell();
		drawn_valid = true;

	} else {

		// One layout pass from the top, drawing only what the blit
		// exposed. Taken when the new top lies inside the block that
		// was cut short -- there is nothing cached past it to resume
		// from -- or when no cache exists yet.
		draw_body_from(view_h - shift);
	}

	// VERIFY rather than trust. Every step whose whole band sits
	// above the strip came from the blit, so it must be exactly
	// `shift` pixels above where it was. A step reaching INTO the
	// strip was redrawn regardless and is not required to match --
	// demanding that rejects almost every scroll, since the block at
	// the seam is precisely the one whose surroundings changed.
	for (int i = 0; i < nsteps && k + i < old_nsteps; i++) {
		int band_end = (i + 1 < nsteps) ? (int)step_y[i + 1] : view_h;
		if (band_end > view_h - shift) break;
		if (step_ln[i] != old_step_ln[k + i] ||
			step_sub[i] != old_step_sub[k + i] ||
			(int)step_y[i] != (int)old_step_y[k + i] - shift) {
			draw_body_from(0);
			break;
		}
	}

	update_scrollbar();
	body_dirty = false;
	drawn_valid = true;

#if READ_PROFILE
	// The fast path does not go through repaint_body(), which is
	// where the report used to be armed -- so without this, turning
	// the profile on measured only the scrolls that fell back.
	rp_total += rp_cyc() - t_scroll0;
	rp_pending = 1;
#endif

}

static void repaint_body(void) {

#if READ_PROFILE
	uint32_t t0 = rp_cyc();
#endif

	draw_body();
	update_scrollbar();

	body_dirty = false;

#if READ_PROFILE
	rp_total += rp_cyc() - t0;
	rp_pending = 1;
#endif

}

// ---------------------------------------------------------------
// scrolling
// ---------------------------------------------------------------

// Display lines a parsed block occupies. Zero for MD_SKIP, which
// draws nothing at all and must therefore cost no scroll step either
// -- see draw_body(), which continues past it without consuming one.
static int subs_of(const md_line_t *ml) {

	uint16_t starts[MAX_SUB], ends[MAX_SUB];

	if (ml->kind == MD_SKIP) return 0;
	if (ml->kind == MD_BLANK || ml->kind == MD_RULE) return 1;

	return wrap_line(ml, view_w, starts, ends);

}

// How many display lines the block at `ln` occupies, and how many
// source lines it spans. One seek; for single queries only.
static int block_subs(uint32_t ln, uint32_t *used) {

	if (used) *used = 1;

	if (fh < 0) return 1;

	md_state_t st;
	seek_line(ln, &st);

	static md_line_t ml;

	uint32_t u = read_block(&st, &ml);

	if (!u) { if (used) *used = 0; return 1; }

	if (used) *used = u;

	return subs_of(&ml);

}

// Scrolls down `n` display lines in ONE forward pass.
//
// This used to call block_subs() per step -- and block_subs() seeks,
// which means a file seek plus a replay from the nearest checkpoint
// for EVERY display line moved. A page scroll did that thirty times
// over, re-parsing the same blocks repeatedly; on the real book that
// is what made PageDown feel slow.
//
// Reading forward is naturally sequential: the stream is already
// positioned after each block, so the next one costs a read and
// nothing else.
// Returns the number of display lines actually advanced -- less than
// `n` when the document ends first. scroll_forward() needs the real
// figure to index the step table; every other caller ignores it.
static int scroll_down(int n) {

	// A selection is anchored to SCREEN rows (see visline_t), so
	// scrolling would leave it highlighting whatever moved into
	// those rows -- text the reader never selected. Dropping it is
	// the honest answer; keeping it would need source-anchored
	// positions, which is a document model this app does not have.
	sel_clear();

	if (n <= 0 || fh < 0) return 0;

	md_state_t st;
	seek_line(top_line, &st);

	static md_line_t ml;

	uint32_t line = top_line;
	uint32_t sub = top_sub;

	uint32_t used = read_block(&st, &ml);
	if (!used) return 0;

	int subs = subs_of(&ml);
	int rem = n;

	while (rem > 0) {

		// A step within the current block is free.
		if (subs > 0 && (int)sub + 1 < subs) { sub++; rem--; continue; }

		// Otherwise move to the next block. read_block() continues
		// from where the last one stopped -- no seek.
		md_state_t nst = st;
		md_line_t *nml = &ml;
		uint32_t nline = line + used;

		static md_line_t tmp;
		uint32_t nused = read_block(&nst, &tmp);

		if (!nused) break;			// end of file: stay where we are

		st = nst;
		*nml = tmp;
		line = nline;
		used = nused;
		sub = 0;
		subs = subs_of(&ml);

		// Entering a block that draws nothing costs no step.
		if (subs > 0) rem--;

	}

	top_line = line;
	top_sub = sub;

	return n - rem;


}

// Blocks between the nearest checkpoint at or before `target` and
// `target` itself, with their display-line counts. Returns how many
// were recorded.
//
// One seek for the whole span, which is what makes scrolling UP a
// page cheap -- the alternative is prev_block() per display line, and
// prev_block() seeks.
#define BACK_MAX 96

static int collect_before(uint32_t target, uint32_t *lines, int *subs,
	int max) {

	if (!target || fh < 0) return 0;

	extend_index(target, 0);

	int e = 0;

	for (int i = 0; i < idx_n; i++)
		if (idx[i].line < target) e = i; else break;

	rd_seek(idx[e].off);

	md_state_t st = idx[e].st;
	static md_line_t ml;

	uint32_t ln = idx[e].line;
	int n = 0;

	while (ln < target) {

		uint32_t at = ln;
		uint32_t used = read_block(&st, &ml);
		if (!used) break;

		int c = subs_of(&ml);

		if (c > 0) {
			if (n < max) {
				lines[n] = at;
				subs[n] = c;
				n++;
			} else {
				// Older entries fall off the front; the caller loops
				// and re-collects from an earlier checkpoint if it
				// still needs to go further back.
				for (int i = 1; i < max; i++) {
					lines[i - 1] = lines[i];
					subs[i - 1] = subs[i];
				}
				lines[max - 1] = at;
				subs[max - 1] = c;
			}
		}

		ln += used;

	}

	return n;

}

static void scroll_up(int n) {

	sel_clear();		// see scroll_down()

	if (n <= 0 || fh < 0) return;

	static uint32_t lines[BACK_MAX];
	static int subs[BACK_MAX];

	while (n > 0) {

		// Steps remaining inside the current block are free.
		if (top_sub) {
			uint32_t take = (uint32_t)n < top_sub ? (uint32_t)n : top_sub;
			top_sub -= take;
			n -= (int)take;
			continue;
		}

		if (!top_line) return;

		int cnt = collect_before(top_line, lines, subs, BACK_MAX);
		if (!cnt) return;

		// Walk back through the collected blocks.
		int i = cnt - 1;

		while (n > 0 && i >= 0) {

			top_line = lines[i];
			top_sub = (uint32_t)(subs[i] - 1);
			n--;						// entering the block is one step

			if (n <= 0) break;

			uint32_t inside = top_sub;
			uint32_t take = (uint32_t)n < inside ? (uint32_t)n : inside;

			top_sub -= take;
			n -= (int)take;

			i--;

		}

		// Still more to go: loop, which re-collects from an earlier
		// checkpoint now that top_line has moved back.
		if (n > 0 && top_line == lines[0] && cnt < BACK_MAX) return;

	}

}

// ---------------------------------------------------------------
// links
// ---------------------------------------------------------------

// GitHub's heading slug: lowercase, spaces to hyphens, everything
// else that isn't alphanumeric or hyphen dropped.
static void slugify(const char *s, char *out, int cap) {

	int n = 0;

	for (int i = 0; s[i] && n < cap - 1; i++) {

		char c = s[i];

		if (c >= 'A' && c <= 'Z') c = (char)(c + 32);

		if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-')
			out[n++] = c;
		else if (c == ' ')
			out[n++] = '-';

	}

	out[n] = 0;

}

// Finds the heading whose slug matches `anchor` and moves there.
// Returns false if there is no such heading.
//
// A full scan of the document. It only happens on a click, and the
// alternative -- an anchor table built at open time -- would cost
// memory proportional to the number of headings, which is exactly
// what this design is avoiding.
static bool goto_anchor(const char *anchor) {

	if (fh < 0) return false;

	rd_seek(0);

	md_state_t st;
	md_state_init(&st);

	static md_line_t ml;
	char slug[128];

	uint32_t ln = 0;

	for (;;) {

		uint32_t at = ln;
		uint32_t used = read_block(&st, &ml);
		if (!used) break;

		if (ml.kind == MD_HEADING) {
			slugify(ml.text, slug, sizeof(slug));
			if (!strcmp(slug, anchor)) {
				top_line = at;
				top_sub = 0;
				return true;
			}
		}

		ln += used;

	}

	return false;

}

static bool open_path(const char *p);

static void push_history(void) {

	if (hist_n >= HIST_MAX) {
		// Drop the oldest rather than refusing to navigate.
		for (int i = 1; i < HIST_MAX; i++) hist[i - 1] = hist[i];
		hist_n--;
	}

	int i = 0;
	for (; path[i] && i < Z_FLIST_PATH_MAX - 1; i++)
		hist[hist_n].path[i] = path[i];
	hist[hist_n].path[i] = 0;

	hist[hist_n].line = top_line;
	hist[hist_n].sub = top_sub;
	hist_n++;

}

static void go_back(void) {

	if (!hist_n) return;

	hist_n--;

	char p[Z_FLIST_PATH_MAX];
	int i = 0;
	for (; hist[hist_n].path[i] && i < Z_FLIST_PATH_MAX - 1; i++)
		p[i] = hist[hist_n].path[i];
	p[i] = 0;

	uint32_t l = hist[hist_n].line, s = hist[hist_n].sub;

	// Same file: just move, don't reopen and re-index.
	if (!strcmp(p, path)) {
		top_line = l;
		top_sub = s;
	} else if (open_path(p)) {
		top_line = l;
		top_sub = s;
	}

	sel_link = -1;
	repaint();

}

static void follow(const char *target) {

	if (!target || !target[0]) return;

	// In-document anchor.
	if (target[0] == '#') {
		push_history();
		if (!goto_anchor(target + 1)) {
			hist_n--;		// nothing moved, so nothing to go back to
			z_dialog_confirm(&dlg_ctx, "Not found",
				"No heading matches\nthat link.", Z_DIALOG_OK_CANCEL);
		}
		sel_link = -1;
		repaint();
		return;
	}

	// Anything with a scheme is not ours to open.
	if (!strncmp(target, "http://", 7) || !strncmp(target, "https://", 8)) {
		z_dialog_confirm(&dlg_ctx, "External link",
			"This link points off\nthis machine.", Z_DIALOG_OK_CANCEL);
		return;
	}

	// A path, possibly with its own anchor, possibly relative.
	char p[Z_FLIST_PATH_MAX];
	char anchor[128];
	int n = 0, a = 0;
	bool in_anchor = false;

	if (target[0] != '/') {
		// Relative to the directory the current file is in.
		int last = 0;
		for (int i = 0; path[i]; i++) if (path[i] == '/') last = i;
		for (int i = 0; i <= last && n < Z_FLIST_PATH_MAX - 1; i++)
			p[n++] = path[i];
	}

	for (int i = 0; target[i]; i++) {
		if (target[i] == '#') { in_anchor = true; continue; }
		if (in_anchor) { if (a < (int)sizeof(anchor) - 1) anchor[a++] = target[i]; }
		else if (n < Z_FLIST_PATH_MAX - 1) p[n++] = target[i];
	}

	p[n] = 0;
	anchor[a] = 0;

	// A link to a non-markdown file hands off to whatever opens it,
	// the same way the file browser does -- one table decides that
	// for the whole system (sw/common/ztype.h).
	const char *app = z_ftype_app_for(p);

	if (app && strcmp(app, "read")) {
		z_launch_arg_set(p);
		z_proc_run(app);
		return;
	}

	push_history();

	if (!open_path(p)) {
		hist_n--;
		z_dialog_confirm(&dlg_ctx, "Can't open",
			"That file could not\nbe read.", Z_DIALOG_OK_CANCEL);
		return;
	}

	if (anchor[0]) goto_anchor(anchor);

	sel_link = -1;
	repaint();

}

// ---------------------------------------------------------------
// files
// ---------------------------------------------------------------

static void update_title(void) {

	char t[32];
	int n = 0;

	const char *base = path[0] ? path : "read";

	for (const char *p = path; *p; p++)
		if (*p == '/') base = p + 1;

	for (const char *p = base; *p && n < (int)sizeof(t) - 1; p++) t[n++] = *p;

	t[n] = 0;

	z_win_set_title(&win, t);

}

static bool open_path(const char *p) {

	// Close the current document BEFORE opening the next one.
	//
	// This used to open first and close after, so that a failed open
	// left the reader with the document it already had. Sound in
	// principle, but it means holding TWO handles across the open --
	// and there are only Z_FS_MAX_OPEN (4) in the whole system,
	// shared by every process. With the file dialog holding one or
	// two of its own, opening a second document intermittently ran
	// out and reported "Can't open" for a file that was perfectly
	// readable.
	//
	// The safety it was buying is kept by reopening the previous
	// path when the new one fails, which costs nothing in the normal
	// case and one extra open in the rare one.
	char prev[Z_FLIST_PATH_MAX];
	bool had_prev = (fh >= 0);

	if (had_prev) {
		int q = 0;
		for (; path[q] && q < Z_FLIST_PATH_MAX - 1; q++) prev[q] = path[q];
		prev[q] = 0;
		fs_close_handle(fh);
		fh = -1;
	}

	int nf = fs_open_read(p);

	if (nf < 0) {

		// Put the previous document back. The index is still valid --
		// same file, same contents -- but the buffered reader state
		// refers to a handle that no longer exists.
		if (had_prev) {
			fh = fs_open_read(prev);
			rbuf_len = 0;
			rbuf_pos = 0;
			rbuf_base = 0;
			peek_valid = false;
			pos_valid = false;
			if (fh >= 0) rd_seek(0);
		}

		return false;
	}

	fh = nf;

	// The buffered reader still holds bytes from the PREVIOUS
	// document, and rbuf_base still claims they belong at that
	// offset. rd_seek() serves a target from the buffer without
	// touching the file when the offset falls inside it -- which
	// offset 0 of a freshly opened document always does -- so
	// without this the new file renders as the old one's contents.
	//
	// Harmless before rd_seek() had that fast path, because every
	// seek discarded the buffer. Adding the optimisation moved the
	// obligation here.
	rbuf_len = 0;
	rbuf_pos = 0;
	rbuf_base = 0;
	peek_valid = false;
	pos_valid = false;

	fsize = (uint32_t)fs_size((char *)p);

	int i = 0;
	for (; p[i] && i < Z_FLIST_PATH_MAX - 1; i++) path[i] = p[i];
	path[i] = 0;

	index_init();

	drawn_valid = false;
	cache_valid = false;

	top_line = 0;
	top_sub = 0;
	top_off = 0;
	sel_link = -1;

	// Remember the directory for the next Open dialog.
	int last = 0;
	for (int k = 0; path[k]; k++) if (path[k] == '/') last = k;
	if (last) {
		for (i = 0; i < last && i < Z_FLIST_PATH_MAX - 1; i++)
			last_dir[i] = path[i];
		last_dir[i] = 0;
	} else {
		last_dir[0] = '/'; last_dir[1] = 0;
	}

	update_title();

	return true;

}

static void do_open(void) {

	char p[Z_FLIST_PATH_MAX];

	// Set across the dialog, so the redraw it triggers on the way out
	// doesn't render a document that is about to be replaced. See
	// `loading`.
	loading = true;

	if (!z_dialog_open(&dlg_ctx, last_dir, p, sizeof(p))) {
		// Cancelled: nothing is being loaded, so the window still
		// needs the content it already had put back.
		loading = false;
		repaint();
		return;
	}

	// Say so BEFORE the blocking open, not after: open_path() does
	// not return until the document is indexed, and nothing in this
	// app runs in between.
	set_loading_msg(p);
	draw_loading();

	bool ok = open_path(p);

	loading = false;
	loading_msg[0] = 0;

	if (!ok)
		z_dialog_confirm(&dlg_ctx, "Can't open",
			"That file could not\nbe read.", Z_DIALOG_OK_CANCEL);

	hist_n = 0;
	repaint();

}

// ---------------------------------------------------------------
// layout and input
// ---------------------------------------------------------------

static void layout(void) {

	int cw = z_win_content_w(&win);
	int ch = z_win_content_h(&win);

	view_w = cw - Z_SB_THICK - MARGIN - 1;
	if (view_w < 8) view_w = 8;

	view_h = ch;

	sb_len = ch - Z_WIN_GRIP_INSET;
	if (sb_len < 0) sb_len = 0;

	z_scrollbar_set_geom(&sbar, cw - Z_SB_THICK, 0, sb_len);

}

static void handle_key(uint32_t keysym, uint8_t mods) {

	int page = view_h / (BODY_FONT->h + 1);
	if (page < 1) page = 1;

	switch (keysym) {

		case Z_KEY_DOWN:    scroll_forward(1); return;
		case Z_KEY_UP:      scroll_up(1); repaint_body(); return;
		// Space is page down, the convention every pager shares --
		// it is the key a hand rests on while reading. Shift+Space
		// goes back up, for the same reason.
		case ' ':
			if (mods & Z_KBD_MOD_SHIFT) scroll_up(page - 1);
			else scroll_down(page - 1);
			repaint_body();
			return;

		case Z_KEY_PAGEDOWN: scroll_down(page - 1); repaint_body(); return;
		case Z_KEY_PAGEUP:   scroll_up(page - 1); repaint_body(); return;

		case Z_KEY_HOME:
			top_line = 0; top_sub = 0; sel_link = -1; repaint_body(); return;

		case Z_KEY_END:
			// The only operation that genuinely needs the whole
			// document read, since the end is not knowable otherwise.
			// The scan builds the index, so it is paid once.
			extend_index(0xFFFFFFFFu, 0xFFFFFFFFu);
			if (eof_seen && eof_line > 1) {
				top_line = eof_line - 1;
				top_sub = 0;
				scroll_up(page - 1);
			}
			sel_link = -1;
			repaint_body();
			return;

		case '\t':
			// Cycle the links on screen. Only the visible ones --
			// tabbing to something off screen would move the view
			// under the reader without being asked.
			if (!nvlinks) return;
			if ((mods & Z_KBD_MOD_SHIFT))
				sel_link = (sel_link <= 0) ? nvlinks - 1 : sel_link - 1;
			else
				sel_link = (sel_link + 1) % nvlinks;
			repaint_body();
			return;

		case 0x0d:
			if (sel_link >= 0) follow(vlinks[sel_link].target);
			return;

		case 0x7f:		// Backspace -- back, as in every reader
			go_back();
			return;

		default: break;

	}

	if (keysym == 0x1b) {						// Escape
		sel_clear();
		repaint_body();
		return;
	}

	if (mods & Z_KBD_MOD_CTRL) {
		if (keysym == 0x03) sel_copy();			// Ctrl+C
		else if (keysym == 0x0f) do_open();		// Ctrl+O
		else if (keysym == 0x11) {				// Ctrl+Q
			if (fh >= 0) fs_close_handle(fh);
			z_win_destroy(&win);
			exit(0);
		}
	}

}

static uint8_t last_buttons;

static void handle_mouse(uint32_t packed) {

	int cx, cy;
	bool inside = z_win_mouse_content_xy(&win, packed, &cx, &cy);

	uint8_t buttons = (uint8_t)Z_WM_UNPACK_MOUSE_BUTTONS(packed);

	if (!inside && !z_scrollbar_has_pointer(&sbar, cx, cy)) {
		last_buttons = buttons;
		return;
	}

	if (z_scrollbar_has_pointer(&sbar, cx, cy)) {

		bool was_down = (last_buttons & Z_MOUSE_BTN_LEFT) != 0;
		bool now_down = (buttons & Z_MOUSE_BTN_LEFT) != 0;

		if (z_scrollbar_mouse(&sbar, cx, cy, buttons)) {

			// The scrollbar is in bytes; find the block that starts
			// at or before that offset. Extending the index to reach
			// it is the one place a jump into unread territory pays
			// for a scan -- and that scan builds the index, so the
			// same jump is instant afterwards.
			top_line = line_at_offset((uint32_t)sbar.value);
			top_sub = 0;
			sel_link = -1;
			sel_clear();		// see scroll_down()

			// The thumb has already moved (z_scrollbar_mouse drew
			// it). Defer the expensive half -- see defer_body().
			defer_body();

		}

		// Released: the reader has stopped, so show the page now
		// rather than waiting out the settle time.
		if (was_down && !now_down && body_dirty) repaint_body();

		last_buttons = buttons;
		return;

	}

	z_scrollbar_mouse(&sbar, cx, cy, buttons);

	// Right button copies, as a shortcut for Ctrl+C, on the press --
	// there is no drag gesture on it. Same as sw/apps/text and
	// sw/apps/term.
	if ((buttons & Z_MOUSE_BTN_RIGHT) && !(last_buttons & Z_MOUSE_BTN_RIGHT))
		sel_copy();

	bool was_down = (last_buttons & Z_MOUSE_BTN_LEFT) != 0;
	bool now_down = (buttons & Z_MOUSE_BTN_LEFT) != 0;

	last_buttons = buttons;

	int row, col;
	hit_cell(cx, cy, &row, &col);

	if (now_down && !was_down) {

		// Press. This might become either a click on a link or a
		// selection drag, and which one is not knowable yet -- so
		// anchor a selection here and decide on release. Following
		// the link now would make it impossible to select the text
		// OF a link, which is exactly the text most worth copying.
		sel_clear();

		sel_ar = sel_cr = row;
		sel_ac = sel_cc = col;
		sel_on = true;
		sel_dragging = true;
		sel_moved = false;

		return;

	}

	if (now_down && sel_dragging) {

		if (row == sel_cr && col == sel_cc) return;

		// Only the rows between the old and new ends changed
		// appearance. Redraw those from vlines[] and re-overlay --
		// no file access, no parse, no wrap, and no flash.
		int lo = sel_cr < row ? sel_cr : row;
		int hi = sel_cr > row ? sel_cr : row;

		if (sel_ar < lo) lo = sel_ar;
		if (sel_ar > hi) hi = sel_ar;

		sel_cr = row;
		sel_cc = col;
		sel_moved = true;

		for (int r = lo; r <= hi; r++) redraw_row(r);

		draw_selection();

		return;

	}

	if (was_down && !now_down) {

		sel_dragging = false;

		// A press that never moved is a click. Follow a link under
		// it if there is one; otherwise just drop the empty
		// selection.
		if (!sel_moved) {

			sel_clear();

			for (int i = 0; i < nvlinks; i++) {
				if (cx >= vlinks[i].x && cx < vlinks[i].x + vlinks[i].w &&
					cy >= vlinks[i].y && cy < vlinks[i].y + vlinks[i].h) {
					sel_link = i;
					follow(vlinks[i].target);
					return;
				}
			}

			redraw_row(row);

		}

	}

}

static void forward_msg(z_msg_t *msg, void *user) {

	(void)user;

	switch (msg->subject) {

		case Z_WM_REDRAW:

			if (msg->obj.type != Z_UINT32) break;
			if (z_win_redraw_id(msg->obj.val.uint32) != win.id) break;

			z_win_apply_redraw(&win, msg->obj.val.uint32);

			if (loading) {
				// A new document is about to replace this one --
				// clear and ack, but don't render what is being
				// thrown away. See `loading`. The message goes back
				// on so an expose during the load does not leave a
				// blank window with no explanation.
				z_win_clear(&win);
				draw_loading();
				drawn_valid = false;
				cache_valid = false;
			} else {
				repaint();
			}

			z_win_redraw_done(&win);

			break;

		case Z_WM_WINDOW_MOVED:
			z_win_parse_rect(&win, &msg->obj);
			break;

		case Z_WM_WINDOW_RESIZED:
			z_win_apply_resized(&win, &msg->obj);
			layout();
			break;

		default:
			break;

	}

}

int main(void) {

	printf("read: starting\n");

	/* Which glyph path this binary actually uses. ~5,300 cycles per
	 * glyph is what SOFTWARE rendering costs (roughly 40 pixels, each
	 * a read-modify-write across the bus); the hardware path is seven
	 * register writes. If this says OFF, the build lost
	 * -DZ_GFX_HW_BLIT -- which a command-line CFLAGS used to do
	 * silently -- and no amount of optimising anything else will
	 * matter. */
#ifdef Z_GFX_HW_BLIT
	printf("read: glyph blit HW\n");
#else
	printf("read: glyph blit SOFTWARE -- build lost -DZ_GFX_HW_BLIT\n");
#endif

	if (z_win_create_flags(&win, "read", WIN_W, WIN_H, -1, -1,
		Z_WIN_FLAG_CLOSE_ICON | Z_WIN_FLAG_CLOSE_KILLS_OWNER |
		Z_WIN_FLAG_RESIZABLE | Z_WIN_FLAG_MIN_IS_CREATE |
		Z_WIN_FLAG_OPEN_ICON) != Z_OK) {
		printf("read: failed to create window -- is wm running?\n");
		return 1;
	}

	z_scrollbar_init(&sbar, &win, Z_SB_VERT);

	dlg_ctx.parent = &win;
	dlg_ctx.on_msg = forward_msg;
	dlg_ctx.user = NULL;

	layout();

	{
		char arg[Z_FLIST_PATH_MAX];
		if (z_launch_arg_take(arg, sizeof(arg))) open_path(arg);
	}

	repaint();

	for (;;) {

		z_msg_t msg;

		/* Shallow stack here, unlike the draw path. No-op unless
		 * READ_PROFILE is set. */
		rp_report();

		while (z_msg_read(&msg) == Z_OK) {

			switch (msg.subject) {

				case Z_WM_KEY:

					if (msg.obj.type != Z_UINT32) break;
					if (!Z_WM_UNPACK_KEY_PRESSED(msg.obj.val.uint32)) break;

					handle_key(Z_WM_UNPACK_KEY_KEYSYM(msg.obj.val.uint32),
						(uint8_t)Z_WM_UNPACK_KEY_MODIFIERS(msg.obj.val.uint32));

					break;

				case Z_WM_MOUSE:

					if (msg.obj.type == Z_UINT32)
						handle_mouse(msg.obj.val.uint32);

					break;

				case Z_WM_TITLEBAR_ICON:

					if (msg.obj.type != Z_UINT32) break;
					if ((int)Z_WM_UNPACK_TBICON_ID(msg.obj.val.uint32) != win.id)
						break;
					if (Z_WM_UNPACK_TBICON_KIND(msg.obj.val.uint32) ==
						Z_WM_TBICON_OPEN) do_open();

					break;

				default:

					forward_msg(&msg, NULL);
					break;

			}

		}

		// Block until something arrives -- but no longer than the
		// deferred body redraw's deadline, or a drag that ends by the
		// pointer simply stopping would never repaint.
		if (body_dirty) {

			uint32_t now = z_uptime_ticks();

			if ((int32_t)(now - body_deadline) >= 0) {
				repaint_body();
				continue;
			}

			z_proc_wait(body_deadline - now);

		} else {

			z_proc_wait(0);

		}

	}

	return 0;

}
