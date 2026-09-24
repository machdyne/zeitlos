/*
 * Host test for sw/apps/keyboard -- the real keyboard.c, clicked with
 * a scripted mouse, its injected key events captured by a scripted
 * kernel, and its window rendered.
 *
 *   cc -std=gnu99 -Wall -no-pie -I sw/common -o /tmp/kb_keys \
 *      sw/apps/keyboard/tests/keys.c sw/common/zwin.c sw/common/zobj.c \
 *      sw/common/zeitlos.c sw/common/zfont_data.c sw/common/zkbd.c
 *   /tmp/kb_keys /tmp/kb        # also writes /tmp/kb-de.pbm, /tmp/kb-ja.pbm
 *
 * Needs -no-pie and vm.mmap_min_addr=0 (sw/common/tests/ztramp.h);
 * exits 77 without them. See docs/keyboard_app.md.
 */

#include "../../../common/tests/zrender.h"

#define main keyboard_main_unused
#include "../keyboard.c"
#undef main

int z_fb_scroll_debug, z_fb_scroll_dbg_armed, z_fb_scroll_align;
void z_fb_draw_icon(int x, int y, int icon_id, int fg, int bg, const z_clip_t *clip) {
	(void)x; (void)y; (void)icon_id; (void)fg; (void)bg; (void)clip;
}

// -- a scripted kernel: the layout, and the injected events --

static z_obj_t k_ok, k_fail;
static int active = 0;
static int32_t injected[64];
static int ninj;

static uint32_t *k_syscall(uint32_t id, uint32_t *args, uint32_t b) {
	(void)b;
	z_obj_t *o = (z_obj_t *)args;
	switch (id) {
	case Z_SYS_KBD_LAYOUT:
		if (o->val.int32 >= 0) active = o->val.int32;
		o->type = Z_INT32;
		o->val.int32 = active;
		return (uint32_t *)&k_ok;
	case Z_SYS_HID_INJECT:
		if (ninj < 64) injected[ninj++] = o->val.int32;
		return (uint32_t *)&k_ok;
	case Z_SYS_MSG_READ:
		return (uint32_t *)&k_fail;
	default:
		return (uint32_t *)&k_ok;
	}
}

static bool k_install(void) {
	if ((uintptr_t)(void *)k_syscall > 0xFFFFFFFFu) return false;
	void *page = mmap((void *)0, 4096, PROT_READ | PROT_WRITE,
		MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
	if (page == MAP_FAILED) return false;
	k_ok.type = Z_UINT32; k_ok.val.uint32 = Z_OK;
	k_fail.type = Z_UINT32; k_fail.val.uint32 = Z_FAIL;
	*(volatile uint32_t *)0x0000000c = (uint32_t)(uintptr_t)k_syscall;
	return true;
}

static int checks, failures;
static void expect(bool ok, const char *what) {
	checks++;
	if (!ok) { failures++; printf("  FAIL: %s\n", what); }
}

// Clicks key (r, c): a press and a release at its centre.
static void click(int r, int c) {
	z_clip_t cr;
	z_win_content_rect(&win, &cr);
	int x = cr.x0 + key_x(r, c) + rows[r][c].w * Q / 2;
	int y = cr.y0 + MARGIN + r * KEY_H + KEY_H / 2;
	mouse(Z_WM_PACK_MOUSE(x, y, Z_MOUSE_BTN_LEFT, 1));
	mouse(Z_WM_PACK_MOUSE(x, y, 0, 1));
}

// The row/column of the key with this usage.
static void find(uint8_t usage, uint8_t kind, int *r, int *c) {
	for (int i = 0; i < ROWS; i++)
		for (int j = 0; j < 16 && rows[i][j].w; j++)
			if (rows[i][j].usage == usage && rows[i][j].kind == kind) { *r = i; *c = j; return; }
	*r = *c = -1;
}

static void use_layout(const char *name) {
	active = layout = z_kbd_layout_find(name);
	set_status_layout();
}

int main(int argc, char **argv) {

	if (!z_render_open(&win, WIN_W, WIN_H)) return 77;
	if (!k_install()) return 77;

	// Every row is the full width.
	for (int r = 0; r < ROWS; r++) {
		int q = 0;
		for (int c = 0; c < 16 && rows[r][c].w; c++) q += rows[r][c].w;
		expect(q == 60, "a row is not 60 quarters");
	}

	int r, c;

	// -- a plain key: a press and a release of its usage --
	use_layout("de");
	ninj = 0;
	find(0x1C, K_KEY, &r, &c);								// the key US calls Y
	click(r, c);
	expect(ninj == 2, "one click, two events");
	expect(Z_KBD_EV_USAGE(injected[0]) == 0x1C && Z_KBD_EV_PRESSED(injected[0]),
		"press of the key");
	expect(!Z_KBD_EV_PRESSED(injected[1]), "then its release");
	expect(!strncmp(status, "z ", 2), "status says it typed z on German");

	// -- labels come from the layout --
	char s[16];
	label(&rows[r][c], s);
	expect(!strcmp(s, "z"), "German: the Y key is labelled z");
	find(0x34, K_KEY, &r, &c);
	label(&rows[r][c], s);
	expect(!strcmp(s, "\xC3\xA4"), "German: a-umlaut label");
	find(0x2E, K_KEY, &r, &c);
	label(&rows[r][c], s);
	// The lone acute is not in ISO 8859-15, so the label is the ASCII
	// mark that looks like it rather than the missing-glyph box.
	expect(!strcmp(s, "'"), "German: dead acute shows as '");
	find(0x35, K_KEY, &r, &c);
	label(&rows[r][c], s);
	expect(!strcmp(s, "^"), "German: dead circumflex shows ^");

	// -- sticky Shift: held for one key, then released --
	find(Z_KBD_MOD_LSHIFT, K_MOD, &r, &c);
	click(r, c);
	expect(mods == Z_KBD_MOD_LSHIFT, "Shift is held");
	find(0x34, K_KEY, &r, &c);
	label(&rows[r][c], s);
	expect(!strcmp(s, "\xC3\x84"), "label follows Shift: A-umlaut");
	ninj = 0;
	click(r, c);
	expect(Z_KBD_EV_MODS(injected[0]) == Z_KBD_MOD_LSHIFT, "the event carries Shift");
	expect(mods == 0, "Shift let go after one key");

	// -- AltGr: the right-Alt bit, for wm to read as AltGr --
	find(Z_KBD_MOD_RALT, K_MOD, &r, &c);
	click(r, c);
	find(0x14, K_KEY, &r, &c);								// Q
	label(&rows[r][c], s);
	expect(!strcmp(s, "@"), "German AltGr+Q label is @");
	ninj = 0;
	click(r, c);
	expect(Z_KBD_EV_MODS(injected[0]) == Z_KBD_MOD_RALT, "the event carries right Alt");

	// -- Caps Lock stays --
	find(0, K_CAPS, &r, &c);
	click(r, c);
	find(0x34, K_KEY, &r, &c);
	ninj = 0;
	click(r, c);
	expect(Z_KBD_EV_LOCKS(injected[0]) & Z_KBD_LOCK_CAPS, "the event carries Caps Lock");
	expect(caps, "Caps Lock is still on after a key");
	find(0, K_CAPS, &r, &c);
	click(r, c);
	expect(!caps, "and off when tapped again");

	// -- the layout key steps through every layout --
	use_layout("us");
	find(0, K_LAYOUT, &r, &c);
	click(r, c);
	expect(active == 1 && layout == 1, "Lay makes the next layout active");

	// -- a picture of German and of Japanese input --
	if (argc > 1) {
		char path[256];
		use_layout("de");
		draw_all();
		snprintf(path, sizeof(path), "%s-de.pbm", argv[1]);
		z_render_write(path, &win, 2);
		use_layout("fr");
		find(Z_KBD_MOD_RALT, K_MOD, &r, &c);
		click(r, c);
		snprintf(path, sizeof(path), "%s-fr-altgr.pbm", argv[1]);
		z_render_write(path, &win, 2);
		mods = 0;
	}

	printf("keyboard: %d checks, %d failed\n", checks, failures);
	return failures ? 1 : 0;

}
