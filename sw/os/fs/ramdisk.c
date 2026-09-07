/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * See ramdisk.h.
 */

#include <string.h>
#include <stdio.h>

#include "ramdisk.h"
#include "../../common/zeitlos.h"	// z_rv / z_obj_t, which mem.h uses
#include "../mem.h"
#include "fatfs/ff.h"
#include "fatfs/diskio.h"

static uint8_t *rd_base;
static uint32_t rd_sectors;

// End of the kernel image, from the linker script. Used only for the
// sanity check in ramdisk_init().
extern char _end;

bool ramdisk_present(void) { return rd_base != 0; }

uint32_t ramdisk_size(void) {
	return rd_base ? rd_sectors * RAMDISK_SECTOR_SIZE : 0;
}

bool ramdisk_init(uint32_t bytes) {

	uint32_t sectors = bytes / RAMDISK_SECTOR_SIZE;

	if (rd_base) return true;

	// FAT needs room for a boot sector, a FAT and a root directory
	// before it holds any data at all. Below this it is not a small
	// disk, it is a broken one.
	if (sectors < 128) return false;

	rd_base = (uint8_t *)k_mem_alloc(sectors * RAMDISK_SECTOR_SIZE);
	if (!rd_base) return false;

	// Refuse anything overlapping the kernel itself.
	//
	// The pool starts at Z_MEM_BASE and so does the kernel, and
	// nothing tells k_mem_alloc() the bottom of it is occupied until
	// process zero has been created. Called too early, this function
	// is handed the kernel's own image and stack -- and the next
	// thing that happens is f_mkfs() writing a FAT over it.
	//
	// That cost a boot that hung immediately after "memory
	// initialized", with nothing to distinguish it from k_mem_init()
	// having failed. The ordering in kernel.c is the fix; this is so
	// that getting the ordering wrong again says so.
	if ((char *)rd_base < &_end) {
		printf("fs: ramdisk: allocation at %p overlaps the kernel "
			"(ends %p) -- called too early?\n",
			(void *)rd_base, (void *)&_end);
		k_mem_free(rd_base);
		rd_base = 0;
		return false;
	}

	rd_sectors = sectors;

	// Not zeroed. f_mkfs writes what it needs, and zeroing several
	// megabytes at boot is a visible pause for no benefit -- nothing
	// can read a sector the filesystem has not allocated.
	return true;

}

void ramdisk_free(void) {
	if (!rd_base) return;
	k_mem_free(rd_base);
	rd_base = 0;
	rd_sectors = 0;
}

// -- diskio ---------------------------------------------------------

int rd_disk_status(void) {
	return rd_base ? 0 : STA_NOINIT;
}

int rd_disk_initialize(void) {
	return rd_base ? 0 : STA_NOINIT;
}

int rd_disk_read(uint8_t *buff, uint32_t sector, uint32_t count) {

	if (!rd_base) return RES_NOTRDY;

	// Bounds are checked rather than trusted. FatFs asks for what its
	// own metadata says exists, and that metadata lives in this same
	// buffer -- so a corrupted FAT would otherwise read or write
	// straight through the rest of the kernel pool.
	if (sector + count > rd_sectors || sector + count < sector)
		return RES_PARERR;

	memcpy(buff, rd_base + (uint32_t)sector * RAMDISK_SECTOR_SIZE,
		(uint32_t)count * RAMDISK_SECTOR_SIZE);

	return RES_OK;

}

int rd_disk_write(const uint8_t *buff, uint32_t sector, uint32_t count) {

	if (!rd_base) return RES_NOTRDY;

	if (sector + count > rd_sectors || sector + count < sector)
		return RES_PARERR;

	memcpy(rd_base + (uint32_t)sector * RAMDISK_SECTOR_SIZE, buff,
		(uint32_t)count * RAMDISK_SECTOR_SIZE);

	return RES_OK;

}

int rd_disk_ioctl(uint8_t cmd, void *buff) {

	if (!rd_base) return RES_NOTRDY;

	switch (cmd) {

	case CTRL_SYNC:
		return RES_OK;					// nothing is buffered

	case GET_SECTOR_COUNT:
		*(LBA_t *)buff = rd_sectors;
		return RES_OK;

	case GET_SECTOR_SIZE:
		*(WORD *)buff = RAMDISK_SECTOR_SIZE;
		return RES_OK;

	case GET_BLOCK_SIZE:
		*(DWORD *)buff = 1;				// no erase block
		return RES_OK;

	default:
		return RES_PARERR;

	}

}
