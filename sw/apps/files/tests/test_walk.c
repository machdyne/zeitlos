/*
 * Host test for files' find, folder copy/move/delete: the REAL files.c
 * (main renamed), zwalk.c, zflist.c, zwin.c and zwidget.c, over a real
 * directory tree under /tmp through a host version of the zfsapp
 * calls. Dialogs are scripted. `make test` in sw/apps/files.
 *
 * Needs -no-pie and vm.mmap_min_addr=0 (sw/common/tests/ztramp.h);
 * exits 77 (skipped) without them. TEST_PBM=dir also writes renders.
 *
 * No arena here (the trampoline answers ARENA with "refused"), so
 * this runs on files' own small block -- the path a board without
 * arenas takes, and the tighter one.
 */

#include "../../../common/tests/zrender.h"
#include "../../../common/tests/ztramp.h"

#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#include <errno.h>
#include <strings.h>

#define main files_main_unused
#include "../files.c"
#undef main

// -- host fs: real files under root --

static char root[200];

static const char *hp(const char *p) {
	static char b[4][1024];
	static int k;
	k = (k + 1) & 3;
	snprintf(b[k], sizeof(b[k]), "%s%s", root, p);
	return b[k];
}

#define MAXH 8
static FILE *hs[MAXH];

static int hopen(const char *p, const char *mode) {
	FILE *f = fopen(hp(p), mode);
	if (!f) return -1;
	for (int i = 0; i < MAXH; i++) if (!hs[i]) { hs[i] = f; return i; }
	fclose(f);
	return -1;
}
int fs_open_read(const char *p) { return hopen(p, "rb"); }
int fs_open_write(const char *p) { return hopen(p, "wb"); }
int fs_read_chunk(int h, void *b, int n) { return (int)fread(b, 1, (size_t)n, hs[h]); }
int fs_write_chunk(int h, const void *b, int n) { return (int)fwrite(b, 1, (size_t)n, hs[h]); }
int fs_close_handle(int h) { if (h >= 0 && hs[h]) { fclose(hs[h]); hs[h] = NULL; } return 1; }
int fs_unlink(char *p) { return remove(hp(p)) == 0; }
int fs_mkdir(const char *p) { return mkdir(hp(p), 0777) == 0; }
int fs_size(char *p) { struct stat st; return stat(hp(p), &st) ? 0 : (int)st.st_size; }
int fs_write_file(char *p, char *b, int n) {
	FILE *f = fopen(hp(p), "wb");
	if (!f) return -1;
	fwrite(b, 1, (size_t)n, f);
	fclose(f);
	return n;
}

int fs_stat(const char *p, z_fs_info_t *fi) {
	struct stat st;
	if (stat(hp(p), &st)) return 0;
	memset(fi, 0, sizeof(*fi));
	fi->type = S_ISDIR(st.st_mode) ? Z_FS_TYPE_DIR : Z_FS_TYPE_FILE;
	fi->size = S_ISDIR(st.st_mode) ? 0 : (uint32_t)st.st_size;
	return 1;
}

// The list's page-at-a-time source (files.c, list_src), over a real
// directory: used once the list has an arena (the big folder, below).
static DIR *zd;
static int zd_open;
int32_t zfs_opendir_at(zfs_dir_t *d, const char *path, void *buf, uint32_t cap) {
	(void)buf; (void)cap;
	if (zd) closedir(zd);
	zd = opendir(hp(path));
	if (!zd) return -1;
	zd_open++;
	memset(d, 0, sizeof(*d));
	return 0;
}
const z_fs_dirent_t *zfs_readdir(zfs_dir_t *d) {
	static union { z_fs_dirent_t e; char b[sizeof(z_fs_dirent_t) + 256]; } u;
	struct dirent *de;
	(void)d;
	if (!zd) return NULL;
	if (!(de = readdir(zd))) return NULL;
	memset(&u.e, 0, sizeof(u.e));
	u.e.namelen = (uint16_t)strlen(de->d_name);
	memcpy(u.e.name, de->d_name, (size_t)u.e.namelen + 1);
	u.e.info.type = de->d_type == DT_DIR ? Z_FS_TYPE_DIR : Z_FS_TYPE_FILE;
	return &u.e;
}
int32_t zfs_closedir_at(zfs_dir_t *d) {
	(void)d;
	if (zd) { closedir(zd); zd = NULL; zd_open--; }
	return 0;
}

// An arena for the big-folder part: every resize MOVES it and leaves
// the old place unreadable, so a pointer kept across a resize faults
// at once instead of working by luck (as sw/apps/text/tests/test_edit.c).
#define AR_A	((void *)0x10000000)
#define AR_B	((void *)0x18000000)
#define AR_LEN	(16u << 20)
static uint8_t *ar_base;
static uint32_t ar_size;
static long ar_moves;
static uint32_t *ar_tramp(uint32_t id, uint32_t *args, uint32_t b) {
	if (id == Z_SYS_ARENA) {
		z_arena_args_t *a = (z_arena_args_t *)args;
		if (a->size == Z_ARENA_QUERY) {
			a->base = (uint32_t)(uintptr_t)ar_base; a->size = ar_size; a->result = Z_ARENA_OK;
		} else if (a->size == 0) {
			if (ar_base) mprotect(ar_base, AR_LEN, PROT_NONE);
			ar_base = NULL; ar_size = 0; a->base = 0; a->result = Z_ARENA_OK;
		} else if (a->size > AR_LEN) {
			a->base = (uint32_t)(uintptr_t)ar_base; a->size = ar_size; a->result = Z_ARENA_E_NOMEM;
		} else {
			uint8_t *to = (ar_base == (uint8_t *)AR_A) ? AR_B : AR_A;
			mprotect(to, AR_LEN, PROT_READ | PROT_WRITE);
			memset(to, 0x5A, a->size);
			if (ar_base) {
				memcpy(to, ar_base, ar_size < a->size ? ar_size : a->size);
				mprotect(ar_base, AR_LEN, PROT_NONE);
				ar_moves++;
			}
			ar_base = to; ar_size = a->size;
			a->base = (uint32_t)(uintptr_t)ar_base; a->result = Z_ARENA_OK;
		}
		return (uint32_t *)&z_tramp_ok;
	}
	return z_tramp_syscall(id, args, b);
}
static bool ar_install(void) {
	if (mmap(AR_A, AR_LEN, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0) == MAP_FAILED ||
		mmap(AR_B, AR_LEN, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0) == MAP_FAILED)
		return false;
	*(volatile uint32_t *)0x0000000c = (uint32_t)(uintptr_t)ar_tramp;
	return true;
}

static bool list_sorted(void) {
	for (int i = 1; i < flist.count; i++)
		if (flist.isdir[i - 1] == flist.isdir[i] &&
			strcasecmp(flist.pool + flist.name_off[i - 1], flist.pool + flist.name_off[i]) > 0)
			return false;
	return true;
}

// /ram is "another volume": a rename across it is XDEV, as on the board.
static bool on_ram(const char *p) { return !strncmp(p, "/ram", 4) && (!p[4] || p[4] == '/'); }

int fs_rename(const char *a, const char *b, int *err) {
	struct stat st;
	if (on_ram(a) != on_ram(b)) { if (err) *err = Z_FS_ERR_XDEV; return 0; }
	if (stat(hp(b), &st) == 0) { if (err) *err = Z_FS_ERR_EXIST; return 0; }
	if (rename(hp(a), hp(b))) { if (err) *err = Z_FS_ERR_NOENT; return 0; }
	if (err) *err = 0;
	return 1;
}

static int cmpstr(const void *a, const void *b) { return strcmp(*(char **)a, *(char **)b); }

int fs_list_ex(const char *path, char *buf, uint32_t cap, z_fs_info_t *info,
	uint32_t max, uint32_t *count, uint32_t *trunc) {
	DIR *d = opendir(hp(path));
	struct dirent *de;
	char *names[1024];
	int nn = 0;
	uint32_t used = 0, n = 0;
	*count = 0; if (trunc) *trunc = 0;
	if (!d) return 0;
	while ((de = readdir(d)) && nn < 1024)
		if (strcmp(de->d_name, ".") && strcmp(de->d_name, ".."))
			names[nn++] = strdup(de->d_name);
	closedir(d);
	qsort(names, (size_t)nn, sizeof(char *), cmpstr);
	for (int i = 0; i < nn; i++) {
		char e[600];
		z_fs_info_t fi;
		if (n >= max) { if (trunc) *trunc = 1; break; }
		snprintf(e, sizeof(e), "%s%s%s", path,
			path[strlen(path) - 1] == '/' ? "" : "/", names[i]);
		size_t l = strlen(e) + 1;
		if (used + l > cap) { if (trunc) *trunc = 1; break; }
		memcpy(buf + used, e, l);
		used += (uint32_t)l;
		fs_stat(e, &fi);
		if (info) info[n] = fi;
		n++;
	}
	for (int i = 0; i < nn; i++) free(names[i]);
	*count = n;
	return n > 0;
}

int fs_list_into(const char *path, char *buf, uint32_t cap, uint8_t *types,
	uint32_t max, uint32_t *count, uint32_t *trunc) {
	static z_fs_info_t fi[1024];
	int r = fs_list_ex(path, buf, cap, fi, max > 1024 ? 1024 : max, count, trunc);
	if (types) for (uint32_t i = 0; i < *count; i++) types[i] = fi[i].type;
	return r;
}

// -- dialogs, scripted --

static const char *prompt_answer;
static int confirm_answer = Z_DIALOG_YES;
static char last_confirm[256];
static int confirms;

bool z_dialog_prompt(const z_dialog_ctx_t *c, const char *t, const char *m,
	const char *i, char *o, int n) {
	(void)c; (void)t; (void)m; (void)i;
	if (!prompt_answer) return false;
	snprintf(o, (size_t)n, "%s", prompt_answer);
	return true;
}
int z_dialog_confirm(const z_dialog_ctx_t *c, const char *t, const char *m, int b) {
	(void)c; (void)t; (void)b;
	snprintf(last_confirm, sizeof(last_confirm), "%s", m);
	confirms++;
	return confirm_answer;
}
void z_dialog_help(const z_dialog_ctx_t *c, const char *t, const char *x, const char *d) {
	(void)c; (void)t; (void)x; (void)d;
}

// convert is another program; not exercised here
bool z_conv_start(z_conv_t *c, const char *in, const char *out, const char *to,
	uint32_t f) { (void)c; (void)in; (void)out; (void)to; (void)f; return false; }
bool z_conv_msg(z_conv_t *c, const z_msg_t *m) { (void)c; (void)m; return false; }
void z_conv_poll(z_conv_t *c) { (void)c; }
const char *z_conv_strerror(int32_t e) { (void)e; return ""; }

// The row icons go through the hardware icon blitter, which
// zrender.h does not emulate (test_dialog.c does the same).
void z_fb_draw_icon(int x, int y, int icon, int fg, int bg, const z_clip_t *clip) {
	(void)x; (void)y; (void)icon; (void)fg; (void)bg; (void)clip;
}

// -- checks --

static int checks, failures;
static void expect(bool ok, const char *what) {
	checks++;
	if (!ok) { failures++; printf("  FAIL: %s\n", what); }
}

static void put(const char *p, const char *text) {
	FILE *f = fopen(hp(p), "wb");
	fputs(text, f);
	fclose(f);
}
static void mk(const char *p) { mkdir(hp(p), 0777); }
static bool exists(const char *p) { struct stat st; return stat(hp(p), &st) == 0; }
static void sh(const char *fmt, const char *a) {
	char cmd[1200];
	snprintf(cmd, sizeof(cmd), fmt, a, a);
	if (system(cmd)) {}
}
static bool same(const char *a, const char *b) {
	char cmd[1200];
	snprintf(cmd, sizeof(cmd), "diff -r %s%s %s%s >/dev/null", root, a, root, b);
	return system(cmd) == 0;
}

static void tree(void) {
	char cmd[600];
	snprintf(cmd, sizeof(cmd), "rm -rf %s && mkdir -p %s", root, root);
	if (system(cmd)) exit(2);
	mk("/home");
	mk("/ram");
	mk("/home/a");
	put("/home/a/readme.md", "Hello\n");
	put("/home/a/notes.txt", "buy milk\nTODO call\n");
	mk("/home/a/sub");
	put("/home/a/sub/deep.md", "nothing\n");
	mk("/home/a/sub/empty");
	FILE *f = fopen(hp("/home/a/sub/big.bin"), "wb");
	for (int i = 0; i < 20000; i++) fputc(i & 0xff, f);
	fclose(f);
	put("/home/top.md", "x\n");
}

static void pump(void) {
	for (int i = 0; i < 100000 && walking; i++) walk_pump();
}

static void select_name(const char *name) {
	z_flist_refresh_select(&flist, name);
}

static void pbm(const char *name) {
	const char *d = getenv("TEST_PBM");
	char p[600];
	if (!d) return;
	snprintf(p, sizeof(p), "%s/%s", d, name);
	z_render_write(p, &win, 2);
}


// -- drawing checks --
//
// There is no back buffer: anything blanked and drawn again shows as a
// flash. Two ways to catch it here. A MARK is an ink pixel planted in
// the list's empty area below its last row: anything that blanks the
// list takes it away, and nothing else touches it. And SAME_AS_REPAINT:
// whatever was drawn in place must leave exactly the pixels a whole
// repaint() would -- a row painted short, or a selection left behind,
// differs.

static uint32_t snap[Z_RENDER_VRAM_LEN / 4];

static void snapshot(void) {
	memcpy(snap, (const void *)Z_RENDER_VRAM, sizeof(snap));
}

static bool marked;
static void mark_xy(int *x, int *y);

static bool same_as_repaint(void) {
	static uint32_t now[Z_RENDER_VRAM_LEN / 4];
	// the mark is not part of the picture: a repaint rightly clears it
	if (marked) {
		int x, y;
		mark_xy(&x, &y);
		z_fb_set_pixel(x, y, 0, NULL);
		marked = false;
	}
	memcpy(now, (const void *)Z_RENDER_VRAM, sizeof(now));
	repaint();
	bool ok = !memcmp(now, (const void *)Z_RENDER_VRAM, sizeof(now));
	if (!ok)
		for (unsigned i = 0; i < sizeof(now) / 4; i++)
			if (now[i] != ((const uint32_t *)Z_RENDER_VRAM)[i]) {
				printf("  first difference at x=%u y=%u\n",
					(i * 32) % Z_SCREEN_W, (i * 32) / Z_SCREEN_W);
				break;
			}
	return ok;
}

// The pixel near the bottom-left of the list's row area, absolute.
static void mark_xy(int *x, int *y) {
	z_clip_t c;
	z_win_content_rect(&win, &c);
	*x = c.x0 + MARGIN + 4;
	*y = c.y0 + list_y + list_h - 4;
}
static void mark_set(void) {
	int x, y;
	mark_xy(&x, &y);
	z_fb_set_pixel(x, y, 1, NULL);
	marked = true;
}
static bool mark_kept(void) {
	int x, y;
	mark_xy(&x, &y);
	return z_render_get(x, y) == 1;
}

// The path/status line, as drawn.
static bool line_says(const char *what) {
	return line_valid && strstr(line_shown, what) != NULL;
}

static bool all_closed(void) {
	for (int i = 0; i < MAXH; i++) if (hs[i]) return false;
	return true;
}

int main(void) {

	snprintf(root, sizeof(root), "/tmp/files_walk.%d", (int)getpid());

	if (!z_render_open(&win, WIN_W, WIN_H) || !z_tramp_install()) {
		printf("test_walk: skipped (needs -no-pie and vm.mmap_min_addr=0)\n");
		return 77;
	}

	tree();

	widgets_init();
	dlg_ctx.parent = &win;
	z_flist_init(&flist, &win);
	flist.info = finfo;
	z_listbox_init(&rlist, &win);
	layout();
	z_flist_chdir(&flist, "/home");
	repaint();

	printf("find by name\n");
	prompt_answer = "*.md";
	handle_key(0x06, Z_KBD_MOD_CTRL);
	expect(walking && results, "Ctrl+F starts a find and shows results");
	expect(!widgets[BTN_COPY].enabled && !strcmp(widgets[BTN_FIND].label, "Stop"),
		"buttons: only Stop while it runs");
	pump();
	expect(!walking && results, "done, results stay up");
	expect(res_n == 3, "three .md files");
	expect(res_n == 3 && !strcmp(res_label(NULL, 0), "a/readme.md") &&
		!strcmp(res_label(NULL, 1), "a/sub/deep.md") &&
		!strcmp(res_label(NULL, 2), "top.md"), "relative to where it started, in order");
	expect(walk_mem == walk_static, "no arena here: files' own block");
	repaint();
	pbm("find.pbm");

	printf("Backspace on a result: its folder\n");
	z_listbox_select(&rlist, 1);
	handle_key(0x08, 0);
	expect(!results && !strcmp(flist.path, "/home/a/sub"), "in its folder");
	expect(z_flist_selected(&flist) && !strcmp(z_flist_selected(&flist), "deep.md"), "with it selected");
	expect(walk_mem == NULL, "memory given back");

	printf("find text, and Esc stops at once\n");
	z_flist_chdir(&flist, "/home");
	prompt_answer = "\"todo\"";
	handle_key(0x06, Z_KBD_MOD_CTRL);
	expect(!strcmp(walk.text, "todo") && !walk.name[0], "the quoted part is text");
	zw_step(&walk);
	handle_key(0x1b, 0);
	expect(!walking && results, "first Esc stops, results stay");
	expect(all_closed(), "nothing left open");
	handle_key(0x1b, 0);
	expect(!results && !walk_mem, "second Esc: back to the folder");
	prompt_answer = "*.txt \"TODO\"";
	handle_key(0x06, Z_KBD_MOD_CTRL);
	expect(!strcmp(walk.name, "*.txt") && !strcmp(walk.text, "TODO"), "name and text");
	pump();
	expect(res_n == 1 && !strcmp(res_label(NULL, 0), "a/notes.txt"), "found it");
	handle_key(0x1b, 0);

	printf("copy a folder\n");
	select_name("a");
	prompt_answer = "/home/b";
	confirms = 0;
	button(BTN_COPY);
	expect(walking && walk_kind == WK_COPY, "a walk");
	pbm("copying.pbm");
	pump();
	expect(!walking && same("/home/a", "/home/b") && exists("/home/b/sub/empty"), "the whole tree");
	expect(confirms == 0, "no dialog when it worked");
	expect(z_flist_selected(&flist) && !strcmp(z_flist_selected(&flist), "b"), "the copy selected");

	printf("copy into an existing folder means into it\n");
	select_name("top.md");
	prompt_answer = "/home/b";
	button(BTN_COPY);
	pump();
	expect(exists("/home/b/top.md"), "/home/b/top.md");

	printf("copy onto an existing file asks, and replaces\n");
	put("/home/b/top.md", "old\n");
	confirm_answer = Z_DIALOG_YES;
	button(BTN_COPY);
	pump();
	expect(same("/home/top.md", "/home/b/top.md"), "replaced");

	printf("stop a copy part-way\n");
	sh("rm -rf %s/home/c", root);
	select_name("a");
	prompt_answer = "/home/c";
	button(BTN_COPY);
	while (!walk.infile || strcmp(walk.src + strlen(walk.src) - 7, "big.bin")) zw_step(&walk);
	zw_step(&walk);				// a chunk of big.bin written
	confirms = 0;
	handle_key(0x1b, 0);
	expect(!walking && confirms == 1 && strstr(last_confirm, "left"), "says what is left");
	expect(!exists("/home/c/sub/big.bin"), "no half file");
	expect(exists("/home/c/readme.md"), "what was copied stays");
	expect(all_closed(), "nothing open");

	printf("move a folder to another volume: copy, then delete\n");
	select_name("c");
	prompt_answer = "/ram";
	button(BTN_MOVE);
	expect(walking && walk_kind == WK_MOVE, "a walk, not a rename");
	pump();
	expect(exists("/ram/c/readme.md") && !exists("/home/c"), "moved");

	printf("move within a volume is a rename\n");
	select_name("b");
	prompt_answer = "/home/b2";
	button(BTN_MOVE);
	expect(!walking && exists("/home/b2/sub/deep.md") && !exists("/home/b"), "renamed at once");

	printf("delete a folder with everything in it\n");
	select_name("b2");
	confirm_answer = Z_DIALOG_YES;
	confirms = 0;
	button(BTN_DELETE);
	expect(confirms == 1 && strstr(last_confirm, "everything"), "asks, saying so");
	pump();
	expect(!exists("/home/b2"), "gone");

	printf("delete: No does nothing\n");
	select_name("a");
	confirm_answer = Z_DIALOG_NO;
	button(BTN_DELETE);
	expect(!walking && exists("/home/a"), "still there");

	printf("Ctrl+F does nothing during a copy; Delete key neither\n");
	confirm_answer = Z_DIALOG_YES;
	prompt_answer = "/home/d";
	button(BTN_COPY);
	zw_step(&walk);
	handle_key(0x06, Z_KBD_MOD_CTRL);
	expect(walk_kind == WK_COPY, "still copying");
	handle_key(Z_KEY_DELETE, 0);
	expect(exists("/home/a"), "nothing deleted");
	pump();
	expect(same("/home/a", "/home/d"), "and it finished");


	printf("drawing: arrow keys repaint two rows, nothing blanked\n");
	z_flist_chdir(&flist, "/home");
	repaint();
	mark_set();
	handle_key(Z_KEY_DOWN, 0);
	handle_key(Z_KEY_DOWN, 0);
	handle_key(Z_KEY_UP, 0);
	expect(mark_kept(), "the list was not blanked");
	expect(same_as_repaint(), "and looks as a whole repaint would");

	printf("drawing: type-to-find in the folder\n");
	mark_set();
	handle_key('t', 0);
	expect(z_flist_selected(&flist) && !strcmp(z_flist_selected(&flist), "top.md"), "t: top.md");
	flist.find_len = 0;		// a second later (the trampoline's clock stands still)
	handle_key('a', 0);
	expect(z_flist_selected(&flist) && !strcmp(z_flist_selected(&flist), "a"), "a: the folder a");
	expect(mark_kept() && same_as_repaint(), "drawn in place");

	printf("drawing: opening a folder paints it once, in place\n");
	mark_set();
	handle_key(0x0d, 0);
	expect(!strcmp(flist.path, "/home/a"), "in /home/a");
	expect(mark_kept(), "not blanked first");
	expect(same_as_repaint(), "and complete");
	expect(line_says("/home/a"), "the path line says where");
	mark_set();
	handle_key(0x7f, 0);
	expect(!strcmp(flist.path, "/home") && mark_kept() && same_as_repaint(), "and back up");

	printf("drawing: results stream in without blanking the list\n");
	prompt_answer = "*.md";
	handle_key(0x06, Z_KBD_MOD_CTRL);
	mark_set();
	pump();
	expect(res_n >= 3 && rlist.count == res_n, "all of them in the list");
	expect(mark_kept(), "the list was not blanked per hit");
	{
		char want[24];
		snprintf(want, sizeof(want), "%d found", res_n);
		expect(line_says(want), "the line says how many");
	}
	expect(same_as_repaint(), "and looks as a whole repaint would");
	expect(!strcmp(widgets[BTN_FIND].label, "Find"), "Stop is Find again");

	printf("drawing: Ctrl+F over the results empties them in place\n");
	mark_set();
	prompt_answer = "readme";
	handle_key(0x06, Z_KBD_MOD_CTRL);
	expect(walking && results && rlist.count == 0, "a new find, an empty list");
	expect(mark_kept(), "not blanked: the rows went one by one");
	pump();
	expect(res_n >= 1 && same_as_repaint(), "found, and drawn right");
	handle_key(Z_KEY_DOWN, 0);
	expect(same_as_repaint(), "arrow keys in the results");
	handle_key(0x1b, 0);
	expect(!results && same_as_repaint(), "Esc: the folder back, whole");

	printf("results full: the find stops and says so\n");
	// in folders of twenty: one listing of two hundred would not fit
	// the walk's own small block, and be walked only in part
	mk("/home/many");
	for (int i = 0; i < 200; i++) {
		char p[64];
		snprintf(p, sizeof(p), "/home/many/d%d", i / 20);
		if (i % 20 == 0) mk(p);
		snprintf(p, sizeof(p), "/home/many/d%d/f%03d.md", i / 20, i);
		put(p, "x\n");
	}
	prompt_answer = "*.md";
	handle_key(0x06, Z_KBD_MOD_CTRL);
	pump();
	expect(res_full && !walking, "stopped when full");
	expect(res_n > 0 && res_n < 200, "with what fitted");
	{
		char want[40];
		snprintf(want, sizeof(want), "(first %d matches)", res_n);
		expect(line_says(want), "the line says \"(first N matches)\"");
	}
	expect(walk.files < 200, "and did not look at the rest");
	expect(rlist.count == res_n && same_as_repaint(), "all shown, drawn right");
	pbm("full.pbm");
	handle_key(0x1b, 0);
	sh("rm -rf %s/home/many", root);
	z_flist_refresh(&flist);
	repaint();

	printf("copying one file shows a percentage\n");
	select_name("a");
	handle_key(0x0d, 0);
	select_name("sub");
	handle_key(0x0d, 0);
	select_name("big.bin");
	prompt_answer = "/home/big2.bin";
	button(BTN_COPY);
	expect(walking && copy_total == 20000, "the size is known");
	expect(line_says("0% of 19 KB"), "0% at the start");
	for (int i = 0; i < 6; i++) zw_step(&walk);
	status_draw();
	expect(line_says("% of 19 KB") && !line_says(" 0% "), "and moves on");
	pbm("copy-progress.pbm");
	pump();
	expect(exists("/home/big2.bin"), "copied");
	expect(same_as_repaint(), "the bar gone, the path line back, in place");
	remove(hp("/home/big2.bin"));
	z_flist_chdir(&flist, "/home");
	repaint();

	// -- a big folder, read a page at a time into the arena --
	printf("a folder of 2500, with an arena that moves\n");
	if (!ar_install()) {
		printf("  (skipped: cannot map the arena's two places)\n");
	} else {
		mk("/home/big");
		for (int i = 0; i < 2500; i++) {
			char n[64];
			snprintf(n, sizeof(n), "/home/big/n%04d.txt", (i * 7919) % 2500);
			put(n, "x");
		}
		mk("/home/big/zz-folder");
		list_setup();
		expect(list_bytes > 0 && flist.src == &list_src, "the list's store in the arena, read through fs");
		z_flist_chdir(&flist, "/home/big");
		expect(flist.loading && flist.count <= 64, "the first page at once");
		repaint();
		int steps = 0;
		while (flist.loading && steps < 1000) {
			z_flist_load_step(&flist, 64);
			z_flist_draw(&flist, false);
			steps++;
		}
		expect(flist.count == 2501 && !z_flist_truncated(&flist), "all 2500 and the folder");
		expect(list_cap == 4096 && ar_moves > 0, "the store grew, and the arena moved");
		expect(list_sorted() && flist.isdir[0], "in order, the folder first");
		expect(zd_open == 0, "the directory closed");

		// a find above it: the arena grows, moves, and the list is
		// pointed at it again
		long m0 = ar_moves;
		prompt_answer = "n00*.txt";
		handle_key(0x06, Z_KBD_MOD_CTRL);
		expect(walking && walk_arena, "the find's memory above the list's");
		pump();
		// as many as the walk's listing of the folder holds -- a walk
		// takes a folder in one listing, as far as it fits (zwalk.h);
		// this host's listing stops at 1024 entries
		expect(!walking && res_n > 20 && res_n <= 100, "found them, as far as the walk's listing goes");
		handle_key(0x1b, 0);
		expect(!results && !walk_mem, "back to the folder, the find's memory given back");
		expect(ar_moves > m0, "the arena moved under the list");
		expect(flist.count == 2501 && list_sorted() &&
			!strcmp(flist.pool + flist.name_off[1], "n0000.txt"), "and the list is whole");
		repaint();
		pbm("big-folder.pbm");
		expect(same_as_repaint(), "drawn in place");
	}

	{
		char cmd[600];
		snprintf(cmd, sizeof(cmd), "rm -rf %s", root);
		if (system(cmd)) {}
	}

	printf("test_walk: %d checks, %d failed\n", checks, failures);
	return failures ? 1 : 0;

}
