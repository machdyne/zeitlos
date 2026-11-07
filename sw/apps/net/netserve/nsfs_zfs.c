/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * nsfs.h on Zeitlos: the fs driver's client, sw/common/zfsc.c. Each
 * call is one request to `fs` and its reply; every other message
 * netserve receives meanwhile stays queued (z_msg_recv_match()), so
 * no session's data is lost while a file is read or written.
 */
#include <stdlib.h>
#include <string.h>

#include "nsfs.h"
#include "../../../common/zfsc.h"
#include "../../../common/zrtc.h"

int32_t nsfs_open(const char *path, uint32_t zflags, z_fs_info_t *info) {
	z_fs_info_t fi;
	return zfs_open_info(path, zflags, info ? info : &fi);
}

int32_t nsfs_close(int32_t fd) { return zfs_close(fd); }
int32_t nsfs_read(int32_t fd, void *buf, uint32_t len) { return zfs_read(fd, buf, len); }
int32_t nsfs_write(int32_t fd, const void *buf, uint32_t len) { return zfs_write(fd, buf, len); }

int32_t nsfs_seek(int32_t fd, uint32_t off) {
	int32_t r = zfs_lseek(fd, (int32_t)off, Z_SEEK_SET);
	return r < 0 ? r : 0;
}

int32_t nsfs_fstat(int32_t fd, z_fs_info_t *info) { return zfs_fstat(fd, info); }
int32_t nsfs_stat(const char *path, z_fs_info_t *info) { return zfs_stat(path, info); }
int32_t nsfs_unlink(const char *path) { return zfs_unlink(path); }
int32_t nsfs_mkdir(const char *path) { return zfs_mkdir(path); }
int32_t nsfs_rename(const char *from, const char *to) { return zfs_rename(from, to); }
int32_t nsfs_fsync(int32_t fd) { return zfs_fsync(fd); }
int32_t nsfs_ftruncate(int32_t fd, uint32_t len) { return zfs_ftruncate(fd, len); }
int32_t nsfs_statvfs(const char *path, z_fs_vfs_t *out) { return zfs_statvfs(path, out); }

struct nsfs_dir { zfs_dir_t *d; };

int32_t nsfs_opendir(const char *path, nsfs_dir_t **out) {
	z_fs_info_t fi;
	int32_t r = zfs_stat(path, &fi);
	if (r < 0) return r;
	if (fi.type != Z_FS_TYPE_DIR) return -Z_ENOTDIR;
	nsfs_dir_t *h = malloc(sizeof(*h));
	if (!h) return -Z_EMFILE;
	h->d = zfs_opendir(path);		// a 2KB page, from the heap
	if (!h->d) { free(h); return -Z_EIO; }
	*out = h;
	return 0;
}

int32_t nsfs_readdir(nsfs_dir_t *h, char *name, uint32_t cap, z_fs_info_t *info) {
	const z_fs_dirent_t *e = zfs_readdir(h->d);
	if (!e) return zfs_dir_error(h->d);
	uint32_t n = e->namelen < cap - 1 ? e->namelen : cap - 1;
	memcpy(name, e->name, n);
	name[n] = 0;
	*info = e->info;
	return 1;
}

void nsfs_closedir(nsfs_dir_t *h) {
	if (!h) return;
	zfs_closedir(h->d);
	free(h);
}

uint32_t nsfs_now(void) {
	return (z_rtc_available() && z_rtc_valid()) ? z_rtc_seconds() : 0;
}
