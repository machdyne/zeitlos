#ifndef _ZEITLOS_DIRENT_H
#define _ZEITLOS_DIRENT_H

/*
 * Zeitlos -- <dirent.h> for apps built with POSIX_IO = 1
 * (sw/common/zposix.c, docs/posix_io.md).
 *
 * newlib's dirent.h for this target is a single `#error`, so an app
 * that lists directories the POSIX way needs this one: the Makefile
 * rule puts sw/common/posix on the include path ahead of the C
 * library's. opendir()/readdir()/closedir() are requests to `fs`; a
 * DIR holds one page of entries at a time.
 */

#include <stdint.h>

#define DT_UNKNOWN	0
#define DT_DIR		4
#define DT_REG		8

struct dirent {
	uint32_t	d_ino;		// 1: there are no inode numbers here
	uint8_t		d_type;		// DT_DIR or DT_REG
	char		d_name[256];
};

typedef struct _zeitlos_DIR DIR;

DIR *opendir(const char *path);
struct dirent *readdir(DIR *dp);
int closedir(DIR *dp);

#endif
