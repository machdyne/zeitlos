#ifndef Z_FS_H
#define Z_FS_H

#include <stdint.h>

#include "fatfs/ff.h"
#include "../../common/zexec.h"

int fs_mount(void);
// forced (non-deferred) mount -- see fs.c for why this exists
int fs_mount_now(void);
int fs_format(void);
uint32_t fs_total(void);
uint32_t fs_free(void);

int fs_load(uint32_t dst, char *path);

// Zeitlos executable format -- see sw/common/zexec.h. Inspect first
// (the caller needs the image size to allocate), then load. Handles
// legacy raw --pad-to binaries transparently: they report bss_size 0
// and load exactly as fs_load() would.
int fs_exec_info(char *path, z_exec_info_t *info);
int fs_load_exec(uint32_t dst, char *path, const z_exec_info_t *info);

// Filesystem first, flash core-app archive underneath -- see fs.c.
// Every process-launch path should use these rather than the two
// above, so that a card-less board behaves identically.
// Executable resolution. A bare name is searched for in the root,
// then apps/, then the flash archive; a name containing '/' is taken
// literally. All three of these share one resolver in fs.c so they
// cannot disagree -- see its comment for why the search lives here
// rather than as an "apps/" prefix at every call site.
int fs_exec_info_any(char *path, z_exec_info_t *info);

// Both capacity figures in KB from a single FAT scan -- see fs.c for
// why calling fs_total() and fs_free() separately is worth avoiding.
void fs_df_kb(uint32_t *total_kb, uint32_t *free_kb);
int fs_load_exec_any(uint32_t dst, char *path, const z_exec_info_t *info);
int fs_exec_is_flash(char *path);	// 1 if resolved to flash
void *fs_mallocfile(char *path);
uint32_t fs_size(char *path);
int fs_write_file(char *path, char *buf, uint32_t len);

int fs_touch(char *path);
int fs_mkdir(char *path);
int fs_unlink(char *path);
void fs_list_dir(char *path);

// -- chunked (streaming) read/write --
//
// for moving a file to/from disk incrementally, without needing the
// whole thing in memory at once -- see sw/common/zstream.h, which
// this is meant to pair with. FIL (from fatfs/ff.h) is exposed
// directly rather than wrapped, since this is already a thin
// FatFs-backed API.

int fs_open_write(FIL *f, char *path);
int fs_write_chunk(FIL *f, const void *buf, uint32_t len);
int fs_close_write(FIL *f);
// flush FatFs's buffered metadata to the card -- see fs.c
int fs_sync(FIL *f);
// flush and unmount; call before cutting power or reprogramming
int fs_unmount(void);

int fs_open_read(FIL *f, char *path);
int32_t fs_read_chunk(FIL *f, void *buf, uint32_t maxlen);
int fs_close_read(FIL *f);

#endif
