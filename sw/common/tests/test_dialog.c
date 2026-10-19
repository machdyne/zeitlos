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

#include "../zdialog.c"

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

	printf("test_dialog: %d checks, %d failed\n", checks, failures);
	return failures ? 1 : 0;

}
