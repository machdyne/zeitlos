/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * FatFs physical drive dispatch.
 *
 * FatFs calls disk_read(pdrv, ...) and expects ONE implementation to
 * serve every drive. sdmm.c used to be that implementation and simply
 * refused any pdrv but 0. With a second volume there has to be a
 * dispatcher, so sdmm.c's entry points were renamed sd_disk_* and
 * this file owns the FatFs-facing names.
 *
 *   drive 0   the SD card         (sdmm.c)
 *   drive 1   the ramdisk         (../ramdisk.c)
 *   drive 2   USB mass storage    (../../usb/usbh_msc.c)
 *
 * Keeping this in its own file rather than inside sdmm.c means the
 * vendored driver stays recognisably the upstream sample, which is
 * worth something the next time it is updated.
 */

#include "ff.h"
#include "diskio.h"
#include "../ramdisk.h"
#include "../../usb/usbh_msc.h"

// sdmm.c, renamed so this file can own the FatFs-facing names.
DSTATUS sd_disk_status(BYTE drv);
DSTATUS sd_disk_initialize(BYTE drv);
DRESULT sd_disk_read(BYTE drv, BYTE *buff, LBA_t sector, UINT count);
DRESULT sd_disk_write(BYTE drv, const BYTE *buff, LBA_t sector, UINT count);
DRESULT sd_disk_ioctl(BYTE drv, BYTE ctrl, void *buff);

// -- drive 2, USB mass storage --
//
// Everything below blocks, which is only safe because FatFs is always
// entered through k_fs_enter() and so runs with the scheduler off
// anyway. Read the header of usbh_msc.c before calling any of it from
// somewhere else.
//
// z_usbh_msc_start() lives in disk_initialize() rather than at bind
// time for the same reason: bind runs from the IRQ 9 handler and the
// ktimer, and waiting for TEST UNIT READY there would stall the
// system. FatFs calls disk_initialize() from f_mount(), in process
// context, where blocking is already normal.

DSTATUS disk_status(BYTE pdrv) {
	if (pdrv == 0) return sd_disk_status(0);
	if (pdrv == 1) return (DSTATUS)rd_disk_status();
	if (pdrv == 2) return z_usbh_msc_ready() ? 0 : STA_NOINIT;
	return STA_NOINIT;
}

DSTATUS disk_initialize(BYTE pdrv) {
	if (pdrv == 0) return sd_disk_initialize(0);
	if (pdrv == 1) return (DSTATUS)rd_disk_initialize();
	if (pdrv == 2) {
		if (!z_usbh_msc_present()) return STA_NODISK;
		if (z_usbh_msc_ready()) return 0;
		return z_usbh_msc_start() == Z_USBH_MSC_OK ? 0 : STA_NOINIT;
	}
	return STA_NOINIT;
}

DRESULT disk_read(BYTE pdrv, BYTE *buff, LBA_t sector, UINT count) {
	if (pdrv == 0) return sd_disk_read(0, buff, sector, count);
	if (pdrv == 1) return (DRESULT)rd_disk_read(buff, (uint32_t)sector, count);
	if (pdrv == 2)
		return z_usbh_msc_read((uint32_t)sector, buff, count) ==
		       Z_USBH_MSC_OK ? RES_OK : RES_ERROR;
	return RES_PARERR;
}

DRESULT disk_write(BYTE pdrv, const BYTE *buff, LBA_t sector, UINT count) {
	if (pdrv == 0) return sd_disk_write(0, buff, sector, count);
	if (pdrv == 1) return (DRESULT)rd_disk_write(buff, (uint32_t)sector, count);
	if (pdrv == 2)
		return z_usbh_msc_write((uint32_t)sector, buff, count) ==
		       Z_USBH_MSC_OK ? RES_OK : RES_ERROR;
	return RES_PARERR;
}

DRESULT disk_ioctl(BYTE pdrv, BYTE cmd, void *buff) {
	if (pdrv == 0) return sd_disk_ioctl(0, cmd, buff);
	if (pdrv == 1) return (DRESULT)rd_disk_ioctl(cmd, buff);
	if (pdrv == 2) {
		switch (cmd) {
		case CTRL_SYNC: return RES_OK;
		case GET_SECTOR_COUNT:
			*(LBA_t *)buff = (LBA_t)z_usbh_msc_sectors();
			return RES_OK;
		case GET_SECTOR_SIZE: *(WORD *)buff = 512; return RES_OK;
		// Erase block size, in sectors. A USB stick does its own
		// wear levelling and does not expose one, so 1 is both the
		// honest answer and what FatFs assumes by default.
		case GET_BLOCK_SIZE: *(DWORD *)buff = 1; return RES_OK;
		default: return RES_PARERR;
		}
	}
	return RES_PARERR;
}
