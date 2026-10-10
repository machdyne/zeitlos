#ifndef KBLK_H
#define KBLK_H

/*
 * Zeitlos OS
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * The kernel's block devices and flash archive for the filesystem
 * driver: kblk.c.
 */

#include <stdint.h>
#include <stdbool.h>
#include "../common/zobj.h"

// The RAM disk: `bytes` of kernel memory, rounded down to whole
// sectors. Only once, after process zero exists (kernel.c).
bool k_ramdisk_init(uint32_t bytes);
uint32_t k_ramdisk_bytes(void);

z_obj_t *k_blk(z_obj_t *args);	// Z_SYS_BLK
z_obj_t *k_zar(z_obj_t *args);	// Z_SYS_ZAR

#endif
