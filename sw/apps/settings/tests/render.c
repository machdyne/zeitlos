/*
 * Host test and render for sw/apps/settings: the REAL settings.c, with
 * a scripted kernel that keeps /zeitlos.cfg in memory and a config
 * store loaded from it by the same parser the kernel uses.
 *
 *   sudo sysctl -w vm.mmap_min_addr=0      # see sw/common/tests/ztramp.h
 *   cc -std=gnu99 -Wall -no-pie -I sw/common -o /tmp/settings_render \
 *      sw/apps/settings/tests/render.c sw/common/zwin.c \
 *      sw/common/zwidget.c sw/common/zfont_data.c sw/common/zobj.c \
 *      sw/common/zeitlos.c sw/common/zfsapp.c sw/common/zcfg.c \
 *      sw/common/zrtc.c
 *   /tmp/settings_render /tmp/settings
 *
 * Exit 0 pass, 1 fail, 77 skipped.
 *
 * The check that matters is the one the app exists to get right:
 * saving a setting through it leaves every OTHER line of the file --
 * comments, keys nothing here knows, lines that are not settings at
 * all -- exactly as it was. See docs/config.md.
 *
 * Dialogs are replaced by a scripted answer: zdialog.c needs a real wm
 * round trip, and what is being tested is what the app does with the
 * answer.
 */

#include "../../../common/tests/zrender.h"

#include <stdlib.h>
#include "zfs.h"		// the FS_SIZE/READ/WRITE argument blocks

// save() must not allocate: on the device an app's heap and stack share
// 16KB, and an 8KB malloc there failed with "out of memory" while
// succeeding here. Every allocation settings.c makes is counted, and
// the test fails if saving makes any.
static int app_allocs;
__attribute__((unused)) static void *counted_malloc(size_t n) { app_allocs++; return malloc(n); }
__attribute__((unused)) static char *counted_mallocfile(char *name) { (void)name; app_allocs++; return NULL; }
#define malloc(n) counted_malloc(n)
#define fs_mallocfile(n) counted_mallocfile(n)

#define main settings_main_unused
#include "../settings.c"
#undef main
#undef malloc
#undef fs_mallocfile

// -- scripted dialogs --

static const char *prompt_answer;	// NULL = the user cancels
static int confirms;

bool z_dialog_prompt(const z_dialog_ctx_t *c, const char *t, const char *m,
	const char *initial, char *out, int n) {
	(void)c; (void)t; (void)m; (void)initial;
	if (!prompt_answer || !*prompt_answer) return false;
	snprintf(out, (size_t)n, "%s", prompt_answer);
	return true;
}

int z_dialog_confirm(const z_dialog_ctx_t *c, const char *t, const char *m, int b) {
	(void)c; (void)t; (void)m; (void)b;
	confirms++;
	return Z_DIALOG_YES;
}

// -- a scripted kernel: one file, and a store loaded from it --

static char file[Z_CFG_FILE_MAX + 1];	// always NUL-terminated at file_len
static int file_len = -1;		// -1: no file

static struct { char k[Z_CFG_KEY_MAX], v[Z_CFG_VAL_MAX]; } store[32];
static int store_n;
static uint32_t generation = 1;
static z_obj_t k_ok, k_fail;
static uint32_t ticks = 5000;
static void ticks_bump(void) { ticks += 2 * 732; }

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

static uint32_t *k_syscall(uint32_t id, uint32_t *args, uint32_t b) {

	(void)b;

	switch (id) {

	case Z_SYS_MSG_READ:
		return (uint32_t *)&k_fail;

	case Z_SYS_UPTIME:
		((z_obj_t *)args)->type = Z_UINT32;
		((z_obj_t *)args)->val.uint32 = ticks;
		return (uint32_t *)&k_ok;

	case Z_SYS_FS_SIZE: {
		z_fs_size_args_t *a = (z_fs_size_args_t *)args;
		a->size = (!strcmp(a->name, Z_CFG_PATH) && file_len > 0) ? (uint32_t)file_len : 0;
		return (uint32_t *)&k_ok;
	}

	case Z_SYS_FS_READ: {
		z_fs_read_args_t *a = (z_fs_read_args_t *)args;
		a->len = 0;
		if (!strcmp(a->name, Z_CFG_PATH) && file_len > 0) {
			a->len = (uint32_t)file_len < a->maxlen ? (uint32_t)file_len : a->maxlen;
			memcpy(a->buf, file, a->len);
		}
		return (uint32_t *)&k_ok;
	}

	case Z_SYS_FS_WRITE: {
		z_fs_write_args_t *a = (z_fs_write_args_t *)args;
		if (strcmp(a->name, Z_CFG_PATH) || a->len > Z_CFG_FILE_MAX) {
			a->written = 0;
			return (uint32_t *)&k_ok;
		}
		memcpy(file, a->buf, a->len);
		file_len = (int)a->len;
		file[file_len] = 0;
		a->written = a->len;
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
		return (uint32_t *)&k_ok;

	}

}

static bool k_install(void) {
	if ((uintptr_t)(void *)k_syscall > 0xFFFFFFFFu) return false;
	if (mmap((void *)0, 4096, PROT_READ | PROT_WRITE,
		MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0) == MAP_FAILED)
		return false;
	// socctl, for z_video_mode_present(): reads 0, "no register".
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

// Clicks list row `row`, scrolling it into view first without selecting
// it (the row above is selected, then the click moves the selection).
static void click_row(int row) {
	z_listbox_select(&tz_list, row > 0 ? row - 1 : row);
	int r = row - tz_list.top;
	click(tz_list.x + 20, tz_list.y + 1 + r * (z_font_5x8.h + 2) + 4);
}

static void click_widget(int idx) {
	click(widgets[idx].x + widgets[idx].w / 2, widgets[idx].y + widgets[idx].h / 2);
}

static void render(const char *prefix, const char *name) {
	char path[256];
	z_render_clear();
	repaint();
	snprintf(path, sizeof(path), "%s-%s.pbm", prefix, name);
	z_render_write(path, &win, 2);
}

int main(int argc, char **argv) {

	const char *prefix = argc > 1 ? argv[1] : "/tmp/settings";

	if (!z_render_open(&win, WIN_W, WIN_H) || !k_install()) {
		printf("settings render: skipped (cannot map fixed addresses)\n");
		return 77;
	}

	// -- 1. no file: defaults --
	printf("1. no /zeitlos.cfg\n");
	z_listbox_init(&tz_list, &win);
	z_listbox_set_items(&tz_list, TZ_ITEMS, tz_label, NULL);
	read_values();
	widgets_init();
	layout();
	expect(!prefs[0].in_file && !tz_in_file, "nothing from the file");
	expect(!strcmp(tz_value, "UTC"), "time zone shows its default");
	expect(z_listbox_selected(&tz_list) == 0, "UTC row selected");

	// every row's value parses back to that row
	{
		bool round = true;
		char v[Z_CFG_VAL_MAX];
		for (int i = 0; i < TZ_ITEMS; i++) {
			tz_row_value(i, v, sizeof(v));
			if (tz_row_for(v) != i) { round = false; printf("  row %d '%s'\n", i, v); }
		}
		expect(round, "every list row saves a value that selects the same row");
	}
	render(prefix, "1-defaults");

	// Widgets must stay inside the content area and clear of each other.
	{
		int cw = z_win_content_w(&win), ch = z_win_content_h(&win);
		bool inside = true;
		for (int i = 0; i < W_COUNT; i++)
			if (widgets[i].x < 0 || widgets[i].y < 0 ||
				widgets[i].x + widgets[i].w > cw || widgets[i].y + widgets[i].h > ch)
				inside = false;
		expect(inside, "every control inside the window");
		expect(y_status + LINE_H <= ch, "status line inside the window");
	}

	// -- 2. a file with things settings does not know about --
	printf("2. editing preserves the rest of the file\n");
	file_len = snprintf(file, sizeof(file),
		"# my config\r\n"
		"\r\n"
		"apps.someone.else: keep this = exactly\r\n"
		"system.rtc.timezone: UTC\r\n"
		"not a valid line!\r\n"
		"apps.term.auto_connect: port repl0\r\n");
	store_load();
	read_values();
	expect(prefs[0].in_file && !strcmp(prefs[0].value, "port repl0"), "auto_connect read");
	expect(other_keys == 1, "one other key counted");
	render(prefix, "2-from-file");

	// Tab order: Edit -> list -> Reload
	z_widget_focus_set(&wset, W_EDIT_TERM);
	handle_key('\t', 0);
	expect(list_focus && wset.focused < 0, "Tab from Edit enters the list");

	// type-to-find, then Enter saves
	// "mu" is Mumbai (it sorts first); "mun" is Munich
	handle_key('m', 0);
	handle_key('u', 0);
	expect(z_listbox_selected(&tz_list) >= 1 &&
		!strcmp(z_tz_cities[z_listbox_selected(&tz_list) - 1].city, "Mumbai"),
		"typing 'mu' finds Mumbai");
	handle_key('n', 0);
	expect(z_listbox_selected(&tz_list) >= 1 &&
		!strcmp(z_tz_cities[z_listbox_selected(&tz_list) - 1].city, "Munich"),
		"typing 'mun' finds Munich");
	expect(!strstr(file, "Munich"), "selecting alone writes nothing");
	expect(widgets[W_SET_TZ].enabled, "Set time zone enabled once a different zone is selected");
	expect(strstr(status, "Munich selected") != NULL, "status line says how to use it");
	handle_key(0x0d, 0);
	expect(strstr(file, "system.rtc.timezone: Munich\n") != NULL, "Enter saves the zone");
	expect(app_allocs == 0, "saving allocates nothing (static buffers)");
	expect(!widgets[W_SET_TZ].enabled, "Set disabled once the selection is the zone in use");
	expect(strstr(file, "# my config\r\n\r\napps.someone.else: keep this = exactly\r\n") == file,
		"comment, blank line and unknown key untouched");
	expect(strstr(file, "not a valid line!\r\n") != NULL, "invalid line untouched");
	expect(strstr(file, "apps.term.auto_connect: port repl0\r\n") != NULL,
		"the other known key untouched");
	expect(!strcmp(tz_value, "Munich") && tz_in_file, "reloaded value shown");
	render(prefix, "3-after-edit");

	// a repeated letter walks the matches: Nairobi, then New York
	tz_list.find_tick = 0;
	ticks_bump();
	handle_key('n', 0);
	int first = z_listbox_selected(&tz_list);
	handle_key('n', 0);
	int second = z_listbox_selected(&tz_list);
	expect(first >= 1 && !strcmp(z_tz_cities[first - 1].city, "Nairobi") &&
		second >= 1 && !strcmp(z_tz_cities[second - 1].city, "New York"),
		"repeating a letter moves to the next match");

	// A letter jumps to the FIRST row with it; the same letter again
	// walks the matches; a fresh press on a row that already starts
	// with it moves on rather than staying put.
	{
		#define SEL_CITY() (z_listbox_selected(&tz_list) >= 1 && \
			z_listbox_selected(&tz_list) <= z_tz_city_count ? \
			z_tz_cities[z_listbox_selected(&tz_list) - 1].city : "?")
		ticks_bump();
		z_listbox_select(&tz_list, tz_row_for("Tokyo"));
		handle_key('b', 0);
		expect(!strcmp(SEL_CITY(), "Bangkok"), "B from Tokyo is Bangkok");
		handle_key('b', 0);
		expect(!strcmp(SEL_CITY(), "Beijing"), "B again is Beijing");
		handle_key('b', 0);
		expect(!strcmp(SEL_CITY(), "Berlin"), "and again Berlin");
		ticks_bump();
		z_listbox_select(&tz_list, tz_row_for("Brussels"));
		handle_key('b', 0);
		expect(!strcmp(SEL_CITY(), "Bucharest"),
			"B on Brussels, after a pause, moves on (used to stay put)");
		ticks_bump();
		handle_key('B', Z_KBD_MOD_LSHIFT);
		expect(!strcmp(SEL_CITY(), "Buenos Aires"), "capital B works the same");
		ticks_bump();
		handle_key('b', 0);
		expect(!strcmp(SEL_CITY(), "Bangkok"), "wraps back to the first B");
		ticks_bump();
		handle_key('x', 0);
		expect(!strcmp(SEL_CITY(), "Bangkok"), "a letter with no rows leaves the selection");
	}

	handle_key(Z_KEY_END, 0);
	expect(z_listbox_selected(&tz_list) == TZ_ITEMS - 1, "End reaches UTC+14");
	handle_key('\t', 0);
	expect(!list_focus && wset.focused == W_SET_TZ,
		"Tab leaves the list for Set while the selection is unsaved");

	char before[sizeof(file)];
	memcpy(before, file, sizeof(file));

	// cancel writes nothing
	prompt_answer = NULL;
	activate(W_EDIT_TERM);
	expect(!memcmp(before, file, sizeof(file)), "cancel writes nothing");

	// 'default' removes the line
	prompt_answer = "default";
	activate(W_EDIT_TERM);
	expect(strstr(file, "auto_connect") == NULL, "'default' removes the line");
	expect(strstr(file, "apps.someone.else: keep this = exactly") != NULL,
		"unknown key still there after a removal");
	expect(!prefs[0].in_file, "auto_connect back to its default");

	prompt_answer = "telnet bbs.example.com";
	activate(W_EDIT_TERM);
	expect(strstr(file, "apps.term.auto_connect: telnet bbs.example.com\n") != NULL,
		"auto_connect appended");

	prompt_answer = "launch the missiles";
	confirms = 0;
	memcpy(before, file, sizeof(file));
	activate(W_EDIT_TERM);
	expect(confirms == 1 && !memcmp(before, file, sizeof(file)),
		"a value that is not a connection is refused");

	// with focus on a button, a letter still goes to the list
	z_widget_focus_set(&wset, 1);
	ticks_bump();
	handle_key('b', 0);
	expect(list_focus && !strcmp(SEL_CITY(), "Bangkok"),
		"a letter while a button has focus moves focus to the list and jumps");
	expect(!strstr(file, "Bangkok"), "jumping does not save");
	handle_key('\t', 0);

	// -- the mouse path: single click selects, the button saves --
	ticks_bump();
	click_row(tz_row_for("Berlin"));
	expect(z_listbox_selected(&tz_list) == tz_row_for("Berlin"), "single click selects Berlin");
	expect(!strstr(file, "Berlin"), "a single click does not save");
	expect(widgets[W_SET_TZ].enabled && strstr(status, "Berlin selected"),
		"button enabled and the hint shown");
	render(prefix, "5-selected-unsaved");
	click_widget(W_SET_TZ);
	expect(strstr(file, "system.rtc.timezone: Berlin\n") != NULL, "clicking Set time zone saves");
	expect(!strcmp(tz_value, "Berlin") && !widgets[W_SET_TZ].enabled,
		"now in use; button disabled again");
	expect(strstr(status, "saved") != NULL, "status confirms the save");
	expect(list_focus, "focus returns to the list after Set");

	// Tab from the list goes to Set while there is something to set
	ticks_bump();
	handle_key('t', 0);			// Taipei
	handle_key('\t', 0);
	expect(!list_focus && wset.focused == W_SET_TZ, "Tab from the list reaches Set");
	handle_key(0x0d, 0);
	expect(strstr(file, "system.rtc.timezone: Taipei\n") != NULL, "Enter on Set saves");
	handle_key('\t', 0);

	// a hand-written offset with minutes has no row, and is shown
	file_len = snprintf(file, sizeof(file), "system.rtc.timezone: UTC+5:30\n");
	store_load();
	read_values();
	expect(z_listbox_selected(&tz_list) < 0 && !strcmp(tz_value, "UTC+5:30"),
		"a value with no row: no selection, value still shown");

	render(prefix, "4-final");

	printf("--- final /zeitlos.cfg ---\n%.*s--- end ---\n", file_len, file);
	printf("settings render: %d checks, %d failed\n", checks, failures);
	return failures ? 1 : 0;

}
