/*
 * Zeitlos OS
 * Copyright (c) 2025 Lone Dynamics Corporation. All rights reserved.
 *
 * Filesystem syscalls. See fsapi.h for the full design writeup.
 */

#include <stdint.h>
#include <stddef.h>
#include <string.h>

#include "kernel.h"
#include "fsapi.h"
#include "fs/fs.h"

z_obj_t *k_fs_size(z_obj_t *args) {

	z_fs_size_args_t *a = (z_fs_size_args_t *)args;

	if (!a || !a->name) {
		if (a) a->size = 0;
		return (&z_fail);
	}

	// fs_size() (sw/os/fs/fs.c) itself already returns 0 for "not
	// found" -- treated as success here (see fsapi.h's own comment):
	// a missing file isn't an error for any of this syscall's actual
	// callers, it's the normal "this will be a new file" case.
	a->size = fs_size(a->name);

	return (&z_ok);

}

z_obj_t *k_fs_read(z_obj_t *args) {

	z_fs_read_args_t *a = (z_fs_read_args_t *)args;

	if (!a || !a->name || !a->buf || a->maxlen == 0) {
		if (a) a->len = 0;
		return (&z_fail);
	}

	a->len = 0;

	FIL f;
	FRESULT res = f_open(&f, a->name, FA_READ | FA_OPEN_EXISTING);
	if (res != FR_OK) return (&z_fail);

	FSIZE_t sz = f_size(&f);
	if (sz > a->maxlen) {
		// caller's buffer is smaller than the file -- refuse rather
		// than silently truncating; matches fs_mallocfile()'s own
		// "load the whole thing or don't" contract (te's own
		// README.md's own note on this: "fs_mallocfile() is expected
		// to load the WHOLE file into one buffer" -- a short read
		// here would otherwise look like success with corrupt/
		// incomplete content).
		f_close(&f);
		return (&z_fail);
	}

	UINT br = 0;
	res = f_read(&f, a->buf, (UINT)sz, &br);
	f_close(&f);

	if (res != FR_OK) return (&z_fail);

	a->len = (uint32_t)br;
	return (&z_ok);

}

z_obj_t *k_fs_write(z_obj_t *args) {

	z_fs_write_args_t *a = (z_fs_write_args_t *)args;

	if (!a || !a->name || (!a->buf && a->len > 0)) {
		if (a) a->written = 0;
		return (&z_fail);
	}

	a->written = 0;

	FIL f;
	FRESULT res = f_open(&f, a->name, FA_WRITE | FA_CREATE_ALWAYS);
	if (res != FR_OK) return (&z_fail);

	UINT bw = 0;
	res = f_write(&f, a->buf, (UINT)a->len, &bw);
	FRESULT cres = f_close(&f);

	if (res != FR_OK || cres != FR_OK) return (&z_fail);

	a->written = (uint32_t)bw;
	return (&z_ok);

}

z_obj_t *k_fs_unlink(z_obj_t *args) {

	z_fs_unlink_args_t *a = (z_fs_unlink_args_t *)args;

	if (!a || !a->name) return (&z_fail);

	// fs_unlink() (sw/os/fs/fs.c) itself returns 0 on SUCCESS, 1 on
	// failure -- inverted from fs_size()/fs_write_file()'s own "0 =
	// failure" convention, easy to get backwards, worth calling out
	// explicitly right here rather than trusting the reader to
	// remember it. Reused as-is (rather than calling f_unlink()
	// directly) purely for the one-function-does-it precedent every
	// other kernel-side fs.c caller (sh.c) already follows; its own
	// unconditional printf() (harmless -- this file's own header
	// comment already accepts printf() from kernel-compiled code,
	// unlike the snprintf()/malloc() concerns raised there) just
	// means every app-triggered delete-file shows up on the serial
	// console too, same visibility every other fs.c operation
	// already has.
	if (fs_unlink(a->name) != 0) return (&z_fail);

	return (&z_ok);

}

// see zfs.h's own comment for the full design writeup on why chunked
// I/O needs a kernel-side handle table at all.
static struct {
	bool		used;
	uint32_t	owner_pid;
	FIL		fil;
} z_fs_handles[Z_FS_MAX_OPEN];

static int z_fs_alloc_handle(void) {
	for (int i = 0; i < Z_FS_MAX_OPEN; i++)
		if (!z_fs_handles[i].used) return i;
	return -1;
}

z_obj_t *k_fs_open_write(z_obj_t *args) {

	z_fs_open_args_t *a = (z_fs_open_args_t *)args;
	if (a) a->handle = -1;

	if (!a || !a->name) return (&z_fail);

	int slot = z_fs_alloc_handle();
	if (slot < 0) return (&z_fail);

	FRESULT res = f_open(&z_fs_handles[slot].fil, a->name, FA_WRITE | FA_CREATE_ALWAYS);
	if (res != FR_OK) return (&z_fail);

	z_fs_handles[slot].used = true;
	z_fs_handles[slot].owner_pid = z_pid;
	a->handle = slot;

	return (&z_ok);

}

z_obj_t *k_fs_open_read(z_obj_t *args) {

	z_fs_open_args_t *a = (z_fs_open_args_t *)args;
	if (a) a->handle = -1;

	if (!a || !a->name) return (&z_fail);

	int slot = z_fs_alloc_handle();
	if (slot < 0) return (&z_fail);

	FRESULT res = f_open(&z_fs_handles[slot].fil, a->name, FA_READ | FA_OPEN_EXISTING);
	if (res != FR_OK) return (&z_fail);

	z_fs_handles[slot].used = true;
	z_fs_handles[slot].owner_pid = z_pid;
	a->handle = slot;

	return (&z_ok);

}

// returns &z_ok with a->len == 0 for a clean end-of-file (NOT &z_fail
// -- EOF is the normal, expected way a read loop ends, not an error;
// matches fs_read_chunk()'s own existing kernel-native convention,
// sw/os/fs/fs.c, which sh.c's own tget already relies on the same way).
// &z_fail is reserved for a genuine problem: a bad/foreign handle, or
// FatFs itself reporting an error.
z_obj_t *k_fs_read_chunk(z_obj_t *args) {

	z_fs_read_chunk_args_t *a = (z_fs_read_chunk_args_t *)args;
	if (a) a->len = 0;

	if (!a || a->handle < 0 || a->handle >= Z_FS_MAX_OPEN || !a->buf || a->maxlen == 0)
		return (&z_fail);

	if (!z_fs_handles[a->handle].used || z_fs_handles[a->handle].owner_pid != z_pid)
		return (&z_fail);

	UINT br = 0;
	FRESULT res = f_read(&z_fs_handles[a->handle].fil, a->buf, (UINT)a->maxlen, &br);
	if (res != FR_OK) return (&z_fail);

	a->len = (uint32_t)br;
	return (&z_ok);

}

z_obj_t *k_fs_write_chunk(z_obj_t *args) {

	z_fs_write_chunk_args_t *a = (z_fs_write_chunk_args_t *)args;
	if (a) a->written = 0;

	if (!a || a->handle < 0 || a->handle >= Z_FS_MAX_OPEN || (!a->buf && a->len > 0))
		return (&z_fail);

	if (!z_fs_handles[a->handle].used || z_fs_handles[a->handle].owner_pid != z_pid)
		return (&z_fail);

	UINT bw = 0;
	FRESULT res = f_write(&z_fs_handles[a->handle].fil, a->buf, (UINT)a->len, &bw);
	if (res != FR_OK) return (&z_fail);

	a->written = (uint32_t)bw;
	return (&z_ok);

}

z_obj_t *k_fs_close(z_obj_t *args) {

	z_fs_close_args_t *a = (z_fs_close_args_t *)args;

	if (!a || a->handle < 0 || a->handle >= Z_FS_MAX_OPEN) return (&z_fail);
	if (!z_fs_handles[a->handle].used || z_fs_handles[a->handle].owner_pid != z_pid)
		return (&z_fail);

	FRESULT res = f_close(&z_fs_handles[a->handle].fil);
	z_fs_handles[a->handle].used = false;

	return (res == FR_OK) ? (&z_ok) : (&z_fail);

}
// each entry as `prefix + short-name` where `prefix` is always
// exactly one leading AND trailing "/" for the root case, or the
// caller's own `path` normalized to end in exactly one "/" --
// deliberately NOT reusing fs_list_dir()'s own "%s/%s" (fs.c, this
// same file) unmodified: that formula is only correct for a path
// WITHOUT its own trailing slash, but every existing caller (sh.c's
// `ls`) has only ever passed "/" itself, producing the doubled
// "//NAME" this function fixes for good.
//
// No snprintf()/malloc() here -- same reasoning as k_fs_read()/
// k_fs_write() above (fsapi.h's own header comment on why kernel-
// compiled code stays away from that libc machinery); the prefix and
// each combined name are built with plain strlen()/memcpy() into
// small fixed local buffers instead.
z_obj_t *k_fs_list(z_obj_t *args) {

	z_fs_list_args_t *a = (z_fs_list_args_t *)args;

	if (!a || !a->out || a->out_cap == 0) {
		if (a) { a->count = 0; a->truncated = 0; }
		return (&z_fail);
	}

	a->count = 0;
	a->truncated = 0;

	const char *dir_path = (a->path && a->path[0]) ? a->path : "/";

	DIR dir;
	FRESULT res = f_opendir(&dir, dir_path);
	if (res != FR_OK) return (&z_fail);

	char prefix[64];
	{
		size_t len = strlen(dir_path);
		if (len >= sizeof(prefix) - 2) len = sizeof(prefix) - 2;
		memcpy(prefix, dir_path, len);
		if (len == 0 || prefix[len - 1] != '/') prefix[len++] = '/';
		prefix[len] = 0;
	}
	size_t prefix_len = strlen(prefix);

	uint32_t max_entries = a->max_entries ? a->max_entries : 0xFFFFFFFFu;
	uint32_t written = 0;
	uint32_t count = 0;

	FILINFO fno;
	while (count < max_entries) {

		res = f_readdir(&dir, &fno);
		if (res != FR_OK || fno.fname[0] == 0) break;

		size_t nlen = strlen(fno.fname);
		char full[96];
		if (prefix_len + nlen >= sizeof(full)) {
			// pathologically long combined path -- skip this one
			// entry rather than overflow `full` (never happens with
			// real 8.3 short names, FF_SFN_BUF is 12, see ffconf.h)
			continue;
		}
		memcpy(full, prefix, prefix_len);
		memcpy(full + prefix_len, fno.fname, nlen + 1); // +1: copy the NUL too

		size_t flen = prefix_len + nlen;
		if (written + flen + 1 > a->out_cap) {
			a->truncated = 1;
			break;
		}

		memcpy(a->out + written, full, flen + 1); // +1: include the NUL separator
		written += (uint32_t)(flen + 1);
		count++;

	}

	f_closedir(&dir);

	a->count = count;

	return (&z_ok);

}
