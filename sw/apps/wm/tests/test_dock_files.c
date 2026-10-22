/*
 * Host test for the dock built from its two files (docs/dock.md): the
 * REAL wm.c -- dock_build(), dock_reload(), draw_dock() -- with a
 * scripted kernel that serves /data/wm/wm.cfg and /data/wm/dock.zic
 * from memory, says which apps are installed and which hardware the
 * bitstream has, and a software framebuffer (sw/common/tests/zrender.h)
 * to look at the result.
 *
 *   sudo sysctl -w vm.mmap_min_addr=0      # see sw/common/tests/ztramp.h
 *   cc -std=gnu99 -w -no-pie -I sw/common -o /tmp/wmdock \
 *      sw/apps/wm/tests/test_dock_files.c sw/common/zwin.c sw/common/zwidget.c \
 *      sw/common/zfont_data.c sw/common/zobj.c sw/common/zeitlos.c \
 *      sw/common/zkbd.c sw/common/zspeak.c sw/common/zcaption.c \
 *      sw/common/zcfg.c sw/apps/wm/dock_cfg.c sw/apps/wm/win_icons.c \
 *      sw/apps/wm/workspace.c
 *   /tmp/wmdock sw/data/wm.cfg sw/data/icons/dock.zic /tmp/wmdock
 *
 * Exit 0 pass, 1 fail, 77 skipped (no fixed mappings on this host).
 */
#include "../../../common/tests/zrender.h"

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

// -- a scripted kernel --

static z_obj_t k_ok, k_fail;
static uint32_t ticks = 10000;
static uint32_t cfg_gen = 1;

// The files, by path. NULL data: not there. `chunk` caps every
// READ_CHUNK, so wm.cfg arrives in pieces as it may on the board.
#define NFILES 3
static struct { const char *path; const uint8_t *data; uint32_t len; } files[NFILES] = {
	{ Z_PATH_WM_CONFIG, NULL, 0 },
	{ Z_PATH_WM_ICONS, NULL, 0 },
	{ Z_PATH_WM_CURSOR, NULL, 0 },
};
static uint32_t chunk_cap = 7;
static int fs_calls;

static struct { int file; uint32_t pos; bool open; } handle;

static int file_of(const char *path) {
	for (int i = 0; i < NFILES; i++)
		if (strcmp(files[i].path, path) == 0 && files[i].data) return i;
	return -1;
}

// What is installed: everything but these.
static const char *missing[8];
static int nmissing;
static bool installed(const char *name) {
	for (int i = 0; i < nmissing; i++) if (strcmp(missing[i], name) == 0) return false;
	return true;
}

// Z_SYS_PROC_RUN: the pid it gives (0: a failure), and what was run.
static uint32_t run_pid = 20;
static int runs;
static char last_run[32];

// The process table Z_SYS_PROC_LIST reports.
static z_proc_info_t ptab[40];
static int nptab;
static int proc_lists;
static void proc(const char *name, uint32_t flags) {
	z_proc_info_t *p = &ptab[nptab];
	memset(p, 0, sizeof(*p));
	p->pid = (uint32_t)nptab + 2;
	p->flags = flags;
	strncpy(p->name, name, sizeof(p->name) - 1);
	nptab++;
}

static uint32_t *k_syscall(uint32_t id, uint32_t *args, uint32_t b) {
	(void)b;
	switch (id) {
	case Z_SYS_UPTIME:
		ticks += 3;
		((z_obj_t *)args)->type = Z_UINT32;
		((z_obj_t *)args)->val.uint32 = ticks;
		return (uint32_t *)&k_ok;
	case Z_SYS_MSG_READ:
		return (uint32_t *)&k_fail;
	case Z_SYS_CFG_GET: {
		z_cfg_get_args_t *a = (z_cfg_get_args_t *)args;
		a->generation = cfg_gen;
		a->found = 0;
		return (uint32_t *)&k_ok;
	}
	case Z_SYS_FS_SIZE: {
		z_fs_size_args_t *a = (z_fs_size_args_t *)args;
		int f = file_of(a->name);
		fs_calls++;
		a->size = f >= 0 ? files[f].len : 0;
		return (uint32_t *)&k_ok;
	}
	case Z_SYS_FS_READ: {
		z_fs_read_args_t *a = (z_fs_read_args_t *)args;
		int f = file_of(a->name);
		fs_calls++;
		if (f < 0 || a->maxlen < files[f].len) return (uint32_t *)&k_fail;
		memcpy(a->buf, files[f].data, files[f].len);
		a->len = files[f].len;
		return (uint32_t *)&k_ok;
	}
	case Z_SYS_FS_OPEN_READ: {
		z_fs_open_args_t *a = (z_fs_open_args_t *)args;
		int f = file_of(a->name);
		fs_calls++;
		if (f < 0 || handle.open) { a->handle = -1; return (uint32_t *)&k_fail; }
		handle.file = f; handle.pos = 0; handle.open = true;
		a->handle = 3;
		return (uint32_t *)&k_ok;
	}
	case Z_SYS_FS_READ_CHUNK: {
		z_fs_read_chunk_args_t *a = (z_fs_read_chunk_args_t *)args;
		fs_calls++;
		if (!handle.open || a->handle != 3) return (uint32_t *)&k_fail;
		uint32_t left = files[handle.file].len - handle.pos;
		uint32_t n = left < a->maxlen ? left : a->maxlen;
		if (n > chunk_cap) n = chunk_cap;
		memcpy(a->buf, files[handle.file].data + handle.pos, n);
		handle.pos += n;
		a->len = n;
		return (uint32_t *)&k_ok;
	}
	case Z_SYS_FS_CLOSE:
		handle.open = false;
		return (uint32_t *)&k_ok;
	case Z_SYS_PROC_RUN: {
		z_obj_t *o = (z_obj_t *)args;
		runs++;
		strncpy(last_run, o->val.str, sizeof(last_run) - 1);
		o->type = Z_UINT32;
		o->val.uint32 = run_pid;
		return run_pid ? (uint32_t *)&k_ok : (uint32_t *)&k_fail;
	}
	case Z_SYS_PROC_LIST: {
		z_proc_list_args_t *a = (z_proc_list_args_t *)args;
		uint32_t n = (uint32_t)nptab < a->max ? (uint32_t)nptab : a->max;
		memcpy(a->out, ptab, n * sizeof(z_proc_info_t));
		a->count = n;
		a->truncated = (uint32_t)nptab > a->max;
		proc_lists++;
		return (uint32_t *)&k_ok;
	}
	case Z_SYS_EXEC_EXISTS_MANY: {
		z_exec_many_args_t *a = (z_exec_many_args_t *)args;
		for (uint32_t i = 0; i < a->count; i++) a->found[i] = installed(a->names[i]);
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

// The bitstream's CSRs (sw/common/zsoc.h): which hardware it has.
static void hardware(bool audio, bool gpio) {
	reg_csr_magic = Z_CSR_MAGIC;
	reg_csr_features = audio ? Z_FEATURE_AUDIO : 0;
	reg_csr_features2 = (gpio ? Z_FEATURE2_GPIO : 0) | ((uint32_t)Z_CSR_FEATURES2_SIG << 16);
}

// -- checking --

static int checks, failures;
static void expect(bool ok, const char *what) {
	checks++;
	if (!ok) { failures++; printf("  FAIL: %s\n", what); }
}

static const char *prefix = "/tmp/wmdock";

static void dump(const char *name) {
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

static void *slurp(const char *path, uint32_t *len) {
	FILE *f = fopen(path, "rb");
	if (!f) return NULL;
	fseek(f, 0, SEEK_END);
	long n = ftell(f);
	fseek(f, 0, SEEK_SET);
	void *buf = malloc((size_t)n);
	if (fread(buf, 1, (size_t)n, f) != (size_t)n) { fclose(f); free(buf); return NULL; }
	fclose(f);
	*len = (uint32_t)n;
	return buf;
}

static void set_file(int i, const void *data, uint32_t len) {
	files[i].data = (const uint8_t *)data;
	files[i].len = len;
}

static void set_cfg(const char *text) {
	set_file(0, text, text ? (uint32_t)strlen(text) : 0);
}

static int find_app(const char *name) {
	for (int i = 0; i < dock_app_count; i++) if (strcmp(dock_apps[i].name, name) == 0) return i;
	return -1;
}

// Where the slot's icon is on screen, whichever side the dock is on.
static void slot_at(int slot, int *x, int *y) {
	wm_window_t *w = &windows[dock_idx];
	dock_slot_xy(slot, x, y);
	*x += (int)w->x;
	*y += (int)w->y;
}
static int slot_x(int slot) { int x, y; slot_at(slot, &x, &y); return x; }

// Does the slot at `slot` on screen show `bitmap` (ink exactly where its
// bits are, inside the slot's outline)?
static bool slot_shows(int slot, const uint8_t *bitmap) {
	int x0, y0;
	slot_at(slot, &x0, &y0);
	for (int y = 1; y < DOCK_ICON_SIZE - 1; y++)
		for (int x = 1; x < DOCK_ICON_SIZE - 1; x++) {
			int bit = (bitmap[y * 4 + x / 8] >> (7 - (x % 8))) & 1;
			if (bit != z_render_get(x0 + x, y0 + y)) return false;
		}
	return true;
}

// Item i's icon, expanded from the store, or NULL: it has none.
static const uint8_t *bits_of(uint16_t off, uint8_t len) {
	static uint8_t b[4][DK_ICON_BYTES];
	static int k;
	uint8_t *o = b[k++ & 3];
	if (!len || !dk_icon_expand(dock_store + off, len, o)) return NULL;
	return o;
}
static const uint8_t *icon_of(int i) { return bits_of(dock_apps[i].icon_off, dock_apps[i].icon_len); }

static bool shows_item(int slot, int i) {
	const uint8_t *b = icon_of(i);
	return b && slot_shows(slot, b);
}

static int slot_ink(int slot) {
	int x0, y0, n = 0;
	slot_at(slot, &x0, &y0);
	for (int y = 2; y < DOCK_ICON_SIZE - 2; y++)
		for (int x = 2; x < DOCK_ICON_SIZE - 2; x++) n += z_render_get(x0 + x, y0 + y);
	return n;
}

static void draw_all(void) {
	z_render_clear();
	repair_region(0, 0, WM_SCREEN_W, WM_SCREEN_H, -1);
}

int main(int argc, char **argv) {

	z_win_t screen;
	uint32_t cfg_len = 0, zic_len = 0;

	if (argc < 3) {
		printf("usage: %s wm.cfg dock.zic [prefix]\n", argv[0]);
		return 1;
	}
	if (argc > 3) prefix = argv[3];

	char *cfg_shipped = slurp(argv[1], &cfg_len);
	uint8_t *zic_shipped = slurp(argv[2], &zic_len);
	if (!cfg_shipped || !zic_shipped) { printf("cannot read %s or %s\n", argv[1], argv[2]); return 1; }
	cfg_shipped = realloc(cfg_shipped, cfg_len + 1);
	cfg_shipped[cfg_len] = 0;

	if (!z_render_open(&screen, WM_SCREEN_W, WM_SCREEN_H) || !k_install()) {
		printf("wm dock test: skipped (cannot map fixed addresses)\n");
		return 77;
	}
	zr_region_strict = true;
	my_pid = 1;
	dock_idx = -1;

	// The dock as it was before groups: every app in one row.
	static const char flat[] =
		"row wide\n"
		"\tterm files text read web sheet draw view\n"
		"\ttrack midi play calc clock cal info settings keyboard\n"
		"\task slide irc hex mmod\n"
		"\tcasino chess kidgames basic chip8 space3d\n"
		"\tsnake blocks bricks gpu3d mausrun\n";

	// -- 1. the shipped files: the row, with three groups --
	printf("1. the shipped wm.cfg and dock.zic\n");
	hardware(true, true);
	set_cfg(cfg_shipped);
	set_file(1, zic_shipped, zic_len);
	dock_build();
	printf("  store: %lu of %d bytes\n", (unsigned long)dock_store_used, DOCK_STORE_MAX);
	expect(dock_nmain == 17 && !dock_paged, "17 in the row, no NEXT");
	expect(dock_nviews == 3, "three groups: Sound, Tools, Games (More apps is empty)");
	int sound = find_app("sound"), tools = find_app("tools"), games = find_app("games");
	expect(sound == 14 && tools == 15 && games == 16, "their icons at the end of the row");
	expect(dock_apps[games].view >= 0 && dock_views[dock_apps[games].view].count == 10, "Games: ten");
	int apps = 0, all_icons = 1;
	for (int i = 0; i < dock_app_count; i++) {
		if (dock_apps[i].view < 0) apps++;
		if (!icon_of(i)) all_icons = 0;
	}
	expect(apps == 34, "34 apps: the 33 before groups, and backup");
	expect(all_icons, "every one, and every group, with its icon");
	expect(bits_of(dock_next_icon, dock_next_len) && bits_of(dock_back_icon, dock_back_len),
		"NEXT and BACK");
	expect(strcmp(dock_spoken(0), "Terminal") == 0, "term is spoken as Terminal");
	expect(strcmp(dock_spoken(games), "Games") == 0, "a group by its name");
	expect(dock_frame_len == z_dock_width_for(17), "the frame: the row's width, the widest view");
	dock_idx = create_dock();
	expect(dock_idx >= 0 && (int)windows[dock_idx].w == dock_frame_len, "the dock window");
	draw_all();
	expect(shows_item(0, 0), "slot 0 shows term's icon");
	expect(shows_item(16, games), "slot 16 shows the Games icon");
	dump("1-shipped");

	// -- 2. a group, opened in place --
	printf("2. opening Games\n");
	int casino = find_app("casino");
	dock_launch(games);			// a group's icon: opens it, starts nothing
	expect(dock_view == dock_apps[games].view && runs == 0, "the group is shown, nothing run");
	expect(dock_selected == casino, "the selection on its first app");
	expect(!dock_paged, "ten fit, no NEXT");
	expect((int)windows[dock_idx].w == dock_frame_len, "the frame did not move");
	draw_all();
	expect(slot_shows(0, bits_of(dock_back_icon, dock_back_len)), "BACK in slot 0");
	expect(shows_item(1, casino), "casino in slot 1");
	expect(slot_ink(11) == 0, "and nothing after the tenth");
	dump("2-games");
	dock_handle_key(0x1b, true);		// Esc
	expect(dock_view == -1 && dock_selected == games, "Esc: back to the row, on the Games icon");
	dock_handle_key(CH_CR, true);		// Enter on it
	expect(dock_view >= 0, "Enter opens it again");
	dock_handle_key(Z_KEY_LEFT, true);
	expect(dock_selected == DOCK_SEL_BACK, "Left from the first app: BACK");
	dock_handle_key(Z_KEY_LEFT, true);
	expect(dock_selected == casino + 9, "Left from BACK: around to the last");
	dock_handle_key(Z_KEY_RIGHT, true);
	expect(dock_selected == DOCK_SEL_BACK, "Right from the last: BACK");
	dock_handle_key(CH_CR, true);
	expect(dock_view == -1 && dock_selected == games, "Enter on BACK: the row");
	dock_handle_key(0x1b, true);
	expect(dock_view == -1, "Esc in the row is not the dock's");
	expect(!dock_handle_key(0x1b, true), "and not consumed");

	// -- 3. by the mouse --
	printf("3. clicking\n");
	{
		wm_window_t *w = &windows[dock_idx];
		int cy = (int)w->y + DOCK_PADDING + 10;
		dock_click(slot_x(15) + 10, cy);
		expect(dock_view == dock_apps[tools].view, "a click on Tools opens it");
		dock_click(slot_x(0) + 10, cy);
		expect(dock_view == -1, "a click on BACK closes it");
		dock_click(slot_x(16) + 10, cy);
		dock_click(slot_x(12) + 10, cy);
		expect(dock_view >= 0 && runs == 0, "a click past the group's last app does nothing");
		dock_close_view(true);
	}

	// -- 4. launching from a group: back to the row when it is over --
	printf("4. launching from a group\n");
	dock_launch(games);
	int snake = find_app("snake");
	dock_launch(snake);
	expect(runs == 1 && strcmp(last_run, "snake") == 0, "snake was run");
	expect(dock_view >= 0 && dock_launching[snake], "the group stays while it starts");
	dock_return_check();
	expect(dock_view >= 0, "still, until its window");
	dock_launching[snake] = false;		// its window came (handle_message())
	dock_return_check();
	expect(dock_view == -1 && dock_selected == games, "then back to the row");
	run_pid = 0;
	dock_launch(games);
	dock_launch(find_app("chess"));
	expect(dock_view == -1, "a launch that fails: back to the row at once");
	run_pid = 20;

	// -- 5. a group bigger than the dock --
	printf("5. a group that pages\n");
	set_cfg("row wide term @big text\n"
		"group big Big casino chess kidgames chip8 space3d snake blocks bricks gpu3d mausrun\n"
		"\tsheet slide draw view calc clock cal info settings keyboard\n");
	cfg_gen++;
	dock_reload();
	expect(dock_nmain == 3 && dock_views[0].count == 20, "three in the row, twenty in the group");
	expect(dock_frame_len == WM_SCREEN_W - 2 * DOCK_MARGIN, "the frame: full width, for the group");
	dock_launch(1);
	expect(dock_paged && dock_pages == 2 && dock_psize() == 15, "BACK, 15 apps, NEXT: two pages");
	draw_all();
	expect(slot_shows(0, bits_of(dock_back_icon, dock_back_len)), "BACK at the start");
	expect(slot_shows(DOCK_NEXT_SLOT, bits_of(dock_next_icon, dock_next_len)), "NEXT at the end");
	expect(shows_item(15, dock_views[0].start + 14), "the fifteenth in slot 15");
	dump("5-big-group");
	dock_set_page(1);
	draw_all();
	expect(shows_item(1, dock_views[0].start + 15) && slot_ink(6) == 0, "page 2: five more");
	expect(dock_selected == dock_views[0].start + 15, "the selection follows the page");
	dock_set_page(2);
	expect(dock_page == 0, "NEXT wraps");
	dock_close_view(true);
	expect(dock_page == 0 && dock_view == -1, "BACK: the row, its page");
	draw_all();
	expect(slot_ink(4) == 0, "the row in a frame made for the group: room after it");
	dump("5-row-in-wide-frame");

	// -- 6. hardware the bitstream lacks --
	printf("6. no AUDIO, no GPIO\n");
	hardware(false, false);
	set_cfg(cfg_shipped);
	dock_build();
	expect(find_app("sound") < 0 && dock_nmain == 16, "Sound left with nothing: gone from the row");
	expect(dock_views[dock_apps[find_app("tools")].view].count == 6, "Tools without mmod");
	expect(find_app("track") < 0 && find_app("mmod") < 0, "the apps gone");
	hardware(true, true);

	// -- 7. a card with only some of the apps --
	printf("7. a card without the games\n");
	static const char *const nogames[] = { "casino", "chess", "kidgames", "chip8", "space3d",
		"snake", "blocks", "bricks", "gpu3d", "mausrun" };
	nmissing = 0;
	for (int i = 0; i < 10; i++)		// all but chess and kidgames
		if (strcmp(nogames[i], "chess") && strcmp(nogames[i], "kidgames")) missing[nmissing++] = nogames[i];
	dock_build();
	expect(find_app("games") >= 0 && dock_views[dock_apps[find_app("games")].view].count == 2,
		"Games with the two that are there");
	nmissing = 0;

	// -- 8. a flat wm.cfg of one's own, with an app that has no icon --
	printf("8. a wm.cfg of one's own\n");
	set_cfg("# mine\nrow wide\n\ttext term\n\tmyapp   # no icon for it\n\t@rest\n");
	dock_build();
	expect(strcmp(dock_apps[0].name, "text") == 0 && strcmp(dock_apps[1].name, "term") == 0,
		"its order");
	expect(strcmp(dock_apps[2].name, "myapp") == 0 && !icon_of(2), "myapp, with no icon");
	expect(find_app("ask") == 3, "then @rest, in name order");
	expect(find_app("poker") < 0 && find_app("@next") < 0 && find_app("@games") < 0,
		"@rest leaves out the hidden ones and the dock's own");
	expect(dock_app_count == 3 + 34 - 2 && dock_nviews == 0, "@rest: every other app with an icon");
	set_cfg(flat);
	dock_build();
	expect(dock_app_count == 33 && dock_paged && dock_pages == 3, "the old flat dock (it names its 33): three pages");
	expect(dock_frame_len == WM_SCREEN_W - 2 * DOCK_MARGIN, "full width while paging");

	// -- 9. reload: the dock is rebuilt and resized --
	printf("9. cfg reload with a short row\n");
	set_cfg("row wide text term myapp\n");
	cfg_gen++;
	dock_reload();
	expect(dock_app_count == 3 && !dock_paged, "three apps, no paging");
	expect((int)windows[dock_idx].w == z_dock_width(3), "the dock shrank to them");
	expect(dock_cfg_gen == cfg_gen, "the new generation is remembered");
	draw_all();
	expect(shows_item(0, 0), "text's icon");
	expect(slot_ink(2) > 20, "myapp drawn as its name");
	expect(z_render_get((int)windows[dock_idx].x + z_dock_width(3) + 20,
		(int)windows[dock_idx].y + 20) == 0, "nothing left where the long dock was");
	dump("9-reloaded");

	// -- 10. no dock.zic: names in boxes --
	printf("10. no dock.zic\n");
	set_file(1, NULL, 0);
	set_cfg(cfg_shipped);
	cfg_gen++;
	dock_reload();
	expect(dock_nmain == 17 && dock_zic_count == 0, "still every app and group");
	expect(!icon_of(0) && !dock_next_len && !dock_back_len, "no icons");
	draw_all();
	expect(slot_ink(0) > 20 && slot_ink(16) > 20, "names drawn, a group's too");
	dump("10-no-icons");
	dock_launch(16);
	draw_all();
	expect(slot_ink(0) > 2, "BACK drawn as <");
	dock_close_view(true);

	// -- 11. damaged dock.zic --
	printf("11. a damaged dock.zic\n");
	set_file(1, zic_shipped, zic_len - 40);
	dock_build();
	expect(dock_zic_count > 0 && icon_of(find_app("ask")) && dock_nmain == 17,
		"cut short in its icons: the first are there");
	int last_ok = 1;
	for (int i = 0; i < dock_app_count; i++) if (strcmp(dock_apps[i].name, "work") && !icon_of(i)) last_ok = 0;
	expect(!last_ok, "some at the end drawn as names");
	{
		static uint8_t bad[16384];
		memcpy(bad, zic_shipped, zic_len);
		bad[6] = 52;					// another entry layout
		set_file(1, bad, zic_len);
		dock_build();
		expect(dock_zic_count == 0 && dock_nmain == 17 && !icon_of(0), "a damaged table: no icons");
	}
	set_file(1, zic_shipped, zic_len);

	// -- 12. no wm.cfg at all --
	printf("12. no wm.cfg\n");
	set_cfg(NULL);
	dock_build();
	expect(strcmp(dock_apps[0].name, "term") == 0 && strcmp(dock_apps[1].name, "files") == 0 &&
		strcmp(dock_apps[2].name, "text") == 0, "the core apps first");
	expect(dock_app_count == 34, "then every app with an icon");

	// -- 13. a wm.cfg with no row wide, and one with problems --
	printf("13. no row wide; problems\n");
	set_cfg("row tall term\n");
	dock_build();
	expect(dock_app_count == 34, "no row wide: every app with an icon");
	set_cfg("side left\nfrobnicate\nrow wide term @nope\n");
	dock_build();
	expect(dock_app_count == 1 && dock_cfg_errors == 1, "the good lines apply");

	// -- 14. nothing at all, then something --
	printf("14. a dock that was empty\n");
	set_cfg("row wide\n");
	cfg_gen++;
	dock_reload();
	expect(dock_app_count == 0, "an empty row: no apps");
	expect(!dock_handle_key(Z_KEY_RIGHT, true), "the keys do nothing on it");
	set_cfg(flat);
	cfg_gen++;
	dock_reload();
	expect(dock_app_count == 33 && (int)windows[dock_idx].w == WM_SCREEN_W - 2 * DOCK_MARGIN,
		"back to full");

	// -- 15. running dots --
	printf("15. a dot per running copy\n");
	nptab = 0;
	proc("wm0", Z_PROC_FLAG_ACTIVE);
	proc("text", Z_PROC_FLAG_ACTIVE);
	proc("text", Z_PROC_FLAG_ACTIVE | Z_PROC_FLAG_BLOCKED);	// waiting is running
	proc("text", Z_PROC_FLAG_ACTIVE | Z_PROC_FLAG_DIE);	// on its way out
	proc("term", Z_PROC_FLAG_ACTIVE);
	proc("web0", Z_PROC_FLAG_ACTIVE);			// registered: base + number
	proc("", Z_PROC_FLAG_ACTIVE);				// the kernel's own
	expect(dock_dots, "dots on, as the shipped wm.cfg says");
	expect(dock_count_due, "a count is due after a build");
	dock_count();
	int ti = find_app("text"), wi = find_app("web"), mi = find_app("term"), fi = find_app("files");
	expect(dock_running[ti] == 2, "text: 2 (blocked counts, dying does not)");
	expect(dock_running[wi] == 1 && dock_running[mi] == 1, "web0 is web; term");
	expect(dock_running[fi] == 0, "files: none");
	expect(!dock_count_due, "and nothing more is due");
	draw_all();
	dump("15-dots");
	{
		// the dots: in the row under each icon, 2x2, 2 apart, centred
		wm_window_t *w = &windows[dock_idx];
		int y = (int)w->y + DOCK_PADDING + DOCK_ICON_SIZE + 1;
		int runs[4];
		for (int k = 0; k < 4; k++) {
			int slot = k == 0 ? ti : k == 1 ? wi : k == 2 ? mi : fi;
			int x0 = slot_x(slot), n = 0, prev = 0, ink = 0;
			for (int x = x0; x < x0 + DOCK_ICON_SIZE; x++) {
				int v = z_render_get(x, y);
				if (v && !prev) n++;
				ink += v + z_render_get(x, y + 1);
				prev = v;
			}
			runs[k] = n;
			if (k == 0) expect(ink == 2 * 4, "text's two dots are 2x2");
		}
		expect(runs[0] == 2 && runs[1] == 1 && runs[2] == 1 && runs[3] == 0, "2, 1, 1 and no dots drawn");
		int x0 = slot_x(ti);
		expect(z_render_get(x0 + 13, y) && z_render_get(x0 + 14, y) &&
			!z_render_get(x0 + 15, y) && z_render_get(x0 + 17, y), "centred under the icon");
		expect(!z_render_get(x0 + 13, y - 1) && !z_render_get(x0 + 13, y + 2), "clear above and below");
	}

	printf("16. at most six; a seventh changes nothing drawn\n");
	for (int k = 0; k < 4; k++) proc("text", Z_PROC_FLAG_ACTIVE);
	dock_count();
	expect(dock_running[ti] == 6, "six counted");
	draw_all();
	{
		wm_window_t *w = &windows[dock_idx];
		int y = (int)w->y + DOCK_PADDING + DOCK_ICON_SIZE + 1, x0 = slot_x(ti), n = 0, prev = 0;
		for (int x = x0; x < x0 + DOCK_ICON_SIZE; x++) { int v = z_render_get(x, y); if (v && !prev) n++; prev = v; }
		expect(n == 6, "six drawn");
	}
	{
		// a seventh: no dot changes, so the dock is not repainted
		z_render_clear();
		proc("text", Z_PROC_FLAG_ACTIVE);
		dock_count();
		expect(dock_running[ti] == 7, "seven counted");
		expect(dock_dots_shown(), "dots are up: the recount wakes wm");
		int ink = 0;
		for (int x = 0; x < WM_SCREEN_W; x++) ink += z_render_get(x, (int)windows[dock_idx].y + 10);
		expect(ink == 0, "and nothing repainted for it");
		draw_all();
	}

	printf("17. counting is due when a window opens or closes, and every 2 s\n");
	{
		int lists = proc_lists;
		dock_count_due = false;
		int idx = create_window(42, "App", 100, 80, 50, 50, 0);
		expect(dock_count_due, "a window created: due");
		dock_count();
		dock_count_due = false;
		destroy_window((uint32_t)idx);
		expect(dock_count_due, "a window destroyed: due");
		dock_count_at = ticks + 1000;
		expect((int32_t)(ticks - dock_count_at) < 0, "the backstop is in the future");
		expect(proc_lists == lists + 1, "one process list per count");
	}

	printf("18. dots off\n");
	set_cfg("dots off\nrow wide text term\n");
	cfg_gen++;
	dock_reload();
	dock_count();
	expect(!dock_dots && dock_running[0] == 7, "still counted (speech says it), not drawn");
	expect(!dock_dots_shown(), "no dots up: nothing to wake for");
	draw_all();
	{
		wm_window_t *w = &windows[dock_idx];
		int y = (int)w->y + DOCK_PADDING + DOCK_ICON_SIZE + 1, ink = 0;
		for (int x = (int)w->x + 1; x < (int)(w->x + w->w) - 1; x++) ink += z_render_get(x, y);
		expect(ink == 0, "no dots");
	}

	printf("19. a group's icon adds up its apps' copies\n");
	set_cfg(cfg_shipped);
	cfg_gen++;
	dock_reload();
	nptab = 0;
	proc("snake", Z_PROC_FLAG_ACTIVE);
	proc("chess", Z_PROC_FLAG_ACTIVE);
	proc("chess", Z_PROC_FLAG_ACTIVE);
	proc("text", Z_PROC_FLAG_ACTIVE);
	dock_count();
	expect(dock_running[find_app("games")] == 3, "Games: three");
	expect(dock_running[find_app("snake")] == 1, "snake, in the group: one");
	expect(dock_running[find_app("tools")] == 0, "Tools: none");
	{
		char buf[128] = "";
		dock_speech_app(buf, sizeof(buf), find_app("games"));
		expect(strcmp(buf, "Games, 10 apps, 3 running") == 0, "said: Games, 10 apps, 3 running");
	}

	printf("20. a maximized window hides the dock: it comes back on the row\n");
	{
		dock_launch(find_app("games"));
		int wi = create_window(42, "Max", 100, 80, 50, 50, Z_WIN_FLAG_RESIZABLE);
		windows[wi].maxed = true;
		dock_update();
		expect(dock_hidden, "hidden");
		windows[wi].maxed = false;
		dock_update();
		expect(!dock_hidden && dock_view == -1, "back, on the row");
		destroy_window((uint32_t)wi);
	}

	// -- 21. the other sides --
	printf("21. on the left: row tall, a column\n");
	{
		char cfg[2400];
		snprintf(cfg, sizeof(cfg), "%s\nside left\n", cfg_shipped);
		set_cfg(cfg);
		cfg_gen++;
		dock_reload();
		wm_window_t *w = &windows[dock_idx];
		expect(dock_side == DK_SIDE_LEFT && DOCK_SLOTS_TALL == 12, "the left side; 12 fit down it");
		expect(dock_nmain == 12 && !dock_paged, "row tall: 6 apps and 6 groups, no NEXT");
		expect(dock_frame_len == z_dock_len_for(12, WM_SCREEN_H - 2 * DOCK_MARGIN, 12),
			"the frame: the column's height");
		expect((int)w->x == DOCK_MARGIN && (int)w->y == DOCK_MARGIN &&
			(int)w->w == DOCK_THICK && (int)w->h == dock_frame_len, "moved: top left, 41 wide");
		draw_all();
		dump("21-left");
		int net = find_app("net");
		expect(shows_item(0, 0) && shows_item(11, net), "term at the top, Network at the bottom");
		int x0, y0;
		slot_at(0, &x0, &y0);
		expect(x0 == (int)w->x + DOCK_PADDING + DOCK_DOT_ROOM, "the dots' room on the screen side");
		slot_at(1, &x0, &y0);
		expect(y0 == (int)w->y + DOCK_PADDING + DOCK_STRIDE, "the next one below it");

		// a click on a group opens it in place, down the column
		int work = find_app("work");
		slot_at(work, &x0, &y0);
		dock_click(x0 + 10, y0 + 10);
		expect(dock_view == dock_apps[work].view, "a click on Work opens it");
		expect((int)w->h == dock_frame_len && (int)w->y == DOCK_MARGIN, "the frame did not move");
		draw_all();
		expect(slot_shows(0, bits_of(dock_back_icon, dock_back_len)), "BACK at the top");
		expect(shows_item(1, find_app("sheet")), "sheet below it");
		slot_at(1, &x0, &y0);
		int runs0 = runs;
		dock_click(x0 - 3, y0 + 10);
		expect(dock_view >= 0 && runs == runs0, "a click in the padding beside an icon is nothing");
		expect(dock_handle_key(0x1b, true) && dock_view == -1, "Esc: the column again");

		// the dots: a column on the screen side of the icon
		nptab = 0;
		proc("text", Z_PROC_FLAG_ACTIVE);
		proc("text", Z_PROC_FLAG_ACTIVE);
		dock_count();
		draw_all();
		int ti = find_app("text");
		slot_at(ti, &x0, &y0);
		int dx = x0 - 1 - 2, n = 0, prev = 0;
		for (int y = y0; y < y0 + DOCK_ICON_SIZE; y++) {
			int v = z_render_get(dx, y);
			if (v && !prev) n++;
			prev = v;
		}
		expect(n == 2, "two dots, left of text's icon");
		expect(z_render_get(dx, y0 + 13) && z_render_get(dx + 1, y0 + 14) &&
			!z_render_get(dx, y0 + 15) && z_render_get(dx, y0 + 17), "2x2, centred down it");
		expect(!z_render_get(dx - 1, y0 + 13) && !z_render_get(dx + 2, y0 + 13),
			"clear on both sides");
		expect(dx - 2 == (int)w->x, "and the border beyond that");

		// new windows start beside it, not on it
		int nw = create_window(42, "New", 100, 80, -1, -1, 0);
		expect(nw >= 0 && (int)windows[nw].x >= (int)(w->x + w->w), "a new window right of the dock");
		destroy_window((uint32_t)nw);

		// a caption keeps out of the column, centred in what is left
		caption_set("Hello there", 0);
		expect(cap_vis && cap_rect.x0 >= (int)(w->x + w->w), "a caption clear of the dock");
		int mid = (cap_rect.x0 + cap_rect.x1) / 2, want = ((int)(w->x + w->w) + WM_SCREEN_W) / 2;
		expect(mid >= want - 1 && mid <= want + 1, "centred beside it");
		caption_set("", 0);
	}

	printf("22. on the right, and the dots on its right\n");
	{
		char cfg[2400];
		snprintf(cfg, sizeof(cfg), "%s\nside right\n", cfg_shipped);
		set_cfg(cfg);
		cfg_gen++;
		dock_reload();
		wm_window_t *w = &windows[dock_idx];
		expect((int)w->x == WM_SCREEN_W - DOCK_MARGIN - DOCK_THICK && (int)w->y == DOCK_MARGIN,
			"against the right edge");
		dock_count();
		draw_all();
		dump("22-right");
		int x0, y0, ti = find_app("text");
		slot_at(ti, &x0, &y0);
		expect(x0 == (int)w->x + DOCK_PADDING, "the icons 4 in from the inner edge");
		int dx = x0 + DOCK_ICON_SIZE + 1;
		expect(z_render_get(dx, y0 + 13) && z_render_get(dx + 1, y0 + 14) && z_render_get(dx, y0 + 17),
			"two dots right of text's icon");
		expect(dx + 3 == (int)(w->x + w->w) - 1, "the border beyond them");
		int nw = create_window(42, "New", 100, 80, -1, -1, 0);
		expect(nw >= 0 && (int)windows[nw].x <= 20 + 7 * 24 && (int)windows[nw].y <= 20 + 7 * 20,
			"new windows: the usual cascade");
		destroy_window((uint32_t)nw);
	}

	printf("23. along the top: row wide, dots above\n");
	{
		char cfg[2400];
		snprintf(cfg, sizeof(cfg), "%s\nside top\n", cfg_shipped);
		set_cfg(cfg);
		cfg_gen++;
		dock_reload();
		wm_window_t *w = &windows[dock_idx];
		expect(dock_nmain == 17 && (int)w->y == DOCK_MARGIN && (int)w->x == DOCK_MARGIN &&
			(int)w->h == DOCK_THICK, "17 across the top");
		dock_count();
		draw_all();
		dump("23-top");
		int x0, y0, ti = find_app("text");
		slot_at(ti, &x0, &y0);
		expect(y0 == (int)w->y + DOCK_PADDING + DOCK_DOT_ROOM, "the dots' room above");
		int dy = y0 - 1 - 2;
		expect(z_render_get(x0 + 13, dy) && z_render_get(x0 + 17, dy + 1) && !z_render_get(x0 + 15, dy),
			"two dots above text's icon");
		expect(dy - 2 == (int)w->y, "the border beyond them");
		int nw = create_window(42, "New", 100, 80, -1, -1, 0);
		expect(nw >= 0 && (int)windows[nw].y >= (int)(w->y + w->h), "a new window below the dock");
		destroy_window((uint32_t)nw);
		caption_set("Hello there", Z_CAPTION_TOP);
		expect(cap_vis && cap_rect.y0 >= (int)(w->y + w->h), "a caption at the top: below the dock");
		caption_set("Hello there", 0);
		expect(cap_vis && cap_rect.y1 >= WM_SCREEN_H - 40, "at the bottom: at the bottom");
		caption_set("", 0);
	}

	printf("24. a side with no row tall pages row wide down it\n");
	{
		set_cfg("side left\nrow wide\n\tterm files text read web irc ask sheet slide draw view\n"
			"\tcalc clock cal chess snake blocks bricks\n");
		cfg_gen++;
		dock_reload();
		wm_window_t *w = &windows[dock_idx];
		expect(dock_nmain == 18 && dock_paged && dock_pages == 2, "18 apps: two pages of 11");
		expect((int)w->h == WM_SCREEN_H - 2 * DOCK_MARGIN, "the full height");
		int x0, y0;
		slot_at(11, &x0, &y0);
		expect(dock_slot_is_next(11) && y0 + DOCK_ICON_SIZE + DOCK_PADDING == (int)(w->y + w->h),
			"NEXT at the bottom end");
		dock_click(x0 + 10, y0 + 10);
		expect(dock_page == 1, "and it turns the page");
	}

	printf("25. hidden while the side changes: back in the new place\n");
	{
		int wi = create_window(42, "Max", 100, 80, 50, 50, Z_WIN_FLAG_RESIZABLE);
		windows[wi].maxed = true;
		dock_update();
		expect(dock_hidden, "hidden");
		set_cfg(cfg_shipped);			// side bottom
		cfg_gen++;
		dock_reload();
		expect((int)windows[dock_idx].y >= WM_SCREEN_H, "still off the screen");
		windows[wi].maxed = false;
		dock_update();
		wm_window_t *w = &windows[dock_idx];
		expect(!dock_hidden && (int)w->y == WM_SCREEN_H - DOCK_MARGIN - DOCK_THICK &&
			(int)w->x == DOCK_MARGIN && (int)w->w == dock_frame_len, "back, along the bottom");
		destroy_window((uint32_t)wi);
	}

	// -- 26. the pointer's large pair (docs/cursor.md) --
	printf("26. the pointer's shapes\n");
	{
		// a bitstream with fixed shapes: nothing written
		reg_socctl_magic = Z_SOCCTL_MAGIC;
		reg_socctl_cursor = 0;
		reg_socctl_curdata = 0x5555;
		cfg_gen++;
		dock_reload();
		expect(!wm_cursor_hw && reg_socctl_cursor == 0 && reg_socctl_curdata == 0x5555,
			"fixed shapes: nothing written");

		// with them, and no file: the built-in 9x9 pair, the small pair shown
		reg_socctl_cursor = (uint32_t)Z_CURSOR_SIG << 16;
		cfg_gen++;
		dock_reload();
		expect(wm_cursor_hw && reg_socctl_cursor == 0x10, "slots 0 and 1 shown");
		expect(reg_socctl_curdata == 0, "the built-in Z's last row written last (empty)");

		// a file of the user's: its rows go to slots 2 and 3
		static char zsp[1024];
		int k = sprintf(zsp, "zsp 1\nsprite normal 16 16\n");
		for (int r = 0; r < 16; r++) k += sprintf(zsp + k, "1...............\n");
		k += sprintf(zsp + k, "sprite busy 16 16\n");
		for (int r = 0; r < 16; r++) k += sprintf(zsp + k, r == 15 ? "1.1.1.1.1.1.1.1.\n" : "................\n");
		set_file(2, zsp, (uint32_t)k);
		reg_socctl_cursor = (uint32_t)Z_CURSOR_SIG << 16;
		cfg_gen++;
		dock_reload();
		expect(reg_socctl_curdata == 0x5555, "the file's busy, last row, written last");
		expect(reg_socctl_cursor == 0x10, "and the small pair still shown");

		// Super+M's half: the other pair
		wm_cursor_large = true;
		wm_cursor_slots();
		expect(reg_socctl_cursor == 0x32, "the large pair: slots 2 and 3");

		// a broken file: reported, and the built-in pair instead
		set_file(2, "sprite normal 16 16\n....\n", 25);
		reg_socctl_cursor = (uint32_t)Z_CURSOR_SIG << 16;
		reg_socctl_curdata = 0x1234;
		cfg_gen++;
		dock_reload();
		expect(reg_socctl_curdata == 0, "a broken file: the built-in pair");
		set_file(2, NULL, 0);
	}

	printf("  %d file calls in all\n", fs_calls);
	printf("wm dock files test: %d checks, %d failed\n", checks, failures);
	return failures ? 1 : 0;

}
