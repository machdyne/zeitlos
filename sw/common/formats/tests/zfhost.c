/*
 * Zeitlos -- sw/common/formats on the host.
 *
 *   zfhost IN OUT              convert, the types from the names
 *   zfhost -t docx IN OUT      ... or named
 *   zfhost -x IN DIR           extract an archive into DIR
 *   zfhost -d IN               print the detected type
 *   zfhost -c N IN OUT         cancel after N polls (a test of cancelling)
 *   zfhost -m BYTES ...        zf_heap size (default 1MB)
 *   zfhost -p DIR OUT          pack a folder: OUT.zip, .tar or .tar.gz/.tgz
 *
 * Built and driven by tests/run_tests.py, which makes fixtures with
 * python-docx, openpyxl, odfpy and ebooklib, converts them both ways,
 * and checks the results with the same libraries and with LibreOffice.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <errno.h>
#include <dirent.h>
#include <time.h>

#include "formats/zf.h"

static int32_t f_read(zf_io_t *io, void *buf, uint32_t n) {
	size_t k = fread(buf, 1, n, (FILE *)io->h);
	if (ferror((FILE *)io->h)) return -1;
	io->pos += (uint32_t)k;
	return (int32_t)k;
}
static int32_t f_write(zf_io_t *io, const void *buf, uint32_t n) {
	size_t k = fwrite(buf, 1, n, (FILE *)io->h);
	io->pos += (uint32_t)k;
	return (int32_t)k;
}
static bool f_seek(zf_io_t *io, uint32_t pos) {
	if (fseek((FILE *)io->h, (long)pos, SEEK_SET)) return false;
	io->pos = pos;
	return true;
}

static bool io_open(zf_io_t *io, const char *path, bool w) {
	memset(io, 0, sizeof *io);
	FILE *f = fopen(path, w ? "w+b" : "rb");
	if (!f) return false;
	io->h = f;
	io->read = f_read;
	io->write = f_write;
	io->seek = f_seek;
	if (!w) { fseek(f, 0, SEEK_END); io->size = (uint32_t)ftell(f); fseek(f, 0, SEEK_SET); }
	return true;
}

static char root[1024];

static bool d_open(zf_dir_t *d, const char *rel, zf_io_t *out) {
	char p[2048];
	(void)d;
	snprintf(p, sizeof p, "%s/%s", root, rel);
	return io_open(out, p, true);
}
static void d_close(zf_dir_t *d, zf_io_t *out) { (void)d; fclose((FILE *)out->h); }
static void d_remove(zf_dir_t *d, const char *rel) {
	char p[2048];
	(void)d;
	snprintf(p, sizeof p, "%s/%s", root, rel);
	remove(p);
}
static bool d_mkdir(zf_dir_t *d, const char *rel) {
	char p[2048];
	(void)d;
	snprintf(p, sizeof p, "%s/%s", root, rel);
	return !mkdir(p, 0755) || errno == EEXIST;
}

// -- packing: the folder walked here, as convert walks it on the board --

static zf_pack_t pk;
static int pack_err;

static int by_name(const struct dirent **a, const struct dirent **b) { return strcmp((*a)->d_name, (*b)->d_name); }

static void fat_date(time_t t, uint16_t *d, uint16_t *tm) {
	struct tm g;
	gmtime_r(&t, &g);
	*d = (uint16_t)(((g.tm_year - 80) << 9) | ((g.tm_mon + 1) << 5) | g.tm_mday);
	*tm = (uint16_t)((g.tm_hour << 11) | (g.tm_min << 5) | (g.tm_sec / 2));
}

static uint32_t tree_size(const char *path) {
	struct stat st;
	if (stat(path, &st)) return 0;
	if (!S_ISDIR(st.st_mode)) return (uint32_t)st.st_size;
	struct dirent **e;
	int n = scandir(path, &e, NULL, by_name);
	uint32_t t = 0;
	for (int i = 0; i < n; i++) {
		char p[2048];
		if (strcmp(e[i]->d_name, ".") && strcmp(e[i]->d_name, "..")) {
			snprintf(p, sizeof p, "%s/%s", path, e[i]->d_name);
			t += tree_size(p);
		}
		free(e[i]);
	}
	free(e);
	return t;
}

static void pack_tree(const char *path, const char *rel) {
	struct stat st;
	uint16_t d, t;
	if (pack_err || stat(path, &st)) return;
	fat_date(st.st_mtime, &d, &t);
	if (!S_ISDIR(st.st_mode)) {
		zf_io_t in;
		if (!io_open(&in, path, false)) { pack_err = ZF_E_IO; return; }
		pack_err = zf_pack_add(&pk, rel, false, in.size, d, t, &in);
		fclose((FILE *)in.h);
		return;
	}
	pack_err = zf_pack_add(&pk, rel, true, 0, d, t, NULL);
	struct dirent **e;
	int n = scandir(path, &e, NULL, by_name);
	for (int i = 0; i < n; i++) {
		if (strcmp(e[i]->d_name, ".") && strcmp(e[i]->d_name, "..")) {
			char p[2048], r[2048];
			snprintf(p, sizeof p, "%s/%s", path, e[i]->d_name);
			snprintf(r, sizeof r, "%s/%s", rel, e[i]->d_name);
			pack_tree(p, r);
		}
		free(e[i]);
	}
	free(e);
}

static long cancel_after = -1, polls;
static bool cancel_poll(void) { polls++; return cancel_after >= 0 && polls > cancel_after; }

int main(int argc, char **argv) {
	const char *to_name = NULL;
	bool extract = false, detect = false, pack = false;
	uint32_t heap = 1024 * 1024;
	int i = 1;
	for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
		if (!strcmp(argv[i], "-t") && i + 1 < argc) to_name = argv[++i];
		else if (!strcmp(argv[i], "-x")) extract = true;
		else if (!strcmp(argv[i], "-p")) pack = true;
		else if (!strcmp(argv[i], "-d")) detect = true;
		else if (!strcmp(argv[i], "-c") && i + 1 < argc) cancel_after = atol(argv[++i]);
		else if (!strcmp(argv[i], "-m") && i + 1 < argc) heap = (uint32_t)atol(argv[++i]);
		else { fprintf(stderr, "usage\n"); return 2; }
	}
	if (i >= argc) { fprintf(stderr, "usage\n"); return 2; }
	zf_heap = malloc(heap);
	zf_heap_size = heap;
	zf_cancel_poll = cancel_poll;

	if (pack) {
		if (i + 1 >= argc) { fprintf(stderr, "no output\n"); return 2; }
		zf_type_t to = zf_detect(argv[i + 1], NULL);
		zf_io_t out;
		if (!io_open(&out, argv[i + 1], true)) { perror(argv[i + 1]); return 2; }
		char top[1024];
		snprintf(top, sizeof top, "%s", argv[i]);
		size_t tl = strlen(top);
		while (tl > 1 && top[tl - 1] == '/') top[--tl] = 0;
		const char *base = strrchr(top, '/') ? strrchr(top, '/') + 1 : top;
		int r = zf_pack_begin(&pk, &out, to, tree_size(top));
		if (!r) { pack_tree(top, base); r = pack_err; }
		if (!r) r = zf_pack_end(&pk);
		fclose((FILE *)out.h);
		if (r < 0) { fprintf(stderr, "zfhost: %s (%d)\n", zf_strerror(r), r); return 1; }
		return 0;
	}

	zf_io_t in;
	if (!io_open(&in, argv[i], false)) { perror(argv[i]); return 2; }
	zf_type_t from = zf_detect(argv[i], &in);
	if (detect) { printf("%s\n", zf_type_name(from)); return 0; }
	if (i + 1 >= argc) { fprintf(stderr, "no output\n"); return 2; }
	const char *outp = argv[i + 1];
	int r;
	if (extract || (zf_type_is_arch(from) && from != ZF_T_GZ)) {
		zf_dir_t d = { NULL, d_open, d_close, d_mkdir, d_remove };
		snprintf(root, sizeof root, "%s", outp);
		mkdir(root, 0755);
		r = zf_convert(&in, from, NULL, ZF_T_UNKNOWN, &d);
	} else {
		zf_type_t to = to_name ? zf_type_from_name(to_name) : zf_detect(outp, NULL);
		if (from == ZF_T_GZ) to = ZF_T_UNKNOWN;
		zf_io_t out;
		if (!io_open(&out, outp, true)) { perror(outp); return 2; }
		r = zf_convert(&in, from, &out, to, NULL);
		fclose((FILE *)out.h);
	}
	fclose((FILE *)in.h);
	if (zf_tab_dropped) fprintf(stderr, "%u cells outside A1:Z999 left out\n", (unsigned)zf_tab_dropped);
	if (r < 0) { fprintf(stderr, "zfhost: %s (%d)\n", zf_strerror(r), r); return 1; }
	return 0;
}
