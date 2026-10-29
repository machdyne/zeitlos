/*
 * Pictures of the REAL read.c: built like rfind_test.c (read.c
 * #included, system calls stubbed), but the drawing calls paint a
 * framebuffer with the real fonts, so a page can be looked at and the
 * drawing it cost counted.
 *
 *   make pic DOC=docs/read_app.md          -> read.pbm, the first page
 *   ./rpic_test DOC W H SKIP FONT OUT.pbm  -> SKIP display lines down
 *                                             (Lnnn: to source line nnn),
 *                                             FONT 6 (6x12) or 5 (5x8)
 *
 * Prints the draw calls the page cost: text runs, glyphs, rectangles.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <time.h>

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
// -- the picture: content area only, 1 = ink --

#define PW_MAX 1024
#define PH_MAX 1024
static uint8_t pic[PH_MAX][PW_MAX];
static long ops_text, ops_glyph, ops_fill;

static void px(int x, int y, int c, const z_clip_t *k) {
	if (x < 0 || y < 0 || x >= content_w || y >= content_h) return;
	if (k && (x < k->x0 || x > k->x1 || y < k->y0 || y > k->y1)) return;
	pic[y][x] = (uint8_t)c;
}
// Glyph cells paint background too, as the blitter does.
void z_fb_draw_text2(int x, int y, const char *s, int fg, int bg, const z_font_t *f, const z_clip_t *k) {
	ops_text++;
	for (; *s; s++, x += f->w) {
		ops_glyph++;
		int g = z_font_index(f, (uint8_t)*s);
		if (g < 0) g = z_font_index(f, Z_GLYPH_MISSING);
		for (int r = 0; r < f->h; r++) {
			uint8_t bits = g < 0 ? 0 : f->glyphs[g * f->h + r];
			for (int c = 0; c < f->w; c++)
				px(x + c, y + r, (bits & (0x80 >> c)) ? fg : bg, k);
		}
	}
}
void z_fb_draw_text(int x, int y, const char *s, int c, const z_font_t *f, const z_clip_t *k) {
	z_fb_draw_text2(x, y, s, c, !c, f, k);
}
void z_fb_hw_fill_rect(int x, int y, int w, int h, int c) {
	ops_fill++;
	for (int j = 0; j < h; j++) for (int i = 0; i < w; i++) px(x + i, y + j, c, NULL);
}
void z_fb_hw_box(int x0, int y0, int x1, int y1, int c, const z_clip_t *k) {
	ops_fill++;
	for (int y = y0; y <= y1; y++) for (int x = x0; x <= x1; x++) px(x, y, c, k);
}
void z_fb_hw_line(int x0, int y0, int x1, int y1, int c, const z_clip_t *k) {
	ops_fill++;
	if (y0 == y1) { for (int x = x0; x <= x1; x++) px(x, y0, c, k); }
	else for (int y = y0; y <= y1; y++) px(x0, y, c, k);
}
void z_fb_hw_fill_pattern(int x, int y, int w, int h, const uint8_t *p) {
	ops_fill++;
	for (int j = 0; j < h; j++) for (int i = 0; i < w; i++)
		px(x + i, y + j, (p[(y + j) & 7] >> (7 - ((x + i) & 7))) & 1, NULL);
}
// The blit, for real: rows of the rectangle move by dy (negative: up),
// the strip scrolled in keeps what it had -- the caller redraws it.
static bool blit_ok;
static long blits;
void z_fb_hw_scroll(int x, int y, int w, int h, int dy) {
	static uint8_t tmp[PH_MAX][PW_MAX];
	blits++;
	for (int r = 0; r < h; r++) memcpy(tmp[r], &pic[y + r][x], (size_t)w);
	for (int r = 0; r < h; r++) {
		int to = r + dy;
		if (to >= 0 && to < h) memcpy(&pic[y + to][x], tmp[r], (size_t)w);
	}
}
bool z_fb_hw_scroll_allowed(int x, int y, int w, int h) { (void)x; (void)y; (void)w; (void)h; return blit_ok; }
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
void z_win_clear(const z_win_t *w) { (void)w; memset(pic, 0, sizeof(pic)); }
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

int main(int argc, char **argv) {

	if (argc < 7) {
		fprintf(stderr, "usage: %s DOC W H SKIP FONT OUT.pbm\n", argv[0]);
		return 2;
	}

	content_w = atoi(argv[2]);
	content_h = atoi(argv[3]);
	if (content_w > PW_MAX) content_w = PW_MAX;
	if (content_h > PH_MAX) content_h = PH_MAX;
	// SKIP: display lines, or Lnnn: down to the block at source line nnn
	int skip = atoi(argv[4]);
	long to_line = (argv[4][0] == 'L') ? atol(argv[4] + 1) : -1;
#ifndef RPIC_OLD
	body_font = (atoi(argv[5]) == 6) ? &z_font_6x12 : &z_font_5x8;
#endif

	z_scrollbar_init(&sbar, &win, Z_SB_VERT);
	dlg_ctx.parent = &win;
	layout();
	if (!open_path(argv[1])) { fprintf(stderr, "cannot open %s\n", argv[1]); return 1; }
	repaint();
	// SKIP "seam": from every start in the first STARTS display lines,
	// scroll forward 1, 2, 3, 5 and 8 lines the way a key does -- a
	// blit and the strip redrawn -- and back the same distances to it,
	// and compare every pixel with a fresh repaint of the same place.
	if (!strcmp(argv[4], "seam")) {
		static uint8_t got[PH_MAX][PW_MAX];
		static const int dist[] = { 1, 2, 3, 5, 8 };
		int starts = argc > 7 ? atoi(argv[7]) : 300, tested = 0, bad = 0;
		blit_ok = true;
		for (int st0 = 0; st0 < starts; st0++)
			for (int d = 0; d < 10; d++) {
				// d < 5: forward from st0; d >= 5: back to st0
				bool back = d >= 5;
				int dd = dist[d % 5];
				int from = back ? st0 + dd : st0;
				top_line = 0; top_sub = 0;
				if (from && scroll_down(from) < from) goto done;
				cache_valid = false; drawn_valid = false; sel_on = false;
				memset(pic, 0, sizeof(pic));
				repaint_body();
				if (back) scroll_backward(dd); else scroll_forward(dd);
				memcpy(got, pic, sizeof(pic));
				uint32_t tl = top_line, ts = top_sub;
				memset(pic, 0, sizeof(pic));
				cache_valid = false; drawn_valid = false;
				repaint_body();
				tested++;
				int row = -1;
				for (int y = 0; y < content_h && row < 0; y++)
					if (memcmp(got[y], pic[y], (size_t)content_w)) row = y;
				if (row >= 0) {
					if (bad < 5) {
						printf("  SEAM %d%c%d (line %u.%u): row %d differs\n", from,
							back ? '-' : '+', dd, tl, ts, row);
						if (bad == 0) {
							FILE *o = fopen(argv[6], "wb");
							fprintf(o, "P1\n%d %d\n", content_w * 2 + 4, content_h);
							for (int y = 0; y < content_h; y++) {
								for (int x = 0; x < content_w; x++) fputc(got[y][x] ? '1' : '0', o);
								fputs("0000", o);
								for (int x = 0; x < content_w; x++) fputc(pic[y][x] ? '1' : '0', o);
								fputc('\n', o);
							}
							fclose(o);
						}
					}
					bad++;
				}
			}
	done:
		printf("%s: %d blit scrolls (down and up), %d differ from a fresh repaint, %ld blits\n",
			argv[1], tested, bad, blits);
		return bad != 0;
	}

	// SKIP "up": the drawing one line up costs, from STARTS lines down,
	// against a full repaint of the same place.
	if (!strcmp(argv[4], "up")) {
		int from = argc > 7 ? atoi(argv[7]) : 40;
		blit_ok = true;
		long tr = 0, tg = 0, tf = 0, rr = 0, rg = 0, rf = 0, n = 0;
		for (int i = 0; i < 40; i++) {
			top_line = 0; top_sub = 0;
			if (scroll_down(from + i) < from + i) break;
			cache_valid = false; drawn_valid = false;
			repaint_body();
			ops_text = ops_glyph = ops_fill = 0;
			scroll_backward(1);
			tr += ops_text; tg += ops_glyph; tf += ops_fill;
			ops_text = ops_glyph = ops_fill = 0;
			cache_valid = false; drawn_valid = false;
			repaint_body();
			rr += ops_text; rg += ops_glyph; rf += ops_fill;
			n++;
		}
		if (n) printf("%s: one line up, average of %ld: %ld text runs, %ld glyphs, %ld rectangles; "
			"a full repaint: %ld, %ld, %ld\n", argv[1], n, tr / n, tg / n, tf / n, rr / n, rg / n, rf / n);
		return 0;
	}

	// SKIP "all": every page to the end, for the cost of the whole
	// document -- draw calls, and host CPU time as a stand-in for the
	// board's.
	if (!strcmp(argv[4], "all")) {
		clock_t c0 = clock();
		int pages = 0;
		ops_text = ops_glyph = ops_fill = 0;
		for (;;) {
			cache_valid = false; drawn_valid = false;
			repaint_body();
			pages++;
			int page = view_h / (BODY_FONT->h + 1);
			if (scroll_down(page) < page || pages > 20000) break;
		}
		printf("%s: %d pages, %ld text runs, %ld glyphs, %ld rectangles, %.1f ms\n",
			argv[1], pages, ops_text, ops_glyph, ops_fill,
			(double)(clock() - c0) * 1000.0 / CLOCKS_PER_SEC);
		return 0;
	}
	if (skip > 0) scroll_down(skip);
	for (int i = 0; to_line > 0 && (long)top_line < to_line && i < 100000; i++)
		if (scroll_down(1) <= 0) break;

	memset(pic, 0, sizeof(pic));
	ops_text = ops_glyph = ops_fill = 0;
	cache_valid = false;
	drawn_valid = false;
	repaint_body();

	printf("%s: %d text runs, %ld glyphs, %ld rectangles\n", argv[1],
		(int)ops_text, ops_glyph, ops_fill);

	FILE *o = fopen(argv[6], "wb");
	if (!o) { perror(argv[6]); return 1; }
	fprintf(o, "P1\n%d %d\n", content_w, content_h);
	for (int y = 0; y < content_h; y++) {
		for (int x = 0; x < content_w; x++) fputc(pic[y][x] ? '1' : '0', o);
		fputc('\n', o);
	}
	fclose(o);
	return 0;

}
