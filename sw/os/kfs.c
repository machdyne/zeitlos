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
#include "../common/znfsproto.h"	// Z_DIR_NFS
#include "cfg.h"					// k_cfg_find(): system.apps.nfs_first

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
// the card's own entries, as apps see it. A path is from the root (the
// kernel shell's only directory), with or without its leading '/'; a
// file lists itself; a last component with * or ? lists what in its
// directory matches. Not zglob.h's [classes] and UTF-8: that is 1.4KB
// of a kernel with little room, for a debugging shell.
static bool wild(const char *p, const char *n) {
	const char *sp = NULL, *sn = NULL;
	while (*n) {
		char a = *p, b = *n;
		if (a >= 'A' && a <= 'Z') a += 32;
		if (b >= 'A' && b <= 'Z') b += 32;
		if (a == '*') { sp = ++p; sn = n; continue; }
		if (a && (a == '?' || a == b)) { p++; n++; continue; }
		if (!sp) return false;
		p = sp; n = ++sn;
	}
	while (*p == '*') p++;
	return !*p;
}

static void list_entry(const char *name, const z_fs_info_t *fi) {
	if (fi->type == Z_FS_TYPE_DIR) printf("%s/\n", name);
	else printf("%-24s %lu%s\n", name, (unsigned long)fi->size,
		(fi->attr & Z_FS_ATTR_RDONLY) ? "  (ro)" : "");
}

void fs_list_dir(char *arg) {
	static char path[FS_PATH_MAX];
	zfs_dir_t d;
	uint32_t page[128];		// 512 bytes: this may be the shell's stack
	const z_fs_dirent_t *e;
	const char *dir = path, *pat = NULL;
	char *last = path;
	z_fs_info_t fi;

	if (!arg || !arg[0]) arg = "/";
	snprintf(path, sizeof(path), "%s%s", arg[0] == '/' ? "" : "/", arg);

	for (char *q = path; *q; q++) {
		if (*q == '/') { last = q; pat = NULL; }
		else if ((*q == '*' || *q == '?') && !pat) pat = last + 1;
	}
	if (pat) {
		if (last == path) dir = "/";
		else *last = 0;
	} else if (zfs_stat(path, &fi) == 0 && fi.type != Z_FS_TYPE_DIR) {
		list_entry(last + 1, &fi);
		return;
	}

	int32_t r = zfs_opendir_at(&d, dir, page, sizeof(page));
	if (r < 0) {
		printf("ls: %s: %s\n", arg,
			r == -Z_ENODEV ? "no filesystem -- is fs running?" :
			r == -Z_ENOENT ? "no such file or directory" : "cannot list");
		return;
	}
	while ((e = zfs_readdir(&d)) != NULL)
		if (!pat || wild(pat, e->name)) list_entry(e->name, &e->info);
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
	if (n < 0) printf("fs_write_chunk: failed (%ld)\n", (long)n);
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
#define NFS_APPS_DIR	Z_DIR_NFS Z_DIR_APPS "/"
#define RESOLVED_MAX	(64 + sizeof(NFS_APPS_DIR))

// system.apps.nfs_first (zeitlos.cfg), yes unless it says otherwise: a
// bare name is looked for in /nfs/apps first, the PC's freshly built
// copy, while an export is mounted (docs/nfs.md). Unmounted, fs answers
// a /nfs path at once, without asking net.
static bool nfs_first(void) {
	const char *v = k_cfg_find("system.apps.nfs_first");
	if (!v) return true;
	return !(v[0] == 'n' || v[0] == 'N' || v[0] == '0' || v[0] == 'f' || v[0] == 'F' ||
		((v[0] == 'o' || v[0] == 'O') && (v[1] == 'f' || v[1] == 'F')));
}

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

	// Not the drivers: fs and usb are what /nfs is reached through,
	// and net is the client itself, so none of them is taken from it.
	if (nfs_first() && strcmp(name, "fs") && strcmp(name, "usb") && strcmp(name, "net")) {
		strcpy(resolved, NFS_APPS_DIR);
		strcat(resolved, name);
		if (fs_exec_info(resolved, info) == 0 && info->total) return SRC_CARD;
	}

	strcpy(resolved, APPS_DIR);
	strcat(resolved, name);
	if (fs_exec_info(resolved, info) == 0 && info->total) return SRC_CARD;

	strcpy(resolved, name);
	if (z_zar_exec_info(resolved, info) == 0 && info->total) return SRC_ZAR;
	return SRC_NONE;
}

// The last resolution, for the calls that follow it: a launch asks
// three times in a row (the size, where from, the load), and each ask is
// one or two lookups through fs -- on the card, a walk of /apps, which
// was most of what `init: wm loaded in ...` measured. Kept for half a
// second from when it was made: a file replaced in that moment (by
// another process, which the kernel does not see) is the window, and the
// load checks what it reads against the sizes anyway.
#define RESOLVE_KEEP	(Z_TICK_HZ / 2)
static struct {
	char			name[RESOLVED_MAX];
	char			resolved[RESOLVED_MAX];
	exec_src_t		src;
	z_exec_info_t	info;
	uint32_t		tick;
	bool			valid;
} last_res;

static exec_src_t exec_resolve_kept(const char *name, char *resolved, z_exec_info_t *info) {
	z_exec_info_t tmp;
	if (!info) info = &tmp;
	if (name && last_res.valid && !strcmp(name, last_res.name) &&
		z_uptime_ticks() - last_res.tick < RESOLVE_KEEP) {
		strcpy(resolved, last_res.resolved);
		*info = last_res.info;
		return last_res.src;
	}
	exec_src_t src = exec_resolve(name, resolved, info);
	last_res.valid = false;
	if (name && strlen(name) < RESOLVED_MAX && src != SRC_NONE) {
		strcpy(last_res.name, name);
		strcpy(last_res.resolved, resolved);
		last_res.src = src;
		last_res.info = *info;
		last_res.tick = z_uptime_ticks();
		last_res.valid = true;
	}
	return src;
}
#define exec_resolve exec_resolve_kept

int fs_exec_info_any(char *path, z_exec_info_t *info) {
	char resolved[RESOLVED_MAX];
	return exec_resolve(path, resolved, info) == SRC_NONE ? 1 : 0;
}

// Where `path` would run from, for the shell to say: the file it
// resolves to ("/nfs/apps/text", "/apps/text"), or "flash".
int fs_exec_where(char *path, char *out, uint32_t cap) {
	char resolved[RESOLVED_MAX];
	exec_src_t s = exec_resolve(path, resolved, NULL);
	if (s == SRC_NONE || !cap) return 1;
	strncpy(out, s == SRC_ZAR ? "flash" : resolved, cap - 1);
	out[cap - 1] = 0;
	return 0;
}

int fs_exec_is_flash(char *path) {
	char resolved[RESOLVED_MAX];
	return exec_resolve(path, resolved, NULL) == SRC_ZAR ? 1 : 0;
}

// -- compressed executables (docs/executables.md) --
//
// One decoder for every load (3.3KB of tables and buffer), kept here
// rather than on a stack: a load runs on the stack of whoever asked,
// which may be wm's 8KB. A load from a file waits on fs mid-stream, so
// another can start meanwhile; it waits its turn.
static z_exec_lz4_t k_unpack;
static volatile bool k_unpack_busy;

uint32_t k_exec_unpack_ticks, k_exec_unpack_cpu;

int k_exec_unpack(uint8_t *out, uint32_t len, z_exec_fill_t fill, void *ctx) {
	while (k_unpack_busy) k_proc_wait_ticks(1);
	k_unpack_busy = true;
	uint32_t me = z_pid, t0 = z_uptime_ticks(), c0 = z_procs[me].cpu_ticks;
	int r = z_exec_lz4(&k_unpack, out, len, fill, ctx);
	k_exec_unpack_ticks = z_uptime_ticks() - t0;
	k_exec_unpack_cpu = z_procs[me].cpu_ticks - c0;
	k_unpack_busy = false;
	return r ? 1 : 0;
}

typedef struct { int32_t fd; uint32_t left; } card_src_t;

static uint32_t card_fill(void *ctx, uint8_t *buf, uint32_t cap) {
	card_src_t *c = (card_src_t *)ctx;
	if (cap > c->left) cap = c->left;
	if (!cap) return 0;
	int32_t r = zfs_read(c->fd, buf, cap);
	if (r <= 0) { c->left = 0; return 0; }
	c->left -= (uint32_t)r;
	return (uint32_t)r;
}

// The data section to `dst` (physical: a new process's memory), the
// bss zeroed after it. The reads come back in fs's replies and the
// kernel copies each straight to its place -- or, compressed, unpacks
// them there.
static int load_card(uint32_t dst, const char *path, const z_exec_info_t *info) {
	int32_t fd = zfs_open(path, Z_O_RDONLY | Z_O_NOFLASH);
	uint8_t *p = (uint8_t *)dst;
	if (fd < 0) return 1;
	if (info->data_off && zfs_lseek(fd, (int32_t)info->data_off, Z_SEEK_SET) < 0) {
		zfs_close(fd);
		return 1;
	}
	if (info->compressed) {
		card_src_t c = { fd, info->stored_size };
		int r = k_exec_unpack(p, info->data_size, card_fill, &c);
		zfs_close(fd);
		if (r) return 1;
		if (info->bss_size) memset(p + info->data_size, 0, info->bss_size);
		z_icache_flush();
		return 0;
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

	// /nfs/apps first, as exec_resolve() looks there first; then the card's.
	for (int pass = nfs_first() ? 0 : 1; pass < 2; pass++)
	if (zfs_opendir_at(&d, pass ? Z_DIR_APPS : Z_DIR_NFS Z_DIR_APPS, page, sizeof(page)) == 0) {
		while ((e = zfs_readdir(&d)) != NULL) {
			// the card's entries only: flash is asked below, directly
			// (everything on /nfs is read-only, and counts)
			if (e->info.type == Z_FS_TYPE_DIR || !e->info.size ||
				(pass && (e->info.attr & Z_FS_ATTR_RDONLY))) continue;
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
