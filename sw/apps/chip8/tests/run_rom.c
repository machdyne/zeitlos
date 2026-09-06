/*
 * chip8 -- headless ROM runner.
 *
 *   run_rom [options] rom.ch8
 *
 *     -p NAME      profile: chip8 | schip | xochip   (default chip8)
 *     -c N         guest instructions per frame      (default 15)
 *     -f N         frames to run                     (default 120)
 *     -P K@F       press hex key K at frame F, release 8 frames later
 *                  (repeatable, up to 8 times)
 *     -o FILE      write a PGM of the final display
 *     -s N         PGM scale                         (default 4)
 *     -q           print nothing but the hash line
 *
 * Prints one line:
 *
 *     <hash> <profile> <w>x<h> frames=<n> steps=<n> <status>
 *
 * -- why a hash and not just an image --
 *
 * The point of this program is to be run over the CHIP-8 test-suite
 * ROMs from a Makefile and compared against a committed expectation.
 * An expectation that is a directory of PGMs is a binary blob nobody
 * reviews; an expectation that is a text file of hashes shows up in a
 * diff as one changed line naming the ROM that changed behaviour. The
 * PGM is still there for when that line changes and you need to see
 * what it looks like now.
 *
 * The hash covers the LOGICAL display only -- w, h and the pixels
 * inside them. Rows and columns outside the current resolution hold
 * leftovers from whatever ran before and are deliberately not part of
 * the answer.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../core.h"

#define MAX_PRESSES 8

static uint32_t display_hash(const c8_t *c) {

	/* FNV-1a. Chosen for being four lines long and having no tuning
	 * constants to get wrong -- this needs to be stable across
	 * machines and compilers, not fast or cryptographic. */
	uint32_t h = 2166136261u;
	int x, y;

	#define FEED(b) do { h ^= (uint32_t)(uint8_t)(b); h *= 16777619u; } while (0)

	FEED(c->w); FEED(c->w >> 8);
	FEED(c->h); FEED(c->h >> 8);

	for (y = 0; y < c->h; y++)
		for (x = 0; x < c->w; x++)
			FEED(c8_pixel(c, x, y));

	#undef FEED

	return h;

}

static bool write_pgm(const c8_t *c, const char *path, int scale) {

	/* Two palettes, picked the same way the real renderer will pick
	 * them (see two_plane in core.h): a one-plane ROM's colour 1 is
	 * white, a two-plane ROM's colour 1 is the first grey.
	 *
	 * The grey values are not an even ramp because the display cannot
	 * draw one. A 4x4 ordered dither has 17 levels and the four
	 * colours land on 0, 5, 11 and 16 sixteenths, so a preview
	 * written here looks like the board rather than like an ideal
	 * that the board would have to approximate. */
	static const unsigned char grey4[4] = { 0, 80, 176, 255 };
	static const unsigned char grey2[4] = { 0, 255, 255, 255 };
	const unsigned char *grey = c->two_plane ? grey4 : grey2;

	FILE *f = fopen(path, "wb");
	int x, y, sx, sy;

	if (!f) return false;

	fprintf(f, "P5\n%d %d\n255\n", c->w * scale, c->h * scale);

	for (y = 0; y < c->h; y++)
		for (sy = 0; sy < scale; sy++)
			for (x = 0; x < c->w; x++)
				for (sx = 0; sx < scale; sx++)
					fputc(grey[c8_pixel(c, x, y) & 3], f);

	fclose(f);
	return true;

}

static const char *status_name(c8_status_t s) {
	switch (s) {
	case C8_OK:         return "ok";
	case C8_WAIT_KEY:   return "wait-key";
	case C8_WAIT_FRAME: return "wait-frame";
	case C8_HALT:       return "halt";
	default:            return "bad-opcode";
	}
}

int main(int argc, char **argv) {

	c8_t *c;
	FILE *f;
	uint8_t rom[C8_RAM_SIZE];
	size_t len;

	const char *path = NULL, *out = NULL;
	int profile = C8_PROFILE_CHIP8;
	int ipf = 15, frames = 120, scale = 4, quiet = 0;
	int press_key[MAX_PRESSES], press_at[MAX_PRESSES], npress = 0;
	int i, frame;
	c8_status_t st = C8_OK;

	for (i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "-p") && i + 1 < argc) {
			profile = c8_profile_by_name(argv[++i]);
			if (profile < 0) {
				fprintf(stderr, "run_rom: unknown profile '%s'\n", argv[i]);
				return 2;
			}
		} else if (!strcmp(argv[i], "-c") && i + 1 < argc) {
			ipf = atoi(argv[++i]);
		} else if (!strcmp(argv[i], "-f") && i + 1 < argc) {
			frames = atoi(argv[++i]);
		} else if (!strcmp(argv[i], "-o") && i + 1 < argc) {
			out = argv[++i];
		} else if (!strcmp(argv[i], "-s") && i + 1 < argc) {
			scale = atoi(argv[++i]);
		} else if (!strcmp(argv[i], "-q")) {
			quiet = 1;
		} else if (!strcmp(argv[i], "-P") && i + 1 < argc) {
			const char *a = argv[++i];
			char *at = strchr((char *)a, '@');
			if (!at || npress >= MAX_PRESSES) {
				fprintf(stderr, "run_rom: bad -P '%s'\n", a);
				return 2;
			}
			press_key[npress] = (int)strtol(a, NULL, 16);
			press_at[npress] = atoi(at + 1);
			npress++;
		} else if (argv[i][0] == '-') {
			fprintf(stderr, "run_rom: unknown option '%s'\n", argv[i]);
			return 2;
		} else {
			path = argv[i];
		}
	}

	if (!path) {
		fprintf(stderr, "usage: run_rom [-p profile] [-c ipf] [-f frames] "
			"[-P key@frame] [-o out.pgm] [-s scale] [-q] rom.ch8\n");
		return 2;
	}

	f = fopen(path, "rb");
	if (!f) {
		fprintf(stderr, "run_rom: cannot open %s\n", path);
		return 1;
	}
	len = fread(rom, 1, sizeof(rom), f);
	fclose(f);

	/* Heap rather than stack: a c8_t is 64KB of guest RAM plus the
	 * planes, and the default thread stack on some hosts is 8MB but
	 * on others much less. Nothing else here needs the heap. */
	c = malloc(sizeof(*c));
	if (!c) return 1;

	c8_init(c, c8_profile((c8_profile_t)profile), 1);

	if (!c8_load(c, rom, (uint32_t)len)) {
		fprintf(stderr, "run_rom: %s does not fit (%lu bytes)\n",
			path, (unsigned long)len);
		free(c);
		return 1;
	}

	for (frame = 0; frame < frames; frame++) {

		int executed;

		for (i = 0; i < npress; i++) {
			if (press_at[i] == frame) c8_key(c, press_key[i], true);
			if (press_at[i] + 8 == frame) c8_key(c, press_key[i], false);
		}

		/* c8_frame() first: it releases a display wait left over from
		 * the previous frame, which is what makes the CHIP-8 profile
		 * advance at all. */
		c8_frame(c);
		c8_tick_timers(c);

		st = c8_run(c, ipf, &executed);

		/* WAIT_FRAME and WAIT_KEY are the machine working as intended,
		 * not reasons to stop the run. HALT and BAD_OPCODE are. */
		if (st == C8_HALT || st == C8_BAD_OPCODE) break;

	}

	if (out && !write_pgm(c, out, scale > 0 ? scale : 1))
		fprintf(stderr, "run_rom: cannot write %s\n", out);

	if (quiet)
		printf("%08x\n", display_hash(c));
	else
		printf("%08x %s %dx%d frames=%d steps=%lu %s\n",
			display_hash(c), c8_profile_name((c8_profile_t)profile),
			c->w, c->h, frame, (unsigned long)c->steps, status_name(st));

	if (st == C8_BAD_OPCODE)
		fprintf(stderr, "run_rom: bad opcode %04X at %04X\n", c->bad_op, c->pc);

	free(c);
	return 0;

}
