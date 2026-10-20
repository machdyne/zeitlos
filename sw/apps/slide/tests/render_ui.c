/*
 * Render slide's window to images, to be LOOKED AT before it reaches a
 * screen (sw/common/tests/zrender.h): the editor with a selection, a
 * text box being typed into, a drag in progress, and the card sorter.
 * slide.c, zwin.c and zwidget.c are the real sources; only the pixel
 * plotting is software.
 *
 *   make -C sw/apps/slide render     -> /tmp/slide_ui_*.pbm
 */

#include "../../../common/tests/zrender.h"

#define main slide_main_unused
#include <sys/stat.h>
#include "../slide.c"
#undef main

// -- what would need wm, the card or the kernel --------------------------------

static const char *root = "../../..";

static void host_path(const char *p, char *q, int n) {
	if (!strncmp(p, "/sys/fonts/", 11)) {
		const char *e = strrchr(p, '.');
		snprintf(q, (size_t)n, "%s/sw/data/font/%s/%s", root, e ? e + 1 : "", p + 11);
	} else if (!strncmp(p, "/media/images/", 14)) {
		// the card's /media/images is sw/data/images and some of sw/data/demo
		FILE *f;
		snprintf(q, (size_t)n, "%s/sw/data/images/%s", root, p + 14);
		if ((f = fopen(q, "rb")) != NULL) fclose(f);
		else snprintf(q, (size_t)n, "%s/sw/data/demo/%s", root, p + 14);
	} else if (!strncmp(p, "/tmp/", 5)) {
		snprintf(q, (size_t)n, "/tmp/slide_ui_tmp/%s", p + 5);
	} else if (!strncmp(p, "/data/automate/", 15)) {
		snprintf(q, (size_t)n, "%s/sw/data/demo/%s", root, p + 15);
	} else if (!strncmp(p, "/data/slide/", 12)) {
		snprintf(q, (size_t)n, "%s/sw/data/slide/%s", root, p + 12);
	} else snprintf(q, (size_t)n, "%s/%s", root, p);
}

int fs_size(char *p) {
	char q[512];
	FILE *f;
	long n;
	host_path(p, q, sizeof(q));
	f = fopen(q, "rb");
	if (!f) return 0;
	fseek(f, 0, SEEK_END);
	n = ftell(f);
	fclose(f);
	return (int)n;
}

int fs_read_file(char *p, char *buf, int max) {
	char q[512];
	FILE *f;
	int n;
	host_path(p, q, sizeof(q));
	f = fopen(q, "rb");
	if (!f) return -1;
	n = (int)fread(buf, 1, (size_t)max, f);
	fclose(f);
	return n;
}

static FILE *hf[8];
int fs_open_read(const char *p) {
	char q[512];
	int i;
	host_path(p, q, sizeof(q));
	for (i = 0; i < 8; i++) if (!hf[i]) { hf[i] = fopen(q, "rb"); return hf[i] ? i : -1; }
	return -1;
}
int fs_open_write(const char *p) {
	// only /tmp is written (the demo's phone book): to /tmp/slide_ui_tmp
	char q[512];
	int i;
	if (strncmp(p, "/tmp/", 5)) return -1;
	host_path(p, q, sizeof(q));
	mkdir("/tmp/slide_ui_tmp", 0777);
	for (i = 0; i < 8; i++) if (!hf[i]) { hf[i] = fopen(q, "wb"); return hf[i] ? i : -1; }
	return -1;
}
int fs_read_chunk(int h, void *b, int n) {
	if (h < 0 || h >= 8 || !hf[h]) return -1;
	return (int)fread(b, 1, (size_t)n, hf[h]);
}
int fs_write_chunk(int h, const void *b, int n) {
	if (h < 0 || h >= 8 || !hf[h]) return n;
	return (int)fwrite(b, 1, (size_t)n, hf[h]);
}
int fs_unlink(char *p) {
	char q[512];
	if (strncmp(p, "/tmp/", 5)) return 0;
	host_path(p, q, sizeof(q));
	return remove(q) == 0;
}
int fs_write_file(char *p, char *b, int n) { (void)p; (void)b; return n; }
int fs_close_handle(int h) {
	if (h >= 0 && h < 8 && hf[h]) { fclose(hf[h]); hf[h] = NULL; }
	return 1;
}
int fs_seek(int h, uint32_t o) { (void)h; (void)o; return -1; }

bool z_dialog_open(const z_dialog_ctx_t *c, const char *d, char *o, int n) {
	(void)c; (void)d; (void)o; (void)n; return false; }
bool z_dialog_save(const z_dialog_ctx_t *c, const char *d, const char *s, char *o, int n) {
	(void)c; (void)d; (void)s; (void)o; (void)n; return false; }
void z_dialog_help(const z_dialog_ctx_t *c, const char *t, const char *x, const char *d) {
	(void)c; (void)t; (void)x; (void)d; }
static int confirm_answer;		// what the next question is answered (0: Cancel)
int z_dialog_confirm(const z_dialog_ctx_t *c, const char *t, const char *m, int b) {
	(void)c; (void)t; (void)m; (void)b; return confirm_answer; }
bool z_fb_hw_blit_mem_available(void) { return true; }
// the kernel's clock (linked with --wrap): a tick a call
uint32_t __wrap_z_uptime_ticks(void) { static uint32_t t; return t++; }
// no wm to ask: the window keeps its size
// no wm to ask: the window keeps its size, unless resizing is asked for
// (appstart: the window becomes what slide asks, as wm would make it)
static bool resize_for_real;
z_rv __wrap_z_win_resize(z_win_t *w, uint32_t cw, uint32_t ch) {
	if (resize_for_real) { w->w = cw; w->h = ch; }
	return Z_OK;
}
void __wrap_z_win_set_title(const z_win_t *w, const char *t) { (void)w; (void)t; }

static const char *deck_text =
	"(deck (version 1) (page \"classic\" landscape) (title \"UI\")\n"
	" (background \"plain\" (rect \"band\" (box 0 0 512 34) (fill shade 5) (line 0 16)))\n"
	" (card \"intro\" (background \"plain\")\n"
	"  (text \"title\" (box 0 40 512 50) (font \"serif\" 34 bold) (align center middle) (text \"Slides on Zeitlos\"))\n"
	"  (text \"body\" (box 30 100 260 120) (font \"sans\" 15) (text \"A text box, being typed into. Words wrap at its edge.\"))\n"
	"  (rrect \"panel\" (box 310 100 170 80) (fill pattern 13) (line 2 16) (radius 12))\n"
	"  (oval \"dot\" (box 330 200 90 70) (fill shade 9) (line 2 16))\n"
	"  (line \"arrow\" (box 60 300 180 -40) (line 2 16) (arrows end)))\n"
	" (card \"flow\"\n"
	"  (pill \"start\" (box 30 40 110 44) (font \"sans\" 14) (text \"Start\"))\n"
	"  (slant \"in\" (box 30 120 110 50) (font \"sans\" 14) (text \"Read it\"))\n"
	"  (diamond \"ok\" (box 190 110 120 70) (fill pattern 2) (line 1 16) (font \"sans\" 14) (text \"OK?\"))\n"
	"  (document \"doc\" (box 370 40 110 54) (font \"sans\" 14) (text \"Report\"))\n"
	"  (cylinder \"db\" (box 370 190 110 60) (font \"sans\" 14) (text \"Store\"))\n"
	"  (line \"c1\" (box 0 0 1 1) (line 1 16) (arrows end) (connect \"start\" \"in\"))\n"
	"  (poly \"c2\" (box 0 0 1 1) (line 1 16) (arrows end) (points 0 0 1 0 1 1 2 1) (connect \"in\" \"ok\"))\n"
	"  (poly \"c3\" (box 0 0 1 1) (line 1 16) (arrows end) (points 0 0 1 0 1 1 2 1) (connect \"ok\" \"doc\"))\n"
	"  (line \"c4\" (box 0 0 1 1) (line 1 16) (arrows end) (connect \"ok\" \"db\"))\n"
	"  (poly \"wave\" (box 40 260 200 50) (line 2 16) (smooth) (points 40 300 90 260 140 300 190 260 240 300)))\n"
	" (card \"two\" (background \"plain\") (text \"t\" (box 20 120 470 60) (font \"heavy\" 40) (align center middle) (text \"Second\")))\n"
	" (card \"three\" (oval \"o\" (box 100 80 300 200) (fill pattern 5) (line 3 16)))\n"
	" (card \"four\" (text \"t\" (box 20 120 470 60) (font \"script\" 40) (align center) (text \"The end\")))\n"
	" (card \"pics\" (image \"gone\" (box 30 220 100 80) (src \"/media/images/nosuch.png\") (line 1 16))\n"
	"  (image \"sq\" (box 30 60 220 146) (src \"/media/images/squirrel.jpg\") (line 1 16))\n"
	"  (image \"sq2\" (box 270 60 220 146) (src \"/media/images/squirrel.pgm\") (line 1 16)))\n"
	" (card \"form\" (background \"plain\")\n"
	"  (text \"q\" (box 20 40 470 30) (font \"serif\" 22 bold) (text \"A small form\"))\n"
	"  (text \"l1\" (box 20 84 80 22) (font \"sans\" 14) (align left middle) (text \"Name\"))\n"
	"  (field \"name\" (box 100 84 200 24) (font \"sans\" 14) (text \"Ada\"))\n"
	"  (check \"news\" (box 100 120 200 18) (font \"sans\" 14) (align left middle) (text \"Send me news\") (value 1))\n"
	"  (radio \"tea\" (box 100 146 120 18) (font \"sans\" 14) (align left middle) (text \"Tea\") (value 1))\n"
	"  (radio \"cof\" (box 100 168 120 18) (font \"sans\" 14) (align left middle) (text \"Coffee\") (value 0))\n"
	"  (slider \"vol\" (box 100 200 200 18) (value 70) (range 0 100))\n"
	"  (list \"col\" (box 330 84 140 100) (font \"sans\" 14) (pad 3) (text \"Red\\nGreen\\nBlue\\nYellow\") (value 1))\n"
	"  (button \"ok\" (box 330 240 90 26) (radius 6) (font \"sans\" 15) (align center middle) (text \"OK\") (action \"(go 'first)\"))\n"
	"  (button \"no\" (box 220 240 90 26) (radius 6) (font \"sans\" 15) (align center middle) (text \"Cancel\"))))\n";

static uint8_t mem[6 << 20];

// a drag in the window's content, as automate's pointer makes it
static void demo_drag(int x1, int y1, int x2, int y2) {
	edit_press(&ed, x1 - card_x, y1 - card_y, false, false);
	edit_motion(&ed, (x1 + x2) / 2 - card_x, (y1 + y2) / 2 - card_y, false);
	edit_motion(&ed, x2 - card_x, y2 - card_y, false);
	edit_release(&ed, x2 - card_x, y2 - card_y);
	update_card();
}
static bool ck_menu;

static void shot(const char *name) {
	char p[256];
	snprintf(p, sizeof(p), "/tmp/slide_ui_%s.pbm", name);
	z_render_write(p, &win, 2);
	printf("render: wrote %s\n", p);
}

// -- what a click on the calculator costs (valgrind --tool=callgrind,
// make bench-click): bench_* are the units callgrind counts --

static void click_at(int x, int y, int b) {
	z_clip_t c;
	z_win_content_rect(&win, &c);
	handle_mouse(Z_WM_PACK_MOUSE(c.x0 + x, c.y0 + y, b, 1));
}

__attribute__((noinline)) void bench_start(void) { pl_check_card(); }
__attribute__((noinline)) void bench_render(void) { edit_damage_all(&ed); update_card(); }
__attribute__((noinline)) void bench_press(int x, int y) { click_at(x, y, Z_MOUSE_BTN_LEFT); }
__attribute__((noinline)) void bench_release(int x, int y) { click_at(x, y, 0); }
__attribute__((noinline)) void bench_eval(void) { script_eval(ed.d, ed.cur, "(digit \"5\")", NULL, 0); }

// What was drawn a part at a time is what the whole card is.
static bool same_as_whole(const char *what) {
	size_t n = (size_t)bwpl * (size_t)bh * 4, i;
	uint32_t *was = malloc(n);
	int diff = 0;
	memcpy(was, cbuf, n);
	edit_damage_all(&ed);
	update_card();
	for (i = 0; i < n / 4; i++) if (was[i] != cbuf[i]) diff++;
	free(was);
	if (diff) printf("render: %s: drawn in parts, %d words differ from the whole card\n", what, diff);
	return diff == 0;
}

static int bench(void) {
	static const int keys[][2] = { { 163, 232 }, { 225, 232 }, { 349, 176 }, { 287, 232 }, { 163, 176 }, { 287, 288 } };
	unsigned i;
	if (!load_path("sw/data/slide/examples/calculator.zsl")) { printf("bench: no calculator\n"); return 1; }
	if (!app_mode) { printf("bench: the calculator did not open as an app\n"); return 1; }
	z_render_clear();
	repaint();
	bench_start();
	bench_render();
	bench_eval();
	for (i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
		bench_press(keys[i][0], keys[i][1]);
		bench_release(keys[i][0], keys[i][1]);
	}
	{
		obj_t *dsp = act_find(ed.d, ed.cur, "d");
		printf("bench: the display says %s\n", dsp && dsp->text ? dsp->text : "?");
		if (!dsp || !dsp->text || strcmp(dsp->text, "17408")) return 1;
	}
	// what the clicks drew, a part at a time, is what the whole card is
	{
		size_t n = (size_t)bwpl * (size_t)bh * 4;
		uint32_t *was = malloc(n);
		int diff = 0;
		size_t i;
		memcpy(was, cbuf, n);
		edit_damage_all(&ed);
		update_card();
		for (i = 0; i < n / 4; i++) if (was[i] != cbuf[i]) diff++;
		free(was);
		if (diff) { printf("bench: drawn in parts, %d words differ from the whole card\n", diff); return 1; }
		printf("bench: drawn in parts == drawn whole\n");
	}
	z_render_clear();
	repaint();
	shot("calc");
	return 0;
}

int main(int argc, char **argv) {
	static uint8_t scr[96 * 1024], gc[128 * 1024];
	deck_t *d;
	char err[80];
	int ww, wh;
	if (argc > 1) root = argv[1];
	pool_init(mem, sizeof(mem));
	render_init(scr, sizeof(scr), load_file);
	zc_text_cache(gc, sizeof(gc));
	make_icons();
	show_alloc();
	d = deckio_read(deck_text, (uint32_t)strlen(deck_text), err, sizeof(err));
	if (!d) { printf("deck: %s\n", err); return 1; }
	g_deck = d;
	edit_init(&ed, d);
	ww = TOOL_W + 2 * MARGIN + d->page.gw + Z_WIN_CHROME_W;
	wh = 2 * MARGIN + d->page.gh + BAR_H + Z_WIN_CHROME_H;
	if (!z_render_open(&win, ww, wh)) { printf("render: cannot map VRAM\n"); return 77; }
	dlg.parent = &win;
	act_host_editor();
	if (argc > 2 && !strcmp(argv[2], "bench")) return bench();
	if (argc > 2 && !strcmp(argv[2], "appstart")) {
		// slide started on an app deck, as `run slide calculator.zsl`:
		// the window takes the card's size
		resize_for_real = true;
		if (!load_path("/data/slide/examples/calculator.zsl")) return 1;
		layout();
		repaint();
		printf("appstart: content %dx%d card at %d,%d\n", z_win_content_w(&win), z_win_content_h(&win), card_x, card_y);
		pl_check_card();
		pl_sync();
		click_at(card_x + 163, card_y + 232, Z_MOUSE_BTN_LEFT);
		click_at(card_x + 163, card_y + 232, 0);
		apply_dirty();
		printf("appstart: display %s\n", act_find(ed.d, ed.cur, "d")->text);
		z_render_clear();
		repaint();
		shot("appstart");
		return 0;
	}

	// the editor: a selected shape
	edit_select(&ed, d->card[0]->obj[3], false);
	repaint();
	shot("select");

	// typing into the body text
	z_render_clear();
	edit_select(&ed, d->card[0]->obj[1], false);
	edit_text_begin(&ed, d->card[0]->obj[1], 100, 120);
	repaint();
	shot("text");
	edit_text_end(&ed);

	// a rectangle being drawn, and the command line open
	z_render_clear();
	ed.tool = TOOL_RECT;
	edit_press(&ed, 40, 230, false, false);
	edit_motion(&ed, 200, 290, false);
	mode = MODE_COMMAND;
	z_edit_init(&cmd_field, cmd_buf, sizeof(cmd_buf), "fill shade 6");
	cmd_field.focus = true;
	repaint();
	shot("drag");
	edit_cancel(&ed);
	mode = MODE_EDIT;

	// the sorter
	z_render_clear();
	ed.tool = TOOL_SELECT;
	mode = MODE_SORTER;
	sort_sel = 1;
	repaint();
	shot("sorter");

	// zoomed in to 200% on the selected oval: scrollbars, the view
	// centred on it; then scrolled with the wheel's code path
	z_render_clear();
	mode = MODE_EDIT;
	ed.tool = TOOL_SELECT;
	edit_goto(&ed, 0);
	edit_select(&ed, d->card[0]->obj[3], false);
	handle_key('=', Z_KBD_MOD_CTRL);		// the window fits it at 100%: one step is 200%
	repaint();
	shot("zoom");
	if (zoom != 200 || !sb_h_on || !sb_v_on) { printf("render: Ctrl+= is not 200%% with scrollbars (%d)\n", zoom); return 1; }
	{
		// zooming on the selection keeps it in view
		int x0, y0, x1, y1, cx, cy;
		obj_bounds(d->card[0]->obj[3], &x0, &y0, &x1, &y1);
		cx = to_px((x0 + x1) / 2) - scroll_x;
		cy = to_px((y0 + y1) / 2) - scroll_y;
		if (cx < 0 || cx >= view_w || cy < 0 || cy >= view_h) { printf("render: zoom lost the selection\n"); return 1; }
	}
	scroll_to(0, 0);
	z_render_clear();
	repaint();
	shot("zoom_top");
	handle_key('0', Z_KBD_MOD_CTRL);
	if (zoom || sb_h_on || sb_v_on || scroll_x || scroll_y) { printf("render: Ctrl+0 did not fit the card again\n"); return 1; }
	edit_select_none(&ed);

	// the / menu over the card: F4, then /
	z_render_clear();
	mode = MODE_EDIT;
	edit_goto(&ed, 0);
	open_prompt("");
	handle_key('/', 0);
	handle_key(Z_KEY_DOWN, 0);
	handle_key(Z_KEY_DOWN, 0);
	repaint();
	shot("menu");
	handle_key(0x1b, 0);		// the menu goes
	ck_menu = !menu_on && mode == MODE_COMMAND;
	handle_key(0x1b, 0);		// and the line

	// a card of pictures, part loaded: one done, one loading, one missing
	z_render_clear();
	edit_goto(&ed, d->ncard - 1);
	repaint();
	{
		bool fin;
		int i;
		while (render_loading()) {
			obj_t *o = render_load_step(0, &fin);
			if (fin) picture_done(o);
			if (o && !fin && !strcmp(o->name, "sq2")) break;
		}
		for (i = 0; i < 30; i++) render_load_step(0, &fin);
		// one picture loaded, the next part way (a slice at a time), the
		// third missing (the harness's screen is cleared: its blits only
		// add to what is there)
		z_render_clear();
		repaint();
		load_status();
	}
	shot("loading");

	// a form of controls, in the editor (the select tool, OK selected)
	z_render_clear();
	edit_goto(&ed, d->ncard - 1);
	ed.tool = TOOL_SELECT;
	edit_select(&ed, card_find_obj(d->card[d->ncard - 1], "ok"), false);
	repaint();
	shot("controls");

	// the same deck as an app (F7): no tools, no bar; typing into the
	// name field, Cancel held down
	z_render_clear();
	handle_key(Z_KEY_F7, 0);
	if (!app_mode) { printf("render: F7 did not run the deck as an app\n"); return 1; }
	pl_check_card();
	pl_sync();
	{
		bool took, r;
		player_press(&pl, 150, 90, &took);			// into the name field
		player_key(&pl, '!', &r);
		player_press(&pl, 260, 250, &took);			// Cancel, held
		if (!took || !pl.pressed) { printf("render: Cancel did not take the press\n"); return 1; }
	}
	z_render_clear();
	repaint();
	shot("app");
	{
		bool took;
		player_release(&pl, 260, 250);
		player_press(&pl, 340, 250, &took);			// OK: (go 'first)
		player_release(&pl, 340, 250);
		if (ed.cur != 0) { printf("render: OK's action did not go to the first card (%d)\n", ed.cur); return 1; }

	}
	handle_key(Z_KEY_F7, 0);
	if (app_mode) { printf("render: F7 again did not go back to editing\n"); return 1; }
	

	// a flowchart: shapes with words, connectors that follow them, a
	// smooth line, a drawing imported from SVG (grouped, selected)
	z_render_clear();
	edit_goto(&ed, 1);
	ed.tool = TOOL_SELECT;
	{
		static const char svg[] =
			"<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 600 400'>"
			"<g transform='translate(330 250) scale(0.5)'>"
			"<circle cx='100' cy='100' r='90' fill='#c0c0c0' stroke='black' stroke-width='4'/>"
			"<path d='M60 70 a10 10 0 1 0 0.1 0 Z M140 70 a10 10 0 1 0 0.1 0 Z' fill='black'/>"
			"<path d='M50 120 Q100 180 150 120' fill='none' stroke='black' stroke-width='6'/>"
			"</g></svg>";
		int made = edit_import_svg(&ed, svg, sizeof(svg) - 1);
		if (made < 4) { printf("render: the SVG import made %d shapes\n", made); return 1; }
	}
	repaint();
	shot("diagrams");

	// what sw/data/demo/slide.zds does, step for step, at the same
	// places in the window's content: the editor's card at (30, 6)
	if (!load_path("/data/automate/slide_demo.zsl") || app_mode) { printf("render: the demo deck did not open\n"); return 1; }
	z_render_clear();
	mode = MODE_EDIT;
	if (card_x != 30 || card_y != 6 || scale16 != ZC_XF_ONE) {
		printf("render: the demo expects the card at (30, 6) at 100%%, not (%d, %d) at %d/65536\n", card_x, card_y, (int)scale16);
		return 1;
	}
	{
		card_t *k;
		obj_t *pill, *dia, *cyl;
		int n = 0, i, before[4];
		handle_key(13, Z_KBD_MOD_CTRL);		// Ctrl+M as wm sends it: its control code
		if (ed.cur != 1 || edit_card(&ed)->nobj) { printf("render: Ctrl+M did not make a new, empty card\n"); return 1; }
		handle_key('v', 0);
		run_command("pill Start");
		demo_drag(286, 177, 120, 66);
		run_command("diamond Ready?");
		run_command("fill 2");
		run_command("cylinder Saved");
		demo_drag(286, 177, 450, 276);
		handle_key('a', 0);
		demo_drag(120, 66, 286, 177);
		demo_drag(286, 177, 450, 276);
		k = edit_card(&ed);
		pill = card_find_obj(k, "pill 1");
		dia = card_find_obj(k, "diamond 1");
		cyl = card_find_obj(k, "cylinder 1");
		for (i = 0; i < k->nobj; i++) if (obj_is_connector(k->obj[i])) n++;
		if (!pill || !dia || !cyl || pill->x != 90 - 55 || cyl->x != 420 - 55 || n != 2) {
			printf("render: the demo's flowchart is not what it shows (%p %p %p, %d connectors)\n",
				(void *)pill, (void *)dia, (void *)cyl, n);
			for (i = 0; i < k->nobj; i++) printf("  %s %d,%d %dx%d\n", k->obj[i]->name, k->obj[i]->x, k->obj[i]->y, k->obj[i]->w, k->obj[i]->h);
			return 1;
		}
		for (i = 0; i < k->nobj && i < 4; i++) before[i] = k->obj[k->nobj - 1]->x + k->obj[k->nobj - 1]->y;
		handle_key('v', 0);
		demo_drag(286, 177, 330, 120);		// the connectors follow
		if (k->obj[k->nobj - 1]->x + k->obj[k->nobj - 1]->y == before[0]) {
			printf("render: the connector did not follow the diamond\n");
			return 1;
		}
		demo_drag(450, 276, 400, 250);
		handle_key(0x1b, 0);
		if (ed.nsel) { printf("render: Esc did not let the selection go\n"); return 1; }
		run_command("transition iris 600");
		// the pitch, printed to a PDF named on the line (no dialog)
		run_command("go pitch");
		if (strcmp(edit_card(&ed)->name, "pitch") || ed.cur != 2) { printf("render: /go pitch is card %d\n", ed.cur); return 1; }
		{
			char l[200], f[200];
			copy_str(l, sizeof(l), "print this /tmp/pitch.pdf");
			if (!print_file_word(l, f, sizeof(f)) || strcmp(f, "/tmp/pitch.pdf") || strcmp(l, "print this pdf")) {
				printf("render: /print this /tmp/pitch.pdf read as \"%s\" to \"%s\"\n", l, f);
				return 1;
			}
		}
	}
	z_render_clear();
	repaint();
	shot("demo");

	// the show draws the next card ahead, an object at a time: the same
	// pixels as drawing it at once, on a stack of its own that is big enough
	{
		size_t used = 0;
		int k, y;
		for (k = 0; k + 1 < ed.d->ncard; k++) {
			y = show_selftest(ed.d, k, &used);
			if (y < 0) { printf("render: the next card drawn ahead (after %d) is not the card\n", k); return 1; }
			if (used > 16 * 1024 / 2) { printf("render: drawing ahead used %u bytes of its 16 KB stack\n", (unsigned)used); return 1; }
		}
		printf("render: cards drawn ahead == drawn at once (stack %u bytes)\n", (unsigned)used);
	}

	// ... and the app the demo builds: a phone book, in a new deck
	// ("Save first?" No), saved to /tmp, run with F7, used
	z_render_clear();
	confirm_answer = Z_DIALOG_NO;
	handle_key(14, Z_KBD_MOD_CTRL);			// Ctrl+N
	confirm_answer = 0;
	if (ed.d->ncard != 1 || edit_card(&ed)->nobj || path[0]) { printf("render: Ctrl+N did not start a new deck\n"); return 1; }
	{
		card_t *k;
		obj_t *o;
		FILE *f;
		char saved[4096];
		size_t n;
		handle_key('t', 0);
		demo_drag(50, 21, 522, 71);				// a text box across the top
		{
			const char *t = "Phone book";
			while (*t) handle_key((uint32_t)*t++, 0);
		}
		handle_key(0x1b, 0);
		handle_key('v', 0);
		demo_drag(286, 46, 286, 46);			// select it again
		run_command("font serif 28");
		run_command("align center middle");
		handle_key('t', 0);						// the fields' labels
		demo_drag(70, 62, 250, 82);
		{ const char *t = "Name"; while (*t) handle_key((uint32_t)*t++, 0); }
		handle_key(0x1b, 0);
		handle_key('t', 0);
		demo_drag(70, 110, 250, 130);
		{ const char *t = "Phone"; while (*t) handle_key((uint32_t)*t++, 0); }
		handle_key(0x1b, 0);
		handle_key('v', 0);
		run_command("field");
		demo_drag(286, 177, 160, 94);
		run_command("name name");
		run_command("field");
		demo_drag(286, 177, 160, 142);
		run_command("name phone");
		run_command("button Add");
		demo_drag(286, 177, 115, 178);
		run_command("name add");
		run_command("action (add-item \"book\" (take-text \"name\") \"  \" (take-text \"phone\")) (save-deck)");
		run_command("button Delete");
		demo_drag(286, 177, 211, 178);
		run_command("name delete");
		run_command("action (remove-item \"book\") (save-deck)");
		run_command("list Ada Lovelace  1815, Alan Turing  1912");
		demo_drag(286, 177, 340, 126);
		demo_drag(410, 166, 500, 306);			// its corner: larger
		run_command("name book");
		handle_key(0x1b, 0);
		run_command("app on");
		// a phone book from a run before: replaced (the demo answers y)
		confirm_answer = Z_DIALOG_YES;
		run_command("save as /tmp/phonebook.zsl");
		confirm_answer = 0;
		k = edit_card(&ed);
		o = card_find_obj(k, "book");
		if (!card_find_obj(k, "name") || !card_find_obj(k, "phone") || !card_find_obj(k, "add") ||
			!card_find_obj(k, "delete") || !o || o->x != 240 || o->y != 80 || o->w != 230 || o->h != 220 ||
			strcmp(path, "/tmp/phonebook.zsl")) {
			int i;
			printf("render: the phone book is not as the demo builds it (saved as \"%s\")\n", path);
			for (i = 0; i < k->nobj; i++) printf("  %s %d,%d %dx%d\n", k->obj[i]->name, k->obj[i]->x, k->obj[i]->y, k->obj[i]->w, k->obj[i]->h);
			return 1;
		}
		z_render_clear();
		repaint();
		shot("demo_build");
		// run it: the card at (0, 0) of the app's window (here offset)
		handle_key(Z_KEY_F7, 0);
		if (!app_mode) { printf("render: F7 did not run the phone book\n"); return 1; }
		pl_check_card();
		pl_sync();
		click_at(card_x + 130, card_y + 88, Z_MOUSE_BTN_LEFT); click_at(card_x + 130, card_y + 88, 0);
		{ const char *t = "Grace Hopper"; while (*t) handle_key((uint32_t)*t++, 0); }
		click_at(card_x + 130, card_y + 136, Z_MOUSE_BTN_LEFT); click_at(card_x + 130, card_y + 136, 0);
		{ const char *t = "1906"; while (*t) handle_key((uint32_t)*t++, 0); }
		click_at(card_x + 85, card_y + 172, Z_MOUSE_BTN_LEFT); click_at(card_x + 85, card_y + 172, 0);
		if (strcmp(o->text, "Ada Lovelace  1815\nAlan Turing  1912\nGrace Hopper  1906") ||
			card_find_obj(k, "name")->text[0]) {
			printf("render: Add made \"%s\"\n", o->text);
			return 1;
		}
		click_at(card_x + 300, card_y + 91, Z_MOUSE_BTN_LEFT); click_at(card_x + 300, card_y + 91, 0);
		if (o->value != 0) { printf("render: the click did not choose Ada (%d)\n", o->value); return 1; }
		click_at(card_x + 181, card_y + 172, Z_MOUSE_BTN_LEFT); click_at(card_x + 181, card_y + 172, 0);
		if (strcmp(o->text, "Alan Turing  1912\nGrace Hopper  1906")) { printf("render: Delete left \"%s\"\n", o->text); return 1; }
		if (!same_as_whole("the phone book")) return 1;
		// and the deck on the disk has the records
		f = fopen("/tmp/slide_ui_tmp/phonebook.zsl", "rb");
		n = f ? fread(saved, 1, sizeof(saved) - 1, f) : 0;
		if (f) fclose(f);
		saved[n] = 0;
		if (!strstr(saved, "Grace Hopper  1906") || strstr(saved, "Ada Lovelace") || !strstr(saved, "(app)")) {
			printf("render: the saved phone book does not hold its records\n");
			return 1;
		}
		z_render_clear();
		repaint();
		shot("demo_app");
		handle_key(Z_KEY_F7, 0);
	}

	// the tour's controls: a check that shows a text, radios, a slider
	{
		static const int at[][2] = { { 295, 89 }, { 300, 139 }, { 390, 139 }, { 350, 169 }, { 330, 200 } };
		unsigned k;
		if (!load_path("sw/data/slide/tour.zsl")) { printf("render: no tour\n"); return 1; }
		for (k = 0; k < (unsigned)ed.d->ncard; k++) if (!strcmp(ed.d->card[k]->name, "controls")) break;
		edit_goto(&ed, (int)k);
		ed.tool = TOOL_BROWSE;
		pl_check_card();
		pl_sync();
		edit_damage_all(&ed);
		update_card();
		for (k = 0; k < sizeof(at) / sizeof(at[0]); k++) {
			bool took;
			player_press(&pl, at[k][0], at[k][1], &took);
			apply_dirty();
			player_release(&pl, at[k][0], at[k][1]);
			apply_dirty();
		}
		{
			obj_t *sec = act_find(ed.d, ed.cur, "secret");
			if (!sec || (sec->flags & OBF_HIDDEN)) { printf("render: the check did not show the secret\n"); return 1; }
		}
		if (!same_as_whole("the tour's controls")) return 1;
		ed.tool = TOOL_SELECT;
	}
	if (!ck_menu) { printf("render: Esc in the / menu did not go back to the line\n"); return 1; }
	return 0;
}
