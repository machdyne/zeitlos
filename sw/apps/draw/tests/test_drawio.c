/*
 * draw's load and save state machines (drawio.c), on the build machine:
 * slicing, stopping at every step, the .part, damaged files, and the
 * canvas left as it was.  make test
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>

#include "../drawio.h"
#include "../../../common/zbm.h"
#include "hostfs.h"

#define W 640
#define H 480
#define BYTES (W / 32 * 4 * H)

static int fails;
#define CHECK(c) do { if (!(c)) { fails++; \
	printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

static char dir[64], path[128], part[136];
static uint8_t canvas[BYTES], spare[BYTES], old_img[BYTES], buf[BYTES + 64];

static void fill(uint8_t *b, unsigned seed) {
	for (int i = 0; i < BYTES; i++) b[i] = (uint8_t)((i * 31 + seed * 7 + (i >> 9)) ^ seed);
}

static bool exists(const char *p) { return access(p, F_OK) == 0; }

static long file_get(const char *p, uint8_t *b, long cap) {
	FILE *f = fopen(p, "rb");
	long n;
	if (!f) return -1;
	n = (long)fread(b, 1, (size_t)cap, f);
	fclose(f);
	return n;
}

static void file_put(const char *p, const void *hdr, int hlen, const void *b, long n) {
	FILE *f = fopen(p, "wb");
	if (hlen) fwrite(hdr, 1, (size_t)hlen, f);
	fwrite(b, 1, (size_t)n, f);
	fclose(f);
}

// a ZBM of `img` at `p`, as draw writes it
static void put_zbm(const char *p, const uint8_t *img, long n) {
	z_bm_header_t h;
	z_bm_header_init(&h, W, H);
	file_put(p, &h, Z_BM_HEADER_SIZE, img, n);
}

// `p` holds exactly a ZBM of `img`
static bool is_zbm_of(const char *p, const uint8_t *img) {
	z_bm_header_t h;
	if (file_get(p, buf, sizeof(buf)) != Z_BM_HEADER_SIZE + BYTES) return false;
	memcpy(&h, buf, sizeof(h));
	return z_bm_header_valid(&h) && h.width == W && h.height == H &&
		!memcmp(buf + Z_BM_HEADER_SIZE, img, BYTES);
}

// Runs a job to its end, `ticks` a step; returns the steps taken.
static int run(draw_io_t *io, uint32_t ticks) {
	int steps = 0, last = -1;
	while (draw_io_step(io, ticks) == DRAW_IO_RUNNING) {
		int p = draw_io_permille(io);
		CHECK(p >= last && p < 1000);
		last = p;
		CHECK(hostfs_open_count() == 1);
		steps++;
		if (steps > 10000) break;
	}
	return steps + 1;
}

// -- the way draw.c uses a load: into the spare, swapped in on DONE --

static uint8_t *on_screen, *other;

static int load_like_draw(const char *p, int stop_after) {
	draw_io_t io;
	int s, k = 0;
	if (!draw_io_load_start(&io, p, other, W, H)) return io.err ? -io.err : -1;
	while ((s = draw_io_step(&io, 1)) == DRAW_IO_RUNNING)
		if (++k == stop_after) { draw_io_stop(&io); s = io.state; break; }
	if (s == DRAW_IO_DONE) { uint8_t *t = on_screen; on_screen = other; other = t; }
	return s == DRAW_IO_FAILED ? -io.err : s;
}

static void test_save_load(void) {
	draw_io_t io;
	int steps;

	hostfs_reset();
	unlink(path);
	fill(canvas, 1);

	// a new file: one chunk a step when the slice is one tick
	CHECK(draw_io_save_start(&io, path, canvas, W, H));
	CHECK(exists(part));
	steps = run(&io, 1);
	CHECK(io.state == DRAW_IO_DONE);
	CHECK(steps == 1 + (BYTES + DRAW_IO_CHUNK - 1) / DRAW_IO_CHUNK + 1);
	CHECK(draw_io_permille(&io) == 1000);
	CHECK(!exists(part));
	CHECK(is_zbm_of(path, canvas));
	CHECK(hostfs_open_count() == 0);

	// a bigger slice does more a step
	fill(canvas, 2);
	CHECK(draw_io_save_start(&io, path, canvas, W, H));
	CHECK(run(&io, 5) < steps / 3);
	CHECK(io.state == DRAW_IO_DONE);
	CHECK(is_zbm_of(path, canvas));	// replaced the old one (FAT rename refuses: unlink first)
	CHECK(!exists(part));

	// and loads back
	memset(spare, 0xAA, sizeof(spare));
	CHECK(draw_io_load_start(&io, path, spare, W, H));
	steps = run(&io, 1);
	CHECK(io.state == DRAW_IO_DONE && !io.legacy);
	CHECK(steps == 1 + (BYTES + DRAW_IO_CHUNK - 1) / DRAW_IO_CHUNK + 1);
	CHECK(!memcmp(spare, canvas, BYTES));
	CHECK(hostfs_open_count() == 0);

	printf("save/load: %d steps of one chunk\n", steps);
}

// Stopping a save at every step: the old file stays, no .part, nothing open.
static void test_save_stop(void) {
	draw_io_t io;
	int k, n = 0;

	hostfs_reset();
	fill(old_img, 3);
	fill(canvas, 4);

	for (k = 0; ; k++) {
		int s = 0, i;
		put_zbm(path, old_img, BYTES);
		CHECK(draw_io_save_start(&io, path, canvas, W, H));
		for (i = 0; i < k && (s = draw_io_step(&io, 1)) == DRAW_IO_RUNNING; i++) ;
		if (i < k || s == DRAW_IO_DONE) {	// it finished before step k
			CHECK(io.state == DRAW_IO_DONE && is_zbm_of(path, canvas));
			break;
		}
		draw_io_stop(&io);
		CHECK(io.state == DRAW_IO_STOPPED);
		CHECK(!exists(part));
		CHECK(is_zbm_of(path, old_img));
		CHECK(hostfs_open_count() == 0);
		draw_io_stop(&io);					// twice: nothing
		CHECK(io.state == DRAW_IO_STOPPED);
		n++;
	}
	CHECK(n > 15);
	printf("save stopped at %d points\n", n);
}

// Stopping a load at every step: the canvas on screen is the one before.
static void test_load_stop(void) {
	int k, n = 0, s;

	hostfs_reset();
	fill(old_img, 5);
	put_zbm(path, old_img, BYTES);

	for (k = 1; ; k++) {
		fill(canvas, 6);
		on_screen = canvas;
		other = spare;
		s = load_like_draw(path, k);
		CHECK(hostfs_open_count() == 0);
		if (s == DRAW_IO_DONE) { CHECK(!memcmp(on_screen, old_img, BYTES)); break; }
		CHECK(s == DRAW_IO_STOPPED);
		CHECK(on_screen == canvas);
		fill(old_img + 0, 6);				// what canvas held
		CHECK(!memcmp(on_screen, old_img, BYTES));
		fill(old_img, 5);
		n++;
	}
	CHECK(n > 15);
	printf("load stopped at %d points\n", n);
}

// Damaged and foreign files fail, saying why, and leave the canvas.
static void test_load_bad(void) {
	z_bm_header_t h;

	hostfs_reset();
	fill(old_img, 7);
	fill(canvas, 8);
	on_screen = canvas;
	other = spare;

	unlink(path);
	CHECK(load_like_draw(path, 0) == -DRAW_IO_E_OPEN);

	put_zbm(path, old_img, BYTES - 1000);			// cut short
	CHECK(load_like_draw(path, 0) == -DRAW_IO_E_SHORT);

	put_zbm(path, old_img, 0);						// a header only
	CHECK(load_like_draw(path, 0) == -DRAW_IO_E_SHORT);

	file_put(path, "hello", 5, "", 0);				// not a ZBM, too short
	CHECK(load_like_draw(path, 0) == -DRAW_IO_E_NOTZBM);

	file_put(path, NULL, 0, old_img, 20000);		// not a ZBM
	CHECK(load_like_draw(path, 0) == -DRAW_IO_E_NOTZBM);

	z_bm_header_init(&h, 320, 240);					// another size
	file_put(path, &h, Z_BM_HEADER_SIZE, old_img, 320 / 32 * 4 * 240);
	CHECK(load_like_draw(path, 0) == -DRAW_IO_E_SIZE);

	z_bm_header_init(&h, 640, 480);					// a colour picture (pix's)
	h.flags = 4;
	file_put(path, &h, Z_BM_HEADER_SIZE, old_img, 20000);
	CHECK(load_like_draw(path, 0) == -DRAW_IO_E_COLOUR);

	put_zbm(path, old_img, BYTES);					// a read fails
	hostfs_fault.read_at = 5;
	CHECK(load_like_draw(path, 0) == -DRAW_IO_E_READ);
	hostfs_reset();

	CHECK(on_screen == canvas);
	fill(old_img, 8);
	CHECK(!memcmp(canvas, old_img, BYTES));
	CHECK(hostfs_open_count() == 0);

	// a headerless canvas from before ZBM still loads
	fill(old_img, 9);
	file_put(path, NULL, 0, old_img, BYTES);
	CHECK(load_like_draw(path, 0) == DRAW_IO_DONE);
	CHECK(on_screen == spare && !memcmp(on_screen, old_img, BYTES));
	CHECK(hostfs_open_count() == 0);
}

// Failing saves: the old file stays and the .part goes (but for the one
// case where the .part is all that is left).
static void test_save_bad(void) {
	draw_io_t io;
	int k;

	fill(old_img, 10);
	fill(canvas, 11);

	for (k = 1; k <= 1 + (BYTES + DRAW_IO_CHUNK - 1) / DRAW_IO_CHUNK; k++) {
		hostfs_reset();
		hostfs_fault.write_at = k;
		hostfs_fault.write_short = k & 1;
		put_zbm(path, old_img, BYTES);
		CHECK(draw_io_save_start(&io, path, canvas, W, H));
		run(&io, 3);
		CHECK(io.state == DRAW_IO_FAILED && io.err == DRAW_IO_E_WRITE);
		CHECK(!exists(part) && is_zbm_of(path, old_img));
		CHECK(hostfs_open_count() == 0);
	}

	hostfs_reset();
	put_zbm(path, old_img, BYTES);
	hostfs_fault.open_write = 1;
	CHECK(!draw_io_save_start(&io, path, canvas, W, H));
	CHECK(io.state == DRAW_IO_FAILED && io.err == DRAW_IO_E_OPEN);
	CHECK(!exists(part) && is_zbm_of(path, old_img));

	hostfs_reset();
	hostfs_fault.close = 1;							// the flush at close
	CHECK(draw_io_save_start(&io, path, canvas, W, H));
	run(&io, 3);
	CHECK(io.state == DRAW_IO_FAILED && io.err == DRAW_IO_E_WRITE);
	CHECK(!exists(part) && is_zbm_of(path, old_img));

	hostfs_reset();
	hostfs_fault.unlink = 1;						// the old file will not go
	CHECK(draw_io_save_start(&io, path, canvas, W, H));
	run(&io, 3);
	CHECK(io.state == DRAW_IO_FAILED && io.err == DRAW_IO_E_RENAME);
	hostfs_reset();
	CHECK(is_zbm_of(path, old_img));
	unlink(part);	// (the fault also kept fail() from deleting it)

	hostfs_reset();
	hostfs_fault.rename = 1;						// it went, the rename did not
	CHECK(draw_io_save_start(&io, path, canvas, W, H));
	run(&io, 3);
	CHECK(io.state == DRAW_IO_FAILED && io.err == DRAW_IO_E_KEPT);
	CHECK(!exists(path) && is_zbm_of(part, canvas));
	CHECK(hostfs_open_count() == 0);
	unlink(part);

	hostfs_reset();
	hostfs_fault.rename = 1;						// no old file: nothing to lose
	CHECK(draw_io_save_start(&io, path, canvas, W, H));
	run(&io, 3);
	CHECK(io.state == DRAW_IO_FAILED && io.err == DRAW_IO_E_RENAME);
	CHECK(!exists(path) && !exists(part));
	hostfs_reset();
}

int main(void) {
	strcpy(dir, "/tmp/test_draw_XXXXXX");
	if (!mkdtemp(dir)) { perror("mkdtemp"); return 1; }
	snprintf(path, sizeof(path), "%s/pic.zbm", dir);
	snprintf(part, sizeof(part), "%s.part", path);

	test_save_load();
	test_save_stop();
	test_load_stop();
	test_load_bad();
	test_save_bad();

	unlink(path);
	unlink(part);
	rmdir(dir);

	if (fails) { printf("draw io: %d FAILED\n", fails); return 1; }
	printf("draw io: ok\n");
	return 0;
}
