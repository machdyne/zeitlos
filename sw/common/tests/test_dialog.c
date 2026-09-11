/*
 * Host test for sw/common/zdialog.c's text prompt: the REAL key and
 * mouse handlers, driven directly, with no window and no wm.
 *
 *   sudo sysctl -w vm.mmap_min_addr=0      # see ztramp.h
 *   cc -std=gnu99 -Wall -no-pie -I sw/common -o /tmp/test_dialog \
 *      sw/common/tests/test_dialog.c sw/common/zwin.c \
 *      sw/common/zwidget.c sw/common/zflist.c sw/common/zedit.c \
 *      sw/common/zfsapp.c sw/common/zfont_data.c sw/common/zobj.c \
 *      sw/common/zeitlos.c
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

	printf("test_dialog: %d checks, %d failed\n", checks, failures);
	return failures ? 1 : 0;

}
