/*
 * Host test for wm's screen lock (docs/security.md): the REAL wm.c,
 * drawing into a software framebuffer (sw/common/tests/zrender.h), with
 * a scripted kernel that plays one app window and Z_SYS_AUTH.
 *
 *   sudo sysctl -w vm.mmap_min_addr=0      # see sw/common/tests/ztramp.h
 *   cc -std=gnu99 -w -no-pie -I sw/common -o /tmp/wmlock \
 *      sw/apps/wm/tests/test_lock.c sw/common/zwin.c sw/common/zwidget.c \
 *      sw/common/zfont_data.c sw/common/zobj.c sw/common/zeitlos.c \
 *      sw/common/zkbd.c sw/common/zspeak.c sw/common/zcaption.c \
 *      sw/common/zcfg.c sw/apps/wm/dock_icons.c sw/apps/wm/win_icons.c
 *   /tmp/wmlock /tmp/wmlock
 *
 * Exit 0 pass, 1 fail, 77 skipped.
 *
 * What it holds wm to:
 *   - locking freezes the app first (and gets its ack), then gives it an
 *     EMPTY region, then draws the lock screen;
 *   - while locked, nothing wm draws -- a full repair, a caption --
 *     changes a single pixel of the lock screen;
 *   - keys go to the password field only; a chord types nothing; the
 *     field is wiped after every attempt;
 *   - a wrong password keeps it locked; the right one restores the
 *     app's region and asks it to redraw;
 *   - with no password set, Enter opens it; an empty field with a
 *     password set does not even ask the kernel;
 *   - a lock asked for mid-drag waits for the release;
 *   - an app grabbing the screen while locked is revoked at once.
 */
#include "../../../common/tests/zrender.h"

// What zrender.h leaves out and wm uses. The icon is drawn as a solid
// square through the region-honouring fill, so a titlebar icon painted
// while locked would still show up as changed pixels.
void z_fb_draw_icon(int x, int y, int icon_id, int fg, int bg, const z_clip_t *clip) {
	(void)icon_id; (void)bg;
	z_fb_fill_rect(x, y, 8, 8, fg, clip);
}
void z_fb_hw_sync(void) { }
void z_gfx_hw_font_load(const z_font_t *font) { (void)font; }
void z_gfx_hw_icon_load(int icon_id, const uint8_t *bitmap) { (void)icon_id; (void)bitmap; }
#define main wm_main_unused
#include "../wm.c"
#undef main

#define APP 42

// -- a scripted kernel --

static z_obj_t k_ok, k_fail;
static uint32_t ticks = 10000;

static struct {
	int set_clips, freezes, redraws, revokes;
	bool last_empty;            // the last real region sent to APP
} app;

static z_msg_t queue[16];
static int qn;

static struct {
	bool has, nosys;
	int calls;
	int force;                  // a result to return instead, or 0
	uint32_t force_wait;
} ka = { true, false, 0, 0, 0 };

static void app_sees(const z_msg_t *m) {
	if (m->to == 77 && m->subject == Z_WM_GAME_REVOKED) app.revokes++;
	if (m->to != APP) return;
	if (m->subject == Z_WM_REDRAW) app.redraws++;
	if (m->subject != Z_WM_SET_CLIP || m->obj.type != Z_BLOB) return;
	const z_blob_t *b = (const z_blob_t *)m->obj.val.ptr;
	const z_wm_cliprect_t *r = (const z_wm_cliprect_t *)b->data;
	int n = (int)(b->len / sizeof(z_wm_cliprect_t));
	int win = r[0].y1;
	if (n >= 2 && r[1].x0 == Z_WM_CLIP_CTL && r[1].y0 == Z_WM_CLIP_FREEZE) app.freezes++;
	else {
		app.set_clips++;
		app.last_empty = (n == 2 && r[1].x1 < r[1].x0);
	}
	// the app acks every region, as z_win_apply_clip() does
	if (qn < 16) {
		z_msg_t *a = &queue[qn++];
		memset(a, 0, sizeof(*a));
		a->from = APP;
		a->subject = Z_WM_CLIP_DONE;
		a->obj.type = Z_UINT32;
		a->obj.val.uint32 = (uint32_t)win;
	}
}

static uint32_t *k_syscall(uint32_t id, uint32_t *args, uint32_t b) {
	(void)b;
	switch (id) {
	case Z_SYS_UPTIME:
		ticks += 3;
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
	case Z_SYS_AUTH: {
		z_auth_args_t *a = (z_auth_args_t *)args;
		if (ka.nosys) return (uint32_t *)&k_ok;
		a->result = Z_AUTH_OK;
		if (a->op == Z_AUTH_STATUS) {
			memset(a->st, 0, sizeof(*a->st));
			a->st->flags = ka.has ? Z_AUTH_HAS_PASSWORD : 0;
			a->st->lock_idle_min = 1;
		} else if (a->op == Z_AUTH_CHECK) {
			ka.calls++;
			if (ka.force) { a->result = ka.force; a->wait_ms = ka.force_wait; }
			else if (!ka.has) a->result = Z_AUTH_E_NOPASS;
			else if (a->pwlen != 7 || memcmp(a->pw, "hunter2", 7)) a->result = Z_AUTH_E_BAD;
		}
		return (uint32_t *)&k_ok;
	}
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

// -- the screen --

static uint8_t snap[WM_SCREEN_H][WM_SCREEN_W];

static void take(void) {
	for (int y = 0; y < WM_SCREEN_H; y++)
		for (int x = 0; x < WM_SCREEN_W; x++) snap[y][x] = (uint8_t)z_render_get(x, y);
}

static int changed(void) {
	int n = 0, x0 = 9999, y0 = 9999, x1 = -1, y1 = -1;
	for (int y = 0; y < WM_SCREEN_H; y++)
		for (int x = 0; x < WM_SCREEN_W; x++)
			if (snap[y][x] != (uint8_t)z_render_get(x, y)) {
				n++;
				if (x < x0) x0 = x;
				if (y < y0) y0 = y;
				if (x > x1) x1 = x;
				if (y > y1) y1 = y;
			}
	if (n) printf("  %d pixels changed, within %d,%d..%d,%d\n", n, x0, y0, x1, y1);
	return n;
}

static void dump(const char *prefix, const char *name) {
	char path[256];
	snprintf(path, sizeof(path), "%s-%s.pbm", prefix, name);
	FILE *f = fopen(path, "w");
	if (!f) return;
	fprintf(f, "P1\n%d %d\n", WM_SCREEN_W, WM_SCREEN_H);
	for (int y = 0; y < WM_SCREEN_H; y++) {
		for (int x = 0; x < WM_SCREEN_W; x++) fputc(z_render_get(x, y) ? '1' : '0', f);
		fputc('\n', f);
	}
	fclose(f);
	printf("  wrote %s\n", path);
}

static int checks, failures;
static void expect(bool ok, const char *what) {
	checks++;
	if (!ok) { failures++; printf("  FAIL: %s\n", what); }
}

static void type(const char *s) {
	while (*s) lock_key((uint32_t)(uint8_t)*s++, false);
}

static bool pw_wiped(void) {
	for (int i = 0; i < (int)sizeof(lock_pw); i++) if (lock_pw[i]) return false;
	return lock_pw_len == 0;
}

int main(int argc, char **argv) {
	const char *prefix = argc > 1 ? argv[1] : "/tmp/wmlock";
	z_win_t screen;
	int idx;

	if (!z_render_open(&screen, WM_SCREEN_W, WM_SCREEN_H) || !k_install()) {
		printf("wm lock test: skipped (cannot map fixed addresses)\n");
		return 77;
	}
	zr_region_strict = true;     // the device's rule: see zrender.h
	my_pid = 1;
	dock_idx = -1;
	z_render_clear();

	idx = create_window(APP, "App", 200, 150, 100, 100, 0);
	expect(idx >= 0, "an app window");
	send_clip(idx);
	repair_region(0, 0, WM_SCREEN_W, WM_SCREEN_H, -1);
	expect(app.set_clips >= 1 && !app.last_empty, "the app starts with a real region");
	dump(prefix, "1-desktop");

	// -- locking --
	printf("1. locking\n");
	lock_reload();
	memset(&app, 0, sizeof(app));
	lock_pending = true;
	lock_engage();
	expect(wm_locked && !lock_pending, "locked");
	expect(app.freezes == 1, "the app was frozen first");
	expect(app.set_clips >= 1 && app.last_empty, "then given an empty region");
	expect(window_visible_region(idx, (z_clip_t[8]){{0}}, 8) == 0, "its visible region is empty");
	dump(prefix, "2-locked");

	// -- nothing wm draws reaches the lock screen --
	printf("2. wm's own drawing stays off it\n");
	take();
	repair_region(0, 0, WM_SCREEN_W, WM_SCREEN_H, -1);
	expect(changed() == 0, "a full repair changes no pixel");
	caption_set("a caption over the lock screen", 0);
	caption_blit();
	expect(changed() == 0, "a caption changes no pixel");
	send_clip_all();
	expect(app.last_empty, "a region update while locked is still empty");

	// -- keys --
	printf("3. the password field\n");
	type("hunter1");
	expect(lock_pw_len == 7, "typed into the field");
	lock_key('l', true);
	expect(lock_pw_len == 7, "a chord (Super+L) types nothing");
	int redraws = app.redraws;
	lock_key(0x0d, false);
	expect(wm_locked && strstr(lock_msg, "Wrong") && pw_wiped(), "wrong password: locked, field wiped");
	expect(app.redraws == redraws, "and the app was not asked to draw");
	dump(prefix, "3-wrong");

	ka.force = Z_AUTH_E_WAIT;
	ka.force_wait = 2500;
	type("hunter2");
	lock_key(0x0d, false);
	expect(wm_locked && strstr(lock_msg, "wait 3 s") && pw_wiped(), "backoff shown, still locked");
	ka.force = 0;

	int calls = ka.calls;
	lock_key(0x0d, false);
	expect(ka.calls == calls && wm_locked, "an empty field with a password set does not ask the kernel");

	type("abc");
	lock_key(0x7f, false);
	lock_key(0x1b, false);
	expect(pw_wiped(), "Backspace, then Escape, clears the field");

	type("hunter2");
	lock_key(0x0d, false);
	expect(!wm_locked && pw_wiped(), "the right password unlocks and wipes the field");
	expect(!app.last_empty, "the app has its region back");
	expect(app.redraws > redraws, "and is asked to redraw");
	int n = 0;
	{
		z_clip_t reg[8];
		n = window_visible_region(idx, reg, 8);
	}
	expect(n > 0, "its visible region is real again");
	dump(prefix, "4-unlocked");

	// -- no password: Enter opens it --
	printf("4. no password set\n");
	ka.has = false;
	lock_pending = true;
	lock_engage();
	expect(wm_locked, "locks anyway (a curtain)");
	lock_key(0x0d, false);
	expect(!wm_locked, "Enter opens it");
	ka.has = true;

	// -- a kernel without Z_SYS_AUTH --
	ka.nosys = true;
	lock_pending = true;
	lock_engage();
	type("x");
	lock_key(0x0d, false);
	expect(!wm_locked, "an old kernel: the lock opens (nothing to check against)");
	ka.nosys = false;
	lock_reload();

	// -- mid-drag --
	printf("5. a lock asked for mid-drag\n");
	dragging = idx;
	lock_pending = true;
	lock_engage();
	expect(!wm_locked && lock_pending, "waits while a window is being dragged");
	dragging = -1;
	lock_engage();
	expect(wm_locked, "locks once it is released");

	// -- a game grab while locked --
	printf("6. an app grabbing the screen\n");
	{
		z_msg_t m;
		memset(&m, 0, sizeof(m));
		m.from = 77;
		m.subject = Z_WM_GAME_GRAB;
		handle_message(&m);
		expect(game_grab_pid == 0 && app.revokes == 1, "revoked at once while locked");
	}

	// -- messages --
	printf("7. Z_WM_LOCK\n");
	type("hunter2");
	lock_key(0x0d, false);
	{
		z_msg_t m;
		memset(&m, 0, sizeof(m));
		m.from = 5;
		m.subject = Z_WM_LOCK;
		m.obj.type = Z_UINT32;
		m.obj.val.uint32 = Z_WM_LOCK_NOW;
		handle_message(&m);
		expect(lock_pending && !wm_locked, "LOCK_NOW asks; the main loop locks");
		lock_pending = false;
		ka.has = false;
		m.obj.val.uint32 = Z_WM_LOCK_RELOAD;
		handle_message(&m);
		expect(!(lock_st.flags & Z_AUTH_HAS_PASSWORD) && !lock_pending, "LOCK_RELOAD re-reads the policy");
		ka.has = true;
		handle_message(&m);
	}

	// -- the idle deadline --
	printf("8. idle\n");
	lock_last_input = z_uptime_ticks();
	{
		uint32_t w = wm_idle_ticks();
		expect(w > 0 && w <= 60u * Z_TICK_HZ, "wm wakes when the idle lock is due");
	}

	printf("wm lock test: %d checks, %d failed\n", checks, failures);
	return failures ? 1 : 0;
}
