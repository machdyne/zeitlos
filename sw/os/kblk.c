/*
 * Zeitlos OS
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * What the kernel still provides for files, now that the filesystem is
 * a driver (sw/apps/fs, docs/fs_driver.md): block devices it drives
 * itself, and the flash archive.
 *
 *   Z_SYS_BLK  the RAM disk, by sector, for `fs`
 *   Z_SYS_ZAR  the core-app archive's files and directories, read-only
 *
 * The RAM disk is plain kernel memory, allocated at boot (kernel.c) and
 * formatted by `fs`. It stays here rather than in `fs`'s own heap so
 * that its size follows the machine -- a process's memory is fixed by
 * its executable's header -- and so that `fs` restarting does not lose
 * it. A sector transfer costs one syscall and the same memcpy FatFs
 * would have done itself.
 *
 * USB mass storage was here too, while the USB host was the kernel's.
 * Since Phase 3 the drive is the `usb` process's, and `fs` asks it
 * directly (sw/common/zusbproto.h); Z_BLK_USB is refused.
 */

#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#include "kernel.h"
#include "kblk.h"
#include "mem.h"
#include "zar.h"
#include "../common/zsoc.h"

// -- the RAM disk -------------------------------------------------------

static uint8_t *rd_base;
static uint32_t rd_sectors;

extern char _end;

bool k_ramdisk_init(uint32_t bytes) {
	uint32_t sectors = bytes / Z_BLK_SECTOR;
	if (rd_base) return true;
	if (sectors < 128) return false;	// too small to hold a filesystem
	rd_base = (uint8_t *)k_mem_alloc(sectors * Z_BLK_SECTOR);
	if (!rd_base) return false;
	// Called before process zero exists, the pool would hand out the
	// kernel's own image (kernel.c explains the ordering).
	if ((char *)rd_base < &_end) {
		k_mem_free(rd_base);
		rd_base = 0;
		return false;
	}
	rd_sectors = sectors;
	// Not zeroed -- `fs` formats it -- except the first sector, so that
	// no filesystem is there at boot. `fs` mounts one it finds instead
	// of formatting, which is what keeps /ram across a restart of `fs`
	// (sw/apps/fs, ram_start()), and only then.
	memset(rd_base, 0, Z_BLK_SECTOR);
	return true;
}

uint32_t k_ramdisk_bytes(void) { return rd_sectors * Z_BLK_SECTOR; }

static bool rd_span_ok(uint32_t lba, uint32_t count) {
	return rd_base && count && lba < rd_sectors && count <= rd_sectors - lba;
}

// -- Z_SYS_BLK ----------------------------------------------------------

// A WRITE's source: the kernel only reads it, and a driver may read any
// memory anyway (docs/mpu.md). So main memory is accepted wherever it
// is -- the case that matters is `fs` writing a client's sectors
// straight from the client's buffer, which reaches it as a physical
// address (docs/messaging.md): FatFs hands a whole-sector write the
// caller's buffer, not its own. Anything else must be the driver's own.
static bool blk_src_ok(const void *p, uint32_t len) {
	uint32_t a = (uint32_t)(uintptr_t)p;
	uint32_t mb = z_soc_mem_mb();
	uint32_t end = Z_MEM_BASE + (mb ? mb * 1024u * 1024u : Z_MEM_SIZE_DEFAULT);
	if (a >= Z_MEM_BASE && a < end && len <= end - a) return true;
	return k_user_ok(p, len);
}

z_obj_t *k_blk(z_obj_t *args) {
	z_blk_args_t *a = (z_blk_args_t *)args;

	if (z_pid >= Z_PROCS_MAX || !(z_procs[z_pid].flags & Z_PROC_FLAG_DRIVER) ||
		!k_user_ok_words(a, sizeof(*a)))
		return (&z_fail);

	if (a->op == Z_BLK_OP_READ || a->op == Z_BLK_OP_WRITE) {
		if (!a->count || a->count > 0x10000u) return (&z_fail);
		// READ stores into buf: the driver's own memory (or its grant
		// window). WRITE only reads it: see blk_src_ok().
		if (a->op == Z_BLK_OP_READ ? !k_user_ok(a->buf, a->count * Z_BLK_SECTOR)
				: !blk_src_ok(a->buf, a->count * Z_BLK_SECTOR))
			return (&z_fail);
	}

	if (a->dev == Z_BLK_RAM) {
		switch (a->op) {
		case Z_BLK_OP_INFO:
			a->sectors = rd_sectors;
			a->gen = 1;
			a->flags = rd_base ? (Z_BLK_PRESENT | Z_BLK_READY) : 0;
			return (&z_ok);
		case Z_BLK_OP_START:
			return rd_base ? (&z_ok) : (&z_fail);
		case Z_BLK_OP_READ:
			if (!rd_span_ok(a->lba, a->count)) return (&z_fail);
			memcpy(a->buf, rd_base + a->lba * Z_BLK_SECTOR, a->count * Z_BLK_SECTOR);
			return (&z_ok);
		case Z_BLK_OP_WRITE:
			if (!rd_span_ok(a->lba, a->count)) return (&z_fail);
			memcpy(rd_base + a->lba * Z_BLK_SECTOR, a->buf, a->count * Z_BLK_SECTOR);
			return (&z_ok);
		}
		return (&z_fail);
	}

	return (&z_fail);
}

// -- Z_SYS_ZAR ----------------------------------------------------------

// Paths are read from the caller's memory a byte at a time; a bound
// keeps a missing NUL from walking off the end.
static bool zar_path_ok(const char *p) {
	if (!p) return false;
	for (uint32_t i = 0; i < 256; i++) if (!p[i]) return true;
	return false;
}

z_obj_t *k_zar(z_obj_t *args) {
	z_zar_args_t *a = (z_zar_args_t *)args;
	bool d = false;

	if (!k_user_ok_words(a, sizeof(*a))) return (&z_fail);
	if (a->op != Z_ZAR_OP_READ && !zar_path_ok(a->path)) return (&z_fail);

	switch (a->op) {
	case Z_ZAR_OP_CHILD: {
		char name[Z_ZAR_NAME_MAX + 1];
		uint32_t size = 0;
		if (!z_zar_child(a->path, &a->iter, name, &size, &d)) return (&z_fail);
		strncpy(a->name, name, sizeof(a->name) - 1);
		a->name[sizeof(a->name) - 1] = 0;
		a->size = size;
		a->is_dir = d ? 1 : 0;
		return (&z_ok);
	}
	case Z_ZAR_OP_FILE:
		return z_zar_file(a->path, &a->off, &a->size) == 0 ? (&z_ok) : (&z_fail);
	case Z_ZAR_OP_READ:
		if (!a->len || !k_user_ok(a->buf, a->len)) return (&z_fail);
		z_zar_read(a->off, (uint8_t *)a->buf, a->len);
		return (&z_ok);
	case Z_ZAR_OP_ISDIR:
		return z_zar_is_dir(a->path) ? (&z_ok) : (&z_fail);
	}
	return (&z_fail);
}
