/*
 * A read-only filesystem in memory, for netserve's tests: the four
 * zfsapp.h calls the HTTP server uses. fake_fs_open counts handles
 * still open, so a test can check none leak.
 */
#include <string.h>
#include <stdint.h>
#include "../../../../common/zfsapp.h"
#include "../../../../common/zrng.h"

/* The TRNG, faked: zrng.c reads hardware registers. Deterministic. */
static uint64_t fake_rng = 0x2545F4914F6CDD1DULL;
bool fake_rng_secure = true;
void z_rng_bytes(void *out, uint32_t n) {
	uint8_t *p = out;
	while (n--) { fake_rng ^= fake_rng << 13; fake_rng ^= fake_rng >> 7; fake_rng ^= fake_rng << 17; *p++ = (uint8_t)fake_rng; }
}
bool z_rng_secure(void) { return fake_rng_secure; }
void z_rng_stir_event(void) { }

typedef struct { const char *path; const uint8_t *data; uint32_t len; } fake_file_t;

static uint8_t big[5000];
static const char idx[] = "<h1>home</h1>\n";
static const char sub_idx[] = "<p>sub</p>\n";
static const char htm_idx[] = "<p>htm</p>\n";
static const char css[] = "body{color:red}\n";

static fake_file_t files[] = {
	{ "/www/index.html", (const uint8_t *)idx, sizeof(idx) - 1 },
	{ "/www/sub/index.html", (const uint8_t *)sub_idx, sizeof(sub_idx) - 1 },
	{ "/www/old/index.htm", (const uint8_t *)htm_idx, sizeof(htm_idx) - 1 },
	{ "/www/style.CSS", (const uint8_t *)css, sizeof(css) - 1 },
	{ "/www/big.bin", big, sizeof(big) },
	{ "/www/a b.txt", (const uint8_t *)"space\n", 6 },
	{ "/secret.txt", (const uint8_t *)"no\n", 3 },
};
#define NFILES (int)(sizeof(files) / sizeof(files[0]))

static struct { int file; uint32_t pos; int used; } fh[8];
int fake_fs_open;
int fake_fs_limit = 8;

static int find_file(const char *p) {
	for (int i = 0; i < NFILES; i++) if (!strcmp(files[i].path, p)) return i;
	return -1;
}

void fake_fs_init(void) {
	for (int i = 0; i < (int)sizeof(big); i++) big[i] = (uint8_t)(i * 31 + 7);
	memset(fh, 0, sizeof(fh));
	fake_fs_open = 0;
}

const uint8_t *fake_fs_data(const char *p, uint32_t *len) {
	int f = find_file(p);
	if (f < 0) return 0;
	*len = files[f].len;
	return files[f].data;
}

static const char *authkeys_text;           // /user/authkeys, set by a test
void fake_fs_authkeys(const char *t) { authkeys_text = t; }

int fs_read_file(char *p, char *buf, int max) {
	uint32_t n;
	const uint8_t *d;
	if (!strcmp(p, "/user/authkeys")) {
		if (!authkeys_text) return 0;
		n = (uint32_t)strlen(authkeys_text);
		if (n > (uint32_t)max) n = (uint32_t)max;
		memcpy(buf, authkeys_text, n);
		return (int)n;
	}
	d = fake_fs_data(p, &n);
	if (!d) return 0;
	if (n > (uint32_t)max) n = (uint32_t)max;
	memcpy(buf, d, n);
	return (int)n;
}

int fs_size(char *p) {
	int f = find_file(p);
	return f < 0 ? 0 : (int)files[f].len;
}

int fs_open_read(const char *p) {
	int f = find_file(p);
	if (f < 0 || fake_fs_open >= fake_fs_limit) return -1;
	for (int i = 0; i < 8; i++)
		if (!fh[i].used) { fh[i].used = 1; fh[i].file = f; fh[i].pos = 0; fake_fs_open++; return i; }
	return -1;
}

int fs_read_chunk(int h, void *buf, int maxlen) {
	if (h < 0 || h >= 8 || !fh[h].used) return -1;
	fake_file_t *f = &files[fh[h].file];
	uint32_t n = f->len - fh[h].pos;
	if (n > (uint32_t)maxlen) n = (uint32_t)maxlen;
	memcpy(buf, f->data + fh[h].pos, n);
	fh[h].pos += n;
	return (int)n;
}

int fs_close_handle(int h) {
	if (h < 0 || h >= 8 || !fh[h].used) return 0;
	fh[h].used = 0;
	fake_fs_open--;
	return 1;
}
