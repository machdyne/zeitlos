#ifndef Z_FS_H
#define Z_FS_H

#include <stdint.h>

#include "fatfs/ff.h"

int fs_mount(void);
int fs_format(void);
uint32_t fs_total(void);
uint32_t fs_free(void);

int fs_load(uint32_t dst, char *path);
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

int fs_open_read(FIL *f, char *path);
int32_t fs_read_chunk(FIL *f, void *buf, uint32_t maxlen);
int fs_close_read(FIL *f);

#endif
