/*
 * Host test for sw/common/zflist.c with long file names -- the real
 * widget, listing through a scripted kernel's Z_SYS_FS_LIST.
 *
 *   cc -std=gnu99 -Wall -no-pie -I sw/common -o /tmp/test_flist \
 *      sw/common/tests/test_flist.c sw/common/zflist.c sw/common/zwin.c \
 *      sw/common/zwidget.c sw/common/zfsapp.c sw/common/zfont_data.c \
 *      sw/common/zobj.c sw/common/zeitlos.c sw/common/zspeak.c
 *   /tmp/test_flist /tmp/flist    # also writes /tmp/flist.pbm and
 *                                 # /tmp/flist-details.pbm
 *
 * Needs -no-pie and vm.mmap_min_addr=0 (ztramp.h); exits 77 without.
 * See docs/sdcard.md, "Long file names".
 */

#include "zrender.h"
#include "zflist.h"
#include "zfs.h"
#include "zkbd.h"

int z_fb_scroll_debug, z_fb_scroll_dbg_armed, z_fb_scroll_align;
void z_fb_draw_icon(int x, int y, int icon_id, int fg, int bg, const z_clip_t *clip) {
	(void)x; (void)y; (void)icon_id; (void)fg; (void)bg; (void)clip;
}

// -- the directory the scripted kernel lists --

static const char *entries[300];
static uint8_t types[300];
static uint32_t sizes[300];
static int nentries;

static z_obj_t k_ok, k_fail;

static uint32_t *k_syscall(uint32_t id, uint32_t *args, uint32_t b) {
	(void)b;
	if (id == Z_SYS_UPTIME) {
		((z_obj_t *)args)->type = Z_UINT32;
		((z_obj_t *)args)->val.uint32 = 1000;
		return (uint32_t *)&k_ok;
	}
	if (id != Z_SYS_FS_LIST && id != Z_SYS_FS_LIST_EX) return (uint32_t *)&k_ok;

	// As k_fs_list() does: full "/"-prefixed paths, packed. FS_LIST_EX
	// is the same listing with a z_fs_info_t per entry (zfs.h).
	z_fs_info_t *info = NULL;
	z_fs_list_args_t *a = (z_fs_list_args_t *)args;
	if (id == Z_SYS_FS_LIST_EX) {
		info = ((z_fs_list_ex_args_t *)args)->info;
		a = &((z_fs_list_ex_args_t *)args)->list;
	}
	uint32_t w = 0;
	a->count = 0;
	a->truncated = 0;
	for (int i = 0; i < nentries && a->count < a->max_entries; i++) {
		char full[400];
		snprintf(full, sizeof(full), "%s/%s",
			(a->path && strcmp(a->path, "/")) ? a->path : "", entries[i]);
		size_t l = strlen(full);
		if (w + l + 1 > a->out_cap) { a->truncated = 1; break; }
		memcpy(a->out + w, full, l + 1);
		if (a->types) a->types[a->count] = types[i];
		if (info) {
			memset(&info[a->count], 0, sizeof(info[0]));
			info[a->count].type = types[i];
			info[a->count].size = sizes[i];
			info[a->count].fdate = (uint16_t)(((2026 - 1980) << 9) | (9 << 5) | (1 + i));
		}
		w += (uint32_t)(l + 1);
		a->count++;
	}
	return (uint32_t *)(a->count ? &k_ok : &k_fail);
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

static z_win_t win;
static z_flist_t fl;

static const char *LONG_NAME =
	"A very long file name that is far longer than the old twenty-four "
	"byte slots and the old sixty-four byte paths could ever hold.txt";

int main(int argc, char **argv) {

	if (!z_render_open(&win, 320, 200) || !k_install()) return 77;

	entries[0] = "zeitlos.cfg";                      types[0] = Z_FS_TYPE_FILE;
	entries[1] = "Gr\xC3\xBC\xC3\x9F" "e.txt";       types[1] = Z_FS_TYPE_FILE;  // Grüße.txt
	entries[2] = "\xE6\x97\xA5\xE6\x9C\xAC.txt";     types[2] = Z_FS_TYPE_FILE;  // 日本.txt
	entries[3] = LONG_NAME;                          types[3] = Z_FS_TYPE_FILE;
	entries[4] = "Meeting notes";                    types[4] = Z_FS_TYPE_DIR;
	entries[5] = "apps";                             types[5] = Z_FS_TYPE_DIR;
	nentries = 6;

	z_flist_init(&fl, &win);
	z_flist_set_geom(&fl, 0, 0, 300, 180);
	expect(z_flist_chdir(&fl, "/"), "lists the root");
	expect(fl.count == 6, "every entry, long ones included");
	expect(!z_flist_truncated(&fl), "not truncated");

	// Directories first, then A to Z (ASCII case folded; other UTF-8
	// by byte, so after ASCII). Lists ran Z to A before this test.
	expect(fl.isdir[0] && fl.isdir[1], "directories first");
	expect(!strcmp(fl.pool + fl.name_off[0], "apps"), "apps");
	expect(!strcmp(fl.pool + fl.name_off[1], "Meeting notes"), "a name with a space");
	expect(!strcmp(fl.pool + fl.name_off[2], LONG_NAME), "then A...");
	expect(!strcmp(fl.pool + fl.name_off[3], entries[1]), "Gr...");
	expect(!strcmp(fl.pool + fl.name_off[4], "zeitlos.cfg"), "z...");
	expect(!strcmp(fl.pool + fl.name_off[5], entries[2]), "and the Japanese name after ASCII");
	bool seen_long = false, seen_de = false, seen_ja = false;
	for (int i = 0; i < fl.count; i++) {
		const char *n = fl.pool + fl.name_off[i];
		if (!strcmp(n, LONG_NAME)) seen_long = true;
		if (!strcmp(n, entries[1])) seen_de = true;
		if (!strcmp(n, entries[2])) seen_ja = true;
	}
	expect(seen_long, "the long name, whole");
	expect(seen_de && seen_ja, "German and Japanese names, whole");

	// The selected path is built at full length.
	for (int i = 0; i < 8; i++) z_flist_key(&fl, Z_KEY_DOWN);
	char path[Z_FS_PATH_MAX];
	int tries = 0;
	while (strcmp(z_flist_selected(&fl) ? z_flist_selected(&fl) : "", LONG_NAME) && tries++ < 10)
		z_flist_key(&fl, Z_KEY_UP);
	expect(z_flist_selected_path(&fl, path, sizeof(path)), "a selected path");
	expect(!strcmp(path + 1, LONG_NAME) && path[0] == '/', "full path of the long name");

	// The card's /docs: 150 short names, more than the old bound of
	// 128 entries. All of them, and not reported as partial.
	static char docs[300][24];
	for (int i = 0; i < 150; i++) {
		snprintf(docs[i], sizeof(docs[i]), "doc_page_%03d.md", i);
		entries[i] = docs[i];
		types[i] = Z_FS_TYPE_FILE;
	}
	nentries = 150;
	expect(z_flist_chdir(&fl, "/docs"), "lists /docs");
	expect(fl.count == 150, "all 150 entries of /docs");
	expect(!z_flist_truncated(&fl), "/docs not reported as partial");

	// More entries than Z_FLIST_MAX is truncated at the bound.
	for (int i = 150; i < 300; i++) {
		snprintf(docs[i], sizeof(docs[i]), "doc_page_%03d.md", i);
		entries[i] = docs[i];
		types[i] = Z_FS_TYPE_FILE;
	}
	nentries = 300;
	expect(z_flist_chdir(&fl, "/many"), "lists a directory of 300");
	expect(fl.count == Z_FLIST_MAX, "Z_FLIST_MAX of them");
	expect(z_flist_truncated(&fl), "300 reported as truncated");

	// A directory whose names outgrow the pool is truncated, not
	// overflowed: 60 names of ~200 bytes is 12KB against an 8KB pool.
	static char big[60][210];
	for (int i = 0; i < 60; i++) {
		memset(big[i], 'a' + (i % 26), 200);
		snprintf(big[i] + 200, 10, "%03d", i);
		entries[i] = big[i];
		types[i] = Z_FS_TYPE_FILE;
	}
	nentries = 60;
	expect(z_flist_chdir(&fl, "/big"), "lists a directory of long names");
	expect(z_flist_truncated(&fl), "reported as truncated");
	expect(fl.count > 10 && fl.count < 60, "as many as fit");
	for (int i = 0; i < fl.count; i++)
		expect(strlen(fl.pool + fl.name_off[i]) == 203, "each kept name is whole");

	// -- the mouse wheel (z_flist_wheel) --
	{
		static z_flist_t fw;
		static char names[60][12];
		for (int i = 0; i < 60; i++) {
			snprintf(names[i], sizeof(names[i]), "f%02d.txt", i);
			entries[i] = names[i]; types[i] = Z_FS_TYPE_FILE;
		}
		nentries = 60;
		z_flist_init(&fw, &win);
		z_flist_set_geom(&fw, 0, 0, 200, 120);
		expect(z_flist_chdir(&fw, "/"), "wheel: 60 entries listed");
		int rows = (fw.h - 2) / (z_font_5x8.h + 2), total = fw.count;	// zflist.c ROW_H
		int sel = fw.sel;
		z_flist_wheel(&fw, -1);		// toward the user: down
		expect(fw.top == 3, "wheel: a notch down is three rows");
		expect(fw.sel == sel, "wheel: the selection does not move");
		expect((int)fw.sb.value == fw.top, "wheel: the scrollbar follows");
		z_flist_wheel(&fw, 1);
		expect(fw.top == 0, "wheel: a notch up comes back");
		z_flist_wheel(&fw, 5);
		expect(fw.top == 0, "wheel: clamps at the top");
		z_flist_wheel(&fw, -1000);
		expect(fw.top == total - rows && fw.top > 0, "wheel: clamps at the bottom");
		// A short list does not move at all.
		nentries = 3;
		z_flist_chdir(&fw, "/");
		z_flist_wheel(&fw, -4);
		expect(fw.top == 0, "wheel: nothing to scroll in a short list");
	}

	// -- the details column (fl.info, FS_LIST_EX) --
	{
		static z_flist_t fd;
		static z_fs_info_t inf[Z_FLIST_MAX];
		entries[0] = "zeitlos.cfg";   types[0] = Z_FS_TYPE_FILE; sizes[0] = 812;
		entries[1] = "photo.jpg";     types[1] = Z_FS_TYPE_FILE; sizes[1] = 123456;
		entries[2] = "speech.pak";    types[2] = Z_FS_TYPE_FILE; sizes[2] = 7864320;
		entries[3] = LONG_NAME;       types[3] = Z_FS_TYPE_FILE; sizes[3] = 10;
		entries[4] = "Meeting notes"; types[4] = Z_FS_TYPE_DIR;  sizes[4] = 0;
		entries[5] = "apps";          types[5] = Z_FS_TYPE_DIR;  sizes[5] = 0;
		nentries = 6;
		z_flist_init(&fd, &win);
		fd.info = inf;
		z_flist_set_geom(&fd, 0, 0, 300, 180);
		expect(z_flist_chdir(&fd, "/"), "details: lists through FS_LIST_EX");
		expect(fd.count == 6, "details: every entry");
		// Sorted -- and each entry's details went with it.
		expect(!strcmp(fd.pool + fd.name_off[0], "apps") && fd.isdir[0] &&
			inf[0].type == Z_FS_TYPE_DIR && (inf[0].fdate & 31) == 6,
			"details: apps first, its own date");
		bool paired = true;
		for (int i = 0; i < fd.count; i++) {
			const char *n = fd.pool + fd.name_off[i];
			for (int j = 0; j < 6; j++)
				if (!strcmp(n, entries[j]) && (inf[i].size != sizes[j] ||
					(inf[i].fdate & 31) != 1 + j || fd.isdir[i] != types[j]))
					paired = false;
		}
		expect(paired, "details: every entry keeps its own size and date");
		// z_flist_refresh_select() re-selects by name.
		expect(z_flist_refresh_select(&fd, "speech.pak") &&
			z_flist_selected(&fd) && !strcmp(z_flist_selected(&fd), "speech.pak"),
			"refresh_select selects by name");
		const z_fs_info_t *si = z_flist_selected_info(&fd);
		expect(si && si->size == 7864320, "selected_info is the selected entry's");
		if (argc > 1) {
			z_render_clear();
			z_flist_draw(&fd, true);
			char out[256];
			snprintf(out, sizeof(out), "%s-details.pbm", argv[1]);
			z_render_write(out, &win, 2);
		}
	}

	// A picture of the first listing.
	if (argc > 1) {
		entries[0] = "zeitlos.cfg"; entries[1] = "Gr\xC3\xBC\xC3\x9F" "e.txt";
		entries[2] = "\xE6\x97\xA5\xE6\x9C\xAC.txt"; entries[3] = LONG_NAME;
		entries[4] = "Meeting notes"; entries[5] = "apps";
		types[0] = types[1] = types[2] = types[3] = Z_FS_TYPE_FILE;
		types[4] = types[5] = Z_FS_TYPE_DIR;
		nentries = 6;
		z_flist_chdir(&fl, "/");
		z_render_clear();
		z_flist_draw(&fl, true);
		char out[256];
		snprintf(out, sizeof(out), "%s.pbm", argv[1]);
		z_render_write(out, &win, 2);
	}

	printf("test_flist: %d checks, %d failed\n", checks, failures);
	return failures ? 1 : 0;

}
