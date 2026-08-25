/*
 * Zeitlos
 * Copyright (c) 2025 Lone Dynamics Corporation. All rights reserved.
 *
 * App-facing filesystem access. See zfsapp.h for the design writeup.
 */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "zeitlos.h"
#include "zfs.h"
#include "zfsapp.h"

int fs_size(char *filename) {

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

	if (!filename) return NULL;

	// fs_size() returning 0 covers both "genuinely empty" and
	// "doesn't exist" -- same ambiguity te's own README.md already
	// accepts for this function's return value (NULL either way).
	// An empty-but-real file is a vanishingly rare thing to actually
	// open in an editor, so this isn't worth a second syscall just to
	// tell the two apart.
	int sz = fs_size(filename);
	if (sz <= 0) return NULL;

	char *buf = malloc((size_t)sz);
	if (!buf) return NULL;

	z_fs_read_args_t args;
	args.name = filename;
	args.buf = buf;
	args.maxlen = (uint32_t)sz;
	args.len = 0;

	z_kernel_ptr_t z_kernel_ptr = (z_kernel_ptr_t)(uintptr_t)(reg_kernel);
	z_obj_t *rv = (z_obj_t *)z_kernel_ptr(Z_SYS_FS_READ, (uint32_t *)&args, 0);

	if (rv->val.uint32 != Z_OK || (int)args.len != sz) {
		free(buf);
		return NULL;
	}

	return buf;

}

int fs_write_file(char *filename, char *buf, int len) {

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

int fs_open_write(const char *filename) {

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
// (sw/os/fs/fs.c) themselves return, which is exactly why the
// translation happens here rather than being left to every caller.

int fs_mkdir(const char *path) {

	if (!path) return 0;

	z_fs_path_args_t args;
	args.name = (char *)path;

	z_kernel_ptr_t z_kernel_ptr = (z_kernel_ptr_t)(uintptr_t)(reg_kernel);
	z_obj_t *rv = (z_obj_t *)z_kernel_ptr(Z_SYS_FS_MKDIR, (uint32_t *)&args, 0);

	return (rv->val.uint32 == Z_OK) ? 1 : 0;

}

int fs_touch(const char *path) {

	if (!path) return 0;

	z_fs_path_args_t args;
	args.name = (char *)path;

	z_kernel_ptr_t z_kernel_ptr = (z_kernel_ptr_t)(uintptr_t)(reg_kernel);
	z_obj_t *rv = (z_obj_t *)z_kernel_ptr(Z_SYS_FS_TOUCH, (uint32_t *)&args, 0);

	return (rv->val.uint32 == Z_OK) ? 1 : 0;

}

int fs_seek(int handle, uint32_t offset) {

	if (handle < 0) return 0;

	z_fs_seek_args_t args;
	args.handle = (int32_t)handle;
	args.offset = offset;
	args.pos = 0;

	z_kernel_ptr_t z_kernel_ptr = (z_kernel_ptr_t)(uintptr_t)(reg_kernel);
	z_obj_t *rv = (z_obj_t *)z_kernel_ptr(Z_SYS_FS_SEEK, (uint32_t *)&args, 0);

	return (rv->val.uint32 == Z_OK) ? 1 : 0;

}

// Named fs_df(), NOT fs_free() -- the kernel-native fs_free()
// (sw/os/fs/fs.c) already owns that name, and sw/common/zeitlos.h's own
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
