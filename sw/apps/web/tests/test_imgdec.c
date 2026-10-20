/*
 * imgdec.c on the host: a picture decoded into its placeholder box a
 * slice at a time is the picture decoded in one call; stopped at any
 * point -- or dropped for the next one -- it leaves no file open and
 * its spool removed. `make test` (docs/web_app.md, "Pictures").
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "../imgdec.h"

static int fails, checks;

#define ck(cond, msg) do { checks++; if (!(cond)) { fails++; \
	printf("FAIL %s:%d %s\n", __FILE__, __LINE__, msg); } } while (0)

// -- the files and the clock, as the board's syscalls would be --

static FILE *files[8];
static int opened;
int fs_open_read(const char *p) {
	int i;
	for (i = 0; i < 8; i++) if (!files[i]) { files[i] = fopen(p, "rb"); if (files[i]) opened++; return files[i] ? i : -1; }
	return -1;
}
int fs_read_chunk(int h, void *b, int n) { return (int)fread(b, 1, (size_t)n, files[h]); }
int fs_close_handle(int h) { if (files[h]) { fclose(files[h]); opened--; } files[h] = NULL; return 0; }
int fs_size(char *p) {
	FILE *f = fopen(p, "rb");
	long n;
	if (!f) return -1;
	fseek(f, 0, SEEK_END);
	n = ftell(f);
	fclose(f);
	return (int)n;
}
int fs_unlink(char *p) { return unlink(p); }
int fs_seek(int h, uint32_t o) { return fseek(files[h], (long)o, SEEK_SET); }
static uint32_t clock_now;
uint32_t z_uptime_ticks(void) { return clock_now++; }

static bool exists(const char *p) { return access(p, F_OK) == 0; }

// The page's bitmap: as web.c's img_bits, 640 wide, 24 lines of 12.
#define WPL 20
#define H   288
static uint32_t whole[(H + 1) * WPL], bits[(H + 1) * WPL];

static char spool[256];

static bool copy(const char *from, const char *to) {
	FILE *a = fopen(from, "rb"), *b;
	static uint8_t buf[1 << 20];
	size_t n;
	if (!a) return false;
	n = fread(buf, 1, sizeof(buf), a);
	fclose(a);
	b = fopen(to, "wb");
	if (!b) return false;
	fwrite(buf, 1, n, b);
	fclose(b);
	return true;
}

typedef struct { FILE *f; } src_t;
static int src_read(void *c, uint8_t *b, int len) { return (int)fread(b, 1, (size_t)len, ((src_t *)c)->f); }

// In one call, as web.c did before.
static bool decode_whole(const char *path, int w, int h, z_img_t *im) {
	uint8_t hdr[64];
	src_t s;
	int n, rv;
	s.f = fopen(path, "rb");
	if (!s.f) return false;
	n = (int)fread(hdr, 1, sizeof(hdr), s.f);
	rewind(s.f);
	memset(im, 0, sizeof(*im));
	im->read = src_read;
	im->ctx = &s;
	im->doc = whole;
	im->doc_wpl = WPL;
	im->doc_w = w;
	im->doc_h = h;
	z_img_clear(im);
	rv = z_img_decode(im, z_img_sniff(hdr, n));
	fclose(s.f);
	return rv == Z_IMG_OK;
}

static void one(const char *pic, int w, int h) {
	imgdec_t d;
	z_img_t im;
	int st, steps = 0;
	char msg[400];

	memset(&d, 0, sizeof(d));
	snprintf(msg, sizeof(msg), "%s: in one call", pic);
	ck(decode_whole(pic, w, h, &im), msg);

	ck(copy(pic, spool), "spooled");
	memset(bits, 0x5a, sizeof(bits));
	ck(imgdec_start(&d, spool, bits, WPL, w, h) && imgdec_busy(&d), "starts");
	while ((st = imgdec_step(&d, 0)) == Z_IMGJOB_RUNNING) {
		steps++;
		ck(imgdec_permille(&d) < 1000, "progress below 1000");
	}
	snprintf(msg, sizeof(msg), "%s in %dx%d: done in %d slices", pic, w, h, steps);
	ck(st == Z_IMGJOB_DONE && steps >= 4 && !imgdec_busy(&d), msg);
	ck(d.job.w == im.out_w && d.job.h == im.out_h, "the same size");
	snprintf(msg, sizeof(msg), "%s in %dx%d: sliced == one call", pic, w, h);
	ck(!memcmp(bits, whole, (size_t)h * WPL * 4), msg);
	ck(bits[h * WPL] == 0x5a5a5a5au, "nothing below the box");
	ck(!opened && !exists(spool), "done: spool closed and removed");
	printf("imgdec: %s %dx%d in a %dx%d box, %d slices\n", pic, d.job.w, d.job.h, w, h, steps);

	// stopped at every point (Esc, Stop, another page): nothing open,
	// no spool, and the next one right
	{
		int at, k;
		for (at = 0; at <= steps; at += steps / 31 + 1) {
			copy(pic, spool);
			ck(imgdec_start(&d, spool, bits, WPL, w, h), "restarts");
			for (k = 0; k < at && imgdec_step(&d, 0) == Z_IMGJOB_RUNNING; k++) ;
			imgdec_stop(&d);
			ck(!imgdec_busy(&d) && !opened && !exists(spool), "stopped: closed and removed");
			imgdec_stop(&d);		// twice is nothing
		}
		copy(pic, spool);
		ck(imgdec_start(&d, spool, bits, WPL, w, h), "again");
		while (imgdec_step(&d, 3) == Z_IMGJOB_RUNNING) ;
		ck(d.job.state == Z_IMGJOB_DONE && !memcmp(bits, whole, (size_t)h * WPL * 4),
			"after the stops, right again");
		ck(!opened && !exists(spool), "and nothing left");
	}
}

int main(int argc, char **argv) {
	const char *root = argc > 1 ? argv[1] : "../../..";
	static const char *pics[] = { "sw/data/images/squirrel.jpg", "sw/data/icons/icon-view.png",
		"sw/data/demo/squirrel.pgm", NULL };
	char p[512], q[512];
	imgdec_t d;
	int i;

	snprintf(spool, sizeof(spool), "/tmp/test_imgdec.%d.img", (int)getpid());

	for (i = 0; pics[i]; i++) {
		snprintf(p, sizeof(p), "%s/%s", root, pics[i]);
		one(p, 160, H);
		one(p, 640, H);
		one(p, 400, 120);
	}

	// the next picture replaces one still decoding: the first's spool goes
	memset(&d, 0, sizeof(d));
	snprintf(p, sizeof(p), "%s/%s", root, pics[0]);
	snprintf(q, sizeof(q), "%s.2", spool);
	copy(p, spool);
	copy(p, q);
	ck(imgdec_start(&d, spool, bits, WPL, 160, H), "first");
	imgdec_step(&d, 0);
	imgdec_step(&d, 0);
	ck(imgdec_start(&d, q, bits, WPL, 160, H) && !exists(spool) && opened == 1, "replaced: the first spool gone");
	while (imgdec_step(&d, 3) == Z_IMGJOB_RUNNING) ;
	ck(d.job.state == Z_IMGJOB_DONE && !opened && !exists(q), "the second done and cleaned up");

	// not a picture: it fails, and is cleaned up
	snprintf(p, sizeof(p), "%s/README.md", root);
	copy(p, spool);
	ck(imgdec_start(&d, spool, bits, WPL, 160, H), "a text file starts");
	while (imgdec_step(&d, 3) == Z_IMGJOB_RUNNING) ;
	ck(d.job.state == Z_IMGJOB_FAILED && d.job.err == Z_IMG_E_FORMAT && !opened && !exists(spool),
		"and fails: not a picture, nothing left");

	// no spool at all
	ck(!imgdec_start(&d, "/no/such/spool.img", bits, WPL, 160, H) && !imgdec_busy(&d) &&
		d.job.err == Z_IMGJOB_E_OPEN && !opened, "a missing spool fails at once");

	printf("imgdec: %d checks, %d failures\n", checks, fails);
	return fails ? 1 : 0;
}
