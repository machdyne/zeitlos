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

#include "../../common/zpaths.h"
#include "../../common/zeitlos.h"
#include "../../common/zsoc.h"	// Z_TICK_HZ, for the scroll settle time
#include "../../common/zwm.h"
#include "../../common/zspeak.h"
#include "../../common/zsayall.h"	// Super+A -- docs/tts.md
#include "../../common/zwin.h"
#include "../../common/zgfx.h"
#include "../../common/zfont.h"
#include "../../common/zkbd.h"
#include "../../common/zwidget.h"
#include "../../common/zflist.h"
#include "../../common/zdialog.h"
#include "../../common/zfsapp.h"
#include "../../common/ztype.h"
#include "../../common/zutf8.h"		// docs/text_encoding.md
#include "md.h"

#define WIN_W   320
#define WIN_H   300

static z_win_t win;

#define MARGIN      3

// Body font and heading font. Both are resident in glyph memory (see
// docs/window_manager.md, "Fonts in glyph memory"), so both draw
// through the hardware blitter at the same speed.
//
// The body is 5x8, which fits the most on the screen, unless the
// titlebar's Aa asks for 6x12 (do_font()). Headings are 6x12 either way; at 6x12
// body they are told apart by the rules under them (layout_run()).
static const z_font_t *body_font = &z_font_5x8;
#define BODY_FONT   (body_font)
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

// -- finding --
//
// The scans that may read the whole document -- End, a link to a
// heading, a scrollbar drag or a launch offset into a part not read
// yet, going back to a place in another document -- used to run in one
// call, from the main loop. On the 85KB book that was seconds with the
// window dead: no redraw (wm waits for the ack), no Esc, keys queued.
// docs/responsive.md: the main loop never makes a call that can take
// longer than about 20 ms.
//
// So each is a FIND: a goal and a cursor, advanced a block at a time
// for about 20 ms a turn of the main loop (find_step()), with every
// message read between turns. The title says how far it has got, and
// Esc stops it, leaving the page where it was. A state machine rather
// than a coroutine: the scan was already a loop over blocks, and
// everything it keeps between blocks is the frontier (above) and, for
// a heading, one cursor -- nothing to abandon but a few numbers.
enum { FIND_NONE, FIND_END, FIND_OFF, FIND_LINE, FIND_ANCHOR };

static struct {
	int			kind;
	uint32_t	want;		// FIND_OFF: a byte offset. FIND_LINE: a source line.
	uint32_t	sub;		// FIND_LINE: the display line within it

	// FIND_ANCHOR: the heading's slug, and the search's own cursor. A
	// heading may lie behind the frontier, so the search walks from
	// the top of the document; where it passes the frontier it moves
	// the frontier with it, so a search that reads new ground builds
	// the index as it goes, exactly as any other find.
	char		slug[128];
	uint32_t	a_line, a_off;
	md_state_t	a_st;

	bool		hist;		// history was pushed for it: popped if it comes to nothing
	bool		say_missing;	// a link: say so if there is no such heading
	int			pct;		// 0-99, what the title says
} find;

static void find_start(int kind, uint32_t want, uint32_t sub);
static void find_end(bool landed);

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

	// A table row's vertical rules, in characters from x: the text
	// under them is blank border cells, so redraw_row() puts them back.
	uint8_t		nbar;
	uint8_t		bar[MD_TBL_COLS + 1];

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
static char last_dir[Z_FLIST_PATH_MAX] = Z_DIR_HOME;

static void forward_msg(z_msg_t *msg, void *user);
static void repaint(void);
static void rewrapped(void);

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

// A read failed (fs_read_chunk() < 0) since the last rd_seek().
//
// A failed read is NOT the end of the file, and it used to be taken
// for one: rd_raw() returned "no more lines", extend_index() then set
// eof_seen and an eof_line wherever the failure happened -- for good,
// so End and the scrollbar believed in a document that stopped there
// -- and rbuf_base had moved on by the buffer's length while fs's
// position had not, so every later read was off by that much. Now a
// failure sets this, read_block() returns 0 with it set, and every
// caller tells the two apart: nothing is recorded from a failed read
// (no checkpoint, no EOF, no position cache), and fs is put back where
// rbuf_base says.
static bool rd_err;

// Sticky: a read failed since the reader last asked for something. The
// title says "read error" while it is set (update_title()).
static bool io_err;

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
#ifndef READ_SCROLL_DEBUG
#define READ_SCROLL_DEBUG 0
#endif

#if READ_PROFILE || READ_SCROLL_DEBUG
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

/* Build with -DREAD_SCROLL_DEBUG=1 to print one console line per
 * fused scroll batch and per scrollbar drag: impulses in, lines
 * applied, blits taken and refused, seeks, repaints, and how far the
 * thumb's page figure wandered while the drag held it frozen. Off by
 * default; compiles out entirely.
 *
 * This exists because "the scroll does not follow the hand" has three
 * quite different possible answers -- the paint is slow, the impulses
 * queue, or the fast path silently isn't taken -- and each is fixed
 * in a different place. Same clock caveat as everywhere: 732Hz ticks,
 * so milliseconds, not cycles. */
#if READ_SCROLL_DEBUG

static uint32_t sd_keys;	// scroll impulses received
static uint32_t sd_lines;	// display lines applied by flushes
static uint32_t sd_flush;	// batches painted
static uint32_t sd_blit;	// fast-path blits done
static uint32_t sd_noblit;	// blits the visible region refused
static uint32_t sd_seeks;	// line_at_offset() resolutions
static uint32_t sd_repaints;	// repaint_body() calls

// Per-drag: how many samples moved the thumb, and the range the page
// figure WOULD have taken without the freeze (see update_scrollbar()).
static uint32_t sd_samples;
static uint32_t sd_span_min, sd_span_max;
static uint32_t sd_defer_t;	// tick of the last defer_body()
static int sd_drag_on;

static void sd_report_flush(void) {

	static char line[128];
	int n = 0;

	rp_str(line, &n, "read: flush #");
	rp_u32(line, &n, sd_flush);
	rp_str(line, &n, " keys ");
	rp_u32(line, &n, sd_keys);
	rp_str(line, &n, " lines ");
	rp_u32(line, &n, sd_lines);
	rp_str(line, &n, " blit ");
	rp_u32(line, &n, sd_blit);
	rp_str(line, &n, " noblit ");
	rp_u32(line, &n, sd_noblit);
	rp_str(line, &n, " seeks ");
	rp_u32(line, &n, sd_seeks);
	rp_str(line, &n, " rep ");
	rp_u32(line, &n, sd_repaints);
	line[n] = 0;

	puts(line);

}

// Called with the drag's final repaint just done: samples in, seeks
// and repaints spent, the span the page figure ranged over, and the
// time from the last thumb move to the paint being on the glass. A
// trough click reports too, with a 0..0 span -- it never arms the
// range tracker, which is how you tell it from a drag.
static void sd_report_drag(void) {

	if (!sd_samples && !sd_drag_on) return;

	static char line[160];
	int n = 0;

	uint32_t ms = (z_uptime_ticks() - sd_defer_t) * 1000u / Z_TICK_HZ;

	rp_str(line, &n, "read: drag ");
	rp_u32(line, &n, sd_samples);
	rp_str(line, &n, " samples, span ");
	rp_u32(line, &n, sd_span_min);
	rp_str(line, &n, "..");
	rp_u32(line, &n, sd_span_max);
	rp_str(line, &n, " seeks ");
	rp_u32(line, &n, sd_seeks);
	rp_str(line, &n, " rep ");
	rp_u32(line, &n, sd_repaints);
	rp_str(line, &n, " still->painted ");
	rp_u32(line, &n, ms);
	rp_str(line, &n, "ms");
	line[n] = 0;

	puts(line);

	sd_samples = 0;
	sd_drag_on = 0;

}

#define SD(x) (x)
#else
#define SD(x) ((void)0)
#define sd_report_flush() ((void)0)
#define sd_report_drag() ((void)0)
#endif

static uint32_t rd_tell(void) {
	return peek_valid ? peek_off : rbuf_base + rbuf_pos;
}

static void rd_seek(uint32_t off) {

	if (fh < 0) return;

	rd_err = false;

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
// Turns one source line into glyph bytes, in place: one byte per
// character, the byte the hardware font draws it with (ISO 8859-15,
// docs/text_encoding.md). Everything downstream -- the Markdown parser,
// wrapping, links, selection, drawing -- then keeps working in bytes,
// exactly as it did when every file was ASCII.
//
// A line that is valid UTF-8 is decoded: a character Latin-9 has is its
// byte, any other is the missing-glyph box (0x7f). A line that is not
// valid UTF-8 was written in a single-byte encoding and is taken as
// Latin-9 already -- decided per line, which is what lets a file that
// mixes the two, or has one stray bad byte, still read correctly
// everywhere else. Only the text changes: the file offsets the index
// keeps are counted separately (rbuf_base/rbuf_pos), so the output
// shrinking is harmless. Returns the new length.
static int line_to_glyphs(char *s, int n) {

	bool high = false;
	for (int i = 0; i < n; i++) if ((uint8_t)s[i] >= 0x80) { high = true; break; }
	if (!high) return n;

	if (!z_utf8_valid(s, (size_t)n)) {
		// Latin-9 bytes; only the C1 range has no glyph.
		for (int i = 0; i < n; i++)
			if ((uint8_t)s[i] >= 0x80 && (uint8_t)s[i] < 0xA0) s[i] = 0x7f;
		return n;
	}

	const char *p = s, *end = s + n;
	int o = 0;
	while (p < end) {
		uint32_t cp = z_utf8_next(&p, end);
		int w = z_cp_width(cp);
		if (w == 0) continue;				// a combining mark: no cell
		if (w == 2) {
			// A CJK character: the box and a blank, two columns, so a
			// table or code block with Japanese in it still lines up.
			// Never longer than the source: a wide character is three
			// or four bytes of UTF-8.
			s[o++] = 0x7f;
			s[o++] = ' ';
			continue;
		}
		uint8_t b = z_cp_to_l9(cp);
		s[o++] = (char)(b >= 0x20 ? b : 0x7f);
	}
	s[o] = 0;
	return o;

}

static bool rd_raw(char *out, int cap) {

	int n = 0;
	bool any = false;

	// After a failure, nothing more until the next rd_seek(): a read
	// that succeeded now would hand back the bytes AFTER a hole, and
	// rd_tell() would no longer say where the failure was.
	if (rd_err) { out[0] = 0; return false; }

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
			if (r < 0) {
				// An I/O error, not the end of the file -- see rd_err.
				// Where fs left its position after a failure is not
				// known, so put it back where rbuf_base says the next
				// byte is, and drop the partial line: no caller uses
				// a line read across a failure.
				rbuf_len = 0;
				rd_err = true;
				io_err = true;
				fs_seek(fh, rbuf_base);
				out[0] = 0;
				return false;
			}

			rbuf_len = (uint32_t)r;

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
	n = line_to_glyphs(out, n);
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

// The cached position (seek_line()), declared here for rd_forget().
static bool pos_valid;

// Forgets everything buffered and puts fs where the reader says it is.
// For a scan that was stopped or failed: whatever it left in rbuf,
// peek_line and the position cache is trusted by nobody after it.
static void rd_forget(void) {

	if (fh < 0) return;

	uint32_t at = rbuf_base + rbuf_pos;

	rbuf_base = at;
	rbuf_len = 0;
	rbuf_pos = 0;
	peek_valid = false;
	pos_valid = false;
	rd_err = false;

	fs_seek(fh, at);

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
static uint32_t read_block_raw(md_state_t *st, md_line_t *out);

// Returns 0 on a read error too, with rd_err set: a block read across a
// failure may have been joined or parsed with a line missing, so none
// of it is used -- the caller stops, and checks rd_err to tell an error
// from the end of the file.
static uint32_t read_block(md_state_t *st, md_line_t *out) {

	uint32_t used = read_block_raw(st, out);

	return rd_err ? 0 : used;

}

// Widens a table's columns to fit its rows ahead, from its first row
// (md.h): reads on to the end of the table, or TBL_LOOK_ROWS rows or
// TBL_LOOK_BYTES bytes if sooner. A row past that which is wider still
// wraps inside the columns it is given. The corpus's longest table has
// 54 rows and its median 7.
//
// Once a table, at its first row -- the widths then ride in the
// parser state, index checkpoints and all -- and once a DOCUMENT:
// tbl_cache remembers the answer by where the table starts, so a
// repaint with a table's top on screen does not read ahead again.
//
// And the reader is left exactly as it was, buffer included: seeking
// back would throw away the buffer the lookahead refilled and read the
// same chunk again, which is a card read in the middle of a redraw.
#define TBL_LOOK_ROWS   64
#define TBL_LOOK_BYTES  4096
#define TBL_CACHE       24

static struct {
	uint32_t	off;		// the row after the table's first
	uint8_t		n;
	uint8_t		w[MD_TBL_COLS], mw[MD_TBL_COLS];
} tbl_cache[TBL_CACHE];
static int tbl_cache_n, tbl_cache_next;

static void table_lookahead(md_state_t *st, md_line_t *row) {

	static char la[MD_LINE_MAX];
	static uint8_t saved[RBUF];
	uint8_t w[MD_TBL_COLS], mw[MD_TBL_COLS];
	int n;

	// rd_peek() has the row after this one: md_parse() was given it.
	uint32_t back = rd_tell();

	for (int i = 0; i < tbl_cache_n; i++)
		if (tbl_cache[i].off == back) {
			md_table_widen(st, row, tbl_cache[i].w, tbl_cache[i].mw, tbl_cache[i].n);
			return;
		}

	uint32_t s_base = rbuf_base, s_len = rbuf_len, s_pos = rbuf_pos;
	bool s_peek = peek_valid;
	memcpy(saved, rbuf, s_len);

	uint8_t most[MD_TBL_COLS] = { 0 }, most_mw[MD_TBL_COLS] = { 0 };
	int most_n = 0;

	for (int r = 0; r < TBL_LOOK_ROWS && rd_tell() - back < TBL_LOOK_BYTES; r++) {
		if (!rd_line(la, MD_LINE_MAX)) break;
		if (!(n = md_table_measure(la, w, mw))) break;		// the table has ended
		if (n < 0) continue;							// a |---| row
		for (int i = 0; i < n; i++) {
			if (w[i] > most[i]) most[i] = w[i];
			if (mw[i] > most_mw[i]) most_mw[i] = mw[i];
		}
		if (n > most_n) most_n = n;
	}

	// A failed read leaves rd_err for the caller to see, and nothing
	// cached: the widths may be short.
	if (rd_err) return;

	md_table_widen(st, row, most, most_mw, most_n);

	tbl_cache[tbl_cache_next].off = back;
	tbl_cache[tbl_cache_next].n = (uint8_t)most_n;
	memcpy(tbl_cache[tbl_cache_next].w, most, sizeof(most));
	memcpy(tbl_cache[tbl_cache_next].mw, most_mw, sizeof(most_mw));
	tbl_cache_next = (tbl_cache_next + 1) % TBL_CACHE;
	if (tbl_cache_n < TBL_CACHE) tbl_cache_n++;

	// Back as it was. peek_line still holds the row after this one:
	// rd_line() only copied it out.
	if (rbuf_base != s_base) {
		memcpy(rbuf, saved, s_len);
		fs_seek(fh, s_base + s_len);
	}
	rbuf_base = s_base;
	rbuf_len = s_len;
	rbuf_pos = s_pos;
	peek_valid = s_peek;
	peek_off = back;

}

static uint32_t read_block_raw(md_state_t *st, md_line_t *out) {

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

	// A table row: at a table's first row the rest is measured, then
	// each row is laid out in columns (md_table_layout()).
	if (out->kind == MD_TABLE) {
		if ((out->tflags & (MD_TF_FIRST | MD_TF_LAST)) == MD_TF_FIRST)
			table_lookahead(&probe, out);
		*st = probe;
		md_table_layout(out, view_w / BODY_FONT->w);
		return used;
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

	// Stops once `joined` is full, whatever md_continues() says. The
	// text past MD_LINE_MAX was dropped anyway, but the lines were
	// still READ: a document of thousands of lines with no blank
	// between them is one paragraph, and every block read -- every
	// paint, every scroll -- read the whole file to find its end
	// (fuzz case front_unterminated.md). The next line then starts a
	// block of its own. Positions stay block-aligned: where a block
	// ends still depends only on where it starts.
	while (n < MD_LINE_MAX - 1 && md_continues(st, kind, rd_peek())) {

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

// Starts the index. Reads nothing -- see the frontier comment above.
static void index_init(void) {

	idx_n = 0;
	tbl_cache_n = tbl_cache_next = 0;
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

// Reads the block at the frontier and moves the frontier past it,
// recording a checkpoint at each stride boundary; `ml` gets the block.
// Returns 1, or 0 at the end of the file (eof_seen set), or -1 on a
// read error -- then nothing moves and nothing is recorded, so the
// same block is simply tried again by whatever asks next.
//
// This is the only thing that reads unvisited parts of the document,
// and it never re-reads: the frontier only moves forward. One block a
// call, so a long scan can be cut into slices (find_step()).
static int frontier_block(md_line_t *ml) {

	if (fh < 0 || eof_seen) return 0;

	// Usually the reader is still where the last block left it, with
	// the next line already peeked; seek only if something else moved
	// it meanwhile (a repaint between two slices of a find).
	if (rd_tell() != frontier_off) rd_seek(frontier_off);
	rd_err = false;

	md_state_t st = frontier_st;
	uint32_t used = read_block(&st, ml);

	if (!used) {
		if (rd_err) return -1;
		eof_seen = true;
		eof_line = frontier_line;
		return 0;
	}

	frontier_line += used;
	frontier_off = rd_tell();
	frontier_st = st;

	// Checkpoint at each stride boundary, on a block boundary.
	if (idx_n && frontier_line >= idx[idx_n - 1].line + idx_stride) {

		if (idx_n >= IDX_MAX) idx_compress();

		if (idx_n < IDX_MAX && frontier_line >= idx[idx_n - 1].line + idx_stride) {
			idx[idx_n].off = frontier_off;
			idx[idx_n].line = frontier_line;
			idx[idx_n].st = st;
			idx_n++;
		}

	}

	return 1;

}

// Extends the index until it covers source line `want_line` AND byte
// offset `want_off`, or the end of the file, or a read error. Either
// bound may be 0 to mean "no requirement".
//
// In one call -- so only for distances known to be short: a screen or
// so past the frontier, which is as far as reading on ever asks.
// Anything that may have to go far is a find (find_start()).
static void extend_index(uint32_t want_line, uint32_t want_off) {

	static md_line_t ml;

	while (frontier_line < want_line || frontier_off < want_off)
		if (frontier_block(&ml) <= 0) break;

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

	// A read failed on the way: this is not a position, and must not
	// be remembered as one.
	if (rd_err) { pos_valid = false; return; }

	// Record where this ended up, EOF included: a short landing is
	// still a valid position, and caching it stops the next call
	// walking the same ground again.
	pos_valid = true;
	pos_line  = ln;
	pos_off   = rd_tell();
	pos_st    = *st;

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
			*first = *rest = cw;	// room for the bar at its left
			break;

		default:
			*first = *rest = 0;
			break;

	}

}

// Splits `ml` into display lines, filling `starts`/`ends` with
// character ranges. Returns how many.
//
// Code lines are NEVER wrapped: they depend on their own horizontal
// layout, and folding it destroys the only thing they were carrying.
// They are clipped instead. Table rows arrive already in lines.
#define MAX_SUB 64

static int wrap_line(const md_line_t *ml, int width,
	uint16_t *starts, uint16_t *ends) {

	// A table row was laid out into display lines of equal length
	// when it was read (md_table_layout()).
	if (ml->kind == MD_TABLE && (ml->tflags & MD_TF_LAID) && ml->len) {
		int L = md_table_line_len(ml), n = 0;
		for (int i = 0; i + L <= ml->len && n < MAX_SUB; i += L, n++) {
			starts[n] = (uint16_t)i;
			ends[n] = (uint16_t)(i + L);
		}
		if (n) return n;
	}

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

	// Not over the resize grip, which wm drew before this redraw: a
	// rect that reaches its square is filled in two, above the grip's
	// rows and short of it across them (z_win_clear() does the same).
	int gx = c.x1 - Z_WIN_GRIP_INSET + 1, gy = c.y1 - Z_WIN_GRIP_INSET + 1;
	if (win.resizable && x1 >= gx && y1 >= gy) {
		if (y0 < gy) z_fb_hw_fill_rect(x0, y0, x1 - x0 + 1, gy - y0, color);
		if (x0 < gx) z_fb_hw_fill_rect(x0, y0 > gy ? y0 : gy, gx - x0, y1 - (y0 > gy ? y0 : gy) + 1, color);
		return;
	}

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

// -- decorations --
//
// Rules, bars and bullets are rectangles (fill()), one or a few each:
// no glyphs, nothing to load, and next to a line of text they cost
// nothing that shows.

static bool meas_on;		// see "measuring" below: draw nothing

// What a partial redraw owns: rows from `thr` down (layout_run()), and
// after a scroll BACK also the rows above band_lim -- the strip the
// blit exposed at the top (scroll_backward()). 0 the rest of the time.
static int band_lim;

// Fills `w` x rows [y0, y1] at x in ink, only in the rows the redraw
// owns, and only inside the view.
static void vfill(int x, int w, int y0, int y1, int thr) {

	if (meas_on) return;
	if (y1 >= view_h) y1 = view_h - 1;
	if (x + w > MARGIN + view_w) w = MARGIN + view_w - x;
	if (w <= 0) return;

	// The top strip: drawn whole when the two bands meet, its part
	// above band_lim when they do not.
	if (band_lim > 0 && y0 < band_lim) {
		if (thr <= band_lim) thr = y0;
		else {
			int top_end = y1 < band_lim ? y1 : band_lim - 1;
			fill(x, y0, w, top_end - y0 + 1, 1);
		}
	}

	if (y0 < thr) y0 = thr;
	if (y1 < y0) return;

	fill(x, y0, w, y1 - y0 + 1, 1);

}

// -- measuring, for scroll_backward() --
//
// layout_run() with meas_on set draws nothing (its thr is past the
// bottom) and stops at the step (meas_ln, meas_sub), leaving where it
// would have put it in meas_y. Only the blocks before it are read.
static uint32_t meas_ln, meas_sub;
static int      meas_y;

static bool meas_hit(uint32_t ln, uint32_t sub, int y) {
	if (!meas_on || ln != meas_ln || sub != meas_sub) return false;
	meas_y = y;
	return true;
}

// A dotted line, w wide at row y, in one blitter operation: a pattern
// fill (zgfx.h), its dots on the screen's even columns.
static void dots(int x, int w, int y, int thr) {

	static const uint8_t pat[8] = { 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA };

	if (meas_on || y < thr || y >= view_h) return;
	if (x + w > MARGIN + view_w) w = MARGIN + view_w - x;
	if (w <= 0) return;

	z_clip_t c;
	z_win_content_rect(&win, &c);
	z_fb_hw_fill_pattern(c.x0 + x, c.y0 + y, w, 1, pat);

}

// The middle pixel of a laid-out table row's border cell `i`.
static int tbl_bar_x(const md_line_t *ml, int i, int fw) {
	return MARGIN + ml->cell[i] * fw + fw / 2;
}

// A rule across a table at row y.
static void tbl_rule(const md_line_t *ml, int fw, int y, int thr) {
	int x0 = tbl_bar_x(ml, 0, fw);
	vfill(x0, tbl_bar_x(ml, ml->ncells, fw) - x0 + 1, y, y, thr);
}

// A table's vertical rules are drawn once for every row of it on the
// screen, not once a row: layout_run() extends them row by row while
// the table goes on, and draws them when it stops (or the screen does).
static struct {
	int			top, bot;		// rows; top < 0: none open
	int			fw, n;
	uint16_t	cell[MD_TBL_COLS + 1];
} tbars = { -1, -1, 0, 0, { 0 } };

// Not clipped to the redraw strip: the rows are only those of lines
// drawn in this pass and the gaps just above them, which a line
// straddling the strip has just cleared (layout_run()). Above the
// strip they are the same table's rules, so drawing them again there
// changes nothing.
static void tbars_flush(int thr) {
	(void)thr;
	if (tbars.top >= 0)
		for (int i = 0; i <= tbars.n; i++)
			vfill(MARGIN + tbars.cell[i] * tbars.fw + tbars.fw / 2, 1,
				tbars.top, tbars.bot, 0);
	tbars.top = -1;
}

// Rows [top, bot] of the row `ml` have vertical rules.
static void tbars_add(const md_line_t *ml, int fw, int top, int bot, int thr) {

	// Not into the top margin, which a scroll's blit leaves alone.
	if (top < MARGIN) top = MARGIN;

	bool same = tbars.top >= 0 && tbars.n == ml->ncells && tbars.fw == fw &&
		top <= tbars.bot + 1;
	for (int i = 0; same && i <= ml->ncells; i++)
		if (tbars.cell[i] != ml->cell[i]) same = false;

	if (same) { tbars.bot = bot; return; }

	tbars_flush(thr);
	tbars.top = top;
	tbars.bot = bot;
	tbars.fw = fw;
	tbars.n = ml->ncells;
	for (int i = 0; i <= ml->ncells; i++) tbars.cell[i] = ml->cell[i];

}

// A list bullet in the character cell at (x, y): a round dot at the
// top level, a ring one down, a small square below that.
static void draw_bullet(int x, int y, const z_font_t *f, int level, int thr) {

	int bx = x + (f->w - 4) / 2;
	int by = y + (f->h - 4) / 2;

	if (level == 0) {
		vfill(bx + 1, 2, by, by, thr);
		vfill(bx, 4, by + 1, by + 2, thr);
		vfill(bx + 1, 2, by + 3, by + 3, thr);
	} else if (level <= 2) {
		vfill(bx + 1, 2, by, by, thr);
		vfill(bx, 1, by + 1, by + 2, thr);
		vfill(bx + 3, 1, by + 1, by + 2, thr);
		vfill(bx + 1, 2, by + 3, by + 3, thr);
	} else {
		vfill(bx + 1, 2, by + 1, by + 2, thr);
	}

}

// Extra vertical space around a block, in pixels.
static int space_before(const md_line_t *ml) {

	if (ml->kind == MD_HEADING) return ml->level <= 2 ? 5 : 3;
	if (ml->kind == MD_RULE) return 3;

	// A table row has a rule above it in this space -- two under the
	// header -- drawn by layout_run().
	if (ml->kind == MD_TABLE) return (ml->tflags & MD_TF_BODY1) ? 5 : 3;

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

	// From the text's left edge only: a bullet, a quote or code bar
	// left of it is not this row's to repaint.
	fill(v->x, v->y, MARGIN + view_w - v->x, v->h, 0);

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

	// a table row's vertical rules, which the blank cells covered
	for (int b = 0; b < v->nbar; b++)
		vfill(v->x + v->bar[b] * v->fw + v->fw / 2, 1, v->y, v->y + v->h - 1, 0);

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
// The selected text into `out`, trailing spaces of each row trimmed and
// rows joined with newlines: for the clipboard (sel_copy) and for
// Super+C (say_selection). Returns its length.
static int sel_text(char *out, int cap) {

	int r0, c0, r1, c1;
	sel_bounds(&r0, &c0, &r1, &c1);

	int n = 0;

	for (int r = r0; r <= r1 && r < nvlines && n < cap - 1; r++) {

		if (r < 0) continue;

		visline_t *v = &vlines[r];

		int from = (r == r0) ? c0 : 0;
		int to = (r == r1) ? c1 : v->len;

		if (from < 0) from = 0;
		if (to > v->len) to = v->len;

		int last = from - 1;
		for (int k = from; k < to; k++)
			if (v->text[k] != ' ') last = k;

		// Out as UTF-8 -- the clipboard and speech are both across a
		// process boundary (docs/text_encoding.md). A box is the
		// replacement character: the character it stood for is gone.
		for (int k = from; k <= last && n < cap - 1; k++) {
			uint8_t b = (uint8_t)v->text[k];
			char u[Z_UTF8_MAX];
			int len = z_utf8_put(b == 0x7f ? 0xFFFDu : z_l9_to_cp(b), u);
			if (n + len > cap - 1) break;
			for (int i = 0; i < len; i++) out[n++] = u[i];
		}

		if (r != r1 && n < cap - 1) out[n++] = '\n';

	}

	out[n] = 0;
	return n;

}

static void sel_copy(void) {

	if (!sel_active()) return;

	static char out[Z_WM_CLIP_MAX];
	int n = sel_text(out, sizeof(out));
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

		// The end of a table on the screen: its vertical rules.
		if (ml.kind != MD_TABLE) tbars_flush(thr);

		if (ml.kind == MD_BLANK) {
			if (!sub_skip) {
				if (meas_hit(blk_line, 0, y_blk)) break;
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
				// Its line must fit, like any block's: drawn off the
				// bottom and called done, it never appears on a scroll.
				if (y + space_before(&ml) + 2 > view_h) { cut = true; break; }
				if (meas_hit(blk_line, 0, y_blk)) break;
				if (nsteps < VIS_STEPS) {
					step_y[nsteps] = (int16_t)y_blk;
					step_ln[nsteps] = blk_line;
					step_off[nsteps] = blk_off;
					step_sub[nsteps] = 0;
					nsteps++;
				}
				y += space_before(&ml);
				vfill(MARGIN, view_w, y + 1, y + 1, thr);
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

		// Entered at its first line (not part way down, by a scroll):
		// only then does a block get its space and rules above.
		bool from_top = !sub_skip;
		bool last_drawn = false;	// the block's last line, in this pass
		bool table = (ml.kind == MD_TABLE && (ml.tflags & MD_TF_LAID));

		if (from_top) y += space_before(&ml);

		// A table row's vertical rules span the lines drawn: from
		// bar_top to bar_bot, drawn once after them.
		int bar_top = -1, bar_bot = -1;

		// A table row's rule above it; two under the header.
		if (table && from_top) {
			tbl_rule(&ml, f->w, y - 2, thr);
			if (ml.tflags & MD_TF_BODY1) tbl_rule(&ml, f->w, y - 4, thr);
		}

		for (int s = 0; s < n; s++) {

			if (sub_skip) { sub_skip--; continue; }

			// What the line needs below it: its glyphs, and the pixel
			// under them for a code bar's join or an h3's dotted line.
			// A line whose decoration does not fit has not fitted --
			// drawn without it and called done, it would scroll up
			// without it too.
			int need = f->h + ((ml.kind == MD_CODE ||
				(ml.kind == MD_HEADING && ml.level > 2)) ? 1 : 0);

			// A block's last line carries the rule under it: an h1's
			// or h2's, a table's bottom one. Line and rule fit, and
			// are redrawn, together -- a rule that only half fitted,
			// left out and called cut, kept its half out after a
			// scroll too.
			if (s == n - 1) {
				if (ml.kind == MD_HEADING && ml.level <= 2)
					need = lh + (ml.level == 1 ? 2 : 1);
				else if (table && (ml.tflags & MD_TF_LAST))
					need = lh + 1;
			}

			if (y + need > view_h) { cut = true; break; }

			if (meas_hit(blk_line, (uint32_t)s, s ? y : y_blk)) { cut = true; break; }

			// Drawn in this pass: reaching below thr, or in the strip
			// at the top a scroll back exposed.
			bool drawn = (y + need > thr) || (y < band_lim);
			if (s == n - 1) last_drawn = drawn;

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
			if (thr > 0 && y + need > thr && y < thr)
				fill(MARGIN, y, view_w, need, 0);

			// The list marker sits in the hanging indent, on the
			// first display line only.
			if (ml.kind == MD_LIST && s == 0 && drawn) {
				if (ml.marker[0] == '-' && !ml.marker[1]) {
					// a bullet, drawn: the font's own is a dash. Not
					// clipped to thr: a line drawn at all is drawn
					// whole, its band cleared first (above).
					draw_bullet(MARGIN + first_in, y, f, ml.level, 0);
				} else {
					z_clip_t c;
					z_win_content_rect(&win, &c);
					z_clip_t clip = { c.x0 + MARGIN, c.y0 + y,
						c.x0 + MARGIN + view_w - 1, c.y0 + y + f->h - 1 };
					if (clip.y1 > c.y1) clip.y1 = c.y1;
					z_fb_draw_text(c.x0 + MARGIN + first_in, c.y0 + y,
						ml.marker, 1, f, &clip);
				}
			}

			// Code gets a bar down the left, through the gaps
			// between its lines so a block reads as one.
			if (ml.kind == MD_CODE && drawn)
				vfill(MARGIN, 2, y, y + f->h, 0);

			// Block quotes get a rule down the left, which is the
			// only structural cue available without indentation
			// alone doing all the work.
			if (ml.kind == MD_QUOTE && drawn)
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
				v->nbar = 0;
				if (table)
					for (int b = 0; b <= ml.ncells; b++)
						v->bar[v->nbar++] = (uint8_t)ml.cell[b];

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

			if (drawn) {
#if READ_PROFILE
				uint32_t d0 = rp_cyc();
				draw_sub(&ml, starts[s], ends[s], MARGIN, y, indent + extra);
				rp_draw += rp_cyc() - d0; rp_rows++;
#else
				draw_sub(&ml, starts[s], ends[s], MARGIN, y, indent + extra);
#endif
			}

			// The vertical rules go from the rule above (or the gap
			// above) to the bottom of the last line.
			// From the gap under the line above: for a row's first
			// line, from the previous row's last -- or from its own
			// rule, at the top of a table.
			if (table && (drawn || (band_lim > 0 && y - space_before(&ml) - 1 < band_lim))) {
				if (bar_top < 0)
					bar_top = !(s == 0 && from_top) ? y - 1 :
						(ml.tflags & MD_TF_FIRST) ? y - 2 :
						y - space_before(&ml) - 1;
				bar_bot = y + f->h - 1;
			}

			// h3 and below: a dotted line under the text. Dotted, not
			// solid, because solid under text is a link.
			if (ml.kind == MD_HEADING && ml.level > 2 && drawn)
				dots(MARGIN + indent + extra, (ends[s] - starts[s]) * f->w - 1,
					y + f->h, 0);

			y += lh;

		}

		// An h1 gets a heavy rule under it and an h2 a light one -- a
		// font change alone does not mark a chapter, and at the 6x12
		// body there is none.
		// As for a line's decoration above: a rule that does not fit
		// cuts the block, which is laid out again once it can.
		if (ml.kind == MD_HEADING && ml.level <= 2 && !cut) {
			int rh = (ml.level == 1) ? 2 : 1;
			if (y + rh > view_h) cut = true;
			else {
				vfill(MARGIN, view_w, y, y + rh - 1, last_drawn ? 0 : thr);
				y += 2 + rh;    // never gated by thr -- layout must not diverge
			}
		}

		// A table's last row: the rule under it.
		if (table && (ml.tflags & MD_TF_LAST) && !cut) {
			if (y + 1 > view_h) cut = true;
			else {
				tbl_rule(&ml, f->w, y, last_drawn ? 0 : thr);
				// down to it from the last line
				if (last_drawn || y >= thr || y < band_lim) {
					if (bar_top < 0) bar_top = y - 1;
					bar_bot = y;
				}
				y += 3;
			}
		}

		// Drawn after the text, whose cells paint their background:
		// when the table, or the screen, ends.
		if (bar_top >= 0) tbars_add(&ml, f->w, bar_top, bar_bot, thr);

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

	tbars_flush(thr);

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

	// A layout cut short by a read error ends at the failure, not at a
	// block: nothing to resume from.
	cache_valid = !rd_err;

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

// Set when the thumb has moved to a position the body has not been
// repainted at yet. The BYTE offset lives in sbar.value the whole
// drag; resolving it to a (line, parser state) is line_at_offset(),
// a seek and a parse, and running that per pointer SAMPLE is what
// made a dense drag answer the release seconds late -- each sample
// cost more than the gap to the next, so the release sat in the
// mailbox behind them. The thumb tracking the hand is the cheap
// part; the expensive half runs once, at the repaint, against
// wherever the thumb is by then.
static bool body_seek;

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
	//
	// During a thumb drag the range is FROZEN at what the drag
	// started with. The page figure is a real screen's worth of
	// bytes, and that changes as the drag passes headings and wrapped
	// paragraphs -- so re-applying it mid-drag changed the thumb's
	// SIZE under the hand, and the position mapping with it. The
	// fresh figure is applied on release, by the repaint that
	// answers the drag; the drag itself runs on one consistent
	// scale.
	if (!sbar.dragging)
		z_scrollbar_set_range(&sbar, fsize ? (int32_t)fsize : 1, span);
#if READ_SCROLL_DEBUG
	else if (!sd_drag_on) {
		sd_drag_on = 1;
		sd_span_min = sd_span_max = (uint32_t)span;
	} else {
		if ((uint32_t)span < sd_span_min) sd_span_min = (uint32_t)span;
		if ((uint32_t)span > sd_span_max) sd_span_max = (uint32_t)span;
	}
#endif
	// Not while a drag's position is still to be resolved (body_seek)
	// or is being found (FIND_OFF): the thumb shows where the reader
	// asked to go, and the value IS that request -- setting it back to
	// the page still on the glass would lose it. A find that is stopped
	// puts the thumb back by coming through here again.
	if (!body_seek && find.kind != FIND_OFF)
		z_scrollbar_set_value(&sbar, (int32_t)top_off);
	z_scrollbar_draw(&sbar, true);


	// The table now describes what is on the glass -- set once,
	// after the layout completes, not per block inside the loop.
	drawn_valid = true;

}

// Resolves the position a scrollbar drag left behind, once -- see
// body_seek above. The selection goes with the position: it is
// anchored to screen rows, which a new position redefines (see
// scroll_down()).
//
// Returns true when the position lies past the frontier and has become
// a find (find_start()) instead: the page on the glass stays as it is
// until the find lands, so the caller has nothing to redraw.
static bool resolve_seek(void) {

	if (!body_seek) return false;
	body_seek = false;

	SD(sd_seeks++);

	// A newer position replaces whatever an earlier drag is still
	// finding; the thumb is already where the hand left it.
	find_end(false);

	uint32_t off = (uint32_t)sbar.value;

	// Into unread territory: reaching it is a scan, which builds the
	// index as it goes (so the same jump is instant afterwards) -- and
	// is a find, a slice per turn of the main loop, not one call.
	if (!eof_seen && off > frontier_off) {
		find_start(FIND_OFF, off, 0);
		return true;
	}

	// The scrollbar is in bytes; find the block that starts at or
	// before that offset. Inside the index, so a replay from the
	// nearest checkpoint at most.
	top_line = line_at_offset(off);
	top_sub = 0;
	sel_link = -1;
	sel_clear();

	// The step table and vlines[] describe the glass AT THE OLD
	// position; the position just changed under them. scroll_forward()
	// trusts drawn_valid to mean they agree with top_line -- without
	// this, a scroll key answered mid-drag blits the new position's
	// neighbours from the old page's table and lands back where the
	// drag started.
	drawn_valid = false;

	return false;

}

// A full repaint, including the window clear. Only for a Z_WM_REDRAW
// or a resize -- anything that may have left the frame or the area
// outside the body dirty.
static void draw_body(void) {

	draw_body_from(0);

}

// Paints the position the reader is AT. A drag's position still to be
// resolved is left for the main loop (repaint_body()): this answers
// Z_WM_REDRAW, and wm waits for the acknowledgement that follows, so
// it must not start what may be a scan of the file.
static void repaint(void) {

	z_win_clear(&win);

	draw_body();
	update_scrollbar();

	// A drag's position still to be resolved keeps its deadline: the
	// main loop repaints there (and finds it, if it is far) when it
	// comes.
	if (!body_seek) body_dirty = false;

}

// Redraws just the document area.
//
// draw_body() fills its own region before drawing, so the window
// clear in repaint() adds nothing here except a visible blank frame
// -- which at scroll rates is exactly the flash it looks like.

static int scroll_down(int n);
static void scroll_up(int n);
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

		// Ask BEFORE relying on the blit, the way term.c does.
		// z_fb_hw_scroll() refuses silently when the destination is
		// not inside a single visible rectangle -- the blitter
		// cannot clip a copy, so a window covered where it is
		// scrolling must repaint instead (zgfx.c,
		// copy_region_allows_rect()). Without this check a
		// half-covered reader shifted its layout cache and redrew
		// only the strip for a blit that never happened, leaving
		// the old rows standing above the new ones.
		if (!z_fb_hw_scroll_allowed((int)c.x0 + MARGIN,
			(int)c.y0 + MARGIN, view_w, view_h - MARGIN)) {
			SD(sd_noblit++);
			repaint_body();
			return;
		}

		// The body band only, below the top margin: that margin is
		// blank and must stay blank, and blitting from c.y0 would
		// drag the first row of text up into it.
		z_fb_hw_scroll((int)c.x0 + MARGIN, (int)c.y0 + MARGIN,
			view_w, view_h - MARGIN, -shift);
		SD(sd_blit++);
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

// Scrolls back `n` display lines the way scroll_forward() scrolls on:
// the pixels move and only what is new is drawn. It used to be a full
// repaint -- the body cleared and every line drawn again, a visible
// flash on every arrow or wheel notch up.
//
//   1. Where does the old top land? A layout from the new top that
//      draws nothing and stops there (meas_*): it reads only the
//      blocks scrolling in, which have to be read anyway.
//   2. Blit the body DOWN by that much (zgfx.h does a downward copy
//      in strips, bottom first).
//   3. One layout pass from the new top, drawing only the strip
//      exposed at the top (band_lim) and the rows below the last
//      step the old screen showed whole -- the bottom, where a line
//      that fitted before may not fit now.
//
// Every step in between came from the blit, and is checked against
// the old step table like scroll_forward() checks its own; any doubt
// is a full repaint.
static void scroll_backward(int n) {

	static int16_t  o_y[VIS_STEPS];
	static uint32_t o_ln[VIS_STEPS];
	static uint8_t  o_sub[VIS_STEPS];

	bool fast = drawn_valid && !sel_on && sel_link < 0 && fh >= 0;
	int on = nsteps > VIS_STEPS ? VIS_STEPS : nsteps;

	for (int i = 0; i < on; i++) {
		o_y[i] = step_y[i];
		o_ln[i] = step_ln[i];
		o_sub[i] = step_sub[i];
	}

	uint32_t ol = top_line, os = top_sub;

	scroll_up(n);

	if (top_line == ol && top_sub == os) return;	// at the top already

	if (!fast || on < 1 || o_ln[0] != ol || o_sub[0] != os) {
		repaint_body();
		return;
	}

	// 1. measure
	md_state_t st;
	seek_line(top_line, &st);
	meas_on = true;
	meas_ln = ol;
	meas_sub = os;
	meas_y = -1;
	{
		md_state_t mst = st;
		layout_run(&mst, top_line, MARGIN, top_sub, view_h + 1000);
	}
	meas_on = false;
	cache_valid = false;	// that layout stopped early: nothing to resume

	int shift = meas_y - MARGIN;

	if (meas_y < 0 || shift <= 0 || shift >= view_h - MARGIN) {
		repaint_body();
		return;
	}

	// 2. blit
	z_clip_t c;
	z_win_content_rect(&win, &c);

	if (!z_fb_hw_scroll_allowed((int)c.x0 + MARGIN, (int)c.y0 + MARGIN,
		view_w, view_h - MARGIN)) {
		SD(sd_noblit++);
		repaint_body();
		return;
	}

	z_fb_hw_scroll((int)c.x0 + MARGIN, (int)c.y0 + MARGIN,
		view_w, view_h - MARGIN, shift);
	SD(sd_blit++);

	// 3. draw the top strip and the bottom. Everything above an old
	// step's start was drawn whole, so the last old step still on the
	// screen once moved is where the bottom redraw begins -- less the
	// row just above it, the gap a table row's rules reach up into,
	// which belongs to that step and may now be cut.
	int lim = MARGIN + shift;
	int thr = lim;
	for (int i = 0; i < on; i++)
		if (o_y[i] + shift <= view_h) thr = o_y[i] + shift - 1;
	if (thr < lim) thr = lim;

	fill(0, 0, view_w + MARGIN, lim, 0);
	if (thr < view_h) fill(0, thr, view_w + MARGIN, view_h - thr, 0);

	nvlinks = 0;
	nvlines = 0;
	nsteps = 0;

	seek_line(top_line, &st);
	top_off = rd_tell();

	band_lim = lim;
	layout_run(&st, top_line, MARGIN, top_sub, thr);
	band_lim = 0;

	bottom_off = rd_tell();
	if (bottom_off < top_off) bottom_off = top_off;

	// VERIFY: the steps the blit carried are the old ones, `shift`
	// lower, in order.
	int i0 = 0;
	while (i0 < nsteps && step_y[i0] < lim) i0++;
	for (int i = i0; i < nsteps; i++) {
		int band_end = (i + 1 < nsteps) ? (int)step_y[i + 1] : view_h;
		if (band_end > thr) break;
		int k = i - i0;
		if (k >= on || step_ln[i] != o_ln[k] || step_sub[i] != o_sub[k] ||
			(int)step_y[i] != (int)o_y[k] + shift) {
			draw_body_from(0);
			break;
		}
	}

	update_scrollbar();
	body_dirty = false;
	drawn_valid = true;

}

static void repaint_body(void) {

#if READ_PROFILE
	uint32_t t0 = rp_cyc();
#endif

	// Became a find: the page stays, the find redraws when it lands.
	if (resolve_seek()) { body_dirty = false; return; }

	draw_body();
	update_scrollbar();

	body_dirty = false;

	SD(sd_repaints++);

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

static bool open_path(const char *p);
static void update_title(void);
static bool io_err_titled;		// what the title last said -- see update_title()

// ---------------------------------------------------------------
// finding -- see `find` at the top
// ---------------------------------------------------------------

// The title says "Finding... NN%" in steps of ten, and only for a find
// that outlasts its first slice: each retitle costs a repaint of the
// windows below, and a find that lands at once needs no progress.
static bool find_titled;

static void find_title(void) {

	uint32_t done = 0, of = 1;

	switch (find.kind) {
		case FIND_END:		done = frontier_off; of = fsize; break;
		case FIND_OFF:		done = frontier_off; of = find.want; break;
		case FIND_LINE:		done = frontier_line; of = find.want; break;
		case FIND_ANCHOR:	done = find.a_off; of = fsize; break;
	}

	if (!of) of = 1;
	if (done > of) done = of;

	// done * 100 overflows past 42MB; a file that large divides first.
	int pct = (of > 0x01000000u) ? (int)(done / (of / 100u + 1u))
		: (int)(done * 100u / of);
	if (pct > 99) pct = 99;
	pct -= pct % 10;

	if (find_titled && pct == find.pct) return;

	find.pct = pct;
	find_titled = true;
	update_title();

}

// Ends the find, whether it landed, failed or was stopped. A find that
// did not land leaves the page where it was: it never moved top_line.
// What it read is forgotten (rd_forget()) -- the reader's buffer and
// peek line belong to wherever the last slice stopped -- but the index
// it built is kept: the frontier only moves forward, so a find stopped
// halfway has still done half of the next one's work.
static void find_end(bool landed) {

	if (!find.kind) return;

	if (!landed && find.hist && hist_n) hist_n--;	// nothing moved

	find.kind = FIND_NONE;
	rd_forget();

	// The name back, or "read error" on, or off (find_start() clears
	// the last error: it was news about an earlier request).
	if (find_titled || io_err != io_err_titled) {
		find_titled = false;
		update_title();
	}

}

// Esc, or something else the reader asked for: stop, page as it was.
// The thumb goes back to the page from wherever a drag left it.
static void find_stop(void) {

	if (!find.kind) return;

	bool off = (find.kind == FIND_OFF);

	find_end(false);

	// Directly, not update_scrollbar(): that also declares the step
	// table to describe the glass, which a stop says nothing about.
	if (off) {
		z_scrollbar_set_value(&sbar, (int32_t)top_off);
		z_scrollbar_draw(&sbar, true);
	}

}

static void find_start(int kind, uint32_t want, uint32_t sub) {

	// Replaced, not stopped: the new request decides where the thumb
	// and the page go, so nothing is put back for the old one.
	find_end(false);

	if (fh < 0) return;

	find.kind = kind;
	find.want = want;
	find.sub = sub;
	find.hist = false;
	find.say_missing = false;
	find.pct = 0;
	find_titled = false;

	// A new request: the last one's read error is no longer news.
	io_err = false;

}

// From the top of the document for the heading whose slug is `anchor`.
// `hist`: history was pushed for the move, and is popped if there is no
// such heading or the find is stopped. `say`: say so if there is none.
//
// A scan, but not an anchor table built at open time: that would cost
// memory in proportion to the number of headings, which is exactly what
// this design avoids. It is a find, so it costs the window nothing.
static void find_anchor(const char *anchor, bool hist, bool say) {

	find_start(FIND_ANCHOR, 0, 0);

	if (!find.kind) {
		if (hist && hist_n) hist_n--;
		return;
	}

	int i = 0;
	for (; anchor[i] && i < (int)sizeof(find.slug) - 1; i++)
		find.slug[i] = anchor[i];
	find.slug[i] = 0;

	find.a_line = 0;
	find.a_off = 0;
	md_state_init(&find.a_st);
	find.hist = hist;
	find.say_missing = say;

}

// The find has reached its goal: move there and paint, once.
static void find_land(uint32_t line, uint32_t sub) {

	find_end(true);

	top_line = line;
	top_sub = sub;
	sel_link = -1;
	sel_clear();
	drawn_valid = false;

	repaint_body();

}

// A read failed: stop where it is, page as it was, and say so in the
// title ("read error", update_title()). Nothing past the failure was
// recorded -- frontier_block() returns before moving anything.
static void find_fail(void) {

	io_err = true;
	find_stop();		// retitles: the name, and "read error"

}

// One block of a heading search. Returns 1 to go on, 0 when it ended
// (landed, or not found), -1 on a read error.
static int find_anchor_block(void) {

	static md_line_t ml;
	char slug[128];

	uint32_t at = find.a_line;

	if (eof_seen && at >= eof_line) {
		// Not found.
		bool say = find.say_missing;
		find_end(false);
		if (say)
			z_dialog_confirm(&dlg_ctx, "Not found",
				"No heading matches\nthat link.", Z_DIALOG_OK_CANCEL);
		return 0;
	}

	if (at >= frontier_line) {

		// At the frontier: read on through it, moving it -- the
		// search builds the index exactly as End would. The two
		// cursors meet on a block boundary, since both walk the same
		// blocks from the top.
		int r = frontier_block(&ml);
		if (r < 0) return -1;
		if (r == 0) return 1;		// eof_seen now: the test above ends it
		find.a_line = frontier_line;
		find.a_off = frontier_off;
		find.a_st = frontier_st;

	} else {

		// Behind it: through ground the index has, on its own cursor.
		if (rd_tell() != find.a_off) rd_seek(find.a_off);
		rd_err = false;

		md_state_t st = find.a_st;
		uint32_t used = read_block(&st, &ml);
		if (!used) {
			if (rd_err) return -1;
			// The frontier said there was more; trust the file.
			eof_seen = true;
			eof_line = at;
			return 1;
		}
		find.a_line = at + used;
		find.a_off = rd_tell();
		find.a_st = st;

	}

	if (ml.kind == MD_HEADING) {
		slugify(ml.text, slug, sizeof(slug));
		if (!strcmp(slug, find.slug)) {
			find_land(at, 0);
			return 0;
		}
	}

	return 1;

}

// About `budget` ticks of the find, then back to the main loop. Each
// step is one block -- a read from the buffer, a refill of RBUF at most
// -- so a slice overruns its budget by one block's time at most.
static void find_step(uint32_t budget) {

	static md_line_t ml;
	uint32_t t0 = z_uptime_ticks();

	while (find.kind) {

		int r = 1;

		switch (find.kind) {

			case FIND_END:
				r = frontier_block(&ml);
				if (r == 0) {
					// The end, as End always did it: the last line at
					// the top, then back a page less one.
					int page = view_h / (BODY_FONT->h + 1);
					if (page < 1) page = 1;
					find_end(true);
					if (eof_line > 1) {
						top_line = eof_line - 1;
						top_sub = 0;
						scroll_up(page - 1);
					}
					sel_link = -1;
					repaint_body();
					return;
				}
				break;

			case FIND_OFF:
				if (eof_seen || frontier_off >= find.want) {
					uint32_t want = find.want;
					find_end(true);
					find_land(line_at_offset(want), 0);
					return;
				}
				r = frontier_block(&ml);
				break;

			case FIND_LINE:
				if (eof_seen && find.want >= eof_line) {
					// The document is shorter than it was: its end.
					find_land(eof_line ? eof_line - 1 : 0, 0);
					return;
				}
				if (frontier_line >= find.want) {
					find_land(find.want, find.sub);
					return;
				}
				r = frontier_block(&ml);
				break;

			case FIND_ANCHOR:
				r = find_anchor_block();
				if (r == 0) return;
				break;

		}

		if (r < 0) { find_fail(); return; }

		if (z_uptime_ticks() - t0 >= budget) break;

	}

	if (find.kind) find_title();

}

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

// -- reading aloud (Super+A, Super+C) -- docs/tts.md, "Reading a window" --
//
// Super+A reads from the top of the screen, a block at a time -- a
// paragraph, a heading, a list item, as read_block() parses them, so
// what is said is the text as shown, never the Markdown -- and the
// page follows the voice: each block becomes the top of the screen as
// it is spoken. Any key or click stops it, leaving the page where the
// voice was, and Super+A again reads on from there. Super+C says the
// highlighted text.
static uint32_t say_next;		// the source line after the last block handed out
static uint32_t say_line[16];		// unit n's source line, for the last 16 n

static int say_get(void *user, int n, const char **text, uint32_t *flags) {

	(void)user;
	static md_line_t ml;
	static char out[MD_LINE_MAX + 16];

	for (;;) {
		if (eof_seen && say_next >= eof_line) return -1;
		md_state_t st;
		uint32_t at = say_next;
		seek_line(at, &st);
		uint32_t used = read_block(&st, &ml);
		if (!used) return -1;
		say_next += used;
		// Nothing to say for a blank line or a rule; skip to the next
		// block rather than hand over an empty unit.
		if (ml.kind == MD_BLANK || ml.kind == MD_RULE || !ml.text[0]) continue;

		int k = 0;
		// A numbered list item is read with its number; a bullet is not.
		if (ml.kind == MD_LIST && ml.marker[0] >= '0' && ml.marker[0] <= '9') {
			for (int i = 0; ml.marker[i] && k < 8; i++) out[k++] = ml.marker[i];
			out[k++] = ' ';
		}
		for (int i = 0; ml.text[i] && k < (int)sizeof(out) - 1; i++) out[k++] = ml.text[i];
		out[k] = 0;

		// To speech as UTF-8, like everything that leaves the process.
		static char out8[(MD_LINE_MAX + 16) * 3];
		k = (int)z_l9_to_utf8(out, (size_t)k, out8, sizeof(out8));

		say_line[n & 15] = at;
		*text = out8;
		*flags = 0;		// every block ends a sentence: a pause after
		return k;
	}

}

// The page follows the voice: the block being spoken is the top line.
static void say_at(void *user, int n) {
	(void)user;
	top_line = say_line[n & 15];
	top_sub = 0;
	sel_link = -1;
	repaint();
}

static z_sayall_t reader = { say_get, say_at, NULL };

static void say_toggle(void) {
	if (z_sayall_active(&reader)) {
		printf("read: Super+A: reading stopped\n");
		z_sayall_stop(&reader);
		return;
	}
	find_stop();		// reading starts from the page on the glass
	say_next = top_line;
	printf("read: Super+A: reading from line %lu\n", (unsigned long)top_line + 1);
	if (!z_sayall_start(&reader, 0)) {
		printf("read: Super+A: could not start (is speech on?)\n");
		z_speak_static("End of document", Z_TTS_F_INTERRUPT);
	}
}

static void say_selection(void) {
	z_sayall_stop(&reader);
	if (!sel_active()) {
		z_speak_static("Nothing selected", Z_TTS_F_INTERRUPT);
		return;
	}
	static char out[Z_WM_CLIP_MAX];
	int n = sel_text(out, sizeof(out));
	printf("read: Super+C: reading the selection, %d characters\n", n);
	z_speak_n(out, (uint32_t)n, Z_TTS_F_INTERRUPT);
}

static void go_back(void) {

	// First: a find that pushed history pops its own entry when it
	// stops, and the one to go back to is the one under it.
	find_stop();

	if (!hist_n) return;

	hist_n--;

	char p[Z_FLIST_PATH_MAX];
	int i = 0;
	for (; hist[hist_n].path[i] && i < Z_FLIST_PATH_MAX - 1; i++)
		p[i] = hist[hist_n].path[i];
	p[i] = 0;

	uint32_t l = hist[hist_n].line, s = hist[hist_n].sub;

	// Same file: just move, don't reopen and re-index. The index
	// reached that line once already, so this is no scan. A drag's
	// position not resolved yet is overruled by this one.
	if (!strcmp(p, path)) {
		body_seek = false;
		top_line = l;
		top_sub = s;
		sel_link = -1;
		repaint();
		return;
	}

	if (!open_path(p)) {
		sel_link = -1;
		repaint();
		return;
	}

	// Another document, opened afresh: its index reaches nothing yet,
	// so the place is a find. Its top meanwhile, not the document just
	// left -- its links would lead somewhere else entirely.
	sel_link = -1;
	if (!l) top_sub = s;
	repaint();
	if (l) find_start(FIND_LINE, l, s);

}

static void follow(const char *target) {

	if (!target || !target[0]) return;

	// Before push_history(): a find being replaced pops the entry IT
	// pushed, which must not be this one.
	find_stop();

	// In-document anchor. A find: the page stays until it lands, and
	// if there is no such heading the history entry goes again --
	// nothing moved, so nothing to go back to.
	if (target[0] == '#') {
		push_history();
		find_anchor(target + 1, true, true);
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

	sel_link = -1;
	repaint();

	if (anchor[0]) find_anchor(anchor, false, false);

}

// ---------------------------------------------------------------
// files
// ---------------------------------------------------------------

// The document's name -- or, while a find runs, how far it has got
// (find_title()), and after a failed read, "read error": the page may
// be cut short, and that should not look like the end of the document.

static void update_title(void) {

	char t[64];

	if (find.kind && find_titled) {
		static const char pre[] = "Finding... ";
		int n = 0;
		for (int i = 0; pre[i]; i++) t[n++] = pre[i];
		if (find.pct >= 10) t[n++] = (char)('0' + find.pct / 10);
		t[n++] = (char)('0' + find.pct % 10);
		static const char post[] = "%  (Esc stops)";
		for (int i = 0; post[i]; i++) t[n++] = post[i];
		t[n] = 0;
	} else {
		z_win_doc_title(t, (int)sizeof(t) - 14, path, false, "read");
		if (io_err) {
			int n = (int)strlen(t);
			static const char e[] = "  read error";
			for (int i = 0; e[i]; i++) t[n++] = e[i];
			t[n] = 0;
		}
	}

	io_err_titled = io_err;
	z_win_set_title(&win, t);

}

// After anything that read the file: retitle if a read error came or
// went since the title last said.
static void note_io(void) {
	if (io_err != io_err_titled && !(find.kind && find_titled)) update_title();
}

static bool open_path(const char *p) {

	// Close the current document BEFORE opening the next one.
	//
	// This used to open first and close after, so that a failed open
	// left the reader with the document it already had. Sound in
	// principle, but it means holding TWO handles across the open --
	// and Z_FS_MAX_OPEN was 4 at the time, shared by every process.
	// With the file dialog holding one or two of its own, opening a
	// second document intermittently ran out and reported "Can't
	// open" for a file that was perfectly readable.
	//
	// The table is 8 now (raised when sw/apps/hex arrived, which also
	// holds one for its whole lifetime -- see zfs.h), so this
	// particular collision no longer has to happen. Closing first is
	// kept anyway: it is not a workaround for the old limit, it is
	// simply the smaller footprint, and this app has no reason to
	// hold two handles on two documents it is not reading from at
	// once.
	//
	// The safety it was buying is kept by reopening the previous
	// path when the new one fails, which costs nothing in the normal
	// case and one extra open in the rare one.
	char prev[Z_FLIST_PATH_MAX];
	bool had_prev = (fh >= 0);

	// A find belongs to the document it was started in.
	find_stop();
	io_err = false;

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

	// A pending drag position names bytes of the PREVIOUS document;
	// it must not follow the reader into this one.
	body_seek = false;

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

	find_stop();

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

	// Another system's document (docx, odt, rtf, epub): convert
	// turns it into Markdown and opens that -- here again, or in text
	// when it is small enough to edit (docs/convert.md).
	{
		const char *app = z_ftype_app_for(p);
		if (app && !strcmp(app, "convert")) {
			loading = false;
			repaint();
			z_launch_arg_set(p);
			z_proc_run("convert");
			return;
		}
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

// -- fused scroll impulses --
//
// A page costs most of a second on this machine, and a held key
// repeats far faster than that. One repaint per repeat is then a
// queue the reader is still emptying half a minute after the key was
// released -- scrolling that appears to have a mind of its own.
//
// The impulses are requests to MOVE, not requests to PAINT, so:
// accumulate the distance while a batch is pending and paint the
// answer once. A batch is whatever the mailbox held when the loop
// last looked, which under repetition is exactly the work that would
// otherwise have queued. What arrives during one repaint is answered
// by the next, and after the key is released there is at most one
// repaint left -- the one the accumulated distance asks for.
static int key_pend_lines;	// display lines, + down / - up
static bool key_pend_pages;	// a page-sized key contributed

static void scroll_flush(void) {

	if (!key_pend_lines) { key_pend_pages = false; return; }

	// Scrolling is asking for somewhere else: a find still going would
	// pull the page away from under it when it landed.
	find_stop();

	// A scrollbar drag's position is a scroll request too, and an
	// earlier one: the impulses below move FROM where it landed.
	if (resolve_seek()) {
		// The drag's position became a find; these impulses were
		// meant to move from it, which is not known yet. Drop them.
		key_pend_lines = 0;
		key_pend_pages = false;
		return;
	}

	int n = key_pend_lines;
	bool pages = key_pend_pages;
	key_pend_lines = 0;
	key_pend_pages = false;

	SD(sd_flush++);
	SD(sd_lines += (uint32_t)(n < 0 ? -n : n));

	io_err = false;

	if (n > 0) {
		// Arrows alone stay on the blit path -- it is what they
		// take one at a time, and one fused blit stands in for n.
		// A page key anywhere in the batch takes the page path,
		// which scroll_down() walks in a single forward pass.
		if (!pages) scroll_forward(n);
		else { scroll_down(n); repaint_body(); }
	} else {
		scroll_backward(-n);
	}

	sd_report_flush();
	note_io();

}

static void handle_key(uint32_t keysym, uint8_t mods) {

	if (keysym == Z_KEY_HELP) {	// docs/app_help.md
		static const char help[] =
			"A reader for Markdown and text: manuals, notes, books.\n\n"
			"Space, PgDn\tNext page (Shift+Space back)\n"
			"Arrows\tA line\n"
			"Home, End\tStart, end (Esc stops a long find)\n"
			"Tab\tNext link; Enter follows it\n"
			"Backspace\tBack\n"
			"Ctrl+O\tOpen\n"
			"Ctrl+C\tCopy the selection (drag to select)\n\n"
			"A .docx, .odt, .rtf or .epub is converted to read.";
		z_dialog_help(&dlg_ctx, "Read help", help, "read_app");
		return;
	}

	// While a find runs (End, a link to a heading, a jump into unread
	// ground): Esc stops it and does nothing else, the page as it was.
	// End again is the same request, still being answered. Any other
	// key that moves the page asks for something else, and stops it
	// first -- by scroll_flush(), or below.
	if (find.kind) {
		if (keysym == 0x1b) { find_stop(); return; }
		if (keysym == Z_KEY_END && find.kind == FIND_END) return;
	}

	int page = view_h / (BODY_FONT->h + 1);
	if (page < 1) page = 1;

	// Scroll impulses fuse (see above). Anything else is answered
	// only after the pending scroll has painted -- the order the
	// requests arrived in.
	switch (keysym) {

		case Z_KEY_DOWN:    SD(sd_keys++); key_pend_lines++; return;
		case Z_KEY_UP:      SD(sd_keys++); key_pend_lines--; return;
		// Space is page down, the convention every pager shares --
		// it is the key a hand rests on while reading. Shift+Space
		// goes back up, for the same reason.
		case ' ':
			SD(sd_keys++);
			if (mods & Z_KBD_MOD_SHIFT) key_pend_lines -= page - 1;
			else key_pend_lines += page - 1;
			key_pend_pages = true;
			return;

		case Z_KEY_PAGEDOWN:
			SD(sd_keys++);
			key_pend_lines += page - 1;
			key_pend_pages = true;
			return;
		case Z_KEY_PAGEUP:
			SD(sd_keys++);
			key_pend_lines -= page - 1;
			key_pend_pages = true;
			return;

		default: break;

	}

	scroll_flush();

	io_err = false;

	switch (keysym) {

		// Every key below that moves the page stops a find first, or
		// the find would pull the page away when it landed: Home here,
		// the others in what they call (follow(), go_back(),
		// do_open()). Tab and copying leave it running.
		case Z_KEY_HOME:
			find_stop();
			top_line = 0; top_sub = 0; sel_link = -1; repaint_body(); note_io(); return;

		case Z_KEY_END:
			// The only operation that genuinely needs the whole
			// document read, since the end is not knowable otherwise.
			// A find (see `find`): the scan builds the index, so it is
			// paid once, and the window answers throughout. Once the
			// end is known it lands within the first slice.
			find_start(FIND_END, 0, 0);
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

	// A pointer event refers to what is on the glass, so any scroll
	// the keyboard already asked for paints first -- the order the
	// requests arrived in.
	scroll_flush();

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

#if READ_SCROLL_DEBUG
		// A press starts a new measurement; the per-drag counters
		// re-arm from here (see update_scrollbar() for the span).
		if (!was_down && now_down) {
			sd_samples = 0;
			sd_drag_on = 0;
			sd_span_min = sd_span_max = 0;
		}
#endif

		if (z_scrollbar_mouse(&sbar, cx, cy, buttons)) {

			// The thumb has already moved (z_scrollbar_mouse drew
			// it). Where it points is resolved at the repaint, not
			// here -- see body_seek. The expensive half of the
			// gesture runs once, against wherever the thumb is by
			// then, instead of once per sample against every
			// position the hand passed through.
			body_seek = true;
			sel_link = -1;

			SD(sd_samples++);
			SD(sd_defer_t = z_uptime_ticks());

			defer_body();

		}

		// Released: the reader has stopped, so show the page now
		// rather than waiting out the settle time.
		if (was_down && !now_down && body_dirty) {
			repaint_body();
			sd_report_drag();
		}

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

	// Speech's progress reports, pacing Super+A.
	if (z_sayall_msg(&reader, msg)) return;

	switch (msg->subject) {

		// The part of this window not covered by the windows in front
		// of it. Confines every subsequent draw to it -- see
		// z_win_apply_clip() in zwin.c. The ack it sends is not
		// optional: wm waits for it when a region narrows.
		case Z_WM_SET_CLIP:
			z_win_apply_clip(&win, &msg->obj);
			break;

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

			// What is on the glass, and the layout cache, were laid
			// out at the old width: a blit trusting them now moves
			// pixels that are not where the step table says, and a
			// resume from the cache continues the old wrapping.
			rewrapped();
			break;

		default:
			break;

	}

}

// After the width or the font changed: everything laid out is laid out
// at the old one.
static void rewrapped(void) {

	// What is on the glass, and the layout cache, were laid out at the
	// old size: a blit trusting them now moves pixels that are not
	// where the step table says, and a resume from the cache continues
	// the old wrapping.
	drawn_valid = false;
	cache_valid = false;

	// top_sub counts display lines of top_line's block at the OLD
	// size. More lines now and it is still valid; fewer, and it may
	// point past the block's end -- layout_run() would skip all of it
	// and the next one too. One seek, near a checkpoint.
	if (top_sub && fh >= 0) {
		uint32_t used;
		int subs = block_subs(top_line, &used);
		if (subs < 1) subs = 1;
		if (top_sub >= (uint32_t)subs) top_sub = (uint32_t)subs - 1;
	}

}

// The titlebar's Aa: the body between 5x8 and 6x12. Not remembered --
// read opens at 5x8 -- like text's.
static void do_font(void) {

	sel_clear();
	body_font = (body_font == &z_font_6x12) ? &z_font_5x8 : &z_font_6x12;
	rewrapped();
	repaint();

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
		Z_WIN_FLAG_OPEN_ICON | Z_WIN_FLAG_FONT_ICON |
		Z_WIN_FLAG_READABLE) != Z_OK) {
		printf("read: failed to create window -- is wm running?\n");
		return 1;
	}

	z_scrollbar_init(&sbar, &win, Z_SB_VERT);

	dlg_ctx.parent = &win;
	dlg_ctx.on_msg = forward_msg;
	dlg_ctx.user = NULL;

	layout();

	// A place to go to, from the launch argument -- see below.
	static char arg[Z_FLIST_PATH_MAX];
	const char *launch_anchor = NULL;
	uint32_t launch_off = 0;
	bool launch_have_off = false;

	{
		if (z_launch_path_take(arg, sizeof(arg))) {

			/* Split off a "#<decimal>" byte-offset fragment, if
			 * there is one. `ask` (sw/apps/ask, docs/ask_app.md)
			 * hands over a passage as
			 *
			 *     /opt/ark/arklite/books/00000072.md#39006
			 *
			 * so the reader lands ON the matched text rather than
			 * at the top of a 300KB manual.
			 *
			 * Only an ALL-DIGIT fragment is taken as an offset.
			 * follow() already gives "#anchor" its heading meaning
			 * for in-document links, and that is untouched -- a
			 * heading whose anchor is entirely digits is not a
			 * thing Markdown produces. */
			uint32_t want_off = 0;
			bool have_off = false;
			char *heading = NULL;	// "#using-it": a heading's anchor, as a link would

			for (int h = 0; arg[h]; h++) {

				if (arg[h] != '#') continue;

				int d = h + 1;
				bool digits = (arg[d] != 0);
				uint32_t v = 0;

				for (; arg[d]; d++) {
					if (arg[d] < '0' || arg[d] > '9') {
						digits = false;
						break;
					}
					v = v * 10u + (uint32_t)(arg[d] - '0');
				}

				if (digits) {
					arg[h] = 0;
					want_off = v;
					have_off = true;
				} else if (arg[h + 1]) {
					// A link into a page's section from outside,
					// as `read page.md#using-it`.
					arg[h] = 0;
					heading = &arg[h + 1];
				}

				break;

			}

			bool opened = open_path(arg);

			/* Both are finds, started after the first paint below:
			 * the window shows the top of the document at once and
			 * answers throughout, and Esc leaves it there.
			 *
			 * An offset is the scrollbar's FIND_OFF: it extends
			 * the sparse index as far as the offset and lands on
			 * the block containing it, with that point's parser
			 * state restored -- which is the whole reason a jump
			 * into a document renders correctly instead of
			 * resuming mid-block.
			 *
			 * COST: the index frontier only moves FORWARD, so this
			 * streams from the top to want_off. At a realistic
			 * 300-600 KB/s that is about a second for a 344KB
			 * document, which is why tools/ask splits large works
			 * at build time rather than leaving an 8MB Bible
			 * whole. */
			if (opened && heading) launch_anchor = heading;
			if (opened && have_off) {
				launch_off = want_off;
				launch_have_off = true;
			}

		}
	}

	repaint();

	if (launch_anchor) find_anchor(launch_anchor, false, false);
	else if (launch_have_off) find_start(FIND_OFF, launch_off, 0);

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

					// Any key ends a read, before it does anything else --
					// the page is then where the voice was.
					z_sayall_stop(&reader);

					handle_key(Z_WM_UNPACK_KEY_KEYSYM(msg.obj.val.uint32),
						(uint8_t)Z_WM_UNPACK_KEY_MODIFIERS(msg.obj.val.uint32));

					break;

				case Z_WM_WHEEL:

					// Three lines a notch, up = back, as term and text
					// do. Added to the fused impulses (scroll_flush()),
					// so a fast spin that arrives as a burst of notches
					// is one scroll, not one repaint per notch -- and,
					// like the arrows, it scrolls under a reading voice.
					if (msg.obj.type == Z_UINT32)
						key_pend_lines -= 3 * Z_WM_WHEEL_NOTCHES(msg.obj.val.uint32);
					break;

				case Z_WM_MOUSE:

					if (msg.obj.type != Z_UINT32) break;
					// So does a click; the wheel scrolls under the voice.
					if (Z_WM_UNPACK_MOUSE_BUTTONS(msg.obj.val.uint32) & 1)
						z_sayall_stop(&reader);
					handle_mouse(msg.obj.val.uint32);

					break;

				// Super+A and Super+C -- see say_toggle(), say_selection().
				case Z_WM_READ:

					if (msg.obj.type == Z_UINT32 &&
						Z_WM_READ_WIN(msg.obj.val.uint32) == win.id) {
						if (Z_WM_READ_WHAT(msg.obj.val.uint32) == Z_WM_READ_SELECTION)
							say_selection();
						else
							say_toggle();
					}

					break;

				case Z_WM_TITLEBAR_ICON:

					if (msg.obj.type != Z_UINT32) break;
					if ((int)Z_WM_UNPACK_TBICON_ID(msg.obj.val.uint32) != win.id)
						break;
					if (Z_WM_UNPACK_TBICON_KIND(msg.obj.val.uint32) ==
						Z_WM_TBICON_OPEN) { scroll_flush(); do_open(); }
					if (Z_WM_UNPACK_TBICON_KIND(msg.obj.val.uint32) ==
						Z_WM_TBICON_FONT) { scroll_flush(); do_font(); }

					break;

				default:

					forward_msg(&msg, NULL);
					break;

			}

		}

		// One paint answers however many scroll impulses the drain
		// just read -- see scroll_flush().
		scroll_flush();

		// A reader whose progress reports stopped coming gives up.
		z_sayall_poll(&reader);

		uint32_t now = z_uptime_ticks();

		if (body_dirty && (int32_t)(now - body_deadline) >= 0) {
			repaint_body();
			sd_report_drag();
			continue;
		}

		// A find: ~20 ms of it, then back to the messages, without
		// waiting -- docs/responsive.md, "The shape of a main loop".
		if (find.kind) {
			find_step(Z_TICK_HZ / 50);
			continue;
		}

		// Block until something arrives -- but no longer than the
		// deferred body redraw's deadline, or a drag that ends by the
		// pointer simply stopping would never repaint; and, while
		// reading aloud, no longer than a second, or z_sayall_poll()'s
		// give-up would only ever run when some other message came.
		uint32_t wait = 0;

		if (body_dirty) wait = body_deadline - now;
		if (z_sayall_active(&reader) && (!wait || wait > Z_TICK_HZ)) wait = Z_TICK_HZ;

		note_io();

		z_proc_wait(wait);

	}

	return 0;

}
