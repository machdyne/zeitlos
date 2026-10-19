/*
 * Host test for sw/common/zwalk.c: find, copy and delete over a real
 * directory tree under /tmp, through a small host version of the
 * zfsapp calls -- including cancelling after every possible step.
 *
 *   cc -std=gnu99 -Wall -I sw/common -o /tmp/test_zwalk \
 *      sw/common/tests/test_zwalk.c sw/common/zwalk.c
 *   /tmp/test_zwalk
 *
 * The host fs answers as the kernel does where it matters: an empty
 * folder LISTS AS A FAILURE (k_fs_list(); zflist.c), fs_unlink()
 * removes a file or an empty folder, names come back as full paths,
 * and a listing is cut short (truncated) at the caller's limits.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

#include "zfs.h"
#include "zwalk.h"

// -- the host fs --

static char root[200];
static int calls;		// filesystem calls made, to check "one per step"

static const char *hp(const char *p) {
	static char b[2][1024];
	static int k;
	k ^= 1;
	snprintf(b[k], sizeof(b[k]), "%s%s", root, p);
	return b[k];
}

#define MAXH 8
static FILE *hs[MAXH];
static int fail_write_after = -1;	// >= 0: writes fail after this many

int fs_open_read(const char *p) {
	calls++;
	FILE *f = fopen(hp(p), "rb");
	if (!f) return -1;
	for (int i = 0; i < MAXH; i++) if (!hs[i]) { hs[i] = f; return i; }
	fclose(f);
	return -1;
}
int fs_open_write(const char *p) {
	calls++;
	FILE *f = fopen(hp(p), "wb");
	if (!f) return -1;
	for (int i = 0; i < MAXH; i++) if (!hs[i]) { hs[i] = f; return i; }
	fclose(f);
	return -1;
}
int fs_read_chunk(int h, void *b, int n) { calls++; return (int)fread(b, 1, (size_t)n, hs[h]); }
int fs_write_chunk(int h, const void *b, int n) {
	calls++;
	if (fail_write_after == 0) return -1;
	if (fail_write_after > 0) fail_write_after--;
	return (int)fwrite(b, 1, (size_t)n, hs[h]);
}
int fs_close_handle(int h) { fclose(hs[h]); hs[h] = NULL; return 0; }
int fs_unlink(char *p) { calls++; return remove(hp(p)) == 0; }
int fs_mkdir(const char *p) { calls++; return mkdir(hp(p), 0777) == 0; }

int fs_stat(const char *p, z_fs_info_t *fi) {
	struct stat st;
	calls++;
	if (stat(hp(p), &st)) return 0;
	memset(fi, 0, sizeof(*fi));
	fi->type = S_ISDIR(st.st_mode) ? Z_FS_TYPE_DIR : Z_FS_TYPE_FILE;
	fi->size = S_ISDIR(st.st_mode) ? 0 : (uint32_t)st.st_size;
	return 1;
}

static int cmpstr(const void *a, const void *b) { return strcmp(*(char **)a, *(char **)b); }

int fs_list_into(const char *path, char *buf, uint32_t cap, uint8_t *types,
	uint32_t max, uint32_t *count, uint32_t *trunc) {
	DIR *d = opendir(hp(path));
	struct dirent *de;
	char *names[1024];
	int nn = 0;
	uint32_t used = 0, n = 0;
	calls++;
	*count = 0; *trunc = 0;
	if (!d) return 0;
	while ((de = readdir(d)) && nn < 1024)
		if (strcmp(de->d_name, ".") && strcmp(de->d_name, ".."))
			names[nn++] = strdup(de->d_name);
	closedir(d);
	qsort(names, (size_t)nn, sizeof(char *), cmpstr);	// a stable order, like FAT's
	for (int i = 0; i < nn; i++) {
		char e[600], full[1200];
		struct stat st;
		if (n >= max) { *trunc = 1; break; }
		snprintf(e, sizeof(e), "%s%s%s", path,
			path[strlen(path) - 1] == '/' ? "" : "/", names[i]);
		size_t l = strlen(e) + 1;
		if (used + l > cap) { *trunc = 1; break; }
		memcpy(buf + used, e, l);
		used += (uint32_t)l;
		snprintf(full, sizeof(full), "%s/%s", hp(path), names[i]);
		types[n++] = (stat(full, &st) == 0 && S_ISDIR(st.st_mode)) ? Z_FS_TYPE_DIR : Z_FS_TYPE_FILE;
	}
	for (int i = 0; i < nn; i++) free(names[i]);
	*count = n;
	return n > 0;		// the kernel: an empty folder is a failure
}

// -- helpers --

static int checks, failures;
static void expect(bool ok, const char *what) {
	checks++;
	if (!ok) { failures++; printf("  FAIL: %s\n", what); }
}

static void put(const char *p, const char *text) {
	FILE *f = fopen(hp(p), "wb");
	fputs(text, f);
	fclose(f);
}
static void dir(const char *p) { mkdir(hp(p), 0777); }
static bool exists(const char *p) { struct stat st; return stat(hp(p), &st) == 0; }

static void big(const char *p, int n) {
	FILE *f = fopen(hp(p), "wb");
	for (int i = 0; i < n; i++) fputc("abcdefghij\n"[i % 11], f);
	fclose(f);
}

static void tree(void) {
	char cmd[700];
	snprintf(cmd, sizeof(cmd), "rm -rf %s && mkdir -p %s", root, root);
	if (system(cmd)) exit(2);
	dir("/a");
	put("/a/readme.md", "Hello world\n");
	put("/a/notes.txt", "buy milk\nTODO: call\n");
	dir("/a/sub");
	put("/a/sub/deep.md", "nothing here\n");
	dir("/a/sub/empty");
	dir("/a/sub/x");
	big("/a/sub/x/big.bin", 10000);
	// the needle straddles a 2048-byte chunk boundary
	{
		FILE *f = fopen(hp("/a/sub/x/straddle.txt"), "wb");
		for (int i = 0; i < 2044; i++) fputc('.', f);
		fputs("NeedLE found", f);
		fclose(f);
	}
}

static bool same_tree(const char *a, const char *b) {
	char cmd[1200];
	snprintf(cmd, sizeof(cmd), "diff -r %s%s %s%s >/dev/null", root, a, root, b);
	return system(cmd) == 0;
}

static uint8_t mem[16384];

// Runs to the end; collects hits as "path\n".
static int run(zwalk_t *w, char *hits, int cap, int *maxcalls) {
	int r, n = 0;
	if (hits) hits[0] = 0;
	*maxcalls = 0;
	for (int guard = 0; guard < 100000; guard++) {
		int c0 = calls;
		r = zw_step(w);
		if (calls - c0 > *maxcalls) *maxcalls = calls - c0;
		if (r == ZW_HIT && hits) n += snprintf(hits + n, (size_t)(cap - n), "%s\n", w->hit);
		if (r == ZW_DONE || r == ZW_FAIL) return r;
	}
	return -1;
}

int main(void) {

	zwalk_t w;
	char hits[4096];
	int mc;

	snprintf(root, sizeof(root), "/tmp/test_zwalk.%d", (int)getpid());

	printf("names\n");
	expect(zw_name_match("*.md", true, "README.MD"), "glob, case-insensitive");
	expect(!zw_name_match("*.md", true, "readme.mdx"), "glob anchors the end");
	expect(zw_name_match("r?ad*", true, "readme.md"), "? and *");
	expect(zw_name_match("*a*b*c", true, "xxaxxbxxc"), "several stars");
	expect(zw_name_match("note", false, "MyNotes.txt"), "a part of the name");
	expect(!zw_name_match("note", false, "nte.txt"), "not a part");
	expect(zw_name_match("", false, "x"), "empty matches all");
	expect(zw_path_inside("/a/sub", "/a"), "inside");
	expect(zw_path_inside("/A", "/a"), "itself, case-insensitive");
	expect(!zw_path_inside("/ab", "/a"), "a sibling is not inside");
	expect(zw_path_inside("/x", "/"), "everything is inside /");

	printf("find by name\n");
	tree();
	expect(zw_start(&w, ZW_FIND, "/a", NULL, mem, sizeof(mem)), "start");
	zw_find_set(&w, "*.md", "");
	expect(run(&w, hits, sizeof(hits), &mc) == ZW_DONE, "done");
	expect(!strcmp(hits, "/a/readme.md\n/a/sub/deep.md\n"), "both .md files, in order");
	expect(mc <= 1, "one fs call per step");

	printf("find a folder by name\n");
	zw_start(&w, ZW_FIND, "/a/", NULL, mem, sizeof(mem));
	zw_find_set(&w, "empt", "");
	run(&w, hits, sizeof(hits), &mc);
	expect(!strcmp(hits, "/a/sub/empty\n"), "the empty folder");

	printf("find text\n");
	zw_start(&w, ZW_FIND, "/a", NULL, mem, sizeof(mem));
	zw_find_set(&w, "", "needle");
	run(&w, hits, sizeof(hits), &mc);
	expect(!strcmp(hits, "/a/sub/x/straddle.txt\n"), "across a chunk boundary, any case");
	expect(w.bytes > 10000, "it read the files");
	zw_start(&w, ZW_FIND, "/a", NULL, mem, sizeof(mem));
	zw_find_set(&w, "*.txt", "todo");
	run(&w, hits, sizeof(hits), &mc);
	expect(!strcmp(hits, "/a/notes.txt\n"), "name and text together");

	printf("find from a missing folder\n");
	expect(!zw_start(&w, ZW_FIND, "/nope", NULL, mem, sizeof(mem)) && w.err == ZW_E_NOTFOUND, "not found");

	printf("copy a tree\n");
	expect(zw_start(&w, ZW_COPY, "/a", "/b", mem, sizeof(mem)), "start");
	expect(run(&w, NULL, 0, &mc) == ZW_DONE, "done");
	expect(same_tree("/a", "/b"), "identical, the empty folder too");
	expect(exists("/b/sub/empty"), "empty folder copied");
	expect(w.files == 5 && w.dirs == 4, "5 files, 4 folders");
	expect(mc <= 2, "a step is one call, or a chunk read and written");

	printf("copy refusals\n");
	expect(!zw_start(&w, ZW_COPY, "/a", "/b", mem, sizeof(mem)) && w.err == ZW_E_EXISTS, "destination exists");
	expect(!zw_start(&w, ZW_COPY, "/a", "/a/sub/c", mem, sizeof(mem)) && w.err == ZW_E_INSIDE, "into itself");
	expect(!zw_start(&w, ZW_COPY, "/a", "/c", mem, 100) && w.err == ZW_E_MEMORY, "too little memory");

	printf("copy one file\n");
	zw_start(&w, ZW_COPY, "/a/sub/x/big.bin", "/big2.bin", mem, sizeof(mem));
	expect(run(&w, NULL, 0, &mc) == ZW_DONE, "done");
	{
		char cmd[1200];
		snprintf(cmd, sizeof(cmd), "cmp -s %s/a/sub/x/big.bin %s/big2.bin", root, root);
		expect(system(cmd) == 0, "same bytes");
	}

	printf("cancel a copy after every step\n");
	{
		int total = 0, bad = 0;
		// how many steps a whole copy takes
		zw_start(&w, ZW_COPY, "/a", "/c", mem, sizeof(mem));
		while (zw_step(&w) == ZW_MORE) total++;
		for (int k = 0; k <= total; k++) {
			char cmd[700];
			snprintf(cmd, sizeof(cmd), "rm -rf %s/c", root);
			if (system(cmd)) return 2;
			zw_start(&w, ZW_COPY, "/a", "/c", mem, sizeof(mem));
			for (int s = 0; s < k; s++) zw_step(&w);
			bool writing = w.infile;
			char half[Z_FS_PATH_MAX];
			strcpy(half, w.dst);
			zw_cancel(&w);
			if (zw_step(&w) != ZW_DONE) bad++;
			if (writing && exists(half)) bad++;		// no half-written file
			for (int h = 0; h < MAXH; h++) if (hs[h]) bad++;
			if (k > 0 && !w.partial != !exists("/c")) bad++;
		}
		printf("  %d cancel points\n", total + 1);
		expect(!bad, "nothing open, no half file, partial says what is left");
	}

	printf("a write fails\n");
	{
		char cmd[700];
		snprintf(cmd, sizeof(cmd), "rm -rf %s/c", root);
		if (system(cmd)) return 2;
		zw_start(&w, ZW_COPY, "/a", "/c", mem, sizeof(mem));
		fail_write_after = 2;
		int r = run(&w, NULL, 0, &mc);
		fail_write_after = -1;
		expect(r == ZW_FAIL && w.err == ZW_E_WRITE, "stops with a write error");
		expect(!exists(w.hit + 0) || strncmp(w.hit, "/a", 2) == 0, "names the source");
		bool open = false;
		for (int h = 0; h < MAXH; h++) if (hs[h]) open = true;
		expect(!open, "nothing left open");
	}

	printf("delete a tree\n");
	expect(zw_start(&w, ZW_DELETE, "/b", NULL, mem, sizeof(mem)), "start");
	expect(run(&w, NULL, 0, &mc) == ZW_DONE, "done");
	expect(!exists("/b"), "gone, folder and all");
	expect(w.files == 5 && w.dirs == 4, "5 files, 4 folders");
	expect(mc <= 1, "one call per step");

	printf("delete with a small block (listings cut short)\n");
	{
		dir("/many");
		for (int i = 0; i < 300; i++) {
			char p[64];
			snprintf(p, sizeof(p), "/many/file%03d.txt", i);
			put(p, "x");
		}
		static uint8_t small[2112 + 1200];
		expect(zw_start(&w, ZW_DELETE, "/many", NULL, small, sizeof(small)), "start");
		expect(run(&w, NULL, 0, &mc) == ZW_DONE, "done");
		expect(!exists("/many") && w.files == 300, "all 300 and the folder");
	}

	printf("find with a small block says it was cut short\n");
	{
		dir("/many");
		for (int i = 0; i < 300; i++) {
			char p[64];
			snprintf(p, sizeof(p), "/many/file%03d.txt", i);
			put(p, "x");
		}
		static uint8_t small[2112 + 1200];
		zw_start(&w, ZW_FIND, "/many", NULL, small, sizeof(small));
		zw_find_set(&w, "", "");
		run(&w, NULL, 0, &mc);
		expect(w.truncated && w.files < 300, "truncated");
	}

	printf("delete one file\n");
	zw_start(&w, ZW_DELETE, "/big2.bin", NULL, mem, sizeof(mem));
	expect(run(&w, NULL, 0, &mc) == ZW_DONE && !exists("/big2.bin"), "gone");

	{
		char cmd[700];
		snprintf(cmd, sizeof(cmd), "rm -rf %s", root);
		if (system(cmd)) return 2;
	}

	printf("test_zwalk: %d checks, %d failed\n", checks, failures);
	return failures ? 1 : 0;

}
