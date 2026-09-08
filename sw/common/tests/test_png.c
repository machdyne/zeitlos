/*
 * Zeitlos -- host test for the PNG decoder in sw/common/zimg.c.
 *
 *   python3 sw/common/tests/gen_png_corpus.py /tmp/pngc
 *   cc -std=gnu99 -DZ_IMG_HAVE_PNG=1 -I sw/common \
 *      -o /tmp/t sw/common/tests/test_png.c sw/common/zimg.c \
 *      sw/common/zinflate.c && /tmp/t /tmp/pngc
 *
 * Every colour type this decoder claims, and every variant it claims
 * to REFUSE. The refusals matter as much as the successes: a decoder
 * that half-handles interlaced PNG produces a plausible-looking wrong
 * picture, which is worse than a message saying it cannot.
 *
 * The five row filters are not tested individually because they
 * cannot be: libpng picks per row, so one photograph-like image with
 * hard edges and a flat field exercises all of them and no test can
 * ask for a particular one.
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "zimg.h"

static int fails, checks;
static void ck(int c, const char *w) {
	checks++; if (!c) { fails++; printf("FAIL: %s\n", w); } }

static FILE *f;
static int rd(void *c, uint8_t *b, int n) { (void)c; return (int)fread(b, 1, n, f); }

#define WPL 24
static uint32_t doc[WPL * 300];

static int decode(const char *dir, const char *name, int w, int h,
	z_img_t *out) {

	char path[256];
	uint8_t hdr[16];
	z_img_fmt_t fmt;
	int n, rv;

	snprintf(path, sizeof(path), "%s/%s", dir, name);
	f = fopen(path, "rb");
	if (!f) { printf("FAIL: cannot open %s\n", path); exit(2); }

	n = (int)fread(hdr, 1, sizeof(hdr), f);
	fmt = z_img_sniff(hdr, n);
	fseek(f, 0, SEEK_SET);

	memset(out, 0, sizeof(*out));
	out->read = rd;
	out->doc = doc;
	out->doc_wpl = WPL;
	out->doc_w = w;
	out->doc_h = h;
	z_img_clear(out);

	rv = (fmt == Z_IMG_FMT_PNG) ? z_img_decode(out, fmt) : Z_IMG_E_FORMAT;
	fclose(f);
	return rv;

}

// How many pixels are set. A decode that "succeeds" into an empty or
// solid buffer is the failure this catches -- rv == OK says the
// stream parsed, not that a picture came out.
static int ink(const z_img_t *im) {
	int n = 0;
	for (int y = 0; y < im->out_h; y++)
		for (int x = 0; x < im->out_w; x++)
			if ((doc[y * WPL + (x >> 5)] >> (x & 31)) & 1) n++;
	return n;
}

int main(int argc, char **argv) {

	const char *dir = argc > 1 ? argv[1] : ".";
	z_img_t im;
	int base = 0;

	// -- colour types --
	{
		int rv = decode(dir, "rgb.png", 120, 90, &im);
		ck(rv == Z_IMG_OK, "8-bit RGB decodes");
		ck(im.out_w == 120 && im.out_h == 90, "at its own size");
		base = ink(&im);
		ck(base > 200, "and produces a picture, not an empty buffer");
	}

	{
		int rv = decode(dir, "gray.png", 120, 90, &im);
		ck(rv == Z_IMG_OK, "8-bit grayscale decodes");
		// The same image through a different colour type: the dither
		// is identical input, so the result should be very close.
		ck(abs(ink(&im) - base) < base / 4,
			"and looks like the RGB version of the same image");
	}

	{
		int rv = decode(dir, "pal.png", 120, 90, &im);
		ck(rv == Z_IMG_OK, "palette decodes");
		ck(abs(ink(&im) - base) < base / 3, "and resembles the original");
	}

	{
		int rv = decode(dir, "rgba.png", 120, 90, &im);
		ck(rv == Z_IMG_OK, "RGBA decodes");
		ck(abs(ink(&im) - base) < base / 4, "ignoring alpha, as intended");
	}

	{
		int rv = decode(dir, "bilevel.png", 120, 90, &im);
		ck(rv == Z_IMG_OK, "1-bit depth decodes");
		ck(ink(&im) > 100, "and is not blank");
	}

	// -- scaling --
	//
	// Powers of two only, and the box decides. A 120-wide image in a
	// 60-wide box comes back at 1/2.
	{
		int rv = decode(dir, "rgb.png", 60, 45, &im);
		ck(rv == Z_IMG_OK, "decodes into a smaller box");
		ck(im.out_w == 60 && im.out_h == 45, "at half size");
		ck(im.shift == 1, "by one power of two");
	}

	// -- refusals --
	//
	// Each of these would otherwise produce a wrong picture that
	// looks like a rendering bug rather than an unsupported file.
	ck(decode(dir, "adam7.png", 120, 90, &im) == Z_IMG_E_UNSUPPORTED,
		"interlaced (Adam7) is refused, not half-decoded");
	ck(decode(dir, "depth16.png", 120, 90, &im) == Z_IMG_E_UNSUPPORTED,
		"16-bit samples are refused");
	ck(decode(dir, "wide.png", 120, 90, &im) == Z_IMG_E_TOOBIG,
		"a PNG wider than the row buffer is refused");
	ck(decode(dir, "corrupt.png", 120, 90, &im) != Z_IMG_OK,
		"corrupt compressed data fails");
	ck(decode(dir, "truncated.png", 120, 90, &im) != Z_IMG_OK,
		"a truncated file fails rather than reporting success");

	printf("%s: %d checks, %d failures\n", fails ? "FAIL" : "ok",
		checks, fails);
	return fails ? 1 : 0;

}

/* zimg.c's convenience file source pulls in the app FS API. */
int fs_open_read(const char *p) { (void)p; return -1; }
int fs_read_chunk(int a, char *b, int c) { (void)a; (void)b; (void)c; return 0; }
int fs_seek(int a, unsigned b) { (void)a; (void)b; return -1; }
void fs_close_handle(int a) { (void)a; }
