/*
 * Host test for desktop colour (docs/color.md, "Desktop colour"): the
 * REAL wm.c, with the accent plane -- the second VRAM sheet, rows 480
 * up -- caught in an array of its own, and socctl's registers in RAM.
 *
 *   sudo sysctl -w vm.mmap_min_addr=0      # see sw/common/tests/ztramp.h
 *   (make -C sw/apps/wm test builds and runs it)
 *
 * Exit 0 pass, 1 fail, 77 skipped.
 *
 * What it holds wm to:
 *   - on a monochrome board nothing at all: no COLWIN, no accent pixel;
 *   - on an eight-plane board: COLWIN over the whole screen, the apps'
 *     regions saying they may tint, the desktop tinted (or not, with
 *     desktop_background off), the focused window's titlebar and frame
 *     tinted, every other window's not, no window's content tinted;
 *   - a window asked to repaint has its content tints cleared first;
 *   - focus moving moves the tint;
 *   - a focused colour window takes precedence: COLWIN off, the apps
 *     told they may not tint, and the screen app told it has the
 *     colour window only once they have acked; back from it, wm takes
 *     the sheet again after the delay, from scratch, and asks the
 *     windows that had tinted to repaint;
 *   - off (Super+F10's switch) and a game's grab give it up; both
 *     come back.
 */
#include "../../../common/tests/zrender.h"

void z_fb_draw_icon(int x, int y, int icon_id, int fg, int bg, const z_clip_t *clip) {
	(void)icon_id; (void)bg;
	z_fb_fill_rect(x, y, 8, 8, fg, clip);
}
void z_fb_hw_sync(void) { }
void z_gfx_hw_font_load(const z_font_t *font) { (void)font; }
void z_gfx_hw_icon_load(int icon_id, const uint8_t *bitmap) { (void)icon_id; (void)bitmap; }

// The accent plane. zrender.h's VRAM is the 480-row desktop; a fill
// from row 480 lands here instead -- only if zgfx would let it
// (z_fb_set_rows(960)), as on the device.
static uint8_t acc[480][640];
static int acc_writes;
static void test_fill(int x, int y, int w, int h, int c) {
	if (y + h <= 480) { z_fb_hw_fill_rect(x, y, w, h, c); return; }
	if (z_fb_rows() < 960) return;
	acc_writes++;
	for (int j = y; j < y + h; j++)
		for (int i = x; i < x + w; i++)
			if (i >= 0 && i < 640 && j >= 480 && j < 960) acc[j - 480][i] = (uint8_t)(c != 0);
}
#define z_fb_hw_fill_rect test_fill
#define main wm_main_unused
#include "../wm.c"
#undef main
#undef z_fb_hw_fill_rect

#define APP_A 42
#define APP_B 43
#define APP_C 44

static z_obj_t k_ok, k_fail;
static uint32_t ticks = 10000;

static struct {
	int redraws[3], colour_msgs, colour_last;
	int accent_flag[3];       // the WINDOW rect's x1 said Z_WM_CLIP_ACCENT
	int clips[3];
} seen;

static z_msg_t queue[64];
static int qn;

static int app_of(uint32_t pid) { return pid == APP_A ? 0 : pid == APP_B ? 1 : pid == APP_C ? 2 : -1; }

static void app_sees(const z_msg_t *m) {
	int a = app_of(m->to);
	if (a < 0) return;
	if (m->subject == Z_WM_REDRAW) seen.redraws[a]++;
	if (m->subject == Z_WM_COLOR) { seen.colour_msgs++; seen.colour_last = (int)m->obj.val.uint32; }
	if (m->subject != Z_WM_SET_CLIP || m->obj.type != Z_BLOB) return;
	const z_blob_t *b = (const z_blob_t *)m->obj.val.ptr;
	const z_wm_cliprect_t *r = (const z_wm_cliprect_t *)b->data;
	seen.clips[a]++;
	seen.accent_flag[a] = (r[0].x1 == Z_WM_CLIP_ACCENT);
	if (qn < 64) {          // acked, as z_win_apply_clip() does
		z_msg_t *q = &queue[qn++];
		memset(q, 0, sizeof(*q));
		q->from = m->to;
		q->subject = Z_WM_CLIP_DONE;
		q->obj.type = Z_UINT32;
		q->obj.val.uint32 = (uint32_t)r[0].y1;
	}
}

static uint32_t *k_syscall(uint32_t id, uint32_t *args, uint32_t b) {
	(void)b;
	switch (id) {
	case Z_SYS_UPTIME:
		ticks += 1;
		((z_obj_t *)args)->type = Z_UINT32;
		((z_obj_t *)args)->val.uint32 = ticks;
		return (uint32_t *)&k_ok;
	case Z_SYS_MSG_SEND:
		app_sees((z_msg_t *)args);
		return (uint32_t *)&k_ok;
	case Z_SYS_MSG_READ:
		if (!qn) return (uint32_t *)&k_fail;
		*(z_msg_t *)args = queue[0];
		memmove(queue, queue + 1, (size_t)(--qn) * sizeof(z_msg_t));
		return (uint32_t *)&k_ok;
	case Z_SYS_HID_READ_KEY:
		((z_obj_t *)args)->type = Z_INT32;
		((z_obj_t *)args)->val.int32 = -1;
		return (uint32_t *)&k_ok;
	default:
		return (uint32_t *)&k_ok;
	}
}

static bool k_install(void) {
	if ((uintptr_t)(void *)k_syscall > 0xFFFFFFFFu) return false;
	if (mmap((void *)0, 4096, PROT_READ | PROT_WRITE,
		MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0) == MAP_FAILED)
		return false;
	if (mmap((void *)0x70000000UL, 4096, PROT_READ | PROT_WRITE,
		MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0) == MAP_FAILED)
		return false;
	k_ok.type = Z_UINT32; k_ok.val.uint32 = Z_OK;
	k_fail.type = Z_UINT32; k_fail.val.uint32 = Z_FAIL;
	*(volatile uint32_t *)0x0000000c = (uint32_t)(uintptr_t)k_syscall;
	return true;
}

// an eight-plane bitstream with the colour window, or a monochrome one
static void board(bool colour) {
	reg_socctl_color = colour ? ((uint32_t)Z_COLOR_SIG << 16) | Z_COLOR_AVAIL | (2u << Z_COLOR_XP_SHIFT) : 0;
	reg_socctl_colwin = colour ? ((uint32_t)Z_COLWIN_SIG << 16) | Z_COLWIN_AVAIL : 0;
	reg_socctl_colwinend = 0;
}

static void drain(void) {
	z_msg_t m;
	while (z_msg_read(&m) == Z_OK) handle_message(&m);
}

static int checks, failures;
static void expect(bool ok, const char *what) {
	checks++;
	if (!ok) { failures++; printf("  FAIL: %s\n", what); }
}

static int tinted(int x0, int y0, int x1, int y1) {
	int n = 0;
	for (int y = y0; y <= y1; y++)
		for (int x = x0; x <= x1; x++) n += acc[y][x];
	return n;
}

static void dump(const char *prefix, const char *name) {
	char path[256];
	snprintf(path, sizeof(path), "%s-%s.pgm", prefix, name);
	FILE *f = fopen(path, "w");
	if (!f) return;
	fprintf(f, "P2\n%d %d\n3\n", WM_SCREEN_W, WM_SCREEN_H);
	for (int y = 0; y < WM_SCREEN_H; y++) {
		for (int x = 0; x < WM_SCREEN_W; x++)
			fprintf(f, "%d ", z_render_get(x, y) + 2 * acc[y][x]);
		fputc('\n', f);
	}
	fclose(f);
	printf("  wrote %s\n", path);
}

static void service(void) {
	drain();
	colour_window_update();
	acc_service();
	drain();
}

static void focus(int idx) {
	int old = focused;
	focused = idx;
	repair_focus_chrome(old, idx);
	service();
}

int main(int argc, char **argv) {
	const char *prefix = argc > 1 ? argv[1] : "/tmp/wmaccent";
	z_win_t screen;
	int a, b, c;

	if (!z_render_open(&screen, WM_SCREEN_W, WM_SCREEN_H) || !k_install()) {
		printf("wm accent test: skipped (cannot map fixed addresses)\n");
		return 77;
	}
	zr_region_strict = true;
	my_pid = 1;
	dock_idx = -1;
	z_render_clear();

	a = create_window(APP_A, "A", 260, 180, 40, 40, 0);
	b = create_window(APP_B, "B", 200, 150, 340, 200, 0);
	expect(a >= 0 && b >= 0, "two windows");
	send_clip_all();
	repair_region(0, 0, WM_SCREEN_W, WM_SCREEN_H, -1);
	focused = a;
	drain();

	wm_window_t *wa = &windows[a], *wb = &windows[b];
#define AX0 ((int)wa->x)
#define AY0 ((int)wa->y)
#define AX1 ((int)(wa->x + wa->w - 1))
#define AY1 ((int)(wa->y + wa->h - 1))
#define BX0 ((int)wb->x)
#define BY0 ((int)wb->y)
#define BX1 ((int)(wb->x + wb->w - 1))
#define BY1 ((int)(wb->y + wb->h - 1))

	// -- a monochrome board: nothing --
	printf("1. monochrome: nothing\n");
	board(false);
	acc_init();
	service();
	expect(!acc_hw && !acc_live && acc_writes == 0, "no accent plane writes");
	expect(reg_socctl_colwin == 0 && reg_socctl_colwinend == 0, "COLWIN untouched");
	expect(!seen.accent_flag[0] && !seen.accent_flag[1], "no region says tint");

	// -- an eight-plane board --
	printf("2. on: the whole desktop\n");
	board(true);
	acc_init();
	expect(acc_hw, "available");
	service();
	expect(acc_live, "live");
	expect(reg_socctl_colwinend == ((480u << 16) | 640u) && reg_socctl_colwin == Z_COLWIN_EN,
		"COLWIN over 0,0..640,480");
	expect(seen.accent_flag[0] && seen.accent_flag[1], "the apps may tint");
	expect(tinted(0, 0, 30, 30) == 31 * 31, "the desktop tinted");
	expect(tinted(AX0, AY0, AX1, AY0 + Z_WM_TITLEBAR_H) == (AX1 - AX0 + 1) * (Z_WM_TITLEBAR_H + 1),
		"the focused titlebar and separator tinted");
	expect(acc[AY1][AX0 + 50] && acc[AY0 + 60][AX0] && acc[AY0 + 60][AX1], "its frame tinted");
	expect(tinted(AX0 + 1, AY0 + 12, AX1 - 1, AY1 - 1) == 0, "its margin and content not");
	expect(tinted(BX0, BY0, BX1, BY1) == 0, "the other window not at all");
	dump(prefix, "1-on");

	// -- content tints are the window's; a repaint request clears them --
	printf("3. content tints\n");
	for (int y = BY0 + 30; y < BY0 + 40; y++)
		for (int x = BX0 + 10; x < BX0 + 60; x++) acc[y][x] = 1;     // an app's selection
	acc_paint_all();
	expect(tinted(BX0 + 10, BY0 + 30, BX0 + 59, BY0 + 39) == 500, "wm's own repaint leaves them");
	int rb = seen.redraws[1];
	repair_region(BX0, BY0, (int)wb->w, (int)wb->h, -1);
	expect(seen.redraws[1] > rb, "B asked to repaint");
	expect(tinted(BX0 + 2, BY0 + 13, BX1 - 2, BY1 - 2) == 0, "and its content tints cleared first");

	// -- focus --
	printf("4. focus moves the tint\n");
	focus(b);
	expect(tinted(AX0, AY0, AX1, AY0 + 11) == 0, "A's titlebar untinted");
	expect(tinted(BX0, BY0, BX1, BY0 + 11) == (BX1 - BX0 + 1) * 12, "B's tinted");
	dump(prefix, "2-focus-b");

	// -- the background option --
	printf("5. desktop_background off\n");
	acc_bg = false;
	acc_dirty = true;
	service();
	expect(tinted(0, 0, 30, 30) == 0, "the desktop untinted");
	expect(tinted(BX0, BY0, BX1, BY0 + 11) > 0, "the chrome still is");
	acc_bg = true;
	acc_dirty = true;
	service();
	expect(tinted(0, 0, 30, 30) == 31 * 31, "and back");

	// -- precedence --
	printf("6. a colour window takes precedence\n");
	c = create_window(APP_C, "C", 324, 255, 300, 210, Z_WIN_FLAG_COLOR);
	expect(c >= 0, "a screen app's window");
	send_clip_all();
	repair_region((int)windows[c].x, (int)windows[c].y, (int)windows[c].w, (int)windows[c].h, -1);
	drain();
	int colour_msgs = seen.colour_msgs;
	focused = c;
	colour_window_update();          // the focus change, before any ack
	expect(!acc_live && reg_socctl_colwin == 0, "desktop colour off at once");
	expect(!seen.accent_flag[0] && !seen.accent_flag[1], "the apps told: no more tints");
	expect(seen.colour_msgs == colour_msgs, "the screen app not told yet");
	expect(cw_pending == c, "waiting for the acks");
	service();                       // the acks arrive
	expect(seen.colour_msgs == colour_msgs + 1 && seen.colour_last == 1, "then it is told 1");
	expect(!acc_live && reg_socctl_colwin == 0, "and wm stays off the sheet");
	for (int y = 0; y < 480; y++) memset(acc[y], 1, 640);      // its planes trash the sheet
	acc_used[a] = true;              // A had tinted (Z_WM_ACCENT_USED)
	int ra = seen.redraws[0], rb2 = seen.redraws[1];

	printf("7. and gives it back\n");
	focus(a);
	expect(seen.colour_last == 0, "the screen app told 0");
	expect(!acc_live, "wm waits before taking the sheet");
	ticks += 2 * ACC_RETAKE_MS * Z_TICK_HZ / 1000u;
	service();
	expect(acc_live && reg_socctl_colwin == Z_COLWIN_EN, "then takes it");
	expect(seen.accent_flag[0] && seen.accent_flag[1], "the apps may tint again");
	expect(seen.redraws[0] > ra, "A, which had tinted, repaints");
	expect(seen.redraws[1] == rb2, "B, which had not, does not");
	expect(tinted(AX0 + 2, AY0 + 13, AX1 - 2, AY1 - 2) == 0, "no trace of the colour window in A");
	expect(tinted(AX0, AY0, AX1, AY0 + 11) == (AX1 - AX0 + 1) * 12, "A's titlebar tinted");
	expect(tinted(0, 0, 30, 30) == 31 * 31, "the desktop tinted");
	dump(prefix, "3-back");

	// -- a dual app: colour only once it asks --
	printf("7b. a dual app on its desktop face keeps desktop colour\n");
	int d = create_window(APP_B, "D", 200, 120, 60, 300, Z_WIN_FLAG_COLOR | Z_WIN_FLAG_COLOUR_LATER);
	expect(d >= 0, "a dual app's window");
	send_clip_all();
	repair_region((int)windows[d].x, (int)windows[d].y, (int)windows[d].w, (int)windows[d].h, -1);
	colour_msgs = seen.colour_msgs;
	focus(d);
	expect(acc_live && seen.colour_msgs == colour_msgs, "focused, not wanting: desktop colour stays");
	{
		z_msg_t *q = &queue[qn++];
		memset(q, 0, sizeof(*q));
		q->from = APP_B; q->subject = Z_WM_COLOR_WANT; q->tag = (uint32_t)d;
		q->obj.type = Z_UINT32; q->obj.val.uint32 = 1;
	}
	service();
	service();
	expect(!acc_live && seen.colour_msgs == colour_msgs + 1 && seen.colour_last == 1,
		"attached (wants it): precedence, then told 1");
	{
		z_msg_t *q = &queue[qn++];
		memset(q, 0, sizeof(*q));
		q->from = APP_B; q->subject = Z_WM_COLOR_WANT; q->tag = (uint32_t)d;
		q->obj.type = Z_UINT32; q->obj.val.uint32 = 0;
	}
	service();
	ticks += 2 * ACC_RETAKE_MS * Z_TICK_HZ / 1000u;
	service();
	expect(acc_live && seen.colour_last == 0, "detached: told 0, desktop colour back");
	focus(a);

	// -- off, and a grab --
	printf("8. off, and a game's grab\n");
	acc_cfg = false;
	service();
	expect(!acc_live && reg_socctl_colwin == 0 && !acc_apps, "off: COLWIN off, no tints");
	acc_cfg = true;
	service();
	expect(acc_live && acc_apps, "on again");
	game_grab_pid = 77;
	service();
	expect(!acc_live && reg_socctl_colwin == 0, "a grab: off");
	game_grab_pid = 0;
	service();
	expect(acc_live, "released: on");

	// -- the exact path: a bitstream with COLWIN's DESK --
	printf("9. exact gateware: DESK set\n");
	acc_cfg = false;
	service();
	reg_socctl_colwin = ((uint32_t)Z_COLWIN_SIG << 16) | Z_COLWIN_AVAIL | Z_COLWIN_DESK_AVAIL;
	expect(z_color_desktop_exact(), "DESK available");
	acc_cfg = true;
	service();
	expect(acc_live && reg_socctl_colwin == (Z_COLWIN_EN | Z_COLWIN_DESK),
		"COLWIN with EN and DESK");

	printf("wm accent test: %d checks, %d failed\n", checks, failures);
	return failures ? 1 : 0;
}
