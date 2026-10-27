/*
 * learn -- the format's file system on the build machine (lf_fs_t over POSIX).
 */
#include <stdio.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>
#include "hostfs.h"

static bool h_list(void *ctx, const char *dir,
	void (*fn)(void *arg, const char *name, bool is_dir), void *arg) {
	(void)ctx;
	DIR *d = opendir(dir);
	if (!d) return false;
	struct dirent *e;
	while ((e = readdir(d))) {
		if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
		char p[1024];
		struct stat st;
		snprintf(p, sizeof p, "%s/%s", dir, e->d_name);
		bool isdir = !stat(p, &st) && S_ISDIR(st.st_mode);
		fn(arg, e->d_name, isdir);
	}
	closedir(d);
	return true;
}

static long h_read(void *ctx, const char *path, char *buf, long cap) {
	(void)ctx;
	FILE *f = fopen(path, "rb");
	if (!f) return -1;
	long n = (long)fread(buf, 1, (size_t)cap, f);
	fclose(f);
	return n;
}

const lf_fs_t host_fs = { h_list, h_read, NULL };
