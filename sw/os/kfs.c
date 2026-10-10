/*
 * Zeitlos OS
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * The kernel's files, through `fs`: kfs.h.
 */

#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "kernel.h"
#include "kfs.h"
#include "zar.h"
#include "../common/zfsc.h"
#include "../common/zpaths.h"
#include "../common/zsoc.h"

bool kfs_up(void) {
	uint32_t pid;
	return z_pid_lookup(Z_FS_SERVICE, &pid);
}

uint32_t fs_size(char *path) {
	z_fs_info_t fi;
	if (!path || zfs_stat(path, &fi) != 0 || fi.type != Z_FS_TYPE_FILE) return 0;
	return fi.size;
}

int fs_write_file(char *path, char *buf, uint32_t len) {
	int32_t fd = zfs_open(path, Z_O_WRONLY | Z_O_CREAT | Z_O_TRUNC);
	if (fd < 0) {
		printf("write failed (%ld)\n", (long)fd);
		return 0;
	}
	int32_t n = len ? zfs_write(fd, buf, len) : 0;
	if (zfs_close(fd) < 0 || n < 0) return 0;
	return (int)n;
}

int fs_touch(char *path) {
	int32_t fd = zfs_open(path, Z_O_WRONLY | Z_O_CREAT | Z_O_TRUNC);
	if (fd < 0) {
		printf("write failed (%ld)\n", (long)fd);
		return 1;
	}
	zfs_close(fd);
	printf("file touched.\n");
	return 0;
}

int fs_mkdir(char *path) {
	int32_t r;
	printf("making directory '%s' ...\n", path);
	r = zfs_mkdir(path);
	if (r < 0) {
		printf("mkdir failed (%ld)\n", (long)r);
		return 1;
	}
	return 0;
}

int fs_unlink(char *path) {
	return zfs_unlink(path) == 0 ? 0 : 1;
}

int fs_rename(const char *from, const char *to) {
	if (!from || !to || !from[0] || !to[0]) return Z_FS_ERR_INVAL;
	int32_t r = zfs_rename(from, to);
	switch (r) {
	case 0:				return Z_FS_ERR_NONE;
	case -Z_ENOENT:		return Z_FS_ERR_NOENT;
	case -Z_EEXIST:		return Z_FS_ERR_EXIST;
	case -Z_EXDEV:		return Z_FS_ERR_XDEV;
	case -Z_EBUSY:		return Z_FS_ERR_BUSY;
	case -Z_EINVAL:		return Z_FS_ERR_INVAL;
	default:			return Z_FS_ERR_IO;
	}
}

bool fs_stat_info(const char *path, z_fs_info_t *fi) {
	memset(fi, 0, sizeof(*fi));
	return path && zfs_stat(path, fi) == 0;
}

// `ls`: directories with a trailing '/', files with their size. What is
// only in flash is marked read-only by its attribute, and listed with
// the card's own entries, as apps see it.
void fs_list_dir(char *path) {
	zfs_dir_t d;
	uint32_t page[128];		// 512 bytes: this may be the shell's stack
	const z_fs_dirent_t *e;
	int32_t r = zfs_opendir_at(&d, path && path[0] ? path : "/", page, sizeof(page));
	if (r < 0) {
		printf("(cannot list %s: %s)\n", path && path[0] ? path : "/",
			r == -Z_ENODEV ? "no filesystem -- is fs running?" : "error");
		return;
	}
	while ((e = zfs_readdir(&d)) != NULL) {
		if (e->info.type == Z_FS_TYPE_DIR) printf("%s/\n", e->name);
		else printf("%-24s %lu%s\n", e->name, (unsigned long)e->info.size,
			(e->info.attr & Z_FS_ATTR_RDONLY) ? "  (ro)" : "");
	}
	zfs_closedir_at(&d);
}

void fs_df_kb(uint32_t *total_kb, uint32_t *free_kb) {
	z_fs_vfs_t v;
	if (total_kb) *total_kb = 0;
	if (free_kb) *free_kb = 0;
	if (zfs_statvfs("/", &v) != 0) return;
	if (total_kb) *total_kb = v.total_kb;
	if (free_kb) *free_kb = v.free_kb;
}

int fs_format(void) {
	int32_t r;
	printf("formatting ...\n");
	r = zfs_mkfs("/");
	if (r == 0) {
		printf("format succeeded.\n");
		return 0;
	}
	printf("format failed (%ld)\n", (long)r);
	return 1;
}

int fs_mount_now(void) { return zfs_mount("/") == 0 ? 0 : 1; }
int fs_unmount(void) { return zfs_umount("/") == 0 ? 0 : 1; }

bool fs_usb_mount(void) {
	if (zfs_mount(Z_DIR_USB) == 0) return true;
	printf("fs: no usb storage mounted\n");
	return false;
}

void fs_usb_unmount(void) {
	int32_t r = zfs_umount(Z_DIR_USB);
	if (r == -Z_EBUSY) printf(" - usb storage: files are open\n");
	else if (r == 0) printf(" - usb storage: " Z_DIR_USB " unmounted, safe to remove\n");
}

int k_fs_sync_all(void) {
	int32_t n = zfs_syncall();
	return n < 0 ? 0 : (int)n;
}

// -- streaming --

int fs_open_write(kfs_file_t *f, char *path) {
	f->fd = zfs_open(path, Z_O_WRONLY | Z_O_CREAT | Z_O_TRUNC);
	if (f->fd < 0) {
		printf("fs_open_write: failed (%ld)\n", (long)f->fd);
		return 0;
	}
	return 1;
}

int fs_write_chunk(kfs_file_t *f, const void *buf, uint32_t len) {
	int32_t n = zfs_write(f->fd, buf, len);
	return n < 0 ? -1 : (int)n;
}

int fs_sync(kfs_file_t *f) { return zfs_fsync(f->fd) == 0 ? 1 : 0; }
int fs_close_write(kfs_file_t *f) { return zfs_close(f->fd) == 0 ? 1 : 0; }

int fs_open_read(kfs_file_t *f, char *path) {
	f->fd = zfs_open(path, Z_O_RDONLY);
	if (f->fd < 0) {
		printf("fs_open_read: failed (%ld)\n", (long)f->fd);
		return 0;
	}
	return 1;
}

int32_t fs_read_chunk(kfs_file_t *f, void *buf, uint32_t maxlen) {
	int32_t n = zfs_read(f->fd, buf, maxlen);
	return n < 0 ? -1 : n;
}

int fs_close_read(kfs_file_t *f) { return zfs_close(f->fd) == 0 ? 1 : 0; }

int32_t kfs_read_file(const char *path, void *buf, uint32_t cap) {
	z_fs_info_t fi;
	int32_t fd = zfs_open_info(path, Z_O_RDONLY, &fi);
	if (fd < 0) return -1;
	if (fi.size > cap) { zfs_close(fd); return -1; }
	uint32_t n = 0;
	while (n < fi.size) {				// zfs_read() may come back short
		int32_t r = zfs_read(fd, (uint8_t *)buf + n, fi.size - n);
		if (r <= 0) break;
		n += (uint32_t)r;
	}
	zfs_close(fd);
	return n == fi.size ? (int32_t)n : -1;
}

// -- programs --
//
// A bare program name is looked for in /apps on the card, then in the
// flash archive, and nowhere else; a name containing '/' is a path,
// opened as given (and, for /apps/<name> not on the card, the app in
// flash). docs/layout.md, "Finding a program". The card copy wins: the
// only way it got there was somebody putting it there.

#define APPS_DIR		Z_DIR_APPS "/"
#define RESOLVED_MAX	(64 + sizeof(APPS_DIR))

typedef enum { SRC_NONE = 0, SRC_CARD, SRC_ZAR } exec_src_t;

// The card's copy (through fs, never its flash underlay).
int fs_exec_info(char *path, z_exec_info_t *info) {
	uint8_t hdr[Z_EXEC_HEADER_SIZE];
	z_fs_info_t fi;
	int32_t fd, n;
	if (!path || !info) return 1;
	fd = zfs_open_info(path, Z_O_RDONLY | Z_O_NOFLASH, &fi);
	if (fd < 0) return 1;
	n = zfs_read(fd, hdr, sizeof(hdr));
	zfs_close(fd);
	if (n < 0) return 1;
	return z_exec_parse(hdr, (uint32_t)n, fi.size, info);
}

static bool name_is_path(const char *n) {
	for (; *n; n++) if (*n == '/' || *n == '\\') return true;
	return false;
}

static bool apps_prefix(const char *p) {
	const char *a = APPS_DIR;
	for (; *a; a++, p++) {
		char x = *p;
		if (x >= 'A' && x <= 'Z') x += 'a' - 'A';
		if (x != *a) return false;
	}
	return true;
}

static exec_src_t exec_resolve(const char *name, char *resolved, z_exec_info_t *info) {
	z_exec_info_t tmp;
	if (!info) info = &tmp;
	if (!name || !*name) return SRC_NONE;
	size_t len = strlen(name);
	if (len + sizeof(APPS_DIR) > RESOLVED_MAX) return SRC_NONE;

	if (name_is_path(name)) {
		strcpy(resolved, name);
		if (fs_exec_info(resolved, info) == 0 && info->total) return SRC_CARD;
		if (apps_prefix(name) &&
			z_zar_exec_info(name + sizeof(APPS_DIR) - 1, info) == 0 && info->total) {
			strcpy(resolved, name + sizeof(APPS_DIR) - 1);
			return SRC_ZAR;
		}
		return SRC_NONE;
	}

	strcpy(resolved, APPS_DIR);
	strcat(resolved, name);
	if (fs_exec_info(resolved, info) == 0 && info->total) return SRC_CARD;

	strcpy(resolved, name);
	if (z_zar_exec_info(resolved, info) == 0 && info->total) return SRC_ZAR;
	return SRC_NONE;
}

int fs_exec_info_any(char *path, z_exec_info_t *info) {
	char resolved[RESOLVED_MAX];
	return exec_resolve(path, resolved, info) == SRC_NONE ? 1 : 0;
}

int fs_exec_is_flash(char *path) {
	char resolved[RESOLVED_MAX];
	return exec_resolve(path, resolved, NULL) == SRC_ZAR ? 1 : 0;
}

// The data section to `dst` (physical: a new process's memory), the
// bss zeroed after it. The reads come back in fs's replies and the
// kernel copies each straight to its place.
static int load_card(uint32_t dst, const char *path, const z_exec_info_t *info) {
	int32_t fd = zfs_open(path, Z_O_RDONLY | Z_O_NOFLASH);
	uint8_t *p = (uint8_t *)dst;
	if (fd < 0) return 1;
	if (info->data_off && zfs_lseek(fd, (int32_t)info->data_off, Z_SEEK_SET) < 0) {
		zfs_close(fd);
		return 1;
	}
	uint32_t done = 0;
	while (done < info->data_size) {
		int32_t r = zfs_read(fd, p + done, info->data_size - done);
		if (r <= 0) break;
		done += (uint32_t)r;
	}
	zfs_close(fd);
	if (done != info->data_size) return 1;
	// Nothing else zeroes .bss on this OS: no crt0 does it.
	if (info->bss_size) memset(p + done, 0, info->bss_size);
	// Code was just written through the data path; the instruction
	// cache may hold this memory's previous occupant.
	z_icache_flush();
	return 0;
}

int fs_load_exec_any(uint32_t dst, char *path, const z_exec_info_t *info) {
	char resolved[RESOLVED_MAX];
	exec_src_t src = exec_resolve(path, resolved, NULL);
	if (!info) return 1;
	if (src == SRC_ZAR) return z_zar_load_exec(dst, resolved, info);
	if (src == SRC_CARD) return load_card(dst, resolved, info);
	return 1;
}

static bool name_eq_ci(const char *a, const char *b) {
	for (; *a && *b; a++, b++) {
		char x = *a, y = *b;
		if (x >= 'A' && x <= 'Z') x += 'a' - 'A';
		if (y >= 'A' && y <= 'Z') y += 'a' - 'A';
		if (x != y) return false;
	}
	return *a == *b;
}

// Which of `n` names would launch: one listing of /apps (one or two
// requests), not a lookup per name -- wm's dock at startup. A file in
// /apps counts if it is non-empty, without its header being read.
uint32_t fs_exec_exists_many(const char *const *names, uint32_t n, uint8_t *found) {
	uint32_t hits = 0;
	zfs_dir_t d;
	uint32_t page[128];
	const z_fs_dirent_t *e;

	for (uint32_t i = 0; i < n; i++) found[i] = 0;

	if (zfs_opendir_at(&d, Z_DIR_APPS, page, sizeof(page)) == 0) {
		while ((e = zfs_readdir(&d)) != NULL) {
			// the card's entries only: flash is asked below, directly
			if (e->info.type == Z_FS_TYPE_DIR || !e->info.size ||
				(e->info.attr & Z_FS_ATTR_RDONLY)) continue;
			for (uint32_t i = 0; i < n; i++)
				if (!found[i] && names[i] && !name_is_path(names[i]) &&
					name_eq_ci(e->name, names[i])) {
					found[i] = 1;
					hits++;
				}
		}
		zfs_closedir_at(&d);
	}

	for (uint32_t i = 0; i < n; i++) {
		z_exec_info_t xi;
		if (found[i] || !names[i] || !names[i][0]) continue;
		if (!name_is_path(names[i])) {
			if (z_zar_exec_info(names[i], &xi) == 0 && xi.total) found[i] = 1;
		} else {
			if (fs_exec_info_any((char *)names[i], &xi) == 0 && xi.total) found[i] = 1;
		}
		hits += found[i];
	}
	return hits;
}
