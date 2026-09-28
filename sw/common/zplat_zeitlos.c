/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * The platform (zplat.h), on Zeitlos: the card through zfsapp.h, the
 * RTC, the TRNG, the console. Shared by the BBS and fed.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "zeitlos.h"
#include "zfs.h"
#include "zfsapp.h"
#include "zrtc.h"
#include "zrng.h"
#include "zplat.h"

uint32_t plat_now(void) {
	// No clock yet (no NTP answer, no battery): 0, which the core shows
	// as "never" and stores as "unknown" rather than as 1970.
	if (!z_rtc_available() || !z_rtc_valid()) return 0;
	return z_rtc_seconds();
}

uint32_t plat_ms(void) {
	return (uint32_t)((uint64_t)z_uptime_ticks() * 1000u / Z_TICK_HZ);
}

void plat_random(void *buf, uint32_t n) {
	z_rng_bytes(buf, n);
}

void plat_log(const char *line) {
	puts(line);
}

int plat_open(const char *path, int mode) {
	if (mode == PLAT_READ) return fs_open_read(path);
	if (mode == PLAT_UPDATE) return fs_open_rw(path);
	return fs_open_write(path);
}

int plat_read(int h, void *buf, int n) {
	int got = 0;
	while (got < n) {
		int k = fs_read_chunk(h, (char *)buf + got, n - got);
		if (k < 0) return got ? got : -1;
		if (k == 0) break;
		got += k;
	}
	return got;
}

int plat_write(int h, const void *buf, int n) {
	int put = 0;
	while (put < n) {
		int k = fs_write_chunk(h, (const char *)buf + put, n - put);
		if (k <= 0) return put ? put : -1;
		put += k;
	}
	return put;
}

bool plat_sync(int h) {
	return fs_sync(h) == 1;
}

bool plat_seek(int h, uint32_t off) {
	return fs_seek(h, off) == 1;
}

void plat_close(int h) {
	if (h >= 0) fs_close_handle(h);
}

// fs_size() says 0 for a file that does not exist; the core needs to
// tell that from an empty one.
int32_t plat_size(const char *path) {
	z_fs_info_t fi;
	if (fs_stat(path, &fi) != 1 || fi.type != Z_FS_TYPE_FILE) return -1;
	return (int32_t)fi.size;
}

uint32_t plat_mtime(const char *path) {
	z_fs_info_t fi;
	if (fs_stat(path, &fi) != 1) return 0;
	return z_fs_info_time(&fi);
}

bool plat_mkdir(const char *path) {
	z_fs_info_t fi;
	if (fs_stat(path, &fi) == 1) return fi.type == Z_FS_TYPE_DIR;
	return fs_mkdir(path) == 1;
}

// FAT will not rename over an existing file: the old one goes first.
bool plat_rename(const char *from, const char *to) {
	int err = 0;
	fs_unlink((char *)to);
	return fs_rename(from, to, &err) == 1;
}

bool plat_unlink(const char *path) {
	return fs_unlink((char *)path) == 1;
}

static int cmp(const void *a, const void *b) {
	return strcmp(*(char *const *)a, *(char *const *)b);
}

// fs_list_into() gives "/"-prefixed full paths, unsorted: names only,
// sorted, for the core.
int plat_list(const char *dir, char *buf, uint32_t cap) {
	static char raw[2048];
	static uint8_t types[64];
	char *names[64];
	uint32_t count = 0, trunc = 0;
	if (fs_list_into(dir, raw, sizeof(raw), types, 64, &count, &trunc) != 1) return -1;
	int n = 0;
	char *p = raw;
	for (uint32_t i = 0; i < count && n < 64; i++) {
		char *slash = strrchr(p, '/');
		if (types[i] == Z_FS_TYPE_FILE) names[n++] = slash ? slash + 1 : p;
		p += strlen(p) + 1;
	}
	qsort(names, (size_t)n, sizeof(names[0]), cmp);
	uint32_t o = 0;
	int k = 0;
	for (int i = 0; i < n; i++) {
		size_t l = strlen(names[i]) + 1;
		if (o + l > cap) break;
		memcpy(buf + o, names[i], l);
		o += (uint32_t)l;
		k++;
	}
	return k;
}
