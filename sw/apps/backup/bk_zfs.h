/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * bk.c's files (bk_io_t's file calls) over zfsc.c: straight to the fs
 * driver, which serves every volume -- the card, /ram and the backup
 * drive -- so free space (STATVFS) and listings a page at a time
 * (OPENDIR/READDIR) are there for /usb too.
 *
 * Its own file so the host test (tests/test_fat.c) can run it against
 * the real fs driver and FatFs (sw/apps/fs/tests/test_fs.c's kernel
 * stubs), on real FAT images, with the power cut at a sector write.
 */

#ifndef BK_ZFS_H
#define BK_ZFS_H

#include "bk.h"

// Fills io's file calls (open .. free_kb); leaves the rest as it was.
void bk_zfs_io(bk_io_t *io);

#endif
