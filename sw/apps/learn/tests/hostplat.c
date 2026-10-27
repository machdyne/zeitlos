/*
 * learn -- the platform on the build machine (lplat.h): the card is a
 * folder (HOSTPLAT_ROOT, set by the test), speech and boxes are scripted.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#include "hostplat.h"

char hp_root[512] = "/tmp/learn_card";
uint32_t hp_now = 1793000000u;			// 2026-10-26
uint32_t hp_ms_now;
char hp_said[2048];						// the last thing spoken
char hp_prompt_answer[128] = "";		// what the next prompt returns
bool hp_prompt_ok = true;
int hp_confirm_answer = 1;
char hp_ran[128];						// the last app run

static void map(const char *path, char *out, size_t cap) {
	if (path[0] == '/') snprintf(out, cap, "%s%s", hp_root, path);
	else snprintf(out, cap, "%s", path);
}

static bool h_list(void *ctx, const char *dir, void (*fn)(void *, const char *, bool), void *arg) {
	(void)ctx;
	char d[1024];
	map(dir, d, sizeof d);
	DIR *dp = opendir(d);
	if (!dp) return false;
	struct dirent *e;
	while ((e = readdir(dp))) {
		if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
		char p[1400];
		struct stat st;
		snprintf(p, sizeof p, "%s/%s", d, e->d_name);
		fn(arg, e->d_name, !stat(p, &st) && S_ISDIR(st.st_mode));
	}
	closedir(dp);
	return true;
}

static long h_read_at(const char *path, long off, char *buf, long cap) {
	char p[1024];
	map(path, p, sizeof p);
	FILE *f = fopen(p, "rb");
	if (!f) return -1;
	struct stat st;
	if (!stat(p, &st) && S_ISDIR(st.st_mode)) { fclose(f); return -1; }
	fseek(f, off, SEEK_SET);
	long n = cap > 0 ? (long)fread(buf, 1, (size_t)cap, f) : 0;
	fclose(f);
	return n;
}

static long h_read(void *ctx, const char *path, char *buf, long cap) { (void)ctx; return h_read_at(path, 0, buf, cap); }

static bool h_write_whole(const char *path, const char *data, long len) {
	char p[1024], part[1100];
	map(path, p, sizeof p);
	snprintf(part, sizeof part, "%s.part", p);
	FILE *f = fopen(part, "wb");
	if (!f) return false;
	bool ok = fwrite(data, 1, (size_t)len, f) == (size_t)len;
	ok = !fclose(f) && ok;
	return ok && !rename(part, p);
}

static bool h_append(const char *path, const char *data, long len) {
	char p[1024];
	map(path, p, sizeof p);
	FILE *f = fopen(p, "ab");
	if (!f) return false;
	bool ok = fwrite(data, 1, (size_t)len, f) == (size_t)len;
	return !fclose(f) && ok;
}

static bool h_mkdir(const char *path) {
	char p[1024];
	map(path, p, sizeof p);
	mkdir(p, 0777);
	struct stat st;
	return !stat(p, &st) && S_ISDIR(st.st_mode);
}

static bool h_exists(const char *path) {
	char p[1024];
	struct stat st;
	map(path, p, sizeof p);
	return !stat(p, &st);
}

static uint32_t h_now(void) { return hp_now; }
static uint32_t h_ms(void) { return hp_ms_now; }
static void h_speak(const char *t) { snprintf(hp_said, sizeof hp_said, "%s", t); }
static void h_stop(void) { }
static bool h_prompt(const char *title, const char *msg, const char *initial, char *out, int cap) {
	(void)title; (void)msg; (void)initial;
	snprintf(out, (size_t)cap, "%s", hp_prompt_answer);
	return hp_prompt_ok;
}
static bool h_confirm(const char *title, const char *msg) { (void)title; (void)msg; return hp_confirm_answer; }
static bool h_run(const char *app, const char *arg) { snprintf(hp_ran, sizeof hp_ran, "%s %s", app, arg ? arg : ""); return true; }

const lplat_t host_plat = {
	{ h_list, h_read, NULL },
	h_read_at, h_write_whole, h_append, h_mkdir, h_exists,
	h_now, h_ms, h_speak, h_stop, h_prompt, h_confirm, h_run,
};

void hp_reset_card(void) {
	char cmd[1700];
	snprintf(cmd, sizeof cmd, "rm -rf '%s' && mkdir -p '%s/data' '%s/opt/ark'", hp_root, hp_root, hp_root);
	if (system(cmd)) fprintf(stderr, "hostplat: cannot reset %s\n", hp_root);
}
