/*
 * Zeitlos -- convert: packing a folder. pack.h.
 */
#include <string.h>
#include <stdio.h>

#include "zfs.h"
#include "zfsapp.h"
#include "zwalk.h"
#include "pack.h"

uint32_t pack_skipped;

static zwalk_t walk;
static uint8_t walk_mem[16384];
static zf_pack_t pack;
static uint32_t total;
static int cut;					// where an entry's name begins in its path
static const char *const *skips;

static bool skipped(const char *path) {
	for (int i = 0; i < 2; i++)
		if (skips && skips[i] && skips[i][0] && zf_ieq(path, skips[i])) return true;
	return false;
}

// Every file and folder under `top`, `top` itself first, each given to
// `each`; stops at the first error it returns.
static int walk_tree(const char *top, int (*each)(const char *path)) {
	int r = each(top);
	if (r < 0) return r;
	if (!zw_start(&walk, ZW_FIND, top, NULL, walk_mem, sizeof walk_mem)) return ZF_E_IO;
	zw_find_set(&walk, "", "");
	for (;;) {
		if (zf_cancel_poll && zf_cancel_poll()) { zw_cancel(&walk); return ZF_E_CANCEL; }
		int s = zw_step(&walk);
		if (s == ZW_DONE) break;
		if (s == ZW_FAIL) return ZF_E_IO;
		if (s == ZW_HIT && (r = each(walk.hit)) < 0) { zw_cancel(&walk); return r; }
	}
	pack_skipped = walk.skipped + (walk.truncated ? 1u : 0u);
	return ZF_OK;
}

static int count_one(const char *path) {
	z_fs_info_t fi;
	if (!skipped(path) && fs_stat(path, &fi) && fi.type != Z_FS_TYPE_DIR) total += fi.size;
	return ZF_OK;
}

static int pack_one(const char *path) {
	static zf_io_t io;
	z_fs_info_t fi;
	if (skipped(path)) return ZF_OK;
	if (!fs_stat(path, &fi)) return ZF_E_IO;
	const char *name = path + cut;
	if (fi.type == Z_FS_TYPE_DIR) return zf_pack_add(&pack, name, true, 0, fi.fdate, fi.ftime, NULL);
	if (!pack_open_file(path, fi.size, &io)) return ZF_E_IO;
	int r = zf_pack_add(&pack, name, false, fi.size, fi.fdate, fi.ftime, &io);
	pack_close_file(&io);
	return r;
}

int pack_folder(const char *dir, zf_type_t to, zf_io_t *out, const char *const skip[2]) {
	static char top[Z_FS_PATH_MAX];
	snprintf(top, sizeof top, "%s", dir);
	int n = (int)strlen(top);
	while (n > 1 && top[n - 1] == '/') top[--n] = 0;
	const char *slash = strrchr(top, '/');
	cut = slash ? (int)(slash - top) + 1 : 0;
	if (!top[cut]) return ZF_E_PATH;		// "/" has no name to give its entries
	skips = skip;
	total = 0;
	pack_skipped = 0;
	int r = walk_tree(top, count_one);
	if (r < 0) return r;
	if ((r = zf_pack_begin(&pack, out, to, total)) < 0) return r;
	if ((r = walk_tree(top, pack_one)) < 0) return r;
	return zf_pack_end(&pack);
}
