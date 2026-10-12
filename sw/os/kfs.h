#ifndef KFS_H
#define KFS_H

/*
 * Zeitlos OS
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * The kernel's files, now that the filesystem is a driver: everything
 * here is a request to `fs` (sw/apps/fs, docs/fs_driver.md) made with
 * the client in sw/common/zfsc.c, from whichever process is running --
 * the kernel shell (pid 0), or an app inside a syscall (PROC_RUN, the
 * Z_SYS_FS_* calls, CFG_RELOAD). The caller waits for the answer, as
 * z_msg_recv() waits: blocked, the rest of the system running.
 *
 * The names are the ones sw/os/fs/fs.h had when FatFs was the kernel's,
 * so the shell and the loader read as they did. Without `fs` running
 * every call fails, quickly: nothing in the kernel can read the card.
 * Programs still start from the flash archive, which is how `fs` itself
 * starts (sw/os/sh.c).
 */

#include <stdint.h>
#include <stdbool.h>
#include "../common/zexec.h"
#include "../common/zfs.h"

// Paths as the kernel shell holds them (the old fs.h name).
#define FS_PATH_MAX	Z_FS_PATH_MAX

// `fs` is running.
bool kfs_up(void);

uint32_t fs_size(char *path);			// 0: none (or empty)
int fs_write_file(char *path, char *buf, uint32_t len);	// bytes; 0 on failure
int fs_touch(char *path);				// 0 on success
int fs_mkdir(char *path);				// 0 on success
int fs_unlink(char *path);				// 0 on success
int fs_rename(const char *from, const char *to);	// Z_FS_ERR_*
bool fs_stat_info(const char *path, z_fs_info_t *fi);
void fs_list_dir(char *path);			// the shell's `ls`
void fs_df_kb(uint32_t *total_kb, uint32_t *free_kb);	// the card

// Volumes: the shell's `format`, `mount`, `usbmount`, `usbunmount`.
int fs_format(void);					// 0 on success
int fs_mount_now(void);					// 0 on success
int fs_unmount(void);					// 0 on success
bool fs_usb_mount(void);
void fs_usb_unmount(void);
int k_fs_sync_all(void);				// a reboot's flush: files synced

// Streaming, for the shell's tget/tput and cfg.
typedef struct { int32_t fd; } kfs_file_t;
int fs_open_write(kfs_file_t *f, char *path);	// 1 on success
int fs_write_chunk(kfs_file_t *f, const void *buf, uint32_t len);	// bytes, -1
int fs_sync(kfs_file_t *f);
int fs_close_write(kfs_file_t *f);
int fs_open_read(kfs_file_t *f, char *path);	// 1 on success
int32_t fs_read_chunk(kfs_file_t *f, void *buf, uint32_t maxlen);	// bytes, 0 EOF, -1
int fs_close_read(kfs_file_t *f);

// Programs: the card's /apps first, the flash archive underneath
// (docs/layout.md, "Finding a program"). 0 on success, like the rest of
// the exec family.
int fs_exec_info(char *path, z_exec_info_t *info);		// the card only
int fs_exec_info_any(char *path, z_exec_info_t *info);
int fs_exec_is_flash(char *path);
int fs_exec_where(char *path, char *out, uint32_t cap);
int fs_load_exec_any(uint32_t dst, char *path, const z_exec_info_t *info);
uint32_t fs_exec_exists_many(const char *const *names, uint32_t n, uint8_t *found);

// The whole of a small file into `buf` (at most `cap` bytes): its
// length, or -1. For cfg.
int32_t kfs_read_file(const char *path, void *buf, uint32_t cap);

#endif
