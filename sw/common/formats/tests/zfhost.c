/*
 * Zeitlos -- sw/common/formats on the host.
 *
 *   zfhost IN OUT              convert, the types from the names
 *   zfhost -t docx IN OUT      ... or named
 *   zfhost -x IN DIR           extract an archive into DIR
 *   zfhost -d IN               print the detected type
 *   zfhost -c N IN OUT         cancel after N polls (a test of cancelling)
 *   zfhost -m BYTES ...        zf_heap size (default 1MB)
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

static long cancel_after = -1, polls;
static bool cancel_poll(void) { polls++; return cancel_after >= 0 && polls > cancel_after; }

int main(int argc, char **argv) {
	const char *to_name = NULL;
	bool extract = false, detect = false;
	uint32_t heap = 1024 * 1024;
	int i = 1;
	for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
		if (!strcmp(argv[i], "-t") && i + 1 < argc) to_name = argv[++i];
		else if (!strcmp(argv[i], "-x")) extract = true;
		else if (!strcmp(argv[i], "-d")) detect = true;
		else if (!strcmp(argv[i], "-c") && i + 1 < argc) cancel_after = atol(argv[++i]);
		else if (!strcmp(argv[i], "-m") && i + 1 < argc) heap = (uint32_t)atol(argv[++i]);
		else { fprintf(stderr, "usage\n"); return 2; }
	}
	if (i >= argc) { fprintf(stderr, "usage\n"); return 2; }
	zf_heap = malloc(heap);
	zf_heap_size = heap;
	zf_cancel_poll = cancel_poll;

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
