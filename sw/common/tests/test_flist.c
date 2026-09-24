/*
 * Host test for sw/common/zflist.c with long file names -- the real
 * widget, listing through a scripted kernel's Z_SYS_FS_LIST.
 *
 *   cc -std=gnu99 -Wall -no-pie -I sw/common -o /tmp/test_flist \
 *      sw/common/tests/test_flist.c sw/common/zflist.c sw/common/zwin.c \
 *      sw/common/zwidget.c sw/common/zfsapp.c sw/common/zfont_data.c \
 *      sw/common/zobj.c sw/common/zeitlos.c sw/common/zspeak.c
 *   /tmp/test_flist /tmp/flist    # also writes /tmp/flist.pbm
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

static const char *entries[64];
static uint8_t types[64];
static int nentries;

static z_obj_t k_ok, k_fail;

static uint32_t *k_syscall(uint32_t id, uint32_t *args, uint32_t b) {
	(void)b;
	if (id == Z_SYS_UPTIME) {
		((z_obj_t *)args)->type = Z_UINT32;
		((z_obj_t *)args)->val.uint32 = 1000;
		return (uint32_t *)&k_ok;
	}
	if (id != Z_SYS_FS_LIST) return (uint32_t *)&k_ok;

	// As k_fs_list() does: full "/"-prefixed paths, packed.
	z_fs_list_args_t *a = (z_fs_list_args_t *)args;
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

	// A directory whose names outgrow the pool is truncated, not
	// overflowed: 40 names of ~200 bytes is 8KB against a 4KB pool.
	static char big[40][210];
	for (int i = 0; i < 40; i++) {
		memset(big[i], 'a' + (i % 26), 200);
		snprintf(big[i] + 200, 10, "%03d", i);
		entries[i] = big[i];
		types[i] = Z_FS_TYPE_FILE;
	}
	nentries = 40;
	expect(z_flist_chdir(&fl, "/big"), "lists a directory of long names");
	expect(z_flist_truncated(&fl), "reported as truncated");
	expect(fl.count > 10 && fl.count < 40, "as many as fit");
	for (int i = 0; i < fl.count; i++)
		expect(strlen(fl.pool + fl.name_off[i]) == 203, "each kept name is whole");

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
