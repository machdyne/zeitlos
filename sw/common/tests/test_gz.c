/*
 * Zeitlos -- host test for sw/common/zgz.c: reading a .gz file.
 *
 *   python3 sw/common/tests/gen_inflate_corpus.py /tmp/gzc
 *   cc -std=gnu99 -I sw/common -o /tmp/t sw/common/tests/test_gz.c \
 *      sw/common/zgz.c sw/common/zinflate.c && /tmp/t /tmp/gzc
 *
 * The app file API (zfsapp.h) is played by host files. Checked: whole
 * files read back exactly at awkward buffer sizes, and every way a .gz
 * can be bad is reported rather than returned as data -- corrupt
 * compressed data, a trailer whose CRC or length does not match (which
 * zinflate itself does not check), a file cut short, a missing file.
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "zgz.h"

/* -- zfsapp.h, on the host -- */
static FILE *files[8];
int fs_open_read(const char *p) {
	int i;
	for (i = 0; i < 8; i++) if (!files[i]) { if (!(files[i] = fopen(p, "rb"))) return -1; return i; }
	return -1;
}
int fs_read_chunk(int h, void *buf, int n) { return (int)fread(buf, 1, (size_t)n, files[h]); }
int fs_close_handle(int h) { fclose(files[h]); files[h] = 0; return 1; }

static int fails, checks;
static void ck(int c, const char *what) { checks++; if (!c) { fails++; printf("FAIL: %s\n", what); } }
static uint8_t window[Z_INFLATE_WINDOW];
static uint8_t got[1 << 20], want[1 << 20];

static uint32_t slurp(const char *p, uint8_t *b, uint32_t cap) {
	FILE *f = fopen(p, "rb"); uint32_t n;
	if (!f) { printf("FAIL: cannot open %s\n", p); exit(2); }
	n = (uint32_t)fread(b, 1, cap, f); fclose(f); return n;
}

/* the whole file through z_gz_read, `chunk` bytes at a time: the last
 * result, and how many bytes came before it */
static int32_t read_all(const char *path, uint32_t chunk, uint32_t *n) {
	static z_gz_t g;
	int32_t r;
	*n = 0;
	if (z_gz_open(&g, path, window)) return -99;
	while ((r = z_gz_read(&g, got + *n, chunk)) > 0) *n += (uint32_t)r;
	z_gz_close(&g);
	return r;
}

static void write_file(const char *p, const uint8_t *b, uint32_t n) {
	FILE *f = fopen(p, "wb"); fwrite(b, 1, n, f); fclose(f);
}

int main(int argc, char **argv) {
	const char *dir = argc > 1 ? argv[1] : ".";
	static const uint32_t chunks[] = { 1, 7, 4096 };
	char a[256], b[256], msg[160];
	static uint8_t gz[1 << 20];
	uint32_t wn, n, gn, i;
	int32_t r;

	snprintf(b, sizeof(b), "%s/fields.raw", dir);
	wn = slurp(b, want, sizeof(want));
	for (const char *const *f = (const char *const[]){ "fname.gz", "fields.gz", 0 }; *f; f++)
		for (i = 0; i < 3; i++) {
			snprintf(a, sizeof(a), "%s/%s", dir, *f);
			r = read_all(a, chunks[i], &n);
			snprintf(msg, sizeof(msg), "%s, %u bytes at a time: all of it, then a clean end", *f, chunks[i]);
			ck(r == 0 && n == wn && !memcmp(got, want, wn), msg);
		}

	snprintf(a, sizeof(a), "%s/fields.gz", dir);
	gn = slurp(a, gz, sizeof(gz));

	/* a flipped bit in the compressed data */
	memcpy(got, gz, gn); got[gn / 2] ^= 0x10;
	write_file("/tmp/zgz_bad.gz", got, gn);
	r = read_all("/tmp/zgz_bad.gz", 4096, &n);
	ck(r < 0, "corrupt compressed data is an error, never a clean end");

	/* the trailer: CRC, then length */
	memcpy(got, gz, gn); got[gn - 8] ^= 0x01;
	write_file("/tmp/zgz_bad.gz", got, gn);
	r = read_all("/tmp/zgz_bad.gz", 4096, &n);
	ck(r == Z_GZ_E_CRC, "a trailer CRC that does not match: Z_GZ_E_CRC");
	memcpy(got, gz, gn); got[gn - 4] ^= 0x01;
	write_file("/tmp/zgz_bad.gz", got, gn);
	r = read_all("/tmp/zgz_bad.gz", 4096, &n);
	ck(r == Z_GZ_E_CRC, "a trailer length that does not match: Z_GZ_E_CRC");

	/* cut short: no trailer, then half the data */
	write_file("/tmp/zgz_bad.gz", gz, gn - 5);
	r = read_all("/tmp/zgz_bad.gz", 4096, &n);
	ck(r == Z_GZ_E_TRUNCATED, "a file without its whole trailer: truncated");
	write_file("/tmp/zgz_bad.gz", gz, gn / 2);
	r = read_all("/tmp/zgz_bad.gz", 7, &n);
	ck(r == Z_GZ_E_TRUNCATED, "half a file: truncated");

	ck(read_all("/tmp/no/such.gz", 4096, &n) == -99, "a missing file does not open");

	printf("%s: %d checks, %d failures\n", fails ? "FAIL" : "ok", checks, fails);
	return fails ? 1 : 0;
}
