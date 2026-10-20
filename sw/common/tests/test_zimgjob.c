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

// -- into a document (z_imgjob_start_doc(), as view and web use it) --

static bool doc_whole(const char *path, uint32_t *doc, int wpl, int w, int h, z_img_t *im) {
	FILE *f = fopen(path, "rb");
	static uint8_t buf[1 << 20];
	src_t s;
	if (!f) return false;
	s.n = (long)fread(buf, 1, sizeof(buf), f);
	fclose(f);
	s.p = buf;
	s.pos = 0;
	memset(im, 0, sizeof(*im));
	im->read = src_read;
	im->ctx = &s;
	im->doc = doc;
	im->doc_wpl = wpl;
	im->doc_w = w;
	im->doc_h = h;
	z_img_clear(im);
	return z_img_decode(im, z_img_sniff(buf, 64)) == Z_IMG_OK;
}

static size_t most_stack;

// The document is `w` x `h` inside a wpl-word page; `mem_bytes` the
// memory the job gets.
static void one_doc(const char *path, int wpl, int w, int h, size_t mem_bytes) {
	static uint32_t a_doc[(Z_IMG_MAX_H + 1) * Z_IMG_MAX_WPL], b_doc[(Z_IMG_MAX_H + 1) * Z_IMG_MAX_WPL];
	uint8_t *mem = malloc(mem_bytes);
	size_t bytes = (size_t)h * (size_t)wpl * 4;
	z_imgjob_t j;
	z_img_t im;
	int steps = 0, st, last_rows = 0;
	bool rows_up = true, w_early = false;
	char msg[300];

	snprintf(msg, sizeof(msg), "%s: doc whole", path);
	ck(doc_whole(path, a_doc, wpl, w, h, &im), msg);
	memset(b_doc, 0xA5, sizeof(b_doc));		// cleared by the job
	ck(z_imgjob_start_doc(&j, path, b_doc, wpl, w, h, mem, mem_bytes), "doc job starts");
	while ((st = z_imgjob_step(&j, 0)) == Z_IMGJOB_RUNNING) {
		steps++;
		if (j.rows_done < last_rows) rows_up = false;
		last_rows = j.rows_done;
		if (j.rows_done && j.w == im.out_w && j.h == im.out_h) w_early = true;
		ck(j.rows_done <= im.out_h, "rows_done within the picture");
	}
	snprintf(msg, sizeof(msg), "%s: doc done in %d steps", path, steps);
	ck(st == Z_IMGJOB_DONE && j.err == Z_IMG_OK && steps >= 4, msg);
	ck(rows_up && w_early && j.rows_done == im.out_h, "rows_done climbs to h; w x h known early");
	ck(j.w == im.out_w && j.h == im.out_h && j.ordered == im.was_ordered, "same size as one call");
	snprintf(msg, sizeof(msg), "%s: doc sliced == whole", path);
	ck(!memcmp(a_doc, b_doc, bytes), msg);
	ck(b_doc[h * wpl] == 0xA5A5A5A5u, "nothing written below the document");
	ck(!z_imgjob_take(&j), "nothing to take");
	if (zco_stack_used(&j.co) > most_stack) most_stack = zco_stack_used(&j.co);
	z_imgjob_end(&j);
	ck(!files[0] && !files[1], "file closed when done");
	printf("imgjob: doc %s %dx%d in %dx%d, %d slices, stack %u of %u\n", path,
		j.w, j.h, w, h, steps, (unsigned)zco_stack_used(&j.co), (unsigned)j.co.bytes);

	// stopped at every point: the file closed, and the next one right
	{
		int at, total = steps;
		for (at = 0; at <= total; at += total / 29 + 1) {
			int k;
			ck(z_imgjob_start_doc(&j, path, b_doc, wpl, w, h, mem, mem_bytes), "restarts");
			for (k = 0; k < at && z_imgjob_step(&j, 0) == Z_IMGJOB_RUNNING; k++) ;
			z_imgjob_stop(&j);
			ck(j.state == Z_IMGJOB_STOPPED || j.state == Z_IMGJOB_DONE, "doc stopped");
			z_imgjob_end(&j);
			ck(!files[0] && !files[1] && j.fh < 0, "stopping closes the file");
		}
		ck(z_imgjob_start_doc(&j, path, b_doc, wpl, w, h, mem, mem_bytes), "again");
		while (z_imgjob_step(&j, 5) == Z_IMGJOB_RUNNING) ;
		ck(j.state == Z_IMGJOB_DONE && !memcmp(a_doc, b_doc, bytes), "doc after stops, right again");
		z_imgjob_end(&j);
	}

	// a grey job stopped in the middle, then a document job: still right
	{
		int k;
		z_imgjob_start(&j, path, a, r);
		for (k = 0; k < 7 && z_imgjob_step(&j, 0) == Z_IMGJOB_RUNNING; k++) ;
		z_imgjob_stop(&j);
		z_imgjob_end(&j);
		ck(z_imgjob_start_doc(&j, path, b_doc, wpl, w, h, mem, mem_bytes), "after a grey job");
		while (z_imgjob_step(&j, 5) == Z_IMGJOB_RUNNING) ;
		ck(j.state == Z_IMGJOB_DONE && !memcmp(a_doc, b_doc, bytes), "doc after a stopped grey job");
		z_imgjob_end(&j);
		ck(live == 0, "nothing left");
	}
	free(mem);
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
	// view: the whole 640x480 document, the stack it gives the decoder
	{
		static const char *pics[] = { "sw/data/images/squirrel.jpg", "sw/data/icons/icon-view.png",
			"sw/data/demo/squirrel.pgm", NULL };
		int i;
		for (i = 0; pics[i]; i++) {
			snprintf(p, sizeof(p), "%s/%s", root, pics[i]);
			one_doc(p, Z_IMG_MAX_WPL, Z_IMG_MAX_W, Z_IMG_MAX_H, Z_IMGJOB_DOC_MEM);
			// web: a placeholder box in a page 640 wide, at most 288 high
			one_doc(p, Z_IMG_MAX_WPL, 160, 288, Z_IMGJOB_DOC_MEM_STACK(8192));
			one_doc(p, Z_IMG_MAX_WPL, 400, 120, Z_IMGJOB_DOC_MEM_STACK(8192));
		}
		printf("imgjob: doc stack used, most: %u bytes\n", (unsigned)most_stack);
	}
	{
		static uint32_t d[Z_IMG_MAX_H * Z_IMG_MAX_WPL];
		static uint8_t small[2048];
		snprintf(p, sizeof(p), "%s/sw/data/images/squirrel.jpg", root);
		ck(!z_imgjob_start_doc(&j, p, d, Z_IMG_MAX_WPL, 640, 480, small, sizeof(small)) &&
			j.state == Z_IMGJOB_FAILED && j.err == Z_IMGJOB_E_MEM && !files[0], "too little memory fails");
		ck(!z_imgjob_start_doc(&j, "/no/such/file.jpg", d, Z_IMG_MAX_WPL, 640, 480, small, sizeof(small)) &&
			j.state == Z_IMGJOB_FAILED, "a missing file fails at once (doc)");
		{
			uint8_t *mem = malloc(Z_IMGJOB_DOC_MEM);
			snprintf(p, sizeof(p), "%s/README.md", root);
			ck(z_imgjob_start_doc(&j, p, d, Z_IMG_MAX_WPL, 640, 480, mem, Z_IMGJOB_DOC_MEM), "a text file starts (doc)");
			while (z_imgjob_step(&j, 5) == Z_IMGJOB_RUNNING) ;
			ck(j.state == Z_IMGJOB_FAILED && j.err == Z_IMG_E_FORMAT && !files[0], "and fails: not a picture (doc)");
			free(mem);
		}
	}
	printf("imgjob: %d checks, %d failures\n", checks, fails);
	return fails ? 1 : 0;
}
