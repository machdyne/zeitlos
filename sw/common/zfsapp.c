/*
 * Zeitlos
 * Copyright (c) 2025 Lone Dynamics Corporation. All rights reserved.
 *
 * App-facing filesystem access. See zfsapp.h for the design writeup.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "zeitlos.h"
#include "zfs.h"
#include "zfsapp.h"
#include "zfsc.h"

// -- the filesystem driver (docs/fs_driver.md) --
//
// Every file is `fs`'s (sw/apps/fs). The calls below reach it one of
// two ways:
//
//  - through the kernel's Z_SYS_FS_* calls, which make the request
//    from inside this app's syscall (sw/os/fsapi.c). The default: it
//    costs an app nothing in size, and the core apps in flash have no
//    room to spare (docs/flash_apps.md).
//  - straight to `fs`, when the app links zfsc.o and is built with
//    -DZFSAPP_DIRECT: one hop shorter, and
//    on gateware with the grant window, reads land in the app's buffer
//    with no copy at all. ipcbench does this.
//
// The direct path is compiled only with ZFSAPP_DIRECT (APP_CFLAGS), and
// its references are weak, so that zfsapp.o costs other apps nothing.
// Either way a descriptor is fs's plus FS_DRV_HANDLE, the same
// numbering the kernel uses, so a handle works with either path.
#define FS_DRV_HANDLE	0x4000
#define FS_LIST_BUF_SIZE_DRV	4096

#ifdef ZFSAPP_DIRECT
#pragma weak zfs_open
#pragma weak zfs_open_info
#pragma weak zfs_close
#pragma weak zfs_read
#pragma weak zfs_write
#pragma weak zfs_lseek
#pragma weak zfs_fstat
#pragma weak zfs_stat
#pragma weak zfs_unlink
#pragma weak zfs_mkdir
#pragma weak zfs_rename
#pragma weak zfs_fsync
#pragma weak zfs_ftruncate
#pragma weak zfs_opendir
#pragma weak zfs_readdir
#pragma weak zfs_closedir
#pragma weak zfs_dir_error


static bool drv_path(const char *p) {
	return zfs_open && p;
}

static bool drv_handle(int h) {
	return zfs_open && h >= FS_DRV_HANDLE;
}

static int drv_open(const char *path, uint32_t flags) {
	int32_t fd = zfs_open(path, flags);
	return fd < 0 ? -1 : fd + FS_DRV_HANDLE;
}

// The whole file, as fs_mallocfile()/fs_read_file() want it. *size is
// its length; a NULL `buf` with cap 0 asks only that.
static int drv_read_whole(const char *path, char **out, char *buf,
	int cap, int *size) {
	z_fs_info_t fi;
	int32_t fd = zfs_open_info(path, Z_O_RDONLY, &fi);	// one request
	*size = 0;
	if (fd < 0) return 0;
	*size = (int)fi.size;
	if (out) {
		buf = malloc(fi.size + 1);
		if (!buf) { zfs_close(fd); return 0; }
		buf[fi.size] = 0;
		*out = buf;
	} else if (!buf || (int)fi.size > cap) {
		zfs_close(fd);
		return buf ? 0 : 1;
	}
	uint32_t n = 0;
	while (n < fi.size) {			// zfs_read() may come back short
		int32_t r = zfs_read(fd, buf + n, fi.size - n);
		if (r <= 0) break;
		n += (uint32_t)r;
	}
	zfs_close(fd);
	if (n != fi.size) {
		if (out) { free(*out); *out = NULL; }
		return 0;
	}
	return 1;
}

// fs_list_into()/fs_list_ex() for a directory `fs` serves: the same
// packed "/path/name" strings, types and infos.
static int drv_list(const char *path, char *buf, uint32_t cap,
	uint8_t *types, z_fs_info_t *info, uint32_t max_entries,
	uint32_t *count, uint32_t *truncated) {
	zfs_dir_t *d = zfs_opendir(path);
	const z_fs_dirent_t *e;
	uint32_t n = 0, at = 0, pl = strlen(path);
	if (pl && path[pl - 1] == '/') pl--;
	if (!d) return 0;
	while ((e = zfs_readdir(d)) != NULL) {
		uint32_t need = pl + 1 + e->namelen + 1;
		if ((max_entries && n >= max_entries) || at + need > cap) {
			if (truncated) *truncated = 1;
			break;
		}
		memcpy(buf + at, path, pl);
		buf[at + pl] = '/';
		memcpy(buf + at + pl + 1, e->name, e->namelen + 1);
		at += need;
		if (types) types[n] = e->info.type;
		if (info) info[n] = e->info;
		n++;
	}
	int ok = zfs_dir_error(d) == 0;
	zfs_closedir(d);
	if (count) *count = n;
	return ok;
}

#else
// Without ZFSAPP_DIRECT every call goes through the kernel, and the
// direct path compiles away.
#define drv_path(p)		(false)
#define drv_handle(h)	(false)
#define drv_open(p, f)	(-1)
#define drv_read_whole(p, o, b, c, s)	(0)
#define drv_list(p, b, c, t, i, m, n, tr)	(0)
#endif

int fs_size(char *filename) {
	if (drv_path(filename)) {
		z_fs_info_t fi;
		return (zfs_stat(filename, &fi) == 0) ? (int)fi.size : 0;
	}

	if (!filename) return 0;

	z_fs_size_args_t args;
	args.name = filename;
	args.size = 0;

	z_kernel_ptr_t z_kernel_ptr = (z_kernel_ptr_t)(uintptr_t)(reg_kernel);
	z_obj_t *rv = (z_obj_t *)z_kernel_ptr(Z_SYS_FS_SIZE, (uint32_t *)&args, 0);
	if (rv->val.uint32 != Z_OK) return 0;

	return (int)args.size;

}

char *fs_mallocfile(char *filename) {
	if (drv_path(filename)) {
		char *out = NULL;
		int sz;
		if (!drv_read_whole(filename, &out, NULL, 0, &sz) || sz <= 0) {
			free(out);
			return NULL;
		}
		return out;
	}

	if (!filename) return NULL;

	// fs_size() returning 0 covers both "genuinely empty" and
	// "doesn't exist" -- same ambiguity te's own README.md already
	// accepts for this function's return value (NULL either way).
	// An empty-but-real file is a vanishingly rare thing to actually
	// open in an editor, so this isn't worth a second syscall just to
	// tell the two apart.
	int sz = fs_size(filename);
	if (sz <= 0) return NULL;

	char *buf = malloc((size_t)sz + 1);
	if (!buf) {
		// Measured: te TEST.TXT / RFC20.TXT failed with "unable to load"
		// while (free) reported 32 MB. That figure is the kernel
		// pool; this malloc is the process heap, which ends at sp.
		extern char _end;
		register char *sp asm("sp");
		char *brk = (char *)sbrk(0);
		printf("fs_mallocfile: malloc(%d) failed '%s' sbrk=%lu sp=%lu room=%ld grown=%lu\n",
			sz + 1, filename,
			(unsigned long)(uintptr_t)brk, (unsigned long)(uintptr_t)sp,
			(long)(sp - brk),
			(unsigned long)((uint32_t)(uintptr_t)brk - (uint32_t)&_end));
		return NULL;
	}
	buf[sz] = 0;

	z_fs_read_args_t args;
	args.name = filename;
	args.buf = buf;
	args.maxlen = (uint32_t)sz;
	args.len = 0;

	z_kernel_ptr_t z_kernel_ptr = (z_kernel_ptr_t)(uintptr_t)(reg_kernel);
	z_obj_t *rv = (z_obj_t *)z_kernel_ptr(Z_SYS_FS_READ, (uint32_t *)&args, 0);

	if (rv->val.uint32 != Z_OK || (int)args.len != sz) {
		printf("fs_mallocfile: READ fail '%s' sz=%d rv=%lu len=%lu buf=%p\n",
			filename, sz,
			(unsigned long)(rv ? rv->val.uint32 : 0xfffffffful),
			(unsigned long)args.len, (void *)buf);
		free(buf);
		return NULL;
	}

	return buf;

}

int fs_read_file(char *filename, char *buf, int maxlen) {
	if (drv_path(filename)) {
		int sz;
		if (!buf || maxlen < 0) return -1;
		if (!drv_read_whole(filename, NULL, buf, maxlen, &sz)) return -1;
		return sz;
	}

	if (!filename || !buf || maxlen < 0) return -1;

	int sz = fs_size(filename);
	if (sz <= 0) return 0;
	if (sz > maxlen) return -1;

	z_fs_read_args_t args;
	args.name = filename;
	args.buf = buf;
	args.maxlen = (uint32_t)sz;
	args.len = 0;

	z_kernel_ptr_t z_kernel_ptr = (z_kernel_ptr_t)(uintptr_t)(reg_kernel);
	z_obj_t *rv = (z_obj_t *)z_kernel_ptr(Z_SYS_FS_READ, (uint32_t *)&args, 0);

	if (rv->val.uint32 != Z_OK || (int)args.len != sz) return -1;

	return sz;

}

int fs_write_file(char *filename, char *buf, int len) {
	if (drv_path(filename)) {
		if (len < 0) return 0;
		int32_t fd = zfs_open(filename, Z_O_WRONLY | Z_O_CREAT | Z_O_TRUNC);
		if (fd < 0) return 0;
		int32_t n = zfs_write(fd, buf, (uint32_t)len);
		if (zfs_close(fd) < 0 || n < 0) return 0;
		return (int)n;
	}

	if (!filename || len < 0) return 0;

	z_fs_write_args_t args;
	args.name = filename;
	args.buf = buf;
	args.len = (uint32_t)len;
	args.written = 0;

	z_kernel_ptr_t z_kernel_ptr = (z_kernel_ptr_t)(uintptr_t)(reg_kernel);
	z_obj_t *rv = (z_obj_t *)z_kernel_ptr(Z_SYS_FS_WRITE, (uint32_t *)&args, 0);
	if (rv->val.uint32 != Z_OK) return 0;

	return (int)args.written;

}

int fs_unlink(char *filename) {
	if (drv_path(filename)) return zfs_unlink(filename) == 0;

	if (!filename) return 0;

	z_fs_unlink_args_t args;
	args.name = filename;

	z_kernel_ptr_t z_kernel_ptr = (z_kernel_ptr_t)(uintptr_t)(reg_kernel);
	z_obj_t *rv = (z_obj_t *)z_kernel_ptr(Z_SYS_FS_UNLINK, (uint32_t *)&args, 0);

	return (rv->val.uint32 == Z_OK) ? 1 : 0;

}

// default staging-buffer size for one fs_list() call -- bounds how
// many/how-long a single `ls` can return in one go, same "start
// conservative" spirit as te_bridge.c's own TE_MAX_FILE_SIZE
// (docs/editor.md). A root directory of ordinary Zeitlos app binaries
// (short 8.3 names, ~13 bytes worst case incl. the "/" prefix and
// NUL) fits many dozens of entries in this comfortably; a much bigger
// directory just comes back truncated (fs_list()'s own `*count` still
// reports exactly how many actually fit) rather than growing this
// buffer without bound.
#define FS_LIST_BUF_SIZE 4096

char **fs_list(const char *path, uint32_t max_entries, uint32_t *count) {
	if (drv_path(path)) {
		uint32_t n = 0;
		char *buf = malloc(FS_LIST_BUF_SIZE_DRV);
		char **names;
		if (count) *count = 0;
		if (!buf) return NULL;
		if (!drv_list(path, buf, FS_LIST_BUF_SIZE_DRV, NULL, NULL,
			max_entries, &n, NULL) || n == 0 ||
			!(names = malloc(sizeof(char *) * n))) {
			free(buf);
			return NULL;
		}
		char *p = buf;
		for (uint32_t i = 0; i < n; i++) {
			size_t len = strlen(p);
			names[i] = malloc(len + 1);
			if (!names[i]) {
				while (i--) free(names[i]);
				free(names);
				free(buf);
				return NULL;
			}
			memcpy(names[i], p, len + 1);
			p += len + 1;
		}
		free(buf);
		if (count) *count = n;
		return names;
	}

	if (count) *count = 0;

	char *buf = malloc(FS_LIST_BUF_SIZE);
	if (!buf) return NULL;

	z_fs_list_args_t args;
	args.path = (char *)path;
	args.out = buf;
	args.out_cap = FS_LIST_BUF_SIZE;
	args.max_entries = max_entries;
	args.count = 0;
	args.truncated = 0;
	// This function's callers only ever wanted names. Explicitly
	// NULL, not left uninitialized -- the kernel writes through this
	// pointer when it isn't NULL, and an uninitialized one here would
	// be a stack-garbage address scribbled on by a syscall, which is
	// about as unpleasant a bug as this codebase can produce.
	args.types = NULL;

	z_kernel_ptr_t z_kernel_ptr = (z_kernel_ptr_t)(uintptr_t)(reg_kernel);
	z_obj_t *rv = (z_obj_t *)z_kernel_ptr(Z_SYS_FS_LIST, (uint32_t *)&args, 0);

	if (rv->val.uint32 != Z_OK || args.count == 0) {
		free(buf);
		return NULL;
	}

	char **names = malloc(sizeof(char *) * args.count);
	if (!names) {
		free(buf);
		return NULL;
	}

	char *p = buf;
	uint32_t i;
	for (i = 0; i < args.count; i++) {
		size_t len = strlen(p);
		char *s = malloc(len + 1);
		if (!s) {
			// bail out cleanly -- free everything already built
			// rather than leak or return a partially-filled array
			uint32_t j;
			for (j = 0; j < i; j++) free(names[j]);
			free(names);
			free(buf);
			return NULL;
		}
		memcpy(s, p, len + 1);
		names[i] = s;
		p += len + 1;
	}

	free(buf);
	if (count) *count = args.count;
	return names;

}

// Allocation-free sibling of fs_list() above -- see zfsapp.h for when
// to prefer which.
int fs_list_into(const char *path, char *buf, uint32_t buf_cap,
	uint8_t *types, uint32_t max_entries, uint32_t *count,
	uint32_t *truncated) {
	if (drv_path(path)) {
		if (count) *count = 0;
		if (truncated) *truncated = 0;
		if (!buf || buf_cap == 0 || (types && !max_entries)) return 0;
		return drv_list(path, buf, buf_cap, types, NULL, max_entries,
			count, truncated);
	}

	if (count) *count = 0;
	if (truncated) *truncated = 0;

	if (!buf || buf_cap == 0) return 0;
	if (types && !max_entries) return 0;	// see zfs.h / k_fs_list()

	z_fs_list_args_t args;
	args.path = (char *)path;
	args.out = buf;
	args.out_cap = buf_cap;
	args.max_entries = max_entries;
	args.count = 0;
	args.truncated = 0;
	args.types = types;

	z_kernel_ptr_t z_kernel_ptr = (z_kernel_ptr_t)(uintptr_t)(reg_kernel);
	z_obj_t *rv = (z_obj_t *)z_kernel_ptr(Z_SYS_FS_LIST, (uint32_t *)&args, 0);

	if (rv->val.uint32 != Z_OK) return 0;

	if (count) *count = args.count;
	if (truncated) *truncated = args.truncated;

	return 1;

}

int fs_open_write(const char *filename) {
	if (drv_path(filename))
		return drv_open(filename, Z_O_WRONLY | Z_O_CREAT | Z_O_TRUNC);

	if (!filename) return -1;

	z_fs_open_args_t args;
	args.name = (char *)filename;
	args.handle = -1;

	z_kernel_ptr_t z_kernel_ptr = (z_kernel_ptr_t)(uintptr_t)(reg_kernel);
	z_obj_t *rv = (z_obj_t *)z_kernel_ptr(Z_SYS_FS_OPEN_WRITE, (uint32_t *)&args, 0);
	if (rv->val.uint32 != Z_OK) return -1;

	return args.handle;

}

int fs_open_read(const char *filename) {
	if (drv_path(filename)) return drv_open(filename, Z_O_RDONLY);

	if (!filename) return -1;

	z_fs_open_args_t args;
	args.name = (char *)filename;
	args.handle = -1;

	z_kernel_ptr_t z_kernel_ptr = (z_kernel_ptr_t)(uintptr_t)(reg_kernel);
	z_obj_t *rv = (z_obj_t *)z_kernel_ptr(Z_SYS_FS_OPEN_READ, (uint32_t *)&args, 0);
	if (rv->val.uint32 != Z_OK) return -1;

	return args.handle;

}

int fs_read_chunk(int handle, void *buf, int maxlen) {
	if (drv_handle(handle)) {
		if (!buf || maxlen <= 0) return -1;
		int32_t n = zfs_read(handle - FS_DRV_HANDLE, buf, (uint32_t)maxlen);
		return n < 0 ? -1 : (int)n;
	}

	if (handle < 0 || !buf || maxlen <= 0) return -1;

	z_fs_read_chunk_args_t args;
	args.handle = handle;
	args.buf = buf;
	args.maxlen = (uint32_t)maxlen;
	args.len = 0;

	z_kernel_ptr_t z_kernel_ptr = (z_kernel_ptr_t)(uintptr_t)(reg_kernel);
	z_obj_t *rv = (z_obj_t *)z_kernel_ptr(Z_SYS_FS_READ_CHUNK, (uint32_t *)&args, 0);
	if (rv->val.uint32 != Z_OK) return -1;

	return (int)args.len;

}

int fs_write_chunk(int handle, const void *buf, int len) {
	if (drv_handle(handle)) {
		if ((!buf && len > 0) || len < 0) return -1;
		int32_t n = zfs_write(handle - FS_DRV_HANDLE, buf, (uint32_t)len);
		return n < 0 ? -1 : (int)n;
	}

	if (handle < 0 || (!buf && len > 0) || len < 0) return -1;

	z_fs_write_chunk_args_t args;
	args.handle = handle;
	args.buf = (void *)buf;
	args.len = (uint32_t)len;
	args.written = 0;

	z_kernel_ptr_t z_kernel_ptr = (z_kernel_ptr_t)(uintptr_t)(reg_kernel);
	z_obj_t *rv = (z_obj_t *)z_kernel_ptr(Z_SYS_FS_WRITE_CHUNK, (uint32_t *)&args, 0);
	if (rv->val.uint32 != Z_OK) return -1;

	return (int)args.written;

}

int fs_close_handle(int handle) {
	if (drv_handle(handle)) return zfs_close(handle - FS_DRV_HANDLE) == 0;

	if (handle < 0) return 0;

	z_fs_close_args_t args;
	args.handle = handle;

	z_kernel_ptr_t z_kernel_ptr = (z_kernel_ptr_t)(uintptr_t)(reg_kernel);
	z_obj_t *rv = (z_obj_t *)z_kernel_ptr(Z_SYS_FS_CLOSE, (uint32_t *)&args, 0);

	return (rv->val.uint32 == Z_OK) ? 1 : 0;

}

// -- mkdir / touch / seek --
//
// Same shape as fs_unlink() above: build the small arg struct, one
// syscall, translate the kernel's z_ok/z_fail into this file's own
// established 1-on-success/0-on-failure convention (zfsapp.h) -- note
// that's the INVERSE of what the kernel-native fs_mkdir()/fs_touch()
// (sw/apps/fs/fs.c) themselves return, which is exactly why the
// translation happens here rather than being left to every caller.

int fs_mkdir(const char *path) {
	if (drv_path(path)) return zfs_mkdir(path) == 0;

	if (!path) return 0;

	z_fs_path_args_t args;
	args.name = (char *)path;

	z_kernel_ptr_t z_kernel_ptr = (z_kernel_ptr_t)(uintptr_t)(reg_kernel);
	z_obj_t *rv = (z_obj_t *)z_kernel_ptr(Z_SYS_FS_MKDIR, (uint32_t *)&args, 0);

	return (rv->val.uint32 == Z_OK) ? 1 : 0;

}

int fs_touch(const char *path) {
	if (drv_path(path)) {
		int32_t fd = zfs_open(path, Z_O_WRONLY | Z_O_CREAT);
		if (fd < 0) return 0;
		return zfs_close(fd) == 0;
	}

	if (!path) return 0;

	z_fs_path_args_t args;
	args.name = (char *)path;

	z_kernel_ptr_t z_kernel_ptr = (z_kernel_ptr_t)(uintptr_t)(reg_kernel);
	z_obj_t *rv = (z_obj_t *)z_kernel_ptr(Z_SYS_FS_TOUCH, (uint32_t *)&args, 0);

	return (rv->val.uint32 == Z_OK) ? 1 : 0;

}

int fs_seek(int handle, uint32_t offset) {
	if (drv_handle(handle)) {
		if (offset > 0x7fffffffu) return 0;
		return zfs_lseek(handle - FS_DRV_HANDLE, (int32_t)offset, Z_SEEK_SET) >= 0;
	}

	if (handle < 0) return 0;

	z_fs_seek_args_t args;
	args.handle = (int32_t)handle;
	args.offset = offset;
	args.pos = 0;

	z_kernel_ptr_t z_kernel_ptr = (z_kernel_ptr_t)(uintptr_t)(reg_kernel);
	z_obj_t *rv = (z_obj_t *)z_kernel_ptr(Z_SYS_FS_SEEK, (uint32_t *)&args, 0);

	return (rv->val.uint32 == Z_OK) ? 1 : 0;

}

// -- in-place modification --
//
// Same shape as everything above; see zfsapp.h for what each one
// promises and sw/common/zfs.h for the kernel-side design.
//
// One thing worth knowing at every call site: these three are the
// NEWEST syscalls in this tree, so they are the first that an app can
// find missing. A kernel built before them has no entry at their id,
// and k_syscall_entry()'s bounds/NULL check returns z_fail rather than
// jumping somewhere -- so fs_open_rw() on an old kernel returns -1,
// which is indistinguishable from "no such file". An app that wants to
// tell the user WHICH it was has to check the file exists first (see
// sw/apps/hex's open path, which does exactly that).

int fs_open_rw(const char *filename) {
	if (drv_path(filename)) return drv_open(filename, Z_O_RDWR);

	if (!filename) return -1;

	z_fs_open_args_t args;
	args.name = (char *)filename;
	args.handle = -1;

	z_kernel_ptr_t z_kernel_ptr = (z_kernel_ptr_t)(uintptr_t)(reg_kernel);
	z_obj_t *rv = (z_obj_t *)z_kernel_ptr(Z_SYS_FS_OPEN_RW, (uint32_t *)&args, 0);
	if (rv->val.uint32 != Z_OK) return -1;

	return args.handle;

}

int fs_sync(int handle) {
	if (drv_handle(handle)) return zfs_fsync(handle - FS_DRV_HANDLE) == 0;

	if (handle < 0) return 0;

	z_fs_close_args_t args;
	args.handle = (int32_t)handle;

	z_kernel_ptr_t z_kernel_ptr = (z_kernel_ptr_t)(uintptr_t)(reg_kernel);
	z_obj_t *rv = (z_obj_t *)z_kernel_ptr(Z_SYS_FS_SYNC, (uint32_t *)&args, 0);

	return (rv->val.uint32 == Z_OK) ? 1 : 0;

}

int fs_truncate(int handle, uint32_t size) {
	if (drv_handle(handle)) return zfs_ftruncate(handle - FS_DRV_HANDLE, size) == 0;

	if (handle < 0) return 0;

	z_fs_truncate_args_t args;
	args.handle = (int32_t)handle;
	args.size = size;
	args.result = 0;

	z_kernel_ptr_t z_kernel_ptr = (z_kernel_ptr_t)(uintptr_t)(reg_kernel);
	z_obj_t *rv = (z_obj_t *)z_kernel_ptr(Z_SYS_FS_TRUNCATE, (uint32_t *)&args, 0);

	return (rv->val.uint32 == Z_OK) ? 1 : 0;

}

// Named fs_df(), NOT fs_free() -- the kernel-native fs_free()
// (sw/apps/fs/fs.c) already owns that name, and sw/common/zeitlos.h's own
// note explains what happens when an app-facing wrapper collides with a
// kernel-native one of the same name: it fails at kernel-compile time,
// since kernel code includes both headers. Same reason this whole file
// exists separately from zeitlos.h.
bool fs_df(uint32_t *total_kb, uint32_t *free_kb) {

	z_fs_df_args_t args;
	args.total_kb = 0;
	args.free_kb = 0;

	z_kernel_ptr_t z_kernel_ptr = (z_kernel_ptr_t)(uintptr_t)(reg_kernel);
	z_obj_t *rv = (z_obj_t *)z_kernel_ptr(Z_SYS_FS_DF, (uint32_t *)&args, 0);

	if (total_kb) *total_kb = args.total_kb;
	if (free_kb) *free_kb = args.free_kb;

	return (rv->val.uint32 == Z_OK);

}

// -- rename, stat, extended listing, copy -- see zfsapp.h --

int fs_rename(const char *from, const char *to, int *err) {
	if (drv_path(from) && drv_path(to)) {
		int32_t r = zfs_rename(from, to);
		if (err) *err = r == 0 ? Z_FS_ERR_NONE :
			r == -Z_ENOENT ? Z_FS_ERR_NOENT :
			r == -Z_EEXIST ? Z_FS_ERR_EXIST :
			r == -Z_EXDEV ? Z_FS_ERR_XDEV :
			r == -Z_EBUSY ? Z_FS_ERR_BUSY :
			r == -Z_EINVAL ? Z_FS_ERR_INVAL : Z_FS_ERR_IO;
		return r == 0;
	}

	z_fs_rename_args_t args;

	if (err) *err = Z_FS_ERR_INVAL;
	if (!from || !to) return 0;

	args.from = (char *)from;
	args.to = (char *)to;
	args.err = Z_FS_ERR_IO;

	z_kernel_ptr_t z_kernel_ptr = (z_kernel_ptr_t)(uintptr_t)(reg_kernel);
	z_obj_t *rv = (z_obj_t *)z_kernel_ptr(Z_SYS_FS_RENAME, (uint32_t *)&args, 0);

	if (err) *err = args.err;
	return (rv->val.uint32 == Z_OK) ? 1 : 0;

}

int fs_stat(const char *path, z_fs_info_t *info) {
	if (drv_path(path)) return info && zfs_stat(path, info) == 0;

	z_fs_stat_args_t args;

	if (!path || !info) return 0;
	args.name = (char *)path;
	memset(&args.info, 0, sizeof(args.info));

	z_kernel_ptr_t z_kernel_ptr = (z_kernel_ptr_t)(uintptr_t)(reg_kernel);
	z_obj_t *rv = (z_obj_t *)z_kernel_ptr(Z_SYS_FS_STAT, (uint32_t *)&args, 0);

	if (rv->val.uint32 != Z_OK) return 0;
	*info = args.info;
	return 1;

}

int fs_list_ex(const char *path, char *buf, uint32_t buf_cap,
	z_fs_info_t *info, uint32_t max_entries, uint32_t *count,
	uint32_t *truncated) {
	if (drv_path(path)) {
		if (count) *count = 0;
		if (truncated) *truncated = 0;
		if (!buf || buf_cap == 0 || (info && !max_entries)) return 0;
		return drv_list(path, buf, buf_cap, NULL, info, max_entries,
			count, truncated);
	}

	z_fs_list_ex_args_t args;

	if (count) *count = 0;
	if (truncated) *truncated = 0;
	if (!buf || buf_cap == 0) return 0;
	if (info && !max_entries) return 0;	// see zfs.h

	args.list.path = (char *)path;
	args.list.out = buf;
	args.list.out_cap = buf_cap;
	args.list.max_entries = max_entries;
	args.list.count = 0;
	args.list.truncated = 0;
	args.list.types = NULL;
	args.info = info;

	z_kernel_ptr_t z_kernel_ptr = (z_kernel_ptr_t)(uintptr_t)(reg_kernel);
	z_obj_t *rv = (z_obj_t *)z_kernel_ptr(Z_SYS_FS_LIST_EX, (uint32_t *)&args, 0);

	if (rv->val.uint32 != Z_OK) return 0;
	if (count) *count = args.list.count;
	if (truncated) *truncated = args.list.truncated;
	return 1;

}

// fs_copy_file(), fs_move() and fs_strerror() are in zfsutil.c: they
// use only the calls above, so the posix host tests link them against
// their own stubs and test the shipped code.
