/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * bk.c's files (bk_io_t's file calls) over zfsc.c: straight to the fs
 * driver, which serves every volume -- the card, /ram and the backup
 * drive -- so free space (STATVFS) and listings a page at a time
 * (OPENDIR/READDIR) are there for /usb too.
 *
 * Its own file so the host test (tests/test_fat.c) can run it against
 * the real fs driver and FatFs (sw/apps/fs/tests/test_fs.c's kernel
 * stubs), on real FAT images, with the power cut at a sector write.
 */

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "../../common/zfsc.h"
#include "../../common/zfs.h"
#include "../../common/zpaths.h"

#include "bk_zfs.h"

static int err_of(int32_t r) {
	switch (-r) {
	case Z_ENOENT: case Z_ENOTDIR: return BK_ENOENT;
	case Z_EEXIST:  return BK_EEXIST;
	case Z_EBUSY:   return BK_EBUSY;
	case Z_ENOSPC:  return BK_ENOSPC;
	default:        return BK_ERR;
	}
}

static int io_open_read(void *c, const char *p) {
	(void)c;
	int32_t fd = zfs_open(p, Z_O_RDONLY);
	return fd >= 0 ? (int)fd : err_of(fd);
}

static int io_open_write(void *c, const char *p) {
	(void)c;
	int32_t fd = zfs_open(p, Z_O_WRONLY | Z_O_CREAT | Z_O_TRUNC);
	return fd >= 0 ? (int)fd : err_of(fd);
}

static int io_read(void *c, int h, void *b, int n) {
	(void)c;
	int32_t r = zfs_read(h, b, (uint32_t)n);
	return r >= 0 ? (int)r : err_of(r);
}

static int io_write(void *c, int h, const void *b, int n) {
	(void)c;
	int32_t r = zfs_write(h, b, (uint32_t)n);
	return r >= 0 ? (int)r : err_of(r);
}

static int io_seek(void *c, int h, uint32_t off) {
	(void)c;
	return zfs_lseek(h, (int32_t)off, Z_SEEK_SET) >= 0 ? BK_OK : BK_ERR;
}

static int io_sync(void *c, int h) {
	(void)c;
	return zfs_fsync(h) >= 0 ? BK_OK : BK_ERR;
}

static int io_close(void *c, int h) {
	(void)c;
	return zfs_close(h) >= 0 ? BK_OK : BK_ERR;
}

static bool is_dir(const z_fs_info_t *fi) {
	return fi->type == Z_FS_TYPE_DIR || (fi->attr & Z_FS_ATTR_DIR);
}

static int io_stat(void *c, const char *p, bk_stat_t *st) {
	(void)c;
	z_fs_info_t fi;
	int32_t r = zfs_stat(p, &fi);
	if (r < 0) return err_of(r);
	st->dir = is_dir(&fi);
	st->size = st->dir ? 0 : fi.size;
	st->fdate = fi.fdate;
	st->ftime = fi.ftime;
	return BK_OK;
}

static int io_list(void *c, const char *p,
		int (*fn)(void *arg, const char *name, const bk_stat_t *st), void *arg) {
	(void)c;
	zfs_dir_t *d = zfs_opendir(p);
	if (!d) return BK_ENOENT;
	if (zfs_dir_error(d) < 0) { int e = err_of(zfs_dir_error(d)); zfs_closedir(d); return e; }
	const z_fs_dirent_t *e;
	int r = 0;
	while (!r && (e = zfs_readdir(d)) != NULL) {
		if (!strcmp(e->name, ".") || !strcmp(e->name, "..")) continue;
		bk_stat_t st;
		st.dir = is_dir(&e->info);
		st.size = st.dir ? 0 : e->info.size;
		st.fdate = e->info.fdate;
		st.ftime = e->info.ftime;
		r = fn(arg, e->name, &st);
	}
	int32_t de = zfs_dir_error(d);
	zfs_closedir(d);
	if (r) return r;
	return de < 0 ? err_of(de) : BK_OK;
}

static int io_unlink(void *c, const char *p) {
	(void)c;
	int32_t r = zfs_unlink(p);
	return r >= 0 ? BK_OK : err_of(r);
}

static int io_mkdir(void *c, const char *p) {
	(void)c;
	int32_t r = zfs_mkdir(p);
	if (r >= 0) return BK_OK;
	if (-r == Z_EEXIST) return BK_EEXIST;
	return err_of(r);
}

static int io_rename(void *c, const char *a, const char *b) {
	(void)c;
	int32_t r = zfs_rename(a, b);
	return r >= 0 ? BK_OK : err_of(r);
}

// The free space of the volume path is on. FatFs answers from the
// drive's FSINFO when it is valid; when not, the first ask after
// mounting counts the whole FAT once (docs/backup.md, "Drives").
static int io_free_kb(void *c, const char *p, uint32_t *kb) {
	(void)c;
	z_fs_vfs_t v;
	char vol[16];
	if (!strncmp(p, Z_DIR_USB, 4) && (p[4] == 0 || p[4] == '/')) strcpy(vol, Z_DIR_USB);
	else if (!strncmp(p, Z_DIR_RAM, 4) && (p[4] == 0 || p[4] == '/')) strcpy(vol, Z_DIR_RAM);
	else strcpy(vol, "/");
	if (zfs_statvfs(vol, &v) < 0 || !v.total_kb) return BK_ERR;
	*kb = v.free_kb;
	return BK_OK;
}

void bk_zfs_io(bk_io_t *io) {
	io->open_read = io_open_read;
	io->open_write = io_open_write;
	io->read = io_read;
	io->write = io_write;
	io->seek = io_seek;
	io->sync = io_sync;
	io->close = io_close;
	io->stat = io_stat;
	io->list = io_list;
	io->unlink = io_unlink;
	io->mkdir = io_mkdir;
	io->rename = io_rename;
	io->free_kb = io_free_kb;
}
