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
 *   drive 0   the SD card    (sdmm.c)
 *   drive 1   the ramdisk    (../ramdisk.c)
 *
 * Keeping this in its own file rather than inside sdmm.c means the
 * vendored driver stays recognisably the upstream sample, which is
 * worth something the next time it is updated.
 */

#include "ff.h"
#include "diskio.h"
#include "../ramdisk.h"

// sdmm.c, renamed so this file can own the FatFs-facing names.
DSTATUS sd_disk_status(BYTE drv);
DSTATUS sd_disk_initialize(BYTE drv);
DRESULT sd_disk_read(BYTE drv, BYTE *buff, LBA_t sector, UINT count);
DRESULT sd_disk_write(BYTE drv, const BYTE *buff, LBA_t sector, UINT count);
DRESULT sd_disk_ioctl(BYTE drv, BYTE ctrl, void *buff);

DSTATUS disk_status(BYTE pdrv) {
	if (pdrv == 0) return sd_disk_status(0);
	if (pdrv == 1) return (DSTATUS)rd_disk_status();
	return STA_NOINIT;
}

DSTATUS disk_initialize(BYTE pdrv) {
	if (pdrv == 0) return sd_disk_initialize(0);
	if (pdrv == 1) return (DSTATUS)rd_disk_initialize();
	return STA_NOINIT;
}

DRESULT disk_read(BYTE pdrv, BYTE *buff, LBA_t sector, UINT count) {
	if (pdrv == 0) return sd_disk_read(0, buff, sector, count);
	if (pdrv == 1) return (DRESULT)rd_disk_read(buff, (uint32_t)sector, count);
	return RES_PARERR;
}

DRESULT disk_write(BYTE pdrv, const BYTE *buff, LBA_t sector, UINT count) {
	if (pdrv == 0) return sd_disk_write(0, buff, sector, count);
	if (pdrv == 1) return (DRESULT)rd_disk_write(buff, (uint32_t)sector, count);
	return RES_PARERR;
}

DRESULT disk_ioctl(BYTE pdrv, BYTE cmd, void *buff) {
	if (pdrv == 0) return sd_disk_ioctl(0, cmd, buff);
	if (pdrv == 1) return (DRESULT)rd_disk_ioctl(cmd, buff);
	return RES_PARERR;
}
