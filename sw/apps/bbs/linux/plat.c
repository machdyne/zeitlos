/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * bbs -- the platform, on Linux (and any POSIX system): files, the
 * clock, randomness, the log. The host tests link this too.
 */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <errno.h>
#include <time.h>
#include <sys/stat.h>
#include "../core/bbs.h"

// Test hooks: a clock the tests can move.
int plat_fake_time;
uint32_t plat_fake_now, plat_fake_ms;
int plat_quiet;

uint32_t plat_now(void) {
	if (plat_fake_time) return plat_fake_now;
	return (uint32_t)time(NULL);
}

uint32_t plat_ms(void) {
	if (plat_fake_time) return plat_fake_ms;
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint32_t)((uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u);
}

void plat_random(void *buf, uint32_t n) {
	static int fd = -1;
	if (fd < 0) fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
	uint8_t *p = buf;
	while (n) {
		ssize_t k = fd >= 0 ? read(fd, p, n) : -1;
		if (k <= 0) { fprintf(stderr, "bbs: no /dev/urandom\n"); abort(); }
		p += k;
		n -= (uint32_t)k;
	}
}

void plat_log(const char *line) {
	if (plat_quiet) return;
	char ts[32];
	time_t t = time(NULL);
	struct tm tm;
	gmtime_r(&t, &tm);
	strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", &tm);
	fprintf(stderr, "%s %s\n", ts, line);
}

int plat_open(const char *path, int mode) {
	int fl = mode == PLAT_READ ? O_RDONLY : mode == PLAT_UPDATE ? O_RDWR : (O_WRONLY | O_CREAT | O_TRUNC);
	return open(path, fl | O_CLOEXEC, 0644);
}

int plat_read(int h, void *buf, int n) {
	int got = 0;
	while (got < n) {
		ssize_t k = read(h, (char *)buf + got, (size_t)(n - got));
		if (k < 0 && errno == EINTR) continue;
		if (k < 0) return got ? got : -1;
		if (k == 0) break;
		got += (int)k;
	}
	return got;
}

int plat_write(int h, const void *buf, int n) {
	int put = 0;
	while (put < n) {
		ssize_t k = write(h, (const char *)buf + put, (size_t)(n - put));
		if (k < 0 && errno == EINTR) continue;
		if (k <= 0) return put ? put : -1;
		put += (int)k;
	}
	return put;
}

bool plat_seek(int h, uint32_t off) {
	return lseek(h, (off_t)off, SEEK_SET) == (off_t)off;
}

void plat_close(int h) {
	if (h >= 0) close(h);
}

int32_t plat_size(const char *path) {
	struct stat st;
	if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) return -1;
	return (int32_t)st.st_size;
}

uint32_t plat_mtime(const char *path) {
	struct stat st;
	if (stat(path, &st) != 0) return 0;
	return (uint32_t)st.st_mtime;
}

bool plat_mkdir(const char *path) {
	struct stat st;
	if (mkdir(path, 0755) == 0) return true;
	return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

bool plat_rename(const char *from, const char *to) {
	return rename(from, to) == 0;
}

bool plat_unlink(const char *path) {
	return unlink(path) == 0;
}

static int cmp(const void *a, const void *b) {
	return strcmp(*(char *const *)a, *(char *const *)b);
}

int plat_list(const char *dir, char *buf, uint32_t cap) {
	DIR *d = opendir(dir);
	if (!d) return -1;
	char *names[256];
	int n = 0;
	struct dirent *e;
	while ((e = readdir(d)) && n < 256) {
		if (e->d_name[0] == '.') continue;
		names[n] = strdup(e->d_name);
		if (names[n]) n++;
	}
	closedir(d);
	qsort(names, (size_t)n, sizeof(names[0]), cmp);
	uint32_t o = 0;
	int k = 0;
	for (int i = 0; i < n; i++) {
		size_t l = strlen(names[i]) + 1;
		if (o + l <= cap) { memcpy(buf + o, names[i], l); o += (uint32_t)l; k++; }
		free(names[i]);
	}
	return k;
}
