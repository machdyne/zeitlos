/*
 * Host test for sw/common/zdialog.c's text prompt and help box: the
 * REAL key and mouse handlers, driven directly, with no window and no
 * wm. HELP_PBM=<file> also draws a help box into a PBM to look at.
 *
 *   sudo sysctl -w vm.mmap_min_addr=0      # see ztramp.h
 *   cc -std=gnu99 -Wall -no-pie -I sw/common -o /tmp/test_dialog \
 *      sw/common/tests/test_dialog.c sw/common/zwin.c \
 *      sw/common/zwidget.c sw/common/zflist.c sw/common/zedit.c \
 *      sw/common/zfsapp.c sw/common/zfont_data.c sw/common/zobj.c \
 *      sw/common/zeitlos.c sw/common/tests/zspeak_stub.c
 *   /tmp/test_dialog
 *
 * Exit 0 pass, 1 fail, 77 skipped.
 *
 * -- the bug this exists for --
 *
 * A prompt used to start with its OK button focused, and dlg_key()
 * presses the focused button on Space. So typing a space in the field
 * submitted everything typed so far: "port " came back from the
 * prompt as "port", and sw/apps/settings rejected it mid-edit. Every
 * z_dialog_prompt() caller had the same bug; no test reached it,
 * because the apps' own tests replace the dialog with a scripted
 * answer. This drives the dialog itself.
 *
 * The setup is prompt_setup(), the same function z_dialog_prompt()
 * uses, so the state tested is the state a real prompt starts in.
 * dlg_run() and dlg_create() -- the parts that need wm -- are the only
 * parts not exercised.
 */

#include "zrender.h"
#include "ztramp.h"

// A big folder: the dialog's own fs and arena are stood in for below.
#define ZDIALOG_PAGES
#define z_arena_set  test_arena_set
#define z_arena_get  test_arena_get
#include <stdlib.h>
#include "../zfsc.h"
#include "../zeitlos.h"
int32_t test_arena_set(uint32_t size, void **base, uint32_t *size_out);
uint32_t test_arena_get(void **base);

#include "../zdialog.c"

// -- a fake fs: "/big" holds BIG_N files, listed a page at a time --

#define BIG_N 3000
static int big_at;

int32_t zfs_opendir_at(zfs_dir_t *d, const char *path, void *buf, uint32_t cap) {
	memset(d, 0, sizeof(*d));
	d->buf = buf;
	d->cap = cap;
	big_at = strcmp(path, "/big") ? BIG_N : 0;
	return 0;
}

const z_fs_dirent_t *zfs_readdir(zfs_dir_t *d) {
	if (big_at >= BIG_N) return NULL;
	z_fs_dirent_t *e = (z_fs_dirent_t *)d->buf;
	// in no particular order, as a FAT directory is
	int k = (big_at * 1777) % BIG_N;
	memset(&e->info, 0, sizeof(e->info));
	e->namelen = (uint16_t)snprintf(e->name, 32, "F%04d.TXT", k);
	big_at++;
	return e;
}

int32_t zfs_closedir_at(zfs_dir_t *d) { (void)d; return 0; }

// -- a fake arena: every resize moves it, the old copy scribbled on --

static uint8_t *ar_base;
static uint32_t ar_size;
static int ar_moves;

int32_t test_arena_set(uint32_t size, void **base, uint32_t *size_out) {
	if (size == Z_ARENA_QUERY) {
		if (base) *base = ar_base;
		if (size_out) *size_out = ar_size;
		return Z_ARENA_OK;
	}
	uint8_t *n = size ? malloc(size) : NULL;
	if (n && ar_base) memcpy(n, ar_base, size < ar_size ? size : ar_size);
	if (ar_base) { memset(ar_base, 0xA5, ar_size); free(ar_base); ar_moves++; }
	ar_base = n;
	ar_size = size;
	if (base) *base = n;
	if (size_out) *size_out = size;
	return Z_ARENA_OK;
}

uint32_t test_arena_get(void **base) {
	if (base) *base = ar_base;
	return ar_size;
}

static void *moved_to;
static int moved_calls;
static void on_moved(void *base, void *user) { (void)user; moved_to = base; moved_calls++; }

static void expect(bool ok, const char *what);

// The list through a whole folder: sorted, complete, and the app's
// arena as it was afterwards.
static void big_folder(z_win_t *scratch, bool app_arena) {

	z_dialog_ctx_t ctx;
	memset(&ctx, 0, sizeof(ctx));
	if (app_arena) {
		test_arena_set(100, NULL, NULL);
		memset(ar_base, 0x5A, 100);
		ctx.arena_moved = on_moved;
	}
	moved_calls = 0;

	memset(&dlg, 0, sizeof(dlg));
	dlg.kind = DLG_KIND_OPEN;
	dlg.win = *scratch;
	z_flist_init(&dlg_flist, &dlg.win);
	z_flist_set_geom(&dlg_flist, DLG_MARGIN, 20, 180, 60);
	ds_setup(&ctx);
	z_flist_chdir(&dlg_flist, "/big");
	expect(dlg_flist.loading, "a big folder is read in pages");
	expect(dlg_flist.count > 0 && dlg_flist.count < BIG_N, "the first page is shown at once");
	int steps = 0;
	while (dlg_flist.loading && steps < 1000) { z_flist_load_step(&dlg_flist, 64); steps++; }
	expect(dlg_flist.count == BIG_N, "every entry is listed");
	expect(!z_flist_truncated(&dlg_flist), "not a partial list");
	bool sorted = true;
	for (int i = 1; i < dlg_flist.count; i++)
		if (strcmp(dlg_flist.pool + dlg_flist.name_off[i - 1], dlg_flist.pool + dlg_flist.name_off[i]) >= 0)
			sorted = false;
	expect(sorted, "sorted as it came in");
	expect(ar_moves > 0, "the store grew through moves");
	if (app_arena) {
		expect(moved_calls > 0 && moved_to == ar_base, "the app is told where its arena went");
		expect(ar_base[0] == 0x5A && ar_base[99] == 0x5A, "the app's bytes are kept");
	}

	ds_release();
	if (app_arena) {
		expect(ar_size == 100, "the app's arena is its own size again");
		expect(ar_base[0] == 0x5A && ar_base[99] == 0x5A && moved_to == ar_base,
			"and where the app was told");
		test_arena_set(0, NULL, NULL);
	} else {
		expect(ar_size == 0 && !ar_base, "a borrowed arena is given back");
	}
	expect(dlg_flist.pool == dlg_flist.pool_store && dlg_flist.count == 0,
		"the list is back on its own store");

}

// The file list draws row icons through the hardware icon blitter,
// which zrender.h does not emulate. A prompt draws no icons; this only
// satisfies the link.
void z_fb_draw_icon(int x, int y, int icon, int fg, int bg, const z_clip_t *clip) {
	(void)x; (void)y; (void)icon; (void)fg; (void)bg; (void)clip;
}

static int checks, failures;

static void expect(bool ok, const char *what) {
	checks++;
	if (!ok) { failures++; printf("  FAIL: %s\n", what); }
}

static void key(uint32_t k, uint8_t mods) { dlg_key(k, mods); }

static void type(const char *s) {
	for (; *s; s++) key((uint8_t)*s, 0);
}

static bool field_is(const char *s) {
	return dlg.edit.len == (int)strlen(s) && !strncmp(dlg.edit.buf, s, strlen(s));
}

static void new_prompt(const char *initial) {
	memset(&dlg, 0, sizeof(dlg));
	dlg.kind = DLG_KIND_PROMPT;
	dlg.button_set = Z_DIALOG_OK_CANCEL;
	dlg.win.w = 200;
	dlg.win.h = 90;
	dlg.btn_y = 50;
	dlg.field_y = 30;
	prompt_setup(initial);
}

int main(void) {

	z_win_t scratch;

	if (!z_render_open(&scratch, 200, 90) || !z_tramp_install()) {
		printf("test_dialog: skipped (cannot map fixed addresses)\n");
		return 77;
	}

	printf("prompt: starts in the field\n");
	new_prompt("");
	expect(dlg.field_focus, "focus starts in the field");
	expect(dlg.wset.focused < 0, "no button focused while typing");

	printf("prompt: a space is a character, not OK\n");
	type("port ");
	expect(!dlg.done, "typing a space does NOT submit (the bug)");
	type("posix0");
	expect(field_is("port posix0"), "field holds the whole text, space included");
	key(0x0d, 0);
	expect(dlg.done && dlg.result == Z_DIALOG_YES, "Enter in the field is OK");

	printf("prompt: initial text, spaces inside it\n");
	new_prompt("telnet bbs");
	type(" x");
	expect(!dlg.done && field_is("telnet bbs x"), "space after initial text is typed");

	printf("prompt: Tab cycles field -> OK -> Cancel -> field\n");
	new_prompt("abc");
	key('\t', 0);
	expect(!dlg.field_focus && dlg.wset.focused == 0, "Tab: OK");
	key('\t', 0);
	expect(!dlg.field_focus && dlg.wset.focused == 1, "Tab: Cancel");
	key('\t', 0);
	expect(dlg.field_focus && dlg.wset.focused < 0, "Tab: back to the field");
	key('\t', Z_KBD_MOD_LSHIFT);
	expect(!dlg.field_focus && dlg.wset.focused == 1, "Shift+Tab from the field: Cancel");

	printf("prompt: Space and Enter press a FOCUSED button\n");
	new_prompt("abc");
	key('\t', 0);			// OK
	key(' ', 0);
	expect(dlg.done && dlg.result == Z_DIALOG_YES, "Space on OK accepts");

	new_prompt("abc");
	key('\t', 0); key('\t', 0);	// Cancel
	key(0x0d, 0);
	expect(dlg.done && dlg.result == Z_DIALOG_CANCEL, "Enter on Cancel cancels");

	new_prompt("");
	key('\t', 0);
	key(' ', 0);
	expect(!dlg.done, "OK with an empty field still does nothing");

	printf("prompt: z_dialog_prompt_empty_ok() takes an empty field\n");
	new_prompt("");
	prompt_empty_ok = true;
	key(0x0d, 0);
	expect(dlg.done && dlg.result == Z_DIALOG_YES, "Enter on an empty field is OK");
	new_prompt("");
	key(0x1b, 0);
	expect(dlg.done && dlg.result == Z_DIALOG_CANCEL, "Escape still cancels");
	prompt_empty_ok = false;

	printf("prompt: typing on a button goes back to the field\n");
	new_prompt("port");
	key('\t', 0);			// OK
	key('x', 0);
	expect(dlg.field_focus && dlg.wset.focused < 0 && !dlg.done, "focus returned to the field");
	expect(field_is("portx"), "and the character was typed, not lost");

	printf("prompt: Escape\n");
	new_prompt("abc");
	type("de f");
	key(0x1b, 0);
	expect(dlg.done && dlg.result == Z_DIALOG_CANCEL, "Escape cancels");

	printf("prompt: clicking the field takes the caret back\n");
	new_prompt("abc");
	key('\t', 0);
	{
		// Pointer at the field's middle, in content coordinates.
		dlg_mouse(40, dlg.field_y + 4, Z_MOUSE_BTN_LEFT);
		dlg_mouse(40, dlg.field_y + 4, 0);
	}
	expect(dlg.field_focus && dlg.wset.focused < 0, "click in the field focuses it");
	type(" ok");
	expect(!dlg.done && field_is("abc ok"), "and a space typed after is a character");

	printf("confirm: still starts on the affirmative button\n");
	memset(&dlg, 0, sizeof(dlg));
	dlg.kind = DLG_KIND_CONFIRM;
	dlg.button_set = Z_DIALOG_YES_NO;
	dlg_buttons_init(labels_yes_no, 2);
	expect(dlg.wset.focused == 0, "confirm focuses Yes");
	key(' ', 0);
	expect(dlg.done && dlg.result == Z_DIALOG_YES, "Space presses it");

	// -- help (z_dialog_help()) --

	printf("help: wraps at word boundaries, tabs line up\n");
	{
		static const char t[] =
			"A short line.\n"
			"\n"
			"This one is long enough that it has to wrap at least once in a box this narrow.\n"
			"Ctrl+S\tSave the file, and this description wraps under itself.\n"
			"Averyveryverylongwordwithnospacesthatcannotbreakanywhereatallreally\n";
		char line[HELP_COLS_MAX + 1];
		int hang = 0, n = 0, bad = 0, hung = 0;
		help_layout(t, 30 * 5 + 2 * DLG_MARGIN);
		expect(help.cols == 30, "30 columns in a 30-column box");
		for (const char *p = t; *p; n++) {
			int h0 = hang;
			p = help_next(p, help.cols, &hang, line);
			if ((int)strlen(line) > help.cols) bad++;
			if (h0 == HELP_TAB && !strncmp(line, "            ", HELP_TAB) && line[HELP_TAB] != ' ')
				hung++;
			if (n == 2 && strcmp(line, "This one is long enough that"))
				printf("    line 2 is \"%s\"\n", line), bad++;
		}
		expect(n == help.total, "help_count() agrees with drawing");
		expect(!bad, "no line wider than the box, words kept whole");
		expect(hung >= 1, "a wrapped key description continues at the tab column");
		expect(!help.sb_on && help.rows == help.total, "short text: no scrollbar, box fits it");
	}

	printf("help: long text scrolls; keys move it, Enter closes\n");
	{
		static char t[4096];
		int n = 0;
		for (int i = 0; i < 40; i++) {
			const char *l = "F3\tFind the next match.\n";
			while (*l) t[n++] = *l++;
		}
		t[n] = 0;
		memset(&dlg, 0, sizeof(dlg));
		dlg.win.w = HELP_W;
		dlg.win.h = 200;
		help_layout(t, HELP_W - DLG_FRAME_W);
		expect(help.sb_on && help.rows == HELP_ROWS && help.total == 40,
			"40 lines: a scrollbar, HELP_ROWS shown");
		expect(help.cols < (HELP_W - DLG_FRAME_W - 2 * DLG_MARGIN) / 5,
			"the scrollbar takes its width from the text");
		z_scrollbar_init(&help.sb, &dlg.win, Z_SB_VERT);
		z_scrollbar_set_range(&help.sb, help.total, help.rows);
		dlg.btn_y = 170;
		dlg_buttons_init(labels_help, 1);
		z_widget_focus_set(&dlg.wset, 0);
		help_key(Z_KEY_DOWN, 0);
		expect(help.top == 1, "Down scrolls a line");
		help_key(Z_KEY_END, 0);
		expect(help.top == 40 - HELP_ROWS, "End: the last page, not past it");
		help_key(Z_KEY_PAGEUP, 0);
		expect(help.top == 40 - HELP_ROWS - (HELP_ROWS - 1), "PgUp: a page less a line");
		help_key(Z_KEY_HOME, 0);
		expect(help.top == 0, "Home");
		help_wheel(-1);
		expect(help.top == 3, "wheel down: three lines");
		expect(!dlg.done, "scrolling does not close");
		help.doc = NULL;
		help_key(0x0d, 0);
		expect(dlg.done, "Enter on Close closes");
		memset(&dlg, 0, sizeof(dlg));
		help_key(Z_KEY_F1, 0);
		expect(dlg.done, "F1 again closes");
	}

	printf("help: Read docs only by choice\n");
	{
		memset(&dlg, 0, sizeof(dlg));
		dlg.win.w = HELP_W;
		dlg.win.h = 120;
		help_layout("x", HELP_W - DLG_FRAME_W);
		help.doc = NULL;			// so activating it launches nothing here
		dlg.btn_y = 90;
		dlg_buttons_init(labels_help_docs, 2);
		z_widget_focus_set(&dlg.wset, dlg.widget_count - 1);
		expect(dlg.wset.focused == 1, "Close starts focused, not Read docs");
		expect(dlg.widgets[1].x > dlg.widgets[0].x, "Close is the rightmost button");
		expect(dlg.widgets[1].x + DLG_BTN_W == z_win_content_w(&dlg.win) - DLG_MARGIN,
			"at the right margin");
	}

	// A picture, for the eye: HELP_PBM=/path/help.pbm
	if (getenv("HELP_PBM")) {
		static const char t[] =
			"Text is a plain text editor for notes, lists and Markdown.\n"
			"\n"
			"Ctrl+O\tOpen a file\n"
			"Ctrl+S\tSave (Ctrl+Shift+S: save as)\n"
			"Ctrl+F\tFind; F3 finds the next match\n"
			"Ctrl+Z\tUndo the last change, and again for the one before\n"
			"F1\tThis help\n"
			"\n"
			"Select with Shift and the arrows, or drag with the mouse. "
			"Files in /tmp are on the RAM disk: save them to /home to keep them.\n";
		memset(&dlg, 0, sizeof(dlg));
		dlg.win = scratch;
		dlg.win.w = HELP_W;
		help_layout(t, HELP_W - DLG_FRAME_W);
		help.doc = "text_editor";
		dlg.win.h = 4 + DLG_MARGIN + help.rows * DLG_LINE_H + DLG_MARGIN + DLG_BTN_H + DLG_MARGIN;
		dlg.win.clip[0].x1 = dlg.win.x + dlg.win.w - 1;
		dlg.win.clip[0].y1 = dlg.win.y + dlg.win.h - 1;
		dlg.btn_y = z_win_content_h(&dlg.win) - DLG_MARGIN - DLG_BTN_H;
		dlg_buttons_init(labels_help_docs, 2);
		z_widget_focus_set(&dlg.wset, 1);
		help_repaint();
		z_render_write(getenv("HELP_PBM"), &dlg.win, 2);
	}

	// -- print (z_dialog_print()) --

	printf("print: ranges\n");
	{
		int a = 0, b = 0;
		expect(pr_range("3", &a, &b) && a == 3 && b == 3, "\"3\": page 3");
		expect(pr_range(" 2 - 5 ", &a, &b) && a == 2 && b == 5, "\" 2 - 5 \": 2 to 5");
		expect(pr_range("4-", &a, &b) && a == 4 && b == 0, "\"4-\": 4 to the end");
		expect(!pr_range("5-2", &a, &b) && !pr_range("x", &a, &b) && !pr_range("", &a, &b) &&
			!pr_range("0", &a, &b) && !pr_range("2,3", &a, &b), "nothing else");
	}

	printf("print: rows, values, a range typed, Print\n");
	{
		static z_print_opts_t o;
		memset(&o, 0, sizeof(o));
		memset(&dlg, 0, sizeof(dlg));
		memset(&pr, 0, sizeof(pr));
		strcpy(o.printer, "ipp://10.0.0.5/ipp/print");
		strcpy(o.paper_default, "a4");
		o.copies = 1;
		o.nup = 1;
		o.selection = true;
		pr.o = &o;
		pr.choices[pr.nchoices++] = Z_PRINT_PAGES_ALL;
		pr.choices[pr.nchoices++] = Z_PRINT_PAGES_SELECTION;
		pr.choices[pr.nchoices++] = Z_PRINT_PAGES_RANGE;
		dlg.win.w = DLG_CONFIRM_W;
		dlg.win.h = 160;
		pr.val_x = DLG_MARGIN + PR_LABEL_W;
		pr.val_w = z_win_content_w(&dlg.win) - DLG_MARGIN - pr.val_x;
		dlg.btn_y = 130;
		dlg_buttons_init(labels_print, 2);
		z_widget_focus_set(&dlg.wset, -1);
		z_edit_init(&dlg.edit, dlg.field, DLG_FIELD_MAX, "");
		pr.row = PR_TO;

		char v[64];
		pr_value(PR_TO, v, sizeof(v));
		expect(!strcmp(v, "Printer 10.0.0.5"), "the printer by its host");
		pr_value(PR_PAPER, v, sizeof(v));
		expect(!strcmp(v, "Default (A4)"), "the default paper, named");

		pr_key(Z_KEY_RIGHT, 0);
		expect(o.dest == Z_PRINT_TO_PDF, "Right: a PDF file");
		pr_value(PR_TRAY, v, sizeof(v));
		expect(!strcmp(v, "(the printer's)"), "a file has no tray");
		pr_key(Z_KEY_LEFT, 0);
		expect(o.dest == Z_PRINT_TO_PRINTER, "Left: back to the printer");

		pr_key(Z_KEY_DOWN, 0);
		expect(pr.row == PR_PAGES, "Down: Pages");
		pr_key(Z_KEY_RIGHT, 0);
		expect(pr.choices[pr.choice] == Z_PRINT_PAGES_SELECTION, "the selection is a choice");
		pr_key('2', 0);
		expect(pr_typing() && dlg.edit.len == 1, "a digit starts a range");
		pr_key('-', 0);
		pr_key('4', 0);
		expect(dlg.edit.len == 3 && !strncmp(dlg.edit.buf, "2-4", 3), "and it is typed");

		pr_key(Z_KEY_DOWN, 0);
		pr_key(Z_KEY_RIGHT, 0);
		pr_key(Z_KEY_RIGHT, 0);
		expect(o.copies == 3, "Copies: Right twice is 3");
		pr_key(Z_KEY_DOWN, 0);
		pr_key(Z_KEY_LEFT, 0);
		expect(pr.paper == PR_NPAPERS - 1, "Paper: Left from the default wraps to the last");
		pr_key(Z_KEY_DOWN, 0);
		pr_key(Z_KEY_RIGHT, 0);
		pr_key(Z_KEY_DOWN, 0);
		pr_key(Z_KEY_RIGHT, 0);
		pr_key(Z_KEY_RIGHT, 0);
		pr_key(0x0d, 0);
		expect(dlg.done && dlg.result == Z_DIALOG_YES, "Enter prints");
		expect(o.pages == Z_PRINT_PAGES_RANGE && o.first == 2 && o.last == 4, "pages 2 to 4");
		expect(!strcmp(o.paper, "3x5") && !strcmp(o.tray, "main") && o.nup == 4,
			"paper, tray, pages to a sheet as chosen");

		// a range that is not one: Print stays, the focus on Pages
		dlg.done = false;
		pr.row = PR_PAGES;
		z_edit_init(&dlg.edit, dlg.field, DLG_FIELD_MAX, "9-3");
		pr_key(0x0d, 0);
		expect(!dlg.done && pr.row == PR_PAGES, "a bad range is not printed");
		pr_key(0x1b, 0);
		expect(dlg.done && dlg.result == Z_DIALOG_CANCEL, "Escape cancels");
	}

	// A picture: PRINT_PBM=/path/print.pbm
	if (getenv("PRINT_PBM")) {
		static z_print_opts_t o;
		memset(&o, 0, sizeof(o));
		memset(&dlg, 0, sizeof(dlg));
		memset(&pr, 0, sizeof(pr));
		strcpy(o.printer, "ipp://10.0.0.5/ipp/print");
		strcpy(o.paper_default, "a4");
		o.copies = 1;
		o.nup = 1;
		pr.o = &o;
		pr.choices[pr.nchoices++] = Z_PRINT_PAGES_ALL;
		pr.choices[pr.nchoices++] = Z_PRINT_PAGES_RANGE;
		dlg.win = scratch;
		dlg.win.w = DLG_CONFIRM_W;
		dlg.win.h = Z_WM_TITLEBAR_H + 4 + DLG_MARGIN + PR_ROWS * PR_ROW_H + DLG_MARGIN +
			DLG_BTN_H + DLG_MARGIN;		// as z_dialog_print() makes it
		dlg.win.clip[0].x1 = dlg.win.x + dlg.win.w - 1;
		dlg.win.clip[0].y1 = dlg.win.y + dlg.win.h - 1;
		pr.val_x = DLG_MARGIN + PR_LABEL_W;
		pr.val_w = z_win_content_w(&dlg.win) - DLG_MARGIN - pr.val_x;
		dlg.btn_y = z_win_content_h(&dlg.win) - DLG_MARGIN - DLG_BTN_H;
		dlg_buttons_init(labels_print, 2);
		z_widget_focus_set(&dlg.wset, -1);
		z_edit_init(&dlg.edit, dlg.field, DLG_FIELD_MAX, "");
		pr.row = PR_PAGES;
		pr_repaint();
		z_render_write(getenv("PRINT_PBM"), &dlg.win, 2);
	}

	// -- a big folder, in an app with no arena and in one with --
	big_folder(&scratch, false);
	big_folder(&scratch, true);

	printf("test_dialog: %d checks, %d failed\n", checks, failures);
	return failures ? 1 : 0;

}
