#ifndef ZFSC_H
#define ZFSC_H

/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * The filesystem driver's client: POSIX-shaped calls, each one request
 * to `fs` (sw/apps/fs) and its reply. Protocol: zfsproto.h. The whole
 * picture: docs/fs_driver.md.
 *
 * Results follow the protocol: >= 0 success (a descriptor, a byte
 * count, a position), < 0 a -Z_E* error (zfsproto.h). -Z_ENODEV: no
 * `fs` is running; -Z_ETIMEDOUT: it did not answer.
 *
 * The first call opts this process in to drivers writing into it
 * (z_grant_drivers(true)), so reads land straight in the caller's
 * buffer on gateware with the grant window, and come back in the reply
 * (one copy more) without it.
 *
 * Not thread-safe -- there are no threads -- and not for use from an
 * interrupt-like context: it waits for the reply.
 *
 * Who uses it: zfsapp.c, when an app links zfsc.o and is built with
 * -DZFSAPP_DIRECT (ipcbench); otherwise zfsapp goes through the
 * kernel's Z_SYS_FS_* calls, which are this same client built into the
 * kernel (-DZFSC_KERNEL, sw/os/kfs.c, sw/os/fsapi.c).
 */

#include <stdint.h>
#include <stdbool.h>
#include "zfsproto.h"

int32_t zfs_open(const char *path, uint32_t flags);
// zfs_open(), and the file's size (info->size) from the same reply.
int32_t zfs_open_info(const char *path, uint32_t flags, z_fs_info_t *info);
int32_t zfs_close(int32_t fd);
int32_t zfs_read(int32_t fd, void *buf, uint32_t len);
int32_t zfs_write(int32_t fd, const void *buf, uint32_t len);
int32_t zfs_lseek(int32_t fd, int32_t off, uint32_t whence);
int32_t zfs_fstat(int32_t fd, z_fs_info_t *info);
int32_t zfs_stat(const char *path, z_fs_info_t *info);
int32_t zfs_unlink(const char *path);
int32_t zfs_mkdir(const char *path);
int32_t zfs_rename(const char *from, const char *to);
int32_t zfs_fsync(int32_t fd);
int32_t zfs_ftruncate(int32_t fd, uint32_t len);
int32_t zfs_statvfs(const char *path, z_fs_vfs_t *out);

// Directories. zfs_readdir() returns the next entry, or NULL at the end
// (or on an error: zfs_dir_error()). The entry stays valid until the
// next zfs_readdir() or zfs_closedir() on the same directory.
//
// zfs_opendir()/zfs_closedir() allocate the descriptor and a 2KB page;
// zfs_opendir_at()/zfs_closedir_at() use the caller's, `buf` word
// aligned, `cap` bytes (fs sends pages no bigger) -- what the kernel
// uses.
typedef struct zfs_dir {
	uint8_t		*buf;
	uint32_t	cap;
	int32_t		dd;
	int32_t		err;
	uint32_t	at, len;
	bool		end;
} zfs_dir_t;
zfs_dir_t *zfs_opendir(const char *path);
int32_t zfs_opendir_at(zfs_dir_t *d, const char *path, void *buf, uint32_t cap);
const z_fs_dirent_t *zfs_readdir(zfs_dir_t *d);
int32_t zfs_dir_error(const zfs_dir_t *d);
int32_t zfs_closedir(zfs_dir_t *d);
int32_t zfs_closedir_at(zfs_dir_t *d);

// Volumes (zfsproto.h): `path` is "/", "/ram" or "/usb".
int32_t zfs_mkfs(const char *path);
int32_t zfs_mount(const char *path);
int32_t zfs_umount(const char *path);
int32_t zfs_syncall(void);

// True for a path `fs` serves: "/fs" and anything under it.
bool zfs_path_is_ours(const char *path);

// How many ticks a call waits for its reply before -Z_ETIMEDOUT.
#define ZFS_TIMEOUT_TICKS	(732u * 15u)
// ... and for a call that may take that long legitimately: formatting,
// the first free-space count on a large FAT32 card.
#define ZFS_SLOW_TICKS		(732u * 120u)

// Small writes are collected and sent as one request (zfsc.c,
// "write-behind"); any other call sends them first. This sends them
// now, for a process about to hand a file to another.
void zfs_flush(void);

#endif
