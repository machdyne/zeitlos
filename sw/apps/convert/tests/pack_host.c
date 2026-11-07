/*
 * convert's folder packing (pack.c) on the build machine: the real
 * pack.c, zwalk.c and formats, with the fs calls answered from a host
 * directory as the kernel answers them (as sw/common/tests/test_zwalk.c
 * does). Driven by tests/test_pack.py.
 *
 *   pack_host [-c N] [-m HEAP] DIR OUT       OUT's ending picks zip, tar, tar.gz
 *
 * DIR is a path inside the host directory named by $PACK_ROOT, as a
 * card path is inside the card ("/home/photos").
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <dirent.h>
#include <time.h>
#include <sys/stat.h>

#include "zfs.h"
#include "zfsc.h"
#include "../pack.h"

static char root[512];

static const char *hp(const char *p) {
	static char b[2][1024];
	static int k;
	k ^= 1;
	snprintf(b[k], sizeof b[k], "%s%s", root, p);
	return b[k];
}

// -- the fs, as zwalk.c and pack.c call it --

#define MAXH 8
static FILE *hs[MAXH];
static int opened;

static int slot(FILE *f) {
	if (!f) return -1;
	for (int i = 0; i < MAXH; i++) if (!hs[i]) { hs[i] = f; opened++; return i; }
	fclose(f);
	return -1;
}
int fs_open_read(const char *p) { return slot(fopen(hp(p), "rb")); }
int fs_open_write(const char *p) { return slot(fopen(hp(p), "wb")); }
int fs_read_chunk(int h, void *b, int n) { return (int)fread(b, 1, (size_t)n, hs[h]); }
int fs_write_chunk(int h, const void *b, int n) { return (int)fwrite(b, 1, (size_t)n, hs[h]); }
int fs_close_handle(int h) { fclose(hs[h]); hs[h] = NULL; opened--; return 1; }
int fs_unlink(char *p) { return remove(hp(p)) == 0; }
int fs_mkdir(const char *p) { return mkdir(hp(p), 0777) == 0; }

int fs_stat(const char *p, z_fs_info_t *fi) {
	struct stat st;
	struct tm g;
	if (stat(hp(p), &st)) return 0;
	memset(fi, 0, sizeof *fi);
	bool d = S_ISDIR(st.st_mode);
	fi->type = d ? Z_FS_TYPE_DIR : Z_FS_TYPE_FILE;
	fi->attr = d ? Z_FS_ATTR_DIR : Z_FS_ATTR_ARCHIVE;
	fi->size = d ? 0 : (uint32_t)st.st_size;
	gmtime_r(&st.st_mtime, &g);
	fi->fdate = (uint16_t)(((g.tm_year - 80) << 9) | ((g.tm_mon + 1) << 5) | g.tm_mday);
	fi->ftime = (uint16_t)((g.tm_hour << 11) | (g.tm_min << 5) | (g.tm_sec / 2));
	return 1;
}

static int cmpstr(const void *a, const void *b) { return strcmp(*(char **)a, *(char **)b); }

// A folder a page at a time, as zfsc.c (and test_zwalk.c's).
#define PAGE_N 16
static char *dnames[4096];
static uint8_t dtypes[4096];
static int dn, dnext;

static void dir_page(zfs_dir_t *d) {
	int k = dn - dnext < PAGE_N ? dn - dnext : PAGE_N;
	d->at = 0;
	d->len = (uint32_t)k;
	d->end = dnext + k >= dn;
}

int32_t zfs_opendir_at(zfs_dir_t *d, const char *path, void *buf, uint32_t cap) {
	DIR *h = opendir(hp(path));
	struct dirent *de;
	memset(d, 0, sizeof *d);
	d->buf = buf;
	d->cap = cap;
	for (int i = 0; i < dn; i++) free(dnames[i]);
	dn = dnext = 0;
	if (!h) return -2;
	while ((de = readdir(h)) && dn < 4096)
		if (strcmp(de->d_name, ".") && strcmp(de->d_name, ".."))
			dnames[dn++] = strdup(de->d_name);
	closedir(h);
	qsort(dnames, (size_t)dn, sizeof(char *), cmpstr);
	for (int i = 0; i < dn; i++) {
		char full[1200];
		struct stat st;
		snprintf(full, sizeof full, "%s/%s", hp(path), dnames[i]);
		dtypes[i] = stat(full, &st) == 0 && S_ISDIR(st.st_mode) ? Z_FS_TYPE_DIR : Z_FS_TYPE_FILE;
	}
	dir_page(d);
	return 0;
}

const z_fs_dirent_t *zfs_readdir(zfs_dir_t *d) {
	static union { z_fs_dirent_t e; char b[sizeof(z_fs_dirent_t) + 256]; } u;
	if (d->at >= d->len) {
		if (d->end) return NULL;
		dir_page(d);
		if (!d->len) return NULL;
	}
	int i = dnext++;
	d->at++;
	memset(&u.e, 0, sizeof u.e);
	u.e.namelen = (uint16_t)strlen(dnames[i]);
	memcpy(u.e.name, dnames[i], (size_t)u.e.namelen + 1);
	u.e.info.type = dtypes[i];
	return &u.e;
}

int32_t zfs_closedir_at(zfs_dir_t *d) { (void)d; return 0; }

// zwalk.c's older path, which a walk through zfs_readdir() never takes
int fs_list_into(const char *path, char *buf, uint32_t cap, uint8_t *types, uint32_t max,
	uint32_t *count, uint32_t *trunc) {
	(void)path; (void)buf; (void)cap; (void)types; (void)max;
	fprintf(stderr, "pack_host: fs_list_into called\n");
	*count = 0; *trunc = 0;
	return 0;
}

// -- pack.c's file reads --

static int32_t h_read(zf_io_t *io, void *buf, uint32_t n) {
	int r = fs_read_chunk((int)(intptr_t)io->h, buf, (int)n);
	if (r < 0) return -1;
	io->pos += (uint32_t)r;
	return r;
}

bool pack_open_file(const char *path, uint32_t size, zf_io_t *io) {
	int h = fs_open_read(path);
	if (h < 0) return false;
	memset(io, 0, sizeof *io);
	io->h = (void *)(intptr_t)h;
	io->read = h_read;
	io->size = size;
	return true;
}

void pack_close_file(zf_io_t *io) { fs_close_handle((int)(intptr_t)io->h); }

// -- the archive, to a host file --

static int32_t o_write(zf_io_t *io, const void *b, uint32_t n) { return (int32_t)fwrite(b, 1, n, (FILE *)io->h); }
static bool o_seek(zf_io_t *io, uint32_t pos) { io->pos = pos; return fseek((FILE *)io->h, (long)pos, SEEK_SET) == 0; }

static long cancel_after = -1, polls;
static bool cancel(void) { polls++; return cancel_after >= 0 && polls > cancel_after; }
static uint32_t last_pm;
static bool pm_ok = true;
static void progress(uint32_t pm) { if (pm < last_pm || pm > 1000) pm_ok = false; last_pm = pm; }

int main(int argc, char **argv) {
	uint32_t heap = 1024 * 1024;
	int i = 1;
	for (; i < argc && argv[i][0] == '-'; i++) {
		if (!strcmp(argv[i], "-c") && i + 1 < argc) cancel_after = atol(argv[++i]);
		else if (!strcmp(argv[i], "-m") && i + 1 < argc) heap = (uint32_t)atol(argv[++i]);
	}
	if (argc - i != 2 || !getenv("PACK_ROOT")) { fprintf(stderr, "usage: PACK_ROOT=DIR pack_host [-c N] [-m HEAP] DIR OUT\n"); return 2; }
	snprintf(root, sizeof root, "%s", getenv("PACK_ROOT"));
	zf_heap = malloc(heap);
	zf_heap_size = heap;
	zf_cancel_poll = cancel;
	zf_progress = progress;
	FILE *f = fopen(hp(argv[i + 1]), "w+b");
	if (!f) { perror(argv[i + 1]); return 2; }
	zf_io_t out;
	memset(&out, 0, sizeof out);
	out.h = f;
	out.write = o_write;
	out.seek = o_seek;
	const char *skip[2] = { argv[i + 1], NULL };
	int r = pack_folder(argv[i], zf_detect(argv[i + 1], NULL), &out, skip);
	fclose(f);
	if (opened) { fprintf(stderr, "pack_host: %d files left open\n", opened); return 3; }
	if (!pm_ok) { fprintf(stderr, "pack_host: progress went backwards or past 1000\n"); return 3; }
	if (r < 0) { fprintf(stderr, "pack_host: %s\n", zf_strerror(r)); return 1; }
	printf("skipped %u\n", (unsigned)pack_skipped);
	return 0;
}
