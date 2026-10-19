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
	if (!strncmp(p, "/sys/fonts/", 11)) {
		const char *e = strrchr(p, '.');
		snprintf(q, sizeof(q), "%s/sw/data/font/%s/%s", root, e ? e + 1 : "", p + 11);
	} else snprintf(q, sizeof(q), "%s/%s", root, p);
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
int fs_open_write(const char *p) { (void)p; return -1; }
int fs_read_chunk(int h, void *b, int n) {
	if (h < 0 || h >= 8 || !hf[h]) return -1;
	return (int)fread(b, 1, (size_t)n, hf[h]);
}
int fs_write_chunk(int h, const void *b, int n) { (void)h; (void)b; return n; }
int fs_unlink(char *p) { (void)p; return 0; }
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
int z_dialog_confirm(const z_dialog_ctx_t *c, const char *t, const char *m, int b) {
	(void)c; (void)t; (void)m; (void)b; return 0; }
bool z_fb_hw_blit_mem_available(void) { return true; }
// the kernel's clock (linked with --wrap): a tick a call
uint32_t __wrap_z_uptime_ticks(void) { static uint32_t t; return t++; }

static const char *deck_text =
	"(deck (version 1) (page \"classic\" landscape) (title \"UI\")\n"
	" (background \"plain\" (rect \"band\" (box 0 0 512 34) (fill shade 5) (line 0 16)))\n"
	" (card \"intro\" (background \"plain\")\n"
	"  (text \"title\" (box 0 40 512 50) (font \"serif\" 34 bold) (align center middle) (text \"Slides on Zeitlos\"))\n"
	"  (text \"body\" (box 30 100 260 120) (font \"sans\" 15) (text \"A text box, being typed into. Words wrap at its edge.\"))\n"
	"  (rrect \"panel\" (box 310 100 170 80) (fill pattern 13) (line 2 16) (radius 12))\n"
	"  (oval \"dot\" (box 330 200 90 70) (fill shade 9) (line 2 16))\n"
	"  (line \"arrow\" (box 60 300 180 -40) (line 2 16) (arrows end)))\n"
	" (card \"two\" (background \"plain\") (text \"t\" (box 20 120 470 60) (font \"heavy\" 40) (align center middle) (text \"Second\")))\n"
	" (card \"three\" (oval \"o\" (box 100 80 300 200) (fill pattern 5) (line 3 16)))\n"
	" (card \"four\" (text \"t\" (box 20 120 470 60) (font \"script\" 40) (align center) (text \"The end\")))\n"
	" (card \"pics\" (image \"gone\" (box 30 220 100 80) (src \"/media/images/nosuch.png\") (line 1 16))\n"
	"  (image \"sq\" (box 30 60 220 146) (src \"/media/images/squirrel.jpg\") (line 1 16))\n"
	"  (image \"sq2\" (box 270 60 220 146) (src \"/media/images/squirrel.pgm\") (line 1 16))))\n";

static uint8_t mem[6 << 20];
static bool ck_menu;

static void shot(const char *name) {
	char p[256];
	snprintf(p, sizeof(p), "/tmp/slide_ui_%s.pbm", name);
	z_render_write(p, &win, 2);
	printf("render: wrote %s\n", p);
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
	if (!ck_menu) { printf("render: Esc in the / menu did not go back to the line\n"); return 1; }
	return 0;
}
