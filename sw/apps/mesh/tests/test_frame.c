/*
 * Host tests for sw/apps/mesh/mesh_frame.c, the Meshtastic stream
 * framer. `make test` in sw/apps/mesh.
 *
 * What matters is not the happy path but resynchronisation: the stream
 * has no CRC, the node's console text shares it, and bytes can be lost
 * on the far side of a CP2102 with no flow control. Every case here is
 * a way the stream can be wrong, and the check is that the framer
 * finds the next good frame afterwards.
 */

#include <stdio.h>
#include <string.h>
#include <stdint.h>

#include "../mesh_frame.h"

static int run, failed;

#define CHECK(cond, what) do { run++; if (!(cond)) { failed++; \
	printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, what); } } while (0)

// What the callbacks saw.
static uint8_t got[16][600];
static uint32_t got_len[16];
static int nframes;
static char lines[16][200];
static int nlines;

static void on_frame(void *ctx, const uint8_t *pb, uint32_t len) {
	(void)ctx;
	if (nframes < 16) {
		memcpy(got[nframes], pb, len);
		got_len[nframes] = len;
	}
	nframes++;
}

static void on_line(void *ctx, const char *line) {
	(void)ctx;
	if (nlines < 16) snprintf(lines[nlines], sizeof(lines[0]), "%s", line);
	nlines++;
}

static mesh_framer_t fr;

static void fresh(void) {
	mesh_frame_init(&fr, on_frame, on_line, NULL);
	nframes = 0;
	nlines = 0;
}

// Append a framed payload of `len` pattern bytes (seed) to buf.
static uint32_t put_frame(uint8_t *buf, uint32_t at, uint32_t len, uint8_t seed) {
	uint8_t pb[600];
	for (uint32_t i = 0; i < len; i++) pb[i] = (uint8_t)(seed + i * 7);
	return at + mesh_frame_wrap(buf + at, 2000, pb, len);
}

static int frame_ok(int i, uint32_t len, uint8_t seed) {
	if (i >= nframes || got_len[i] != len) return 0;
	for (uint32_t k = 0; k < len; k++)
		if (got[i][k] != (uint8_t)(seed + k * 7)) return 0;
	return 1;
}

static uint32_t put_text(uint8_t *buf, uint32_t at, const char *s) {
	memcpy(buf + at, s, strlen(s));
	return at + (uint32_t)strlen(s);
}

static void test_basic(void) {
	uint8_t buf[2000];
	uint32_t n = 0;

	fresh();
	n = put_text(buf, n, "INFO | booting\r\n");
	n = put_frame(buf, n, 10, 1);
	n = put_text(buf, n, "DEBUG | radio up\n");
	n = put_frame(buf, n, 512, 2);			// the maximum
	n = put_frame(buf, n, 0, 3);			// empty is valid
	n = put_frame(buf, n, 1, 4);
	mesh_frame_feed(&fr, buf, n);

	CHECK(nframes == 4, "four frames");
	CHECK(frame_ok(0, 10, 1), "frame 1 intact");
	CHECK(frame_ok(1, 512, 2), "512-byte frame intact");
	CHECK(got_len[2] == 0, "empty frame");
	CHECK(frame_ok(3, 1, 4), "one-byte frame");
	CHECK(nlines == 2, "two lines");
	CHECK(strcmp(lines[0], "INFO | booting") == 0, "CRLF stripped");
	CHECK(strcmp(lines[1], "DEBUG | radio up") == 0, "LF line");
	CHECK(fr.resyncs == 0, "no resyncs");

	// The same bytes one at a time, and in awkward chunks.
	fresh();
	for (uint32_t i = 0; i < n; i++) mesh_frame_feed(&fr, buf + i, 1);
	CHECK(nframes == 4 && frame_ok(1, 512, 2), "byte at a time");
	fresh();
	for (uint32_t i = 0; i < n; i += 37)
		mesh_frame_feed(&fr, buf + i, n - i < 37 ? n - i : 37);
	CHECK(nframes == 4 && frame_ok(1, 512, 2) && nlines == 2, "37-byte chunks");
}

static void test_utf8_in_text(void) {
	// An em dash is E2 80 94: its last byte is START1. It must come
	// out as text, and must not eat the byte after it.
	uint8_t buf[200];
	uint32_t n = 0;
	fresh();
	n = put_text(buf, n, "a \xe2\x80\x94 b\n");
	n = put_frame(buf, n, 5, 9);
	mesh_frame_feed(&fr, buf, n);
	CHECK(nlines == 1 && strcmp(lines[0], "a \xe2\x80\x94 b") == 0,
		"em dash survives as text");
	CHECK(nframes == 1 && frame_ok(0, 5, 9), "frame after it");

	// START1 immediately before a real header: 94 94 c3 ...
	fresh();
	n = 0;
	buf[n++] = 0x94;
	n = put_frame(buf, n, 6, 10);
	mesh_frame_feed(&fr, buf, n);
	CHECK(nframes == 1 && frame_ok(0, 6, 10), "stray START1 before a header");
}

static void test_bad_length(void) {
	uint8_t buf[200];
	uint32_t n = 0;
	fresh();
	// A header with an impossible length, then a good frame.
	buf[n++] = 0x94; buf[n++] = 0xc3; buf[n++] = 0x02; buf[n++] = 0x01;
	n = put_frame(buf, n, 8, 20);
	mesh_frame_feed(&fr, buf, n);
	CHECK(fr.resyncs == 1, "length 513 is a resync");
	CHECK(nframes == 1 && frame_ok(0, 8, 20), "good frame after it");

	// A header whose "length" is the start of the real header: the
	// START1 START2 of a header whose first two bytes were lost... or a
	// doubled header. 94 c3 94 c3 00 03 ...
	fresh();
	n = 0;
	buf[n++] = 0x94; buf[n++] = 0xc3;
	n = put_frame(buf, n, 3, 30);
	mesh_frame_feed(&fr, buf, n);
	CHECK(nframes == 1 && frame_ok(0, 3, 30), "header inside a bad length");
}

static void test_lost_byte(void) {
	// A byte lost from the middle of frame 1: frame 1 then ends one
	// byte into frame 2, frame 2 is lost, and frame 3 must arrive
	// intact. Checked for a loss at every position in frame 1.
	int recovered = 0, tries = 0;
	for (uint32_t lose = 4; lose < 4 + 40; lose++) {
		uint8_t buf[400], cut[400];
		uint32_t n = 0, m = 0;
		fresh();
		n = put_frame(buf, n, 40, 1);
		n = put_frame(buf, n, 40, 2);
		n = put_frame(buf, n, 40, 3);
		n = put_frame(buf, n, 40, 4);
		for (uint32_t i = 0; i < n; i++) if (i != lose) cut[m++] = buf[i];
		mesh_frame_feed(&fr, cut, m);
		tries++;
		for (int i = 0; i < nframes && i < 16; i++)
			if (frame_ok(i, 40, 3)) { recovered++; break; }
	}
	CHECK(recovered == tries, "frame 3 recovered after a byte lost anywhere in frame 1");
}

static void test_long_line(void) {
	char s[400];
	memset(s, 'x', 350);
	s[350] = '\n';
	s[351] = 0;
	fresh();
	mesh_frame_feed(&fr, (const uint8_t *)s, 351);
	CHECK(nlines == 3, "a 350-byte line comes out in pieces");
	CHECK(strlen(lines[0]) == MESH_LINE_MAX, "first piece full");
	CHECK(strlen(lines[2]) == 350 - 2 * MESH_LINE_MAX, "remainder");
}

static void test_wrap(void) {
	uint8_t out[600], pb[600];
	memset(pb, 0xab, sizeof(pb));
	CHECK(mesh_frame_wrap(out, sizeof(out), pb, 513) == 0, "513 refused");
	CHECK(mesh_frame_wrap(out, 10, pb, 7) == 0, "no room refused");
	CHECK(mesh_frame_wrap(out, sizeof(out), pb, 300) == 304, "300 wraps to 304");
	CHECK(out[0] == 0x94 && out[1] == 0xc3 && out[2] == 1 && out[3] == 44,
		"header big-endian");
}

static void test_reset(void) {
	uint8_t buf[100];
	uint32_t n = put_frame(buf, 0, 30, 5);
	fresh();
	mesh_frame_feed(&fr, buf, 10);		// half a frame
	mesh_frame_reset(&fr);
	mesh_frame_feed(&fr, buf, n);		// a whole one
	CHECK(nframes == 1 && frame_ok(0, 30, 5), "reset drops the partial frame");
}

int main(void) {
	test_basic();
	test_utf8_in_text();
	test_bad_length();
	test_lost_byte();
	test_long_line();
	test_wrap();
	test_reset();
	printf("mesh_frame: %d checks, %d failed\n", run, failed);
	return failed ? 1 : 0;
}
