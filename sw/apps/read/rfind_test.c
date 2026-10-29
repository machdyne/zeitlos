/*
 * Host test for read.c's long work: finds, read errors, and what the
 * window does meanwhile. Host build:  make findtest
 *
 * Builds the REAL read.c -- #included below, its main() renamed --
 * against stubs for the system calls: the file is a host file behind
 * fs_read_chunk()/fs_seek(), drawing does nothing, and the clock is
 * fake: it moves only when the file is read (READ_TICKS a chunk, about
 * what 2KB over SPI costs on the board), so "~20 ms a slice" can be
 * checked as "so many chunks a slice", deterministically.
 *
 * What it checks, each the regression test of a bug that was there:
 *
 *   - End, a link to a heading, a scrollbar drag or a launch offset
 *     into unread ground never read more than a slice's worth in one
 *     call: they are finds, stepped from the main loop.
 *   - Esc stops a find with the page, the reader's buffers and the
 *     history as they were.
 *   - A failed read is not the end of the file: no eof, no checkpoint
 *     from it, fs put back where the reader thinks it is, "read error"
 *     in the title -- and once the card reads again, End finds the
 *     real end.
 *   - Z_WM_REDRAW is acknowledged before any scan.
 *   - A resize clears what was laid out at the old width and keeps
 *     top_sub inside its block.
 *   - A document that opens with an unclosed ---, or with thousands of
 *     table separator rows, paints its first screen reading little.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

#include "../../common/zeitlos.h"
#include "../../common/zwin.h"
#include "../../common/zwidget.h"
#include "../../common/zdialog.h"
#include "../../common/zsayall.h"
#include "../../common/zfsapp.h"

// -- the fake machine --

static FILE *hf;
static long hsize;
static long hpos;			// where fs's own position is
static uint32_t now;		// the clock
#define READ_TICKS 3		// a 2KB chunk's cost on the board, roughly

static long fail_at = -1;	// a read that would cover this offset fails
static int  fail_count;		// how many times it failed
static long chunks;			// fs_read_chunk() calls
static int  redraw_acks;
static long chunks_at_ack;
static int  dialogs;
static char title[128];
static int  content_w = 320, content_h = 280;

int fs_open_read(const char *p) {
	if (hf) fclose(hf);
	hf = fopen(p, "rb");
	if (!hf) return -1;
	fseek(hf, 0, SEEK_END); hsize = ftell(hf); fseek(hf, 0, SEEK_SET);
	hpos = 0;
	return 3;
}
int fs_size(char *p) { (void)p; return (int)hsize; }
int fs_close_handle(int h) { (void)h; if (hf) fclose(hf); hf = NULL; return 1; }
int fs_seek(int h, uint32_t off) { (void)h; hpos = off; return 1; }
int fs_read_chunk(int h, void *b, int max) {
	(void)h;
	chunks++;
	now += READ_TICKS;
	if (fail_at >= 0 && hpos <= fail_at && fail_at < hpos + max) {
		fail_count++;
		hpos += 512;	// fs's position after a failure: anywhere
		return -1;
	}
	fseek(hf, hpos, SEEK_SET);
	int n = (int)fread(b, 1, (size_t)max, hf);
	hpos += n;
	return n;
}
uint32_t z_uptime_ticks(void) { return now; }

void z_clip_set(const char *t, int len) { (void)t; (void)len; }
int z_dialog_confirm(const z_dialog_ctx_t *c, const char *t, const char *m, int k) {
	(void)c; (void)t; (void)m; (void)k; dialogs++; return 1; }
void z_dialog_help(const z_dialog_ctx_t *c, const char *t, const char *h, const char *d) {
	(void)c; (void)t; (void)h; (void)d; }
bool z_dialog_open(const z_dialog_ctx_t *c, const char *d, char *o, int n) {
	(void)c; (void)d; (void)o; (void)n; return false; }
void z_fb_draw_text(int x, int y, const char *s, int c, const z_font_t *f, const z_clip_t *k) {
	(void)x; (void)y; (void)s; (void)c; (void)f; (void)k; }
void z_fb_draw_text2(int x, int y, const char *s, int a, int b, const z_font_t *f, const z_clip_t *k) {
	(void)x; (void)y; (void)s; (void)a; (void)b; (void)f; (void)k; }
void z_fb_hw_box(int a, int b, int c, int d, int e, const z_clip_t *k) {
	(void)a; (void)b; (void)c; (void)d; (void)e; (void)k; }
void z_fb_hw_fill_rect(int x, int y, int w, int h, int c) { (void)x; (void)y; (void)w; (void)h; (void)c; }
void z_fb_hw_line(int a, int b, int c, int d, int e, const z_clip_t *k) {
	(void)a; (void)b; (void)c; (void)d; (void)e; (void)k; }
void z_fb_hw_fill_pattern(int x, int y, int w, int h, const uint8_t *p) { (void)x; (void)y; (void)w; (void)h; (void)p; }
void z_fb_hw_scroll(int x, int y, int w, int h, int dy) { (void)x; (void)y; (void)w; (void)h; (void)dy; }
bool z_fb_hw_scroll_allowed(int x, int y, int w, int h) { (void)x; (void)y; (void)w; (void)h; return true; }
static const uint8_t no_glyphs[1];
const z_font_t z_font_5x8 = { 5, 8, 32, 126, no_glyphs, 0, 0 };
const z_font_t z_font_6x12 = { 6, 12, 32, 126, no_glyphs, 0, 0 };
const char *z_ftype_app_for(const char *p) { (void)p; return "read"; }
void z_launch_arg_set(const char *a) { (void)a; }
bool z_launch_path_take(char *o, int n) { (void)o; (void)n; return false; }
z_rv z_msg_read(z_msg_t *m) { (void)m; return Z_FAIL; }
uint32_t z_proc_run(const char *n) { (void)n; return 0; }
void z_proc_wait(uint32_t t) { (void)t; }
bool z_sayall_active(const z_sayall_t *s) { (void)s; return false; }
bool z_sayall_msg(z_sayall_t *s, const z_msg_t *m) { (void)s; (void)m; return false; }
void z_sayall_poll(z_sayall_t *s) { (void)s; }
bool z_sayall_start(z_sayall_t *s, int f) { (void)s; (void)f; return false; }
void z_sayall_stop(z_sayall_t *s) { (void)s; }
void z_scrollbar_draw(z_scrollbar_t *s, bool f) { (void)s; (void)f; }
bool z_scrollbar_has_pointer(const z_scrollbar_t *s, int x, int y) { (void)s; (void)x; (void)y; return false; }
void z_scrollbar_init(z_scrollbar_t *s, const z_win_t *w, z_sb_orient_t o) { (void)w; (void)o; memset(s, 0, sizeof(*s)); }
bool z_scrollbar_mouse(z_scrollbar_t *s, int x, int y, uint8_t b) { (void)s; (void)x; (void)y; (void)b; return false; }
void z_scrollbar_set_geom(z_scrollbar_t *s, int x, int y, int l) { s->x = (int16_t)x; s->y = (int16_t)y; s->len = (int16_t)l; }
bool z_scrollbar_set_range(z_scrollbar_t *s, int32_t t, int32_t p) { s->total = t; s->page = p; return true; }
bool z_scrollbar_set_value(z_scrollbar_t *s, int32_t v) { s->value = v; return true; }
bool z_speak_n(const char *t, uint32_t l, uint32_t f) { (void)t; (void)l; (void)f; return false; }
bool z_speak_static(const char *t, uint32_t f) { (void)t; (void)f; return false; }
bool z_win_apply_clip(z_win_t *w, z_obj_t *o) { (void)w; (void)o; return true; }
void z_win_apply_redraw(z_win_t *w, uint32_t p) { (void)w; (void)p; }
bool z_win_apply_resized(z_win_t *w, z_obj_t *o) { (void)w; content_w = (int)o->val.uint32; return true; }
void z_win_clear(const z_win_t *w) { (void)w; }
int z_win_content_h(const z_win_t *w) { (void)w; return content_h; }
void z_win_content_rect(const z_win_t *w, z_clip_t *o) { (void)w; o->x0 = 0; o->y0 = 0; o->x1 = content_w - 1; o->y1 = content_h - 1; }
int z_win_content_w(const z_win_t *w) { (void)w; return content_w; }
z_rv z_win_create_flags(z_win_t *w, const char *t, uint32_t a, uint32_t b, int32_t x, int32_t y, uint32_t f) {
	(void)w; (void)t; (void)a; (void)b; (void)x; (void)y; (void)f; return Z_OK; }
void z_win_destroy(const z_win_t *w) { (void)w; }
void z_win_doc_title(char *t, int cap, const char *p, bool m, const char *u) {
	(void)m; const char *b = (p && p[0]) ? p : u;
	for (const char *q = b; *q; q++) if (*q == '/') b = q + 1;
	snprintf(t, (size_t)cap, "%s", b); }
bool z_win_mouse_content_xy(const z_win_t *w, uint32_t p, int *x, int *y) { (void)w; (void)p; *x = *y = 0; return false; }
bool z_win_parse_rect(z_win_t *w, z_obj_t *o) { (void)w; (void)o; return true; }
void z_win_redraw_done(z_win_t *w) { (void)w; redraw_acks++; chunks_at_ack = chunks; }
int z_win_redraw_id(uint32_t p) { (void)p; return 0; }
void z_win_set_title(const z_win_t *w, const char *t) { (void)w; snprintf(title, sizeof(title), "%s", t); }

#define main read_main
#include "read.c"
#undef main

// -- the checks --

static int checks, fails;
static bool verbose = true;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; printf("  FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

// The source lines of the open document, from the host file: the
// truth to check the index and the landings against.
static long *lstart; static long nls;

static void load_lines(const char *p) {
	FILE *f = fopen(p, "rb");
	free(lstart); lstart = malloc(sizeof(long) * (size_t)(hsize + 2)); nls = 0;
	lstart[nls++] = 0;
	long o = 0; int c;
	while ((c = fgetc(f)) != EOF) { o++; if (c == '\n' && o < hsize) lstart[nls++] = o; }
	if (!hsize) nls = 0;
	fclose(f);
}

static void reset(const char *p) {
	win.id = 0;
	content_w = 320; content_h = 280;
	layout();
	fail_at = -1; fail_count = 0; chunks = 0; now = 0; dialogs = 0;
	hist_n = 0; find.kind = FIND_NONE; find_titled = false; io_err = false;
	body_seek = false; body_dirty = false;
	if (!open_path(p)) { printf("cannot open %s\n", p); exit(2); }
	load_lines(p);
	repaint();
}

// Every checkpoint is the start of the line it says, with the parser
// state replaying from the top gives.
static void check_index(const char *what) {
	for (int i = 0; i < idx_n; i++) {
		long ln = idx[i].line;
		bool ok = ln < nls ? (long)idx[i].off == lstart[ln] : (long)idx[i].off == hsize;
		CHECK(ok, "%s: checkpoint %d says line %ld at %u, the file has it at %ld",
			what, i, ln, idx[i].off, ln < nls ? lstart[ln] : hsize);
		if (!ok) return;
	}
}

// Runs a find to its end the way the main loop does, a slice a turn.
// Returns the most chunks read in any one slice but the last: that one
// lands, and painting the page there is a paint like any other.
static long run_find(int *slices) {
	long most = 0; int n = 0;
	while (find.kind && n < 100000) {
		long c0 = chunks;
		find_step(Z_TICK_HZ / 50);
		if (find.kind && chunks - c0 > most) most = chunks - c0;
		n++;
	}
	if (slices) *slices = n;
	return most;
}

// The most chunks a slice may read: its budget, plus the block that
// crosses it (a 1KB line at most: one more chunk).
#define SLICE_MAX ((Z_TICK_HZ / 50) / READ_TICKS + 2)

static void test_end(const char *doc) {
	reset(doc);
	long c0 = chunks;
	handle_key(Z_KEY_END, 0);
	CHECK(chunks - c0 <= 1, "End read %ld chunks before returning", chunks - c0);
	CHECK(find.kind == FIND_END, "End did not start a find");
	int slices;
	long most = run_find(&slices);
	CHECK(most <= SLICE_MAX, "a slice of End read %ld chunks (max %d)", most, SLICE_MAX);
	CHECK(eof_seen && (long)eof_line == nls, "End: eof_line %u, the file has %ld lines", eof_line, nls);
	CHECK(top_line < eof_line || !eof_line, "End landed at %u past %u", top_line, eof_line);
	check_index("after End");
	if (verbose)
		printf("  End      %-28s %5ld lines, %3d slices, at most %ld chunks a slice\n",
			doc, nls, slices, most);
}

static void test_esc(const char *doc) {
	reset(doc);
	uint32_t l0 = top_line, s0 = top_sub;
	handle_key(Z_KEY_END, 0);
	find_step(Z_TICK_HZ / 50);
	if (!find.kind) { printf("  (esc: %s too short to stop)\n", doc); return; }
	CHECK(strstr(title, "Finding") && strstr(title, "Esc stops"), "title during a find: '%s'", title);
	handle_key(0x1b, 0);
	CHECK(!find.kind, "Esc did not stop the find");
	CHECK(top_line == l0 && top_sub == s0, "Esc moved the page to %u.%u", top_line, top_sub);
	CHECK(!peek_valid && !pos_valid && rbuf_len == 0, "Esc left the reader's buffers live");
	CHECK(hpos == (long)rbuf_base, "fs at %ld, the reader at %u", hpos, rbuf_base);
	CHECK(!strstr(title, "Finding"), "title after Esc: '%s'", title);
	check_index("after Esc");

	// A heading link pushes history; stopping it pops it again.
	follow("#no-such-heading-at-all");
	CHECK(find.kind == FIND_ANCHOR && hist_n == 1, "link: find %d, history %d", find.kind, hist_n);
	if (find.kind) find_step(1);
	handle_key(0x1b, 0);
	CHECK(hist_n == 0, "a stopped link left %d history entries", hist_n);

	// Running to the end: not found, said once, history back.
	follow("#no-such-heading-at-all");
	run_find(NULL);
	CHECK(dialogs == 1 && hist_n == 0, "not found: %d dialogs, %d history", dialogs, hist_n);
}

// The line that is exactly `text`, from the host file.
static long line_of(const char *doc, const char *text) {
	FILE *f = fopen(doc, "rb"); char b[2048]; long n = 0;
	while (fgets(b, sizeof(b), f)) {
		b[strcspn(b, "\r\n")] = 0;
		if (!strcmp(b, text)) { fclose(f); return n; }
		n++;
	}
	fclose(f); return -1;
}

static void test_anchor(const char *doc, const char *slug, const char *heading) {
	// The FIRST heading with that slug, whatever its level -- what a
	// link goes to.
	long want_line = -1;
	for (int lv = 1; lv <= 6; lv++) {
		char h[64]; snprintf(h, sizeof(h), "%.*s %s", lv, "######", heading);
		long l = line_of(doc, h);
		if (l >= 0 && (want_line < 0 || l < want_line)) want_line = l;
	}
	if (want_line < 0) return;		// this document has no such heading
	reset(doc);
	follow(slug);
	CHECK(find.kind == FIND_ANCHOR, "a link to %s did not start a find", slug);
	long most = run_find(NULL);
	CHECK(most <= SLICE_MAX, "a slice of the heading search read %ld chunks", most);
	CHECK((long)top_line == want_line, "%s landed at line %u, want %ld", slug, top_line, want_line);
	CHECK(hist_n == 1, "history %d after a link", hist_n);
	// Again: behind the frontier now, through the index.
	top_line = 0; top_sub = 0;
	follow(slug);
	run_find(NULL);
	CHECK((long)top_line == want_line, "%s again landed at %u", slug, top_line);
	check_index("after a heading search");
}

static void test_drag(const char *doc) {
	reset(doc);
	uint32_t off = (uint32_t)(hsize * 3 / 4);
	sbar.value = (int32_t)off;
	body_seek = true;
	defer_body();
	long c0 = chunks;

	// wm asks for a redraw meanwhile: acknowledged before any scan.
	z_msg_t m; memset(&m, 0, sizeof(m));
	m.subject = Z_WM_REDRAW; m.obj.type = Z_UINT32; m.obj.val.uint32 = 0;
	redraw_acks = 0;
	forward_msg(&m, NULL);
	CHECK(redraw_acks == 1, "no redraw ack");
	CHECK(chunks_at_ack - c0 <= 2, "the redraw read %ld chunks before its ack", chunks_at_ack - c0);
	CHECK(body_seek, "the redraw resolved the drag itself");

	c0 = chunks;
	repaint_body();		// the settle deadline
	CHECK(chunks - c0 <= 2, "the drag's repaint read %ld chunks", chunks - c0);
	CHECK(find.kind == FIND_OFF, "a drag into unread ground is not a find");
	CHECK(sbar.value == (int32_t)off, "the thumb jumped back to %d", sbar.value);
	long most = run_find(NULL);
	CHECK(most <= SLICE_MAX, "a slice of the drag read %ld chunks", most);
	long want = 0;
	for (long i = 0; i < nls; i++) if (lstart[i] <= (long)off) want = i;
	CHECK((long)top_line <= want && top_line + 40 > (uint32_t)want,
		"drag to %u landed at line %u, its line is %ld", off, top_line, want);
	check_index("after a drag");
}

static void test_read_error(const char *doc) {
	reset(doc);
	fail_at = hsize / 2;
	handle_key(Z_KEY_END, 0);
	run_find(NULL);
	CHECK(fail_count > 0, "the fault never fired");
	CHECK(!eof_seen, "a failed read set eof_seen (eof_line %u)", eof_line);
	CHECK((long)frontier_off <= fail_at, "the frontier went past the failure");
	CHECK(top_line == 0, "the failed End moved the page to %u", top_line);
	CHECK(strstr(title, "read error"), "title after a failed read: '%s'", title);
	CHECK(hpos == (long)(rbuf_base + rbuf_len), "fs at %ld, the reader's next byte at %u", hpos, rbuf_base + rbuf_len);
	check_index("after a read error");

	// The page still paints, and a seek through the hole is not cached.
	repaint_body();

	// The card reads again: End finds the true end.
	fail_at = -1;
	handle_key(Z_KEY_END, 0);
	run_find(NULL);
	CHECK(eof_seen && (long)eof_line == nls, "after recovery eof_line %u, want %ld", eof_line, nls);
	CHECK(!strstr(title, "read error"), "title still says error: '%s'", title);
	check_index("after recovery");

	// A failure while laying a screen out: not cached, not an end.
	reset(doc);
	handle_key(Z_KEY_END, 0); run_find(NULL);
	top_line = 0; top_sub = 0; pos_valid = false; rd_seek(0);
	rbuf_len = 0; rbuf_base = 0; rbuf_pos = 0; fs_seek(fh, 0);
	// From the first checkpoint: one is always at line 0, and none can
	// lie between it and line 5 on the way.
	fail_at = lstart[2];
	md_state_t st; seek_line(5, &st);
	CHECK(!pos_valid, "a seek through a failed read was cached");
	fail_at = -1;
}

static void test_resize(const char *doc) {
	reset(doc);
	// The first long paragraph's last display line, at a narrow width.
	z_msg_t m; memset(&m, 0, sizeof(m));
	m.subject = Z_WM_WINDOW_RESIZED; m.obj.type = Z_UINT32;
	m.obj.val.uint32 = 120; forward_msg(&m, NULL);
	repaint();
	uint32_t ln = 0; int subs = 1;
	for (uint32_t l = 0; l < 200 && l < (uint32_t)nls; l++) {
		uint32_t used; int s = block_subs(l, &used);
		if (s > subs) { subs = s; ln = l; }
	}
	if (subs < 3) { printf("  (resize: no paragraph long enough in %s)\n", doc); return; }
	top_line = ln; top_sub = (uint32_t)subs - 1;
	repaint();
	CHECK(drawn_valid, "drawn_valid not set by a paint");
	m.obj.val.uint32 = 600; forward_msg(&m, NULL);
	CHECK(!drawn_valid && !cache_valid, "a resize kept the old width's layout");
	uint32_t used; int wide = block_subs(top_line, &used);
	CHECK(top_sub < (uint32_t)wide, "top_sub %u past the block's %d lines", top_sub, wide);
	CHECK(top_line == ln, "a resize moved the line");
}

// A document that draws nothing for a long stretch must still paint
// its first screen, and scroll, reading little.
static void test_first_screen(const char *doc, long max_chunks) {
	reset(doc);
	chunks = 0;
	repaint();
	CHECK(chunks <= max_chunks, "%s: a paint read %ld chunks", doc, chunks);
	CHECK(nsteps > 0, "%s: the first screen is empty", doc);
	chunks = 0;
	key_pend_lines = 1; scroll_flush();
	CHECK(chunks <= max_chunks, "%s: a line down read %ld chunks", doc, chunks);
	printf("  paint    %-28s %d steps, a line down read %ld chunks\n", doc, nsteps, chunks);
}

static void test_launch_off(const char *doc) {
	reset(doc);
	long c0 = chunks;
	find_start(FIND_OFF, (uint32_t)(hsize - 10), 0);
	CHECK(chunks == c0, "starting a find read the file");
	run_find(NULL);
	CHECK(top_line > 0 && (long)top_line < nls, "launch offset landed at %u", top_line);
}

int main(int argc, char **argv) {

	z_scrollbar_init(&sbar, &win, Z_SB_VERT);
	dlg_ctx.parent = &win;

	printf("finds, Esc, read errors, redraw, resize:\n");
	test_end("cases/skiprows.md");
	test_end("cases/front_unterminated.md");
	test_end("cases/empty.md");

	// Each real document given (make findtest: every doc in docs/).
	for (int a = 1; a < argc; a++) {
		const char *big = argv[a];
		reset(big);
		if (hsize < 8192) continue;		// too short to have far to go
		test_end(big);
		test_esc(big);
		test_anchor(big, "#testing", "Testing");
		test_drag(big);
		test_read_error(big);
		test_resize(big);
		test_launch_off(big);
	}
	// skiprows: a screen of its blanks is ~1,800 of its 2,000 lines,
	// so most of the file -- but it stops at the screen, not the file.
	test_first_screen("cases/skiprows.md", 6);
	test_first_screen("cases/front_unterminated.md", 2);
	// A document shorter than a screen asks fs again at its end for
	// each line looked for there: empty reads, cheap. Four of them.
	test_first_screen("cases/fence_info_backtick.md", 4);

	printf("\n%d checks", checks);
	printf(fails ? ", %d FAILED\n" : ", all passed\n", fails);
	return fails != 0;

}
