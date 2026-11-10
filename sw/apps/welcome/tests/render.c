/*
 * Host test and render for sw/apps/welcome: the REAL welcome.c, with a
 * scripted kernel that keeps /sys/zeitlos.cfg and /sys/firstrun in
 * memory, a card that can be in or out, a flash key/value store, and a
 * config store loaded from the file by the same parser the kernel uses.
 *
 *   sudo sysctl -w vm.mmap_min_addr=0      # see sw/common/tests/ztramp.h
 *   make -C sw/apps/welcome test
 *
 * Exit 0 pass, 1 fail, 77 skipped.
 *
 * What it checks is what the app promises (docs/welcome_app.md):
 *
 *   - whatever started it is cleared at once (the card's mark, the
 *     store's key), so it opens by itself only once;
 *   - with no card it asks nothing: one welcome page, and Close;
 *   - with a card, every page can be done with the keyboard alone and
 *     with the pointer alone;
 *   - each answer is written as settings writes it, and every OTHER
 *     line of the file is left exactly as it was;
 *   - an answer that changes nothing writes nothing;
 *   - a board with no display register has no Display page;
 *   - the layouts listed are exactly zkbd's ordinary ones (kbd_names.h
 *     against the real table, zkbd.c, linked here only);
 *   - F1 shows the help in the window, and any key goes back.
 *
 * The display register is scripted: on the build machine it is plain
 * memory, and a write to it would wipe the signature the probe reads.
 */

#include "../../../common/tests/zrender.h"

#include <stdlib.h>
#include "zfs.h"		// the FS_* argument blocks
#include "zsoc.h"		// before the stand-ins below
#include "zwin.h"
#include "zkv.h"

// -- a scripted display --
static bool vid_present;
static uint32_t vid_mode;
static bool t_video_present(void) { return vid_present; }
static uint32_t t_video_get(void) { return vid_present ? vid_mode : Z_VIDEO_MODE_WHITE; }
static bool t_video_set(uint32_t m) { if (!vid_present || m > 3) return false; vid_mode = m; return true; }
#define z_video_mode_present t_video_present
#define z_video_get_mode t_video_get
#define z_video_set_mode t_video_set

// -- scripted launches --
static char ran[8][16], ran_arg[8][64], pending_arg[64];
static int ran_n, arg_takes;
static bool have_pending, apps_missing;
static const char *app_missing;		// one app not on the card
static uint32_t t_proc_run(const char *name) {
	if (apps_missing || (app_missing && !strcmp(name, app_missing))) return 0;
	snprintf(ran[ran_n], sizeof(ran[0]), "%s", name);
	snprintf(ran_arg[ran_n], sizeof(ran_arg[0]), "%s", have_pending ? pending_arg : "");
	have_pending = false;
	ran_n++;
	return 100u + (uint32_t)ran_n;
}
static void t_arg_set(const char *a) { snprintf(pending_arg, sizeof(pending_arg), "%s", a); have_pending = true; }
static bool t_arg_take(char *out, int n) {
	arg_takes++;
	if (!have_pending) { out[0] = 0; return false; }
	snprintf(out, (size_t)n, "%s", pending_arg);
	have_pending = false;
	return true;
}
#define z_proc_run t_proc_run
#define z_launch_arg_set t_arg_set
#define z_launch_arg_take t_arg_take

// Saving must not allocate: on the device an app's heap and stack
// share 16KB (docs/executables.md).
static int app_allocs;
__attribute__((unused)) static void *counted_malloc(size_t n) { app_allocs++; return malloc(n); }
#define malloc(n) counted_malloc(n)

#define main welcome_main_unused
#include "../welcome.c"
#undef main
#undef malloc

// -- a scripted kernel --

static char file[Z_CFG_FILE_MAX + 1];	// always NUL-terminated at file_len
static int file_len = -1;		// -1: no file
static bool card_in = true;		// a volume is mounted (FS_DF)
static bool read_only;			// the card is in, and refuses writes
static bool mark;			// /sys/firstrun is there
static int writes, unlinks;

static bool kv_seen, kv_writable = true;	// Z_WELCOME_KV_SEEN
static int kv_sets;

static struct { char k[Z_CFG_KEY_MAX], v[Z_CFG_VAL_MAX]; } store[32];
static int store_n;
static uint32_t generation = 1;
static z_obj_t k_ok, k_fail;

static void store_load(void) {
	char k[Z_CFG_KEY_MAX], v[Z_CFG_VAL_MAX];
	int pos = 0;
	store_n = 0;
	generation++;
	while (file_len > 0 && pos < file_len) {
		int start = pos;
		while (pos < file_len && file[pos] != '\n') pos++;
		if (z_cfg_parse_line(file + start, (size_t)(pos - start), k, sizeof(k),
			v, sizeof(v)) == Z_CFG_LINE_ENTRY) {
			int i;
			for (i = 0; i < store_n; i++) if (!strcmp(store[i].k, k)) break;
			if (i == store_n && store_n < 32) store_n++;
			snprintf(store[i].k, sizeof(store[i].k), "%s", k);
			snprintf(store[i].v, sizeof(store[i].v), "%s", v);
		}
		pos++;
	}
}

static void set_file(const char *text) {
	if (!text) { file_len = -1; file[0] = 0; }
	else { file_len = (int)strlen(text); memcpy(file, text, (size_t)file_len + 1); }
	store_load();
}

static bool writable(void) { return card_in && !read_only; }

static uint32_t *k_syscall(uint32_t id, uint32_t *args, uint32_t b) {

	(void)b;

	switch (id) {

	case Z_SYS_MSG_READ:
		return (uint32_t *)&k_fail;

	case Z_SYS_UPTIME:
		((z_obj_t *)args)->type = Z_UINT32;
		((z_obj_t *)args)->val.uint32 = 5000;
		return (uint32_t *)&k_ok;

	case Z_SYS_FS_DF: {
		z_fs_df_args_t *a = (z_fs_df_args_t *)args;
		a->total_kb = card_in ? 7774208u : 0;
		a->free_kb = card_in ? 7000000u : 0;
		return (uint32_t *)&k_ok;
	}

	case Z_SYS_FS_SIZE: {
		z_fs_size_args_t *a = (z_fs_size_args_t *)args;
		a->size = 0;
		if (card_in && !strcmp(a->name, Z_CFG_PATH) && file_len > 0) a->size = (uint32_t)file_len;
		if (card_in && !strcmp(a->name, Z_PATH_SYS_FIRSTRUN) && mark) a->size = 40;
		return (uint32_t *)&k_ok;
	}

	case Z_SYS_FS_READ: {
		z_fs_read_args_t *a = (z_fs_read_args_t *)args;
		a->len = 0;
		if (card_in && !strcmp(a->name, Z_CFG_PATH) && file_len > 0) {
			a->len = (uint32_t)file_len < a->maxlen ? (uint32_t)file_len : a->maxlen;
			memcpy(a->buf, file, a->len);
		}
		return (uint32_t *)&k_ok;
	}

	case Z_SYS_FS_WRITE: {
		z_fs_write_args_t *a = (z_fs_write_args_t *)args;
		if (!writable() || strcmp(a->name, Z_CFG_PATH) || a->len > Z_CFG_FILE_MAX) {
			a->written = 0;
			return (uint32_t *)&k_ok;
		}
		memcpy(file, a->buf, a->len);
		file_len = (int)a->len;
		file[file_len] = 0;
		a->written = a->len;
		writes++;
		return (uint32_t *)&k_ok;
	}

	case Z_SYS_FS_UNLINK: {
		z_fs_unlink_args_t *a = (z_fs_unlink_args_t *)args;
		if (!strcmp(a->name, Z_PATH_SYS_FIRSTRUN) && mark && writable()) {
			mark = false;
			unlinks++;
			return (uint32_t *)&k_ok;
		}
		return (uint32_t *)&k_fail;
	}

	case Z_SYS_KV: {
		z_kv_args_t *a = (z_kv_args_t *)args;
		if (!a->key || strcmp(a->key, Z_WELCOME_KV_SEEN)) { a->result = Z_KV_E_NOENT; break; }
		if (a->op == Z_KV_GET) {
			a->result = kv_seen ? Z_KV_OK : Z_KV_E_NOENT;
			a->len = kv_seen ? 1 : 0;
		} else if (a->op == Z_KV_SET) {
			if (!kv_writable) { a->result = Z_KV_E_READONLY; break; }
			kv_seen = a->len == 1 && ((char *)a->val)[0] == '1';
			kv_sets++;
			a->result = Z_KV_OK;
		} else
			a->result = Z_KV_E_INVAL;
		return (uint32_t *)&k_ok;
	}

	case Z_SYS_CFG_GET: {
		z_cfg_get_args_t *a = (z_cfg_get_args_t *)args;
		a->generation = generation;
		a->found = 0;
		for (int i = 0; a->key && i < store_n; i++)
			if (!strcmp(store[i].k, a->key)) {
				snprintf(a->val, a->vallen, "%s", store[i].v);
				a->found = 1;
			}
		return (uint32_t *)&k_ok;
	}

	case Z_SYS_CFG_ENTRY: {
		z_cfg_entry_args_t *a = (z_cfg_entry_args_t *)args;
		a->found = 0;
		if (a->index < (uint32_t)store_n) {
			snprintf(a->key, a->keylen, "%s", store[a->index].k);
			snprintf(a->val, a->vallen, "%s", store[a->index].v);
			a->found = 1;
		}
		return (uint32_t *)&k_ok;
	}

	case Z_SYS_CFG_RELOAD: {
		z_cfg_reload_args_t *a = (z_cfg_reload_args_t *)args;
		store_load();
		a->count = store_n;
		a->generation = generation;
		a->done = 1;
		return (uint32_t *)&k_ok;
	}

	default:
		break;

	}

	return (uint32_t *)&k_ok;

}

static bool k_install(void) {
	if ((uintptr_t)(void *)k_syscall > 0xFFFFFFFFu) return false;
	if (mmap((void *)0, 4096, PROT_READ | PROT_WRITE,
		MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0) == MAP_FAILED)
		return false;
	// socctl: reads 0 -- no colour window, no RTC.
	if (mmap((void *)0x70000000UL, 4096, PROT_READ | PROT_WRITE,
		MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0) == MAP_FAILED)
		return false;
	k_ok.type = Z_UINT32; k_ok.val.uint32 = Z_OK;
	k_fail.type = Z_UINT32; k_fail.val.uint32 = Z_FAIL;
	*(volatile uint32_t *)0x0000000c = (uint32_t)(uintptr_t)k_syscall;
	return true;
}

// -- checks --

static int checks, failures;

static void expect(bool ok, const char *what) {
	checks++;
	if (!ok) { failures++; printf("  FAIL: %s\n", what); }
}

// Pointer at a content-relative point: press, then release.
static void click(int cx, int cy) {
	z_clip_t c;
	z_win_content_rect(&win, &c);
	handle_mouse(Z_WM_PACK_MOUSE(c.x0 + cx, c.y0 + cy, Z_MOUSE_BTN_LEFT, 1));
	handle_mouse(Z_WM_PACK_MOUSE(c.x0 + cx, c.y0 + cy, 0, 1));
}

static void click_widget(int idx) {
	click(widgets[idx].x + widgets[idx].w / 2, widgets[idx].y + widgets[idx].h / 2);
}

// Clicks row `row` of a list, scrolled into view first without
// selecting it (the row above is selected, then the click moves the
// selection; the first row is scrolled to by selecting it).
static void click_row(z_listbox_t *lb, int row) {
	z_listbox_select(lb, row > 0 ? row - 1 : row);
	z_listbox_draw(lb, false);
	click(lb->x + 20, lb->y + 1 + (row - lb->top) * ROW_PX + 4);
}

static void type(const char *s) { for (; *s; s++) handle_key((uint32_t)(unsigned char)*s, 0); }

static void render(const char *prefix, const char *name) {
	char path[256];
	z_render_clear();
	repaint();
	snprintf(path, sizeof(path), "%s-%s.pbm", prefix, name);
	z_render_write(path, &win, 2);
}

// The app from the top, as main() starts it.
static void begin(void) {
	finished = false;
	first_run = false;
	help_on = false;
	status[0] = 0;
	can_set_video = z_video_mode_present();
	has_card = card_mounted();
	take_first_run();
	startup();
	enter_step(S_WELCOME);
}

static const char *sel_layout(void) {
	int r = z_listbox_selected(&kb_list);
	return r < 0 ? "" : kbd_names[r].name;
}

static void sel_zone(char *out, int n) {
	int r = z_listbox_selected(&tz_list);
	out[0] = 0;
	if (r >= 0) tz_row_value(r, out, n);
}

static bool controls_inside(void) {
	int cw = z_win_content_w(&win), ch = z_win_content_h(&win);
	for (int i = 0; i < W_COUNT; i++)
		if (widgets[i].x < 0 || widgets[i].y < 0 ||
			widgets[i].x + widgets[i].w > cw || widgets[i].y + widgets[i].h > ch)
			return false;
	return true;
}

int main(int argc, char **argv) {

	const char *prefix = argc > 1 ? argv[1] : "/tmp/welcome";
	char v[Z_CFG_VAL_MAX];
	int p, t;

	if (!z_render_open(&win, WIN_W, WIN_H) || !k_install()) {
		printf("welcome render: skipped (cannot map fixed addresses)\n");
		return 77;
	}

	// -- 0. the layouts are zkbd's, by name, and only its ordinary ones --
	{
		int n = 0;
		bool same = true;
		for (int i = 0; i < z_kbd_layout_count; i++) {
			if (z_kbd_layouts[i].ime != Z_KBD_IME_NONE) continue;
			if (n >= KB_COUNT || strcmp(kbd_names[n].name, z_kbd_layouts[i].name) ||
				strcmp(kbd_names[n].desc, z_kbd_layouts[i].desc)) same = false;
			n++;
		}
		expect(same && n == KB_COUNT && KB_COUNT > 10,
			"kbd_names.h is zkbd's table of ordinary layouts, in order");
		bool round = true;
		for (int i = 0; i < KB_COUNT; i++) if (kb_row_for(kbd_names[i].name) != i) round = false;
		expect(round && kb_row_for("DE") == kb_row_for("de") && kb_row_for("ja-us") < 0,
			"each row is found again by its name, any case; an input method has none");
	}

	// -- 1. a new card: the mark, no file, no display register --
	printf("1. a new card, by keyboard\n");
	set_file(NULL);
	mark = true;
	begin();
	expect(has_card && first_run && !mark && unlinks == 1, "the mark is removed before anything is asked");
	expect(kv_seen && kv_sets == 1, "and the machine is marked as having shown it");
	expect(step == S_WELCOME && !widgets[W_BACK].enabled && widgets[W_SKIP].enabled,
		"it opens on Welcome, with Skip and nothing to go back to");
	expect(focus == STOP_NAV + 1 && nav.focused == 1, "the keyboard is on Next");
	expect(!step_shown(S_DISPLAY), "no display register: no Display page");
	step_position(S_WELCOME, &p, &t);
	expect(p == 1 && t == 4, "Welcome is 1 of 4 on this board");
	expect(controls_inside(), "every control inside the window");
	expect(kb_list.y + kb_list.h < y_nav - 7, "the lists end above the line over the buttons");
	expect(done_row_y(3) + 2 + 2 * LINE_H < y_nav - 7, "the Ready page's text ends above it too");
	render(prefix, "1-welcome");

	// F1: the help in the window; any key goes back and does nothing else
	handle_key(Z_KEY_HELP, 0);
	expect(help_on, "F1: the help, in the window");
	render(prefix, "1b-help");
	handle_key(0x0d, 0);
	expect(!help_on && step == S_WELCOME, "Enter in the help: back to Welcome, not on to the next page");
	handle_key(Z_KEY_HELP, 0);
	click(100, 100);
	expect(!help_on && step == S_WELCOME, "a click in the help: back too");

	handle_key(0x1b, 0);
	expect(step == S_WELCOME, "Esc on Welcome stays");
	handle_key('\t', 0);
	expect(focus == STOP_PAGE + 0, "Tab from Next: Read the guide (Back is disabled)");
	handle_key('\t', 0);
	expect(focus == STOP_PAGE + 1, "Tab: Skip");
	handle_key('\t', 0);
	expect(focus == STOP_NAV + 1, "Tab again: Next");
	expect(widgets[W_GUIDE].y > y_body && widgets[W_GUIDE].y + BTN_H < y_nav - 7,
		"the guide button is between the words and the buttons");

	// the guide: the card's page in read
	ran_n = 0;
	click_widget(W_GUIDE);
	expect(ran_n == 1 && !strcmp(ran[0], "read") && !strcmp(ran_arg[0], Z_PATH_DOCS_WELCOME),
		"Read the guide: read, with " Z_PATH_DOCS_WELCOME);
	expect(step == S_WELCOME && !finished, "and the welcome stays open behind it");
	app_missing = "read";
	click_widget(W_GUIDE);
	expect(ran_n == 2 && !strcmp(ran[1], "text") && !strcmp(ran_arg[1], Z_PATH_DOCS_WELCOME_TXT) && !have_pending,
		"no read on the card: text, with the plain-text guide in flash");
	app_missing = NULL;
	ran_n = 0;
	expect(focus == STOP_PAGE + 0, "the keyboard stays on the button that was clicked");
	handle_key('\t', 0);
	handle_key('\t', 0);
	expect(focus == STOP_NAV + 1, "Tab, Tab: Next");
	handle_wheel(-1);		// no list on this page: nothing to scroll

	handle_key(0x0d, 0);
	expect(step == S_KEYBOARD && focus == STOP_LIST && kb_list.focused, "Enter: Keyboard, the keyboard on its list");
	expect(!strcmp(sel_layout(), "us"), "US is selected: it is the layout in use");
	expect(widgets[W_BACK].enabled, "Back is enabled now");
	type("g");
	expect(!strcmp(sel_layout(), "de"), "G jumps to German");
	render(prefix, "2-keyboard");
	expect(writes == 0, "selecting writes nothing");
	handle_key(0x0d, 0);
	expect(writes == 1 && strstr(file, "system.keyboard.layouts: de\n") != NULL, "Enter saves system.keyboard.layouts: de");
	expect(step == S_TIME && strstr(status, "German") != NULL, "and goes on to Time zone, saying what was saved");
	expect(tz_list.focused && z_listbox_selected(&tz_list) == 0, "UTC is selected: it is the zone in use");

	{
		bool round = true;
		for (int i = 0; i < TZ_ITEMS; i++) {
			tz_row_value(i, v, sizeof(v));
			if (tz_row_for(v) != i) { round = false; printf("  row %d '%s'\n", i, v); }
		}
		expect(round, "every zone row saves a value that selects the same row");
	}
	{
		int top = tz_list.top;
		handle_wheel(-2);
		expect(tz_list.top == top + 6 && z_listbox_selected(&tz_list) == 0,
			"the wheel scrolls the list and leaves the selection");
		handle_wheel(5);
		expect(tz_list.top == 0, "and back, stopping at the top");
	}
	type("ber");
	sel_zone(v, sizeof(v));
	expect(!strcmp(v, "Berlin"), "b-e-r jumps to Berlin");
	render(prefix, "3-timezone");
	handle_key(0x0d, 0);
	expect(writes == 2 && strstr(file, "system.rtc.timezone: Berlin\n") != NULL, "Enter saves system.rtc.timezone: Berlin");
	expect(strstr(file, "system.keyboard.layouts: de\n") != NULL, "and the keyboard line is still there");
	expect(step == S_DONE, "then Ready: this board has no Display page");
	expect(!strcmp(widgets[W_NEXT].label, "Finish") && focus == STOP_NAV + 1, "Next reads Finish, and has the keyboard");
	render(prefix, "4-ready");

	// -- the Ready page's two buttons (no tour: slide's is slide's own) --
	handle_key('\t', 0);
	expect(focus == STOP_PAGE + 0, "Tab from Finish wraps to the first button");
	handle_key(' ', 0);
	expect(ran_n == 1 && !strcmp(ran[0], "learn") && !ran_arg[0][0], "Start the course: learn");
	expect(step == S_DONE && !finished, "and the welcome stays open behind it");
	click_widget(W_PASSWORD);
	expect(ran_n == 2 && !strcmp(ran[1], "settings") && !strcmp(ran_arg[1], "security"),
		"Set a password: settings, at Security");
	apps_missing = true;
	arg_takes = 0;
	click_widget(W_PASSWORD);
	expect(ran_n == 2 && strstr(status, "settings is not on this card") != NULL, "an app that is not on the card: said so");
	expect(arg_takes == 1 && !have_pending, "and its argument is taken back, not left for the next app");
	apps_missing = false;

	// -- back through the pages: nothing changed, nothing written --
	handle_key(0x1b, 0);
	expect(step == S_TIME && !status[0], "Esc: back to Time zone");
	sel_zone(v, sizeof(v));
	expect(!strcmp(v, "Berlin"), "which now shows Berlin");
	handle_key(0x1b, 0);
	expect(step == S_KEYBOARD && !strcmp(sel_layout(), "de"), "Esc: back to Keyboard, which shows German");
	handle_key(0x0d, 0);
	handle_key(0x0d, 0);
	expect(step == S_DONE && writes == 2, "forward again with the same answers: no writes");
	handle_key(0x0d, 0);
	expect(finished, "Enter on Finish closes it");
	printf("--- the file a new card ends up with ---\n%s--- end ---\n", file);

	// -- 2. started again by hand --
	printf("2. run welcome, on a card that is already somebody's\n");
	begin();
	expect(!first_run && unlinks == 1 && kv_sets == 1, "no mark and the key already there: nothing written");

	// -- 3. a file with other things in it, by pointer alone --
	printf("3. an edited file, by pointer\n");
	static const char edited[] =
		"# my machine\r\n"
		"\r\n"
		"apps.term.auto_connect: port posix0\r\n"
		"system.keyboard.layouts: de, us\r\n"
		"this line is not a setting\r\n"
		"system.rtc.timezone: UTC+5:30\r\n"
		"apps.future.thing: 42\r\n";
	set_file(edited);
	writes = 0;
	begin();
	click_widget(W_NEXT);
	expect(step == S_KEYBOARD && !strcmp(sel_layout(), "de"), "Next: Keyboard shows the first of de, us");
	click_row(&kb_list, kb_row_for("fr"));
	expect(!strcmp(sel_layout(), "fr") && focus == STOP_LIST, "a click selects French");
	click_widget(W_NEXT);
	expect(writes == 1 && strstr(file, "system.keyboard.layouts: fr,de,us\n") != NULL,
		"saved as fr,de,us: the others stay, behind it");
	expect(step == S_TIME && z_listbox_selected(&tz_list) < 0, "Time zone: UTC+5:30 has no row, nothing selected");
	render(prefix, "5-timezone-no-row");
	click_widget(W_NEXT);
	expect(writes == 1 && strstr(file, "system.rtc.timezone: UTC+5:30\r\n") != NULL, "Next with nothing selected keeps it");
	expect(strstr(file, "# my machine\r\n\r\napps.term.auto_connect: port posix0\r\n") != NULL &&
		strstr(file, "this line is not a setting\r\n") != NULL &&
		strstr(file, "apps.future.thing: 42\r\n") != NULL,
		"every other line is as it was: comments, unknown keys, CRLF");
	click_widget(W_BACK);
	click_widget(W_BACK);
	expect(step == S_KEYBOARD, "Back, Back: Keyboard");
	click_row(&kb_list, kb_row_for("us"));
	expect(!strcmp(sel_layout(), "us"), "a click selects English (US), the first row");
	click_widget(W_NEXT);
	expect(strstr(file, "system.keyboard.layouts: us,fr,de\n") != NULL, "US first, the others behind it");
	click_widget(W_NEXT);
	click_widget(W_NEXT);
	expect(finished, "Finish, by pointer");

	// an input method first: shown as it is, and kept behind the choice
	set_file("system.keyboard.layouts: ja-us,us\n# kept\n");
	begin();
	handle_key(0x0d, 0);
	expect(z_listbox_selected(&kb_list) < 0 && !strcmp(kb_first, "ja-us"),
		"ja-us in use: no row selected, the name shown");
	render(prefix, "5b-keyboard-input-method");
	type("english (us");
	expect(!strcmp(sel_layout(), "us"), "typing a longer prefix finds English (US)");
	handle_key(0x0d, 0);
	expect(strstr(file, "system.keyboard.layouts: us,ja-us\n# kept\n") != NULL,
		"US first, ja-us kept behind it, once, and the comment kept");

	// a double-click on a row takes it and goes on
	set_file(NULL);
	begin();
	handle_key(0x0d, 0);
	{
		int row = kb_row_for("it");
		z_listbox_select(&kb_list, row);
		z_listbox_draw(&kb_list, false);
		int y = kb_list.y + 1 + (row - kb_list.top) * ROW_PX + 4;
		click(kb_list.x + 20, y);
		click(kb_list.x + 20, y);
		expect(step == S_TIME && strstr(file, "system.keyboard.layouts: it\n") != NULL,
			"a double-click on Italian saves it and goes on");
	}

	// -- 4. a board with the display register --
	printf("4. a board with display colours\n");
	set_file(NULL);
	writes = 0;
	vid_present = true;
	vid_mode = Z_VIDEO_MODE_WHITE;
	begin();
	expect(step_shown(S_DISPLAY), "the Display page is there");
	step_position(S_DISPLAY, &p, &t);
	expect(p == 4 && t == 5, "as 4 of 5");
	handle_key(0x0d, 0);
	handle_key(0x0d, 0);
	handle_key(0x0d, 0);
	expect(step == S_DISPLAY && writes == 0, "three Enters with nothing changed: Display, no writes");
	expect(z_widget_group_selection(&psets[S_DISPLAY], GROUP_VIDEO) == 0, "White is selected: it is what the display shows");
	render(prefix, "6-display");
	click_widget(W_MODE0 + 1);
	expect(vid_mode == Z_VIDEO_MODE_AMBER && strstr(file, "system.video.mode: amber\n") != NULL,
		"a click on Amber applies it and saves it");
	expect(z_widget_group_selection(&psets[S_DISPLAY], GROUP_VIDEO) == 1, "and Amber is the one selected");
	expect(focus == STOP_PAGE + 1, "the keyboard is on the control that was clicked");
	handle_key(Z_KEY_RIGHT, 0);
	handle_key(' ', 0);
	expect(vid_mode == Z_VIDEO_MODE_GREEN && strstr(file, "system.video.mode: green\n") != NULL,
		"Right, Space: Green");
	expect(step == S_DISPLAY, "still on Display: Space pressed the control, not Next");
	handle_key(Z_KEY_RIGHT, 0);
	handle_key(0x0d, 0);
	expect(vid_mode == Z_VIDEO_MODE_PAPER && step == S_DISPLAY, "Right, Enter: Paper -- Enter on a control presses it");
	handle_key('\t', 0);
	expect(focus == STOP_NAV + 0, "Tab: Back");
	handle_key('\t', 0);
	expect(focus == STOP_NAV + 1, "Tab: Next");
	handle_key('\t', 0);
	expect(focus == STOP_PAGE + 0, "Tab wraps to White");
	handle_key('\t', Z_KBD_MOD_LSHIFT);
	expect(focus == STOP_NAV + 1, "Shift+Tab: Next again");
	handle_key(0x0d, 0);
	expect(step == S_DONE, "Enter: Ready");
	render(prefix, "7-ready-colour-board");
	vid_present = false;

	// -- 5. no card: the welcome only --
	printf("5. no card\n");
	set_file(NULL);
	card_in = false;
	kv_seen = false;
	kv_sets = 0;
	writes = 0;
	begin();
	expect(!has_card && !first_run, "no card mounted");
	expect(kv_seen && kv_sets == 1, "the machine is marked, so it does not open by itself again");
	expect(step_after(S_WELCOME, 1) < 0, "Welcome is the only page: nothing is asked");
	step_position(S_WELCOME, &p, &t);
	expect(t == 1, "1 page, so no \"1 of 1\"");
	expect(!strcmp(widgets[W_NEXT].label, "Close") && !widgets[W_SKIP].enabled && !widgets[W_BACK].enabled,
		"Close, and no Skip or Back");
	expect(focus == STOP_NAV + 1, "the keyboard is on Close");
	handle_key('\t', 0);
	expect(focus == STOP_PAGE + 0, "Tab: Read the guide (Skip is not there)");
	handle_key('\t', 0);
	expect(focus == STOP_NAV + 1, "Tab: Close again");
	render(prefix, "8-no-card");
	app_missing = "read";
	ran_n = 0;
	handle_key('\t', 0);
	handle_key(0x0d, 0);
	expect(ran_n == 1 && !strcmp(ran[0], "text") && !strcmp(ran_arg[0], Z_PATH_DOCS_WELCOME_TXT) && !finished,
		"no card: Enter on Read the guide opens the guide in text, and the welcome stays");
	app_missing = NULL;
	handle_key('\t', 0);
	handle_key(Z_KEY_HELP, 0);
	render(prefix, "8b-no-card-help");
	handle_key(0x1b, 0);
	expect(!help_on && !finished, "Esc in the help: back to the welcome");
	handle_key(0x0d, 0);
	expect(finished && writes == 0 && file_len < 0, "Enter closes it, having written nothing");

	// a store that cannot be written: survivable, and nothing else changes
	kv_seen = false;
	kv_writable = false;
	begin();
	expect(!kv_seen && !finished && step == S_WELCOME, "a read-only store: the welcome still opens");
	kv_writable = true;
	card_in = true;

	// -- 6. a card that refuses writes --
	printf("6. a write-protected card\n");
	set_file(NULL);
	read_only = true;
	mark = true;
	unlinks = 0;
	begin();
	expect(has_card && first_run && mark && unlinks == 0, "the mark cannot be removed, and that is survivable");
	handle_key(0x0d, 0);
	type("g");
	handle_key(0x0d, 0);
	expect(step == S_TIME && strstr(status, "not saved") != NULL, "it says the answer was not saved, and goes on");
	render(prefix, "9-read-only");
	read_only = false;
	mark = false;

	// -- 7. Skip --
	printf("7. Skip\n");
	set_file(NULL);
	writes = 0;
	begin();
	click_widget(W_SKIP);
	expect(finished && writes == 0 && file_len < 0, "Skip closes it and writes nothing");

	expect(app_allocs == 0, "nothing was allocated, start to finish");
	printf("welcome render: %d checks, %d failed\n", checks, failures);
	return failures ? 1 : 0;

}
