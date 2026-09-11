/*
 * <dirent.h> for Zeitlos.
 *
 * newlib's own header for this target is a single `#error "<dirent.h>
 * not supported"`, which is honest: newlib has no directory concept.
 * Zeitlos does -- fs_list_into() in sw/common/zfsapp.h -- but it is
 * not opendir/readdir shaped.
 *
 * nextvi uses this in ONE function, dir_calc() in vi.c, which walks a
 * directory tree to build the file-completion list for `:e`. That is a
 * convenience, not a core path, and it is the only thing lost by the
 * stubs in posix_stubs.c: opendir() returns NULL and dir_calc()
 * returns early, so `:e` completion offers nothing and everything else
 * works.
 *
 * Implementing it over fs_list_into() is a contained job if the
 * completion turns out to be missed -- one function, and the entry
 * format is already documented in sw/common/zfs.h.
 */
#ifndef _ZEITLOS_DIRENT_H
#define _ZEITLOS_DIRENT_H

struct dirent {
	/* d_name is the only field nextvi reads. */
	char d_name[256];
};

typedef struct _zeitlos_DIR DIR;

DIR *opendir(const char *path);
struct dirent *readdir(DIR *dp);
int closedir(DIR *dp);

#endif
