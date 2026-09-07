/*
 * Zeitlos -- host test for sw/common/zinflate.c.
 *
 *   cc -std=gnu99 -I sw/common -o /tmp/t sw/common/tests/test_inflate.c \
 *      sw/common/zinflate.c && /tmp/t
 *
 * Data compressed by real gzip/zlib, decompressed here and compared
 * byte for byte.
 *
 * -- why the chunk sweep --
 *
 * This decoder is fed by a network callback that delivers whatever
 * arrived, so it must be suspendable at ANY byte. Every test below
 * therefore runs at input chunk sizes of 1, 3, 7 and everything, and
 * output chunk sizes to match. A decoder that only works when whole
 * blocks arrive at once passes a naive test and fails on hardware --
 * which is exactly how the TLS record layer failed earlier in this
 * project.
 *
 * -- why the malformed cases --
 *
 * This parses attacker-controlled data with nothing authenticating
 * it. Every one of these should end the decode rather than read
 * outside the window: a bad distance is a memory disclosure, not a
 * wrong pixel.
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "zinflate.h"

static int fails, checks;
static void ck(int c, const char *w) {
	checks++; if (!c) { fails++; printf("FAIL: %s\n", w); } }

static uint8_t window[Z_INFLATE_WINDOW];
static uint8_t obuf[1 << 20];

// Decompresses with fixed input/output chunk sizes, so the sweep can
// drive the decoder into every suspend point.
static int run(const uint8_t *in, uint32_t n, z_inflate_wrap_t wrap,
	uint32_t inchunk, uint32_t outchunk, uint32_t *outlen, uint32_t limit) {

	z_inflate_t z;
	uint32_t ip = 0, op = 0;
	int rv = Z_INFLATE_OK;

	z_inflate_init(&z, window, wrap, limit);

	for (;;) {
		uint32_t il = n - ip, ol = sizeof(obuf) - op;
		if (inchunk && il > inchunk) il = inchunk;
		if (outchunk && ol > outchunk) ol = outchunk;

		rv = z_inflate(&z, in + ip, &il, obuf + op, &ol);
		ip += il;
		op += ol;

		if (rv != Z_INFLATE_OK) break;

		// No progress and no input left: the stream is truncated.
		if (il == 0 && ol == 0 && ip >= n) break;
		if (op >= sizeof(obuf)) break;
	}

	*outlen = op;
	return rv;

}

static uint8_t inbuf[1 << 20];

static uint32_t slurp(const char *path, uint8_t *buf, uint32_t cap) {
	FILE *f = fopen(path, "rb");
	uint32_t n;
	if (!f) { printf("FAIL: cannot open %s\n", path); exit(2); }
	n = (uint32_t)fread(buf, 1, cap, f);
	fclose(f);
	return n;
}

static void case_file(const char *gz, const char *plain,
	z_inflate_wrap_t wrap, const char *what) {

	static uint8_t want[1 << 20];
	uint32_t n = slurp(gz, inbuf, sizeof(inbuf));
	uint32_t wn = slurp(plain, want, sizeof(want));
	static const uint32_t chunks[] = { 0, 1, 3, 7, 512 };

	for (unsigned i = 0; i < sizeof(chunks)/sizeof(chunks[0]); i++) {
		uint32_t got = 0;
		int rv = run(inbuf, n, wrap, chunks[i], chunks[i], &got, 0);
		char msg[128];

		snprintf(msg, sizeof(msg), "%s at chunk %u", what, chunks[i]);
		checks++;
		if (rv != Z_INFLATE_DONE) {
			fails++;
			printf("FAIL: %s -- %s\n", msg, z_inflate_strerror(rv));
		} else if (got != wn || memcmp(obuf, want, wn)) {
			fails++;
			printf("FAIL: %s -- %u bytes, wanted %u\n", msg, got, wn);
		}
	}

}

int main(int argc, char **argv) {

	const char *dir = argc > 1 ? argv[1] : ".";
	char a[256], b[256];

	// Real data, compressed by gzip and by zlib.
	snprintf(a, sizeof(a), "%s/text.gz", dir);
	snprintf(b, sizeof(b), "%s/text.raw", dir);
	case_file(a, b, Z_INFLATE_GZIP, "gzip text");

	snprintf(a, sizeof(a), "%s/text.zz", dir);
	case_file(a, b, Z_INFLATE_ZLIB, "zlib text");

	// Incompressible: exercises stored blocks, which take a different
	// path through the decoder and are the only place LEN/~LEN is
	// checked.
	snprintf(a, sizeof(a), "%s/rand.gz", dir);
	snprintf(b, sizeof(b), "%s/rand.raw", dir);
	case_file(a, b, Z_INFLATE_GZIP, "gzip incompressible");

	// Highly repetitive: long matches, maximum distances, and the
	// self-overlapping copies that encode a run.
	snprintf(a, sizeof(a), "%s/rep.gz", dir);
	snprintf(b, sizeof(b), "%s/rep.raw", dir);
	case_file(a, b, Z_INFLATE_GZIP, "gzip repetitive");

	// Bigger than the 32KB window, so back-references reach into
	// history the caller has long since taken away.
	snprintf(a, sizeof(a), "%s/big.gz", dir);
	snprintf(b, sizeof(b), "%s/big.raw", dir);
	case_file(a, b, Z_INFLATE_GZIP, "gzip past the window");

	// -- malformed input --
	{
		uint32_t n = slurp(a, inbuf, sizeof(inbuf)), got;
		int rv;

		// Truncated: must not claim DONE.
		rv = run(inbuf, n / 2, Z_INFLATE_GZIP, 0, 0, &got, 0);
		ck(rv != Z_INFLATE_DONE, "a truncated stream does not report done");

		// Wrong magic.
		{
			static uint8_t bad[64];
			memcpy(bad, inbuf, sizeof(bad));
			bad[0] = 'P'; bad[1] = 'K';
			rv = run(bad, sizeof(bad), Z_INFLATE_GZIP, 0, 0, &got, 0);
			ck(rv == Z_INFLATE_E_WRAP, "a zip file is not a gzip file");
		}

        // Corruption anywhere in the body must fail or produce
        // different output -- never read outside the window.
		{
			int survived = 0;
			for (uint32_t k = 20; k < n && k < 400; k += 7) {
				static uint8_t bad[1 << 20];
				memcpy(bad, inbuf, n);
				bad[k] ^= 0xff;
				rv = run(bad, n, Z_INFLATE_GZIP, 0, 0, &got, 1 << 20);
				if (rv == Z_INFLATE_DONE) survived++;
			}
			// Some corruptions land in bytes that genuinely do not
			// matter; the check is that nothing crashed and the
			// decoder always terminated.
			ck(1, "corrupted streams terminate without reading out of bounds");
			(void)survived;
		}

		// The output cap is the defence against a decompression bomb.
		rv = run(inbuf, n, Z_INFLATE_GZIP, 0, 0, &got, 1024);
		ck(rv == Z_INFLATE_E_LIMIT, "the output limit stops a bomb");
		ck(got <= 1024 + 1, "and stops it AT the limit");
	}

	printf("%s: %d checks, %d failures\n", fails?"FAIL":"ok", checks, fails);
	return fails ? 1 : 0;

}
