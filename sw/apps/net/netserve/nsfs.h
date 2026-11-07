#ifndef NSFS_H
#define NSFS_H
/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * The files sftp.c and scp.c reach: the few POSIX-shaped calls they
 * need, and nothing else. On Zeitlos they are the fs driver's client
 * (nsfs_zfs.c, over sw/common/zfsc.c); on a build machine, the tests'
 * own (tests/nsfs_posix.c, a real directory standing in for the card).
 * So the protocol code runs unchanged against real OpenSSH clients on
 * the host (tests/test_openssh.sh).
 *
 * Results: >= 0 success, < 0 a -Z_E* error (sw/common/zfsproto.h).
 * Paths are absolute and already normalised (xfer_path()).
 */
#include <stdint.h>
#include <stdbool.h>

#include "../../../common/zfsproto.h"	// Z_O_*, Z_E*, z_fs_vfs_t, z_fs_info_t

int32_t nsfs_open(const char *path, uint32_t zflags, z_fs_info_t *info);
int32_t nsfs_close(int32_t fd);
int32_t nsfs_read(int32_t fd, void *buf, uint32_t len);		// 0 at the end
int32_t nsfs_write(int32_t fd, const void *buf, uint32_t len);
int32_t nsfs_seek(int32_t fd, uint32_t off);					// absolute
int32_t nsfs_fstat(int32_t fd, z_fs_info_t *info);
int32_t nsfs_stat(const char *path, z_fs_info_t *info);
int32_t nsfs_unlink(const char *path);						// a file or an empty folder
int32_t nsfs_mkdir(const char *path);
int32_t nsfs_rename(const char *from, const char *to);		// -Z_EEXIST if `to` is there
int32_t nsfs_fsync(int32_t fd);
int32_t nsfs_ftruncate(int32_t fd, uint32_t len);
int32_t nsfs_statvfs(const char *path, z_fs_vfs_t *out);

// A folder, an entry at a time. nsfs_readdir(): 1 with an entry (a bare
// name, cut to `cap`), 0 at the end, < 0 an error.
typedef struct nsfs_dir nsfs_dir_t;
int32_t nsfs_opendir(const char *path, nsfs_dir_t **out);
int32_t nsfs_readdir(nsfs_dir_t *d, char *name, uint32_t cap, z_fs_info_t *info);
void nsfs_closedir(nsfs_dir_t *d);

// Seconds since 1970 (UTC), or 0 when the clock has not been set: for
// the dates `ls -l` shows.
uint32_t nsfs_now(void);

#endif
