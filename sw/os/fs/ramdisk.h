#ifndef RAMDISK_H
#define RAMDISK_H

#include <stdint.h>
#include <stdbool.h>

/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * A RAM-backed block device, used as FatFs physical drive 1 and
 * reachable from every app as /ram.
 *
 * -- why --
 *
 * The SD card is SPI through the SOC's hardware master, and it still
 * shows: sw/apps/web spends
 * roughly 13 seconds re-reading a 258KB page off the card to index
 * it, against 1.4 seconds writing it. A browser wants a scratch area
 * for one page, not durable storage, and so does anything else that
 * needs to put a few hundred kilobytes somewhere and read it back.
 *
 * -- why a DISK and not a buffer --
 *
 * An app-private buffer would have been a smaller change to `web`,
 * but an app's malloc() is bounded by its stack+heap allowance
 * (z_proc_stack_size_for(), kernel.h), so a megabyte of it needs
 * either a new syscall or a "stack size" that is mostly not stack.
 * Both are new mechanisms for one caller.
 *
 * FatFs already has a block-device seam (diskio.h) and multi-volume
 * support, so a second drive costs a driver this size and one line of
 * configuration -- and every app gets it through the file API it
 * already uses, with no new API at all.
 *
 * -- what it is not --
 *
 * Not persistent: the contents are gone at reset, and the volume is
 * reformatted whenever it is created. Not reserved: the space comes
 * out of the same pool as everything else, so a large ramdisk on a
 * small board is a bad trade. Not arbitrated: two apps sharing it can
 * starve each other, and callers are expected to cope with ENOSPC
 * rather than assume room.
 */

#define RAMDISK_SECTOR_SIZE 512

// Allocates `bytes` (rounded down to a whole number of sectors) from
// the kernel pool and makes drive 1 usable. Returns false if the
// allocation failed or the size is too small to hold a filesystem.
//
// Safe to call when already initialised: returns true and changes
// nothing.
bool ramdisk_init(uint32_t bytes);

// Bytes currently backing the device, or 0 if there is none.
uint32_t ramdisk_size(void);

bool ramdisk_present(void);

// Releases the backing memory. Any FatFs volume on it must be
// unmounted first.
void ramdisk_free(void);

// The diskio entry points for drive 1, called from diskio_mux.c.
int rd_disk_status(void);
int rd_disk_initialize(void);
int rd_disk_read(uint8_t *buff, uint32_t sector, uint32_t count);
int rd_disk_write(const uint8_t *buff, uint32_t sector, uint32_t count);
int rd_disk_ioctl(uint8_t cmd, void *buff);

#endif
