/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * learn on Linux -- files, clock and speech (lplat_linux.h).
 */

#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <dirent.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>

#include "lplat_linux.h"
#include "zpaths.h"

// -- the places --

#define MAX_PACKS 32
static struct { char name[64]; char dir[PATH_MAX]; } packs[MAX_PACKS];
static int npacks;
static char data_dir[PATH_MAX];

static bool is_dir(const char *p) { struct stat st; return !stat(p, &st) && S_ISDIR(st.st_mode); }

static bool has_course_cfg(const char *dir) {
	char p[PATH_MAX * 2];
	if (snprintf(p, sizeof p, "%s/course.cfg", dir) >= (int)sizeof p) return false;
	return access(p, R_OK) == 0;
}

// a folder holding at least one course
static bool is_pack(const char *dir) {
	DIR *d = opendir(dir);
	if (!d) return false;
	struct dirent *e;
	bool yes = false;
	while (!yes && (e = readdir(d))) {
		if (e->d_name[0] == '.') continue;
		char p[PATH_MAX * 2];
		snprintf(p, sizeof p, "%s/%s", dir, e->d_name);
		if (is_dir(p) && has_course_cfg(p)) yes = true;
	}
	closedir(d);
	return yes;
}

// mkdir -p
static bool make_dirs(const char *dir) {
	char p[PATH_MAX];
	snprintf(p, sizeof p, "%s", dir);
	for (char *s = p + 1; *s; s++) {
		if (*s != '/') continue;
		*s = 0;
		mkdir(p, 0755);
		*s = '/';
	}
	mkdir(p, 0755);
	return is_dir(p);
}

bool ll_set_data(const char *dir) {
	snprintf(data_dir, sizeof data_dir, "%s", dir);
	size_t n = strlen(data_dir);
	while (n > 1 && data_dir[n - 1] == '/') data_dir[--n] = 0;
	return make_dirs(data_dir);
}

const char *ll_data(void) { return data_dir; }

// a pack under a name learn's catalogue reads: it takes folders of
// /opt/ark whose name starts with "edu" (lcat.c), and a name once
static void add_pack(const char *dir) {
	if (npacks >= MAX_PACKS) return;
	for (int i = 0; i < npacks; i++) if (!strcmp(packs[i].dir, dir)) return;
	const char *base = strrchr(dir, '/');
	base = base && base[1] ? base + 1 : dir;
	char name[64];
	if (!strncmp(base, "edu", 3)) snprintf(name, sizeof name, "%.60s", base);
	else snprintf(name, sizeof name, "edu-%.56s", base);
	for (char *c = name; *c; c++) if (*c == ' ' || *c == '/') *c = '-';
	char try[64];
	snprintf(try, sizeof try, "%s", name);
	for (int k = 2; ; k++) {
		bool taken = false;
		for (int i = 0; i < npacks; i++) if (!strcmp(packs[i].name, try)) taken = true;
		if (!taken) break;
		snprintf(try, sizeof try, "%.50s-%d", name, k % 1000);
	}
	snprintf(packs[npacks].name, sizeof packs[npacks].name, "%s", try);
	snprintf(packs[npacks].dir, sizeof packs[npacks].dir, "%s", dir);
	npacks++;
}

int ll_add_folder(const char *dir, char *open, int cap) {
	if (open && cap) open[0] = 0;
	char real[PATH_MAX];
	if (!realpath(dir, real) || !is_dir(real)) return 0;
	int before = npacks;
	if (has_course_cfg(real)) {
		// one course: its folder's folder is its pack (its lib/ too), and it opens
		char parent[PATH_MAX];
		snprintf(parent, sizeof parent, "%s", real);
		char *slash = strrchr(parent, '/');
		if (!slash || slash == parent) return 0;
		*slash = 0;
		add_pack(parent);
		for (int i = 0; i < npacks; i++)
			if (!strcmp(packs[i].dir, parent) && open)
				snprintf(open, (size_t)cap, "%s/%s/%.200s", Z_DIR_ARK, packs[i].name, slash + 1);
		return npacks - before;
	}
	if (is_pack(real)) { add_pack(real); return npacks - before; }
	// a folder of packs: each of its folders that holds courses
	DIR *d = opendir(real);
	if (!d) return 0;
	char names[64][256];
	int n = 0;
	struct dirent *e;
	while ((e = readdir(d)) && n < 64) {
		if (e->d_name[0] == '.') continue;
		snprintf(names[n++], sizeof names[0], "%s", e->d_name);
	}
	closedir(d);
	for (int i = 1; i < n; i++)		// in name order: a course in two packs, the first wins (docs/learn.md)
		for (int j = i; j > 0 && strcmp(names[j - 1], names[j]) > 0; j--) {
			char t[256];
			memcpy(t, names[j], sizeof t);
			memcpy(names[j], names[j - 1], sizeof t);
			memcpy(names[j - 1], t, sizeof t);
		}
	for (int i = 0; i < n; i++) {
		char p[PATH_MAX * 2];
		if (snprintf(p, sizeof p, "%s/%s", real, names[i]) >= (int)sizeof p) continue;
		if (is_dir(p) && is_pack(p)) add_pack(p);
	}
	return npacks - before;
}

int ll_npacks(void) { return npacks; }
const char *ll_pack_name(int i) { return i >= 0 && i < npacks ? packs[i].name : ""; }
const char *ll_pack_dir(int i) { return i >= 0 && i < npacks ? packs[i].dir : ""; }

// learn's path as this computer's; false if it is nowhere here
static bool map(const char *path, char *out, size_t cap) {
	size_t ld = strlen(Z_DIR_LEARN_DATA), la = strlen(Z_DIR_ARK);
	if (!strncmp(path, Z_DIR_LEARN_DATA, ld) && (path[ld] == 0 || path[ld] == '/')) {
		return data_dir[0] && snprintf(out, cap, "%s%s", data_dir, path + ld) < (int)cap;
	}
	if (!strncmp(path, Z_DIR_ARK "/", la + 1)) {
		const char *name = path + la + 1;
		size_t nl = strcspn(name, "/");
		for (int i = 0; i < npacks; i++)
			if (strlen(packs[i].name) == nl && !strncmp(packs[i].name, name, nl)) {
				return snprintf(out, cap, "%s%s", packs[i].dir, name + nl) < (int)cap;
			}
	}
	return false;
}

static bool p_list(void *ctx, const char *dir, void (*fn)(void *, const char *, bool), void *arg) {
	(void)ctx;
	if (!strcmp(dir, Z_DIR_ARK) || !strcmp(dir, Z_DIR_ARK "/")) {
		for (int i = 0; i < npacks; i++) fn(arg, packs[i].name, true);
		return true;
	}
	char d[PATH_MAX];
	if (!map(dir, d, sizeof d)) return false;
	DIR *dp = opendir(d);
	if (!dp) return false;
	struct dirent *e;
	while ((e = readdir(dp))) {
		if (e->d_name[0] == '.') continue;		// (. and .., and hidden files: .git)
		char p[PATH_MAX + 256];
		snprintf(p, sizeof p, "%s/%s", d, e->d_name);
		fn(arg, e->d_name, is_dir(p));
	}
	closedir(dp);
	return true;
}

static long p_read_at(const char *path, long off, char *buf, long cap) {
	char p[PATH_MAX];
	if (!map(path, p, sizeof p) || is_dir(p)) return -1;
	int fd = open(p, O_RDONLY);
	if (fd < 0) return -1;
	long got = 0;
	while (got < cap) {
		ssize_t n = pread(fd, buf + got, (size_t)(cap - got), (off_t)(off + got));
		if (n < 0 && errno == EINTR) continue;
		if (n <= 0) break;
		got += n;
	}
	close(fd);
	return got;
}

static long p_read(void *ctx, const char *path, char *buf, long cap) {
	(void)ctx;
	if (!buf || cap <= 0) { char p[PATH_MAX]; struct stat st; return map(path, p, sizeof p) && !stat(p, &st) ? 0 : -1; }
	return p_read_at(path, 0, buf, cap);
}

static bool write_all(int fd, const char *data, long len) {
	while (len > 0) {
		ssize_t n = write(fd, data, (size_t)len);
		if (n < 0 && errno == EINTR) continue;
		if (n <= 0) return false;
		data += n;
		len -= n;
	}
	return true;
}

// the whole file or nothing: NAME.part, written and synced, then renamed
static bool p_write_whole(const char *path, const char *data, long len) {
	char p[PATH_MAX], part[PATH_MAX + 8];
	if (!map(path, p, sizeof p)) return false;
	snprintf(part, sizeof part, "%s.part", p);
	int fd = open(part, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (fd < 0) return false;
	bool ok = write_all(fd, data, len);
	ok = fsync(fd) == 0 && ok;
	ok = close(fd) == 0 && ok;
	if (!ok || rename(part, p)) { unlink(part); return false; }
	return true;
}

// a line added: one write, synced (a line cut off by a crash is not a record)
static bool p_append(const char *path, const char *data, long len) {
	char p[PATH_MAX];
	if (!map(path, p, sizeof p)) return false;
	int fd = open(p, O_WRONLY | O_CREAT | O_APPEND, 0644);
	if (fd < 0) return false;
	bool ok = write_all(fd, data, len);
	ok = fsync(fd) == 0 && ok;
	return close(fd) == 0 && ok;
}

static bool p_mkdir(const char *path) {
	char p[PATH_MAX];
	if (!map(path, p, sizeof p)) return false;
	mkdir(p, 0755);
	return is_dir(p);
}

static bool p_exists(const char *path) {
	if (!strcmp(path, Z_DIR_ARK)) return true;
	char p[PATH_MAX];
	struct stat st;
	return map(path, p, sizeof p) && !stat(p, &st);
}

static uint32_t p_now(void) { return (uint32_t)time(NULL); }

static uint32_t p_ms(void) {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint32_t)((uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u);
}

// -- speech: espeak-ng, a process an utterance; a new one stops the last --

static bool speech_on = true;
static int speech_found = -1;		// -1 not looked for yet
static pid_t speaker;

static bool in_path(const char *prog) {
	const char *path = getenv("PATH");
	if (!path) path = "/usr/bin:/bin";
	char dir[PATH_MAX];
	while (*path) {
		size_t n = strcspn(path, ":");
		snprintf(dir, sizeof dir, "%.*s/%s", (int)n, path, prog);
		if (n && access(dir, X_OK) == 0) return true;
		path += n + (path[n] == ':');
	}
	return false;
}

void ll_speech(bool on) { speech_on = on; }

bool ll_speech_available(void) {
	if (speech_found < 0) speech_found = in_path("espeak-ng");
	return speech_on && speech_found;
}

static void p_speak_stop(void) {
	if (speaker > 0) {
		kill(-speaker, SIGTERM);	// (its process group: espeak-ng and anything it started)
		waitpid(speaker, NULL, 0);
		speaker = 0;
	}
}

static void p_speak(const char *text) {
	if (!ll_speech_available() || !text || !text[0]) return;
	p_speak_stop();
	pid_t pid = fork();
	if (pid < 0) return;
	if (pid == 0) {
		setpgid(0, 0);
		int null = open("/dev/null", O_RDWR);
		if (null >= 0) { dup2(null, 0); dup2(null, 1); dup2(null, 2); }
		execlp("espeak-ng", "espeak-ng", "--", text, (char *)NULL);
		_exit(127);
	}
	setpgid(pid, pid);
	speaker = pid;
}

// prompt, confirm and run: main.c's
lplat_t linux_plat = {
	{ p_list, p_read, NULL },
	p_read_at, p_write_whole, p_append, p_mkdir, p_exists,
	p_now, p_ms, p_speak, p_speak_stop, NULL, NULL, NULL,
};
