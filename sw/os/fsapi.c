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

// Does a launchable executable by this name exist, from ANY source?
//
// Deliberately not k_fs_size(): that only looks at the filesystem, so
// on a board with no sdcard it would report the flash-resident core
// apps as missing -- which is exactly backwards, since those are the
// ones guaranteed to be there. This goes through fs_exec_info_any()
// (sw/os/fs/fs.c), the same resolver every launch path uses, so the
// answer always matches what `run` would actually do.
//
// Added for wm's dock, which builds itself from whichever of its
// candidate apps are actually present -- an icon for a missing app is
// a button that does nothing. See docs/window_manager.md.
//
// Returns the process image size (data + bss), or 0 if not found, so
// callers get a usable size for free rather than needing a second
// call.
static z_obj_t * k_exec_exists__unlocked(z_obj_t *args) {

	if (!args || args->type != Z_STR || !args->val.str) {
		args->type = Z_UINT32;
		args->val.uint32 = 0;
		return (&z_fail);
	}

	z_exec_info_t xi;
	uint32_t total = fs_exec_info_any(args->val.str, &xi) ? 0 : xi.total;

	args->type = Z_UINT32;
	args->val.uint32 = total;

	return total ? (&z_ok) : (&z_fail);

}

static z_obj_t * k_fs_size__unlocked(z_obj_t *args) {

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

static z_obj_t * k_fs_read__unlocked(z_obj_t *args) {

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

static z_obj_t * k_fs_write__unlocked(z_obj_t *args) {

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

static z_obj_t * k_fs_unlink__unlocked(z_obj_t *args) {

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

int k_fs_open_count(void) {

	int n = 0;

	for (int i = 0; i < Z_FS_MAX_OPEN; i++)
		if (z_fs_handles[i].used) n++;

	return n;

}

static z_obj_t * k_fs_open_write__unlocked(z_obj_t *args) {

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

static z_obj_t * k_fs_open_read__unlocked(z_obj_t *args) {

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
static z_obj_t * k_fs_read_chunk__unlocked(z_obj_t *args) {

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

static z_obj_t * k_fs_write_chunk__unlocked(z_obj_t *args) {

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

static z_obj_t * k_fs_close__unlocked(z_obj_t *args) {

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
static z_obj_t * k_fs_list__unlocked(z_obj_t *args) {

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

	// A `types` buffer is sized by the caller in units of entries,
	// and the ONLY thing that tells us how many that is, is
	// max_entries. Without one, the entry count is bounded solely by
	// out_cap -- a 4KB name buffer holds several hundred short names,
	// so writing a type byte per entry into a buffer whose length we
	// were never told is a straightforward overrun of app memory.
	// Refuse rather than guess: zfs.h states the requirement, and a
	// caller that ignored it gets a clean failure instead of silent
	// corruption somewhere else in its address space.
	if (a->types && !a->max_entries) return (&z_fail);
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

		// Optional -- see Z_FS_TYPE_* in zfs.h for why this is a
		// separate buffer rather than a decoration on the name. NULL
		// (every caller that predates it) skips this entirely.
		if (a->types)
			a->types[count] = (fno.fattrib & AM_DIR)
				? Z_FS_TYPE_DIR : Z_FS_TYPE_FILE;

		memcpy(a->out + written, full, flen + 1); // +1: include the NUL separator
		written += (uint32_t)(flen + 1);
		count++;

	}

	f_closedir(&dir);

	a->count = count;

	return (&z_ok);

}

// -- mkdir / touch / seek --
//
// The first two are thin wrappers over fs_mkdir()/fs_touch()
// (sw/os/fs/fs.c), which already do the real work and which sh.c's own
// `mkdir`/`touch` commands have always used -- this just makes them
// reachable from an ordinary app, the same way k_fs_unlink() did for
// fs_unlink(). Both of those fs.c functions follow the SAME inverted
// convention fs_unlink() does (0 on SUCCESS, non-zero on failure) --
// see k_fs_unlink()'s own comment above, which spells out why that's
// worth restating at every call site rather than trusting memory.
//
// Their unconditional printf() (inside fs.c) means an app-triggered
// mkdir/touch also shows up on the serial console -- same visibility
// every other fs.c operation already has, not new behavior introduced
// here.

static z_obj_t * k_fs_mkdir__unlocked(z_obj_t *args) {

	z_fs_path_args_t *a = (z_fs_path_args_t *)args;

	if (!a || !a->name) return (&z_fail);

	return (fs_mkdir(a->name) == 0) ? (&z_ok) : (&z_fail);

}

static z_obj_t * k_fs_touch__unlocked(z_obj_t *args) {

	z_fs_path_args_t *a = (z_fs_path_args_t *)args;

	if (!a || !a->name) return (&z_fail);

	return (fs_touch(a->name) == 0) ? (&z_ok) : (&z_fail);

}

// Repositions an open chunked-I/O handle. Same handle-table ownership
// check every other chunked handler above performs, for the same
// reason (see zfs.h's own "Ownership" note): a small integer handle is
// trivially guessable, so one process must not be able to move another
// process's file position out from under it.
//
// f_lseek() on a READ handle clamps to the file size rather than
// failing when asked to go past EOF, which is exactly the behavior
// sw/apps/repl's `page` wants for a "jump to end" -- the resulting
// position comes back in a->pos so a caller that cares can see where
// it actually landed. On a WRITE handle FatFs would instead EXTEND the
// file, which is a real difference worth knowing about but not
// something this handler needs to police: page only ever opens for
// read, and a caller that opened for write and seeks past the end has
// asked for exactly what FatFs does.
static z_obj_t * k_fs_seek__unlocked(z_obj_t *args) {

	z_fs_seek_args_t *a = (z_fs_seek_args_t *)args;
	if (a) a->pos = 0;

	if (!a || a->handle < 0 || a->handle >= Z_FS_MAX_OPEN) return (&z_fail);

	if (!z_fs_handles[a->handle].used || z_fs_handles[a->handle].owner_pid != z_pid)
		return (&z_fail);

	FRESULT res = f_lseek(&z_fs_handles[a->handle].fil, (FSIZE_t)a->offset);
	if (res != FR_OK) return (&z_fail);

	a->pos = (uint32_t)f_tell(&z_fs_handles[a->handle].fil);

	return (&z_ok);

}

// Filesystem capacity. Both numbers come straight from fs_total()/
// fs_free() (sw/os/fs/fs.c), which wrap FatFs's f_getfree() and have
// been sitting there unused since long before this syscall existed --
// nothing had a way to call them. Each returns 0 on failure (no card,
// not mounted, FatFs error), which this passes through unchanged: a
// volume that genuinely reports 0 total is indistinguishable from one
// that failed to answer, and neither is a state where a caller should
// be told a capacity.
static z_obj_t * k_fs_df__unlocked(z_obj_t *args) {

	z_fs_df_args_t *a = (z_fs_df_args_t *)args;

	if (!a) return (&z_fail);

	// One FAT scan, not two -- see fs_df_kb() in sw/os/fs/fs.c.
	fs_df_kb(&a->total_kb, &a->free_kb);

	return (&z_ok);

}

// -- locked entry points (see fs_lock() in fs.c) --------------------
z_obj_t * k_exec_exists(z_obj_t *args) {
	fs_lock();
	z_obj_t * r = k_exec_exists__unlocked(args);
	fs_unlock();
	return r;
}

z_obj_t * k_fs_size(z_obj_t *args) {
	fs_lock();
	z_obj_t * r = k_fs_size__unlocked(args);
	fs_unlock();
	return r;
}

z_obj_t * k_fs_read(z_obj_t *args) {
	fs_lock();
	z_obj_t * r = k_fs_read__unlocked(args);
	fs_unlock();
	return r;
}

z_obj_t * k_fs_write(z_obj_t *args) {
	fs_lock();
	z_obj_t * r = k_fs_write__unlocked(args);
	fs_unlock();
	return r;
}

z_obj_t * k_fs_unlink(z_obj_t *args) {
	fs_lock();
	z_obj_t * r = k_fs_unlink__unlocked(args);
	fs_unlock();
	return r;
}

z_obj_t * k_fs_open_write(z_obj_t *args) {
	fs_lock();
	z_obj_t * r = k_fs_open_write__unlocked(args);
	fs_unlock();
	return r;
}

z_obj_t * k_fs_open_read(z_obj_t *args) {
	fs_lock();
	z_obj_t * r = k_fs_open_read__unlocked(args);
	fs_unlock();
	return r;
}

z_obj_t * k_fs_read_chunk(z_obj_t *args) {
	fs_lock();
	z_obj_t * r = k_fs_read_chunk__unlocked(args);
	fs_unlock();
	return r;
}

z_obj_t * k_fs_write_chunk(z_obj_t *args) {
	fs_lock();
	z_obj_t * r = k_fs_write_chunk__unlocked(args);
	fs_unlock();
	return r;
}

z_obj_t * k_fs_close(z_obj_t *args) {
	fs_lock();
	z_obj_t * r = k_fs_close__unlocked(args);
	fs_unlock();
	return r;
}

z_obj_t * k_fs_list(z_obj_t *args) {
	fs_lock();
	z_obj_t * r = k_fs_list__unlocked(args);
	fs_unlock();
	return r;
}

z_obj_t * k_fs_mkdir(z_obj_t *args) {
	fs_lock();
	z_obj_t * r = k_fs_mkdir__unlocked(args);
	fs_unlock();
	return r;
}

z_obj_t * k_fs_touch(z_obj_t *args) {
	fs_lock();
	z_obj_t * r = k_fs_touch__unlocked(args);
	fs_unlock();
	return r;
}

z_obj_t * k_fs_seek(z_obj_t *args) {
	fs_lock();
	z_obj_t * r = k_fs_seek__unlocked(args);
	fs_unlock();
	return r;
}

z_obj_t * k_fs_df(z_obj_t *args) {
	fs_lock();
	z_obj_t * r = k_fs_df__unlocked(args);
	fs_unlock();
	return r;
}
