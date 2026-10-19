/*
 * sw/common/zimgjob.c and zco.c on the host: a picture decoded a slice
 * at a time is the picture decoded in one call; it really is in slices
 * (the main loop gets control back many times); stopping it at any
 * point frees everything and leaves the decoder ready for the next.
 * `make -f Makefile.zcanvas imgjob`.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../zimgjob.h"

static int fails, checks;

#define ck(cond, msg) do { checks++; if (!(cond)) { fails++; \
	printf("FAIL %s:%d %s\n", __FILE__, __LINE__, msg); } } while (0)

// -- the files and the clock, as the board's syscalls would be --

static FILE *files[8];
int fs_open_read(const char *p) {
	int i;
	for (i = 0; i < 8; i++) if (!files[i]) { files[i] = fopen(p, "rb"); return files[i] ? i : -1; }
	return -1;
}
int fs_read_chunk(int h, void *b, int n) { return (int)fread(b, 1, (size_t)n, files[h]); }
int fs_close_handle(int h) { if (files[h]) fclose(files[h]); files[h] = NULL; return 0; }
int fs_seek(int h, uint32_t o) { return fseek(files[h], (long)o, SEEK_SET); }
int fs_size(char *p) {
	FILE *f = fopen(p, "rb");
	long n;
	if (!f) return -1;
	fseek(f, 0, SEEK_END);
	n = ftell(f);
	fclose(f);
	return (int)n;
}
static uint32_t clock_now;
uint32_t z_uptime_ticks(void) { return clock_now++; }

static int live;
static void *a(size_t n) { live++; return malloc(n); }
static void r(void *p) { if (p) { live--; free(p); } }

// -- the same picture in one call --
typedef struct { uint8_t *g; int w, h; z_img_t *im; } whole_t;
static void whole_row(void *u, const uint8_t *g, int y, int w) {
	whole_t *s = u;
	if (!s->g) { s->w = s->im->out_w; s->h = s->im->out_h; s->g = calloc((size_t)s->w * s->h, 1); }
	if (y < s->h) memcpy(s->g + (size_t)y * s->w, g, (size_t)w);
}
typedef struct { const uint8_t *p; long n, pos; } src_t;
static int src_read(void *c, uint8_t *b, int len) {
	src_t *s = c;
	long k = s->n - s->pos < len ? s->n - s->pos : len;
	memcpy(b, s->p + s->pos, (size_t)k);
	s->pos += k;
	return (int)k;
}

static bool decode_whole(const char *path, whole_t *out) {
	FILE *f = fopen(path, "rb");
	static uint8_t buf[1 << 20];
	z_img_t im;
	src_t s;
	if (!f) return false;
	s.n = (long)fread(buf, 1, sizeof(buf), f);
	fclose(f);
	s.p = buf;
	s.pos = 0;
	memset(&im, 0, sizeof(im));
	im.read = src_read;
	im.ctx = &s;
	im.doc_w = Z_IMG_MAX_W;
	im.doc_h = Z_IMG_MAX_H;
	memset(out, 0, sizeof(*out));
	out->im = &im;
	return z_img_decode_gray(&im, z_img_sniff(buf, 64), whole_row, out) == Z_IMG_OK && out->g;
}

static void one(const char *path) {
	z_imgjob_t j;
	whole_t w;
	int steps = 0, st;
	char msg[256];
	snprintf(msg, sizeof(msg), "%s: whole", path);
	ck(decode_whole(path, &w), msg);
	ck(z_imgjob_start(&j, path, a, r), "job starts");
	// one resume a step: the smallest slices there are
	while ((st = z_imgjob_step(&j, 0)) == Z_IMGJOB_RUNNING) {
		steps++;
		if (steps % 50 == 0) ck(z_imgjob_permille(&j) < 1000, "progress below 1000 while running");
	}
	snprintf(msg, sizeof(msg), "%s: done in %d steps", path, steps);
	ck(st == Z_IMGJOB_DONE && steps >= 4, msg);
	ck(z_imgjob_permille(&j) == 1000, "1000 when done");
	snprintf(msg, sizeof(msg), "%s: sliced == whole", path);
	ck(j.w == w.w && j.h == w.h && !memcmp(j.gray, w.g, (size_t)w.w * w.h), msg);
	{
		uint8_t *g = z_imgjob_take(&j);
		ck(g && !j.gray, "taken");
		r(g);
	}
	z_imgjob_end(&j);
	ck(live == 0, "nothing left");
	printf("imgjob: %s %dx%d in %d slices\n", path, j.w, j.h, steps);

	// stopped at every point along the way: nothing kept, and the next
	// decode is right
	{
		int at, total = steps;
		for (at = 0; at < total; at += total / 23 + 1) {
			int k;
			z_imgjob_start(&j, path, a, r);
			for (k = 0; k < at && z_imgjob_step(&j, 0) == Z_IMGJOB_RUNNING; k++) ;
			z_imgjob_stop(&j);
			ck(j.state == Z_IMGJOB_STOPPED || j.state == Z_IMGJOB_DONE, "stopped");
			z_imgjob_end(&j);
			ck(live == 0 && !files[0] && !files[1], "stopping frees everything");
		}
		z_imgjob_start(&j, path, a, r);
		while (z_imgjob_step(&j, 5) == Z_IMGJOB_RUNNING) ;
		ck(j.state == Z_IMGJOB_DONE && !memcmp(j.gray, w.g, (size_t)w.w * w.h), "after stops, right again");
		z_imgjob_end(&j);
		ck(live == 0, "nothing left");
	}
	free(w.g);
}

int main(int argc, char **argv) {
	const char *root = argc > 1 ? argv[1] : "../../..";
	char p[512];
	z_imgjob_t j;
	snprintf(p, sizeof(p), "%s/sw/data/images/squirrel.jpg", root); one(p);
	snprintf(p, sizeof(p), "%s/sw/data/icons/icon-view.png", root); one(p);
	snprintf(p, sizeof(p), "%s/sw/data/demo/squirrel.pgm", root); one(p);
	ck(!z_imgjob_start(&j, "/no/such/file.jpg", a, r) && j.state == Z_IMGJOB_FAILED &&
		j.err == Z_IMGJOB_E_OPEN && live == 0, "a missing file fails at once");
	snprintf(p, sizeof(p), "%s/README.md", root);
	ck(z_imgjob_start(&j, p, a, r), "a text file starts");
	while (z_imgjob_step(&j, 5) == Z_IMGJOB_RUNNING) ;
	ck(j.state == Z_IMGJOB_FAILED && live == 0, "and fails: not a picture");
	printf("imgjob: %d checks, %d failures\n", checks, fails);
	return fails ? 1 : 0;
}
