/*
 * Zeitlos OS
 * Copyright (c) 2025 Lone Dynamics Corporation. All rights reserved.
 *
 * The filesystem syscalls (Z_SYS_FS_*), since the filesystem left the
 * kernel (docs/fs_driver.md): each one is a request to `fs`, made from
 * inside the calling app's syscall, as that app (kfs.c). The app waits,
 * blocked, as it would in z_msg_recv().
 *
 * They are kept so that programs which call them directly still work:
 * zcc's runtime (sw/apps/zcc/libz), binaries built before zfsapp spoke
 * to `fs` itself. zfsapp no longer comes here -- it sends its requests
 * to `fs` directly, one hop shorter. The argument structs and their
 * rules are sw/common/zfs.h's, unchanged.
 *
 * Handles are fs's descriptors plus K_FSH_BASE, the same offset zfsapp
 * uses, so a handle never collides with the small integers the kernel's
 * own table used to hand out.
 */

#include <stdint.h>
#include <stddef.h>
#include <string.h>

#include "kernel.h"
#include "fsapi.h"
#include "kfs.h"
#include "../common/zfsc.h"

#define K_FSH_BASE	0x4000

static int32_t fd_of(int32_t handle) {
	return handle >= K_FSH_BASE ? handle - K_FSH_BASE : -1;
}

static z_obj_t *rv(bool ok) { return ok ? (&z_ok) : (&z_fail); }

z_obj_t *k_exec_exists(z_obj_t *args) {
	if (!args || args->type != Z_STR || !args->val.str) {
		if (args) { args->type = Z_UINT32; args->val.uint32 = 0; }
		return (&z_fail);
	}
	z_exec_info_t xi;
	uint32_t total = fs_exec_info_any(args->val.str, &xi) ? 0 : xi.total;
	args->type = Z_UINT32;
	args->val.uint32 = total;
	return rv(total != 0);
}

z_obj_t *k_exec_exists_many(z_obj_t *args) {
	z_exec_many_args_t *a = (z_exec_many_args_t *)args;
	if (!a->names || !a->found || a->count == 0 || a->count > 256)
		return (&z_fail);
	if (!k_user_ok(a->found, a->count)) return (&z_fail);
	fs_exec_exists_many(a->names, a->count, a->found);
	return (&z_ok);
}

z_obj_t *k_fs_size(z_obj_t *args) {
	z_fs_size_args_t *a = (z_fs_size_args_t *)args;
	if (!a || !a->name) { if (a) a->size = 0; return (&z_fail); }
	a->size = fs_size(a->name);		// 0 for "not found", which is not an error
	return (&z_ok);
}

z_obj_t *k_fs_read(z_obj_t *args) {
	z_fs_read_args_t *a = (z_fs_read_args_t *)args;
	if (!a || !a->name || !a->buf || a->maxlen == 0) { if (a) a->len = 0; return (&z_fail); }
	a->len = 0;
	if (!k_user_ok(a->buf, a->maxlen)) return (&z_fail);
	// the whole file or nothing (zfs.h)
	int32_t n = kfs_read_file(a->name, a->buf, a->maxlen);
	if (n < 0) return (&z_fail);
	a->len = (uint32_t)n;
	return (&z_ok);
}

z_obj_t *k_fs_write(z_obj_t *args) {
	z_fs_write_args_t *a = (z_fs_write_args_t *)args;
	if (!a || !a->name || (!a->buf && a->len > 0)) { if (a) a->written = 0; return (&z_fail); }
	a->written = 0;
	int32_t fd = zfs_open(a->name, Z_O_WRONLY | Z_O_CREAT | Z_O_TRUNC);
	if (fd < 0) return (&z_fail);
	int32_t n = a->len ? zfs_write(fd, a->buf, a->len) : 0;
	if (zfs_close(fd) < 0 || n < 0) return (&z_fail);
	a->written = (uint32_t)n;
	return (&z_ok);
}

z_obj_t *k_fs_unlink(z_obj_t *args) {
	z_fs_unlink_args_t *a = (z_fs_unlink_args_t *)args;
	if (!a || !a->name) return (&z_fail);
	return rv(zfs_unlink(a->name) == 0);
}

// `fs` sweeps a dead client's descriptors itself (sw/apps/fs).
void k_fs_release_all(uint32_t pid) { (void)pid; }
int k_fs_open_count(void) { return 0; }

static z_obj_t *open_as(z_fs_open_args_t *a, uint32_t flags) {
	if (a) a->handle = -1;
	if (!a || !a->name) return (&z_fail);
	int32_t fd = zfs_open(a->name, flags);
	if (fd < 0) return (&z_fail);
	a->handle = fd + K_FSH_BASE;
	return (&z_ok);
}

z_obj_t *k_fs_open_write(z_obj_t *args) {
	return open_as((z_fs_open_args_t *)args, Z_O_WRONLY | Z_O_CREAT | Z_O_TRUNC);
}
z_obj_t *k_fs_open_read(z_obj_t *args) {
	return open_as((z_fs_open_args_t *)args, Z_O_RDONLY);
}
// an EXISTING file, read and write, truncating nothing (zfs.h)
z_obj_t *k_fs_open_rw(z_obj_t *args) {
	return open_as((z_fs_open_args_t *)args, Z_O_RDWR);
}

z_obj_t *k_fs_read_chunk(z_obj_t *args) {
	z_fs_read_chunk_args_t *a = (z_fs_read_chunk_args_t *)args;
	if (a) a->len = 0;
	if (!a || !a->buf || a->maxlen == 0 || fd_of(a->handle) < 0) return (&z_fail);
	if (!k_user_ok(a->buf, a->maxlen)) return (&z_fail);
	int32_t n = zfs_read(fd_of(a->handle), a->buf, a->maxlen);
	if (n < 0) return (&z_fail);
	a->len = (uint32_t)n;			// 0 is the end of the file, not an error
	return (&z_ok);
}

z_obj_t *k_fs_write_chunk(z_obj_t *args) {
	z_fs_write_chunk_args_t *a = (z_fs_write_chunk_args_t *)args;
	if (a) a->written = 0;
	if (!a || (!a->buf && a->len > 0) || fd_of(a->handle) < 0) return (&z_fail);
	int32_t n = a->len ? zfs_write(fd_of(a->handle), a->buf, a->len) : 0;
	if (n < 0) return (&z_fail);
	a->written = (uint32_t)n;
	return (&z_ok);
}

z_obj_t *k_fs_close(z_obj_t *args) {
	z_fs_close_args_t *a = (z_fs_close_args_t *)args;
	if (!a || fd_of(a->handle) < 0) return (&z_fail);
	return rv(zfs_close(fd_of(a->handle)) == 0);
}

z_obj_t *k_fs_sync(z_obj_t *args) {
	z_fs_close_args_t *a = (z_fs_close_args_t *)args;
	if (!a || fd_of(a->handle) < 0) return (&z_fail);
	return rv(zfs_fsync(fd_of(a->handle)) == 0);
}

z_obj_t *k_fs_seek(z_obj_t *args) {
	z_fs_seek_args_t *a = (z_fs_seek_args_t *)args;
	if (a) a->pos = 0;
	if (!a || fd_of(a->handle) < 0 || a->offset > 0x7fffffffu) return (&z_fail);
	int32_t p = zfs_lseek(fd_of(a->handle), (int32_t)a->offset, Z_SEEK_SET);
	if (p < 0) return (&z_fail);
	a->pos = (uint32_t)p;
	return (&z_ok);
}

z_obj_t *k_fs_truncate(z_obj_t *args) {
	z_fs_truncate_args_t *a = (z_fs_truncate_args_t *)args;
	z_fs_info_t fi;
	if (a) a->result = 0;
	if (!a || fd_of(a->handle) < 0) return (&z_fail);
	if (zfs_ftruncate(fd_of(a->handle), a->size) != 0) return (&z_fail);
	// left at the new end of the file, as it always was
	zfs_lseek(fd_of(a->handle), 0, Z_SEEK_END);
	if (zfs_fstat(fd_of(a->handle), &fi) != 0) return (&z_fail);
	a->result = fi.size;
	return (&z_ok);
}

z_obj_t *k_fs_mkdir(z_obj_t *args) {
	z_fs_path_args_t *a = (z_fs_path_args_t *)args;
	if (!a || !a->name) return (&z_fail);
	return rv(zfs_mkdir(a->name) == 0);
}

z_obj_t *k_fs_touch(z_obj_t *args) {
	z_fs_path_args_t *a = (z_fs_path_args_t *)args;
	if (!a || !a->name) return (&z_fail);
	int32_t fd = zfs_open(a->name, Z_O_WRONLY | Z_O_CREAT | Z_O_TRUNC);
	if (fd < 0) return (&z_fail);
	return rv(zfs_close(fd) == 0);
}

z_obj_t *k_fs_rename(z_obj_t *args) {
	z_fs_rename_args_t *a = (z_fs_rename_args_t *)args;
	if (!a) return (&z_fail);
	a->err = fs_rename(a->from, a->to);
	return rv(a->err == Z_FS_ERR_NONE);
}

z_obj_t *k_fs_stat(z_obj_t *args) {
	z_fs_stat_args_t *a = (z_fs_stat_args_t *)args;
	if (!a || !a->name) return (&z_fail);
	return rv(fs_stat_info(a->name, &a->info));
}

z_obj_t *k_fs_df(z_obj_t *args) {
	z_fs_df_args_t *a = (z_fs_df_args_t *)args;
	if (!a) return (&z_fail);
	fs_df_kb(&a->total_kb, &a->free_kb);
	return (&z_ok);
}

// FS_LIST and FS_LIST_EX: "prefix/name" strings back to back in the
// caller's buffer, with types or infos alongside (zfs.h).
static z_obj_t *list_core(z_fs_list_args_t *a, z_fs_info_t *info) {
	zfs_dir_t d;
	uint32_t page[128];
	const z_fs_dirent_t *e;
	char prefix[64];

	if (!a || !a->out || a->out_cap == 0) {
		if (a) { a->count = 0; a->truncated = 0; }
		return (&z_fail);
	}
	a->count = 0;
	a->truncated = 0;
	if (!k_user_ok(a->out, a->out_cap) ||
		(a->types && !k_user_ok(a->types, a->max_entries)))
		return (&z_fail);
	if (a->types && !a->max_entries) return (&z_fail);

	const char *path = (a->path && a->path[0]) ? a->path : "/";
	size_t pl = strlen(path);
	if (pl >= sizeof(prefix) - 2) pl = sizeof(prefix) - 2;
	memcpy(prefix, path, pl);
	if (pl == 0 || prefix[pl - 1] != '/') prefix[pl++] = '/';
	prefix[pl] = 0;

	if (zfs_opendir_at(&d, path, page, sizeof(page)) < 0) return (&z_fail);
	uint32_t max = a->max_entries ? a->max_entries : 0xffffffffu;
	uint32_t written = 0, count = 0;
	while (count < max && (e = zfs_readdir(&d)) != NULL) {
		uint32_t need = (uint32_t)pl + e->namelen + 1;
		if (written + need > a->out_cap) { a->truncated = 1; break; }
		memcpy(a->out + written, prefix, pl);
		memcpy(a->out + written + pl, e->name, e->namelen + 1);
		written += need;
		if (a->types) a->types[count] = e->info.type;
		if (info) info[count] = e->info;
		count++;
	}
	if (count == max && !d.end) a->truncated = 1;
	zfs_closedir_at(&d);
	a->count = count;
	return (&z_ok);
}

z_obj_t *k_fs_list(z_obj_t *args) {
	return list_core((z_fs_list_args_t *)args, NULL);
}

z_obj_t *k_fs_list_ex(z_obj_t *args) {
	z_fs_list_ex_args_t *x = (z_fs_list_ex_args_t *)args;
	if (!x) return (&z_fail);
	if (x->info && (!x->list.max_entries || x->list.max_entries > 4096 ||
		!k_user_ok(x->info, x->list.max_entries * sizeof(z_fs_info_t)))) {
		x->list.count = 0;
		x->list.truncated = 0;
		return (&z_fail);
	}
	return list_core(&x->list, x->info);
}
