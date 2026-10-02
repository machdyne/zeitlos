/*
 * Host tests for sw/common/zws.h: the WebSocket handshake of RFC 6455
 * section 1.3, the frames of section 5.7, the checks of section 5,
 * and the mouse and key packets the viewer page sends.
 *
 *   cc -std=gnu99 -Wall -Wextra -I sw/common -o /tmp/test_zws \
 *      sw/common/tests/test_zws.c sw/common/zws.c sw/common/zsha1.c \
 *      && /tmp/test_zws
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "zws.h"

static int run, failed;

#define CHECK(cond, what) do { run++; if (!(cond)) { failed++; \
	printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, what); } } while (0)

// -- a client: masked frames, as the viewer page sends them --

static const uint8_t M0[4] = { 0x37, 0xfa, 0x21, 0x3d };	// the mask of RFC 6455 5.7

static size_t cframe(uint8_t *out, int fin, int op, const uint8_t *pl, size_t len,
	const uint8_t mask[4])
{
	size_t h = 0;

	out[h++] = (uint8_t)((fin ? 0x80 : 0) | op);
	if (len < 126) {
		out[h++] = (uint8_t)(0x80 | len);
	} else if (len < 65536) {
		out[h++] = 0x80 | 126;
		out[h++] = (uint8_t)(len >> 8);
		out[h++] = (uint8_t)len;
	} else {
		out[h++] = 0x80 | 127;
		for (int i = 0; i < 8; i++)
			out[h++] = (uint8_t)((uint64_t)len >> (56 - 8 * i));
	}
	memcpy(out + h, mask, 4);
	h += 4;
	for (size_t i = 0; i < len; i++)
		out[h + i] = pl[i] ^ mask[i & 3];
	return h + len;
}

// -- the server: feed bytes in chunks, log what comes out --
//
// The log is independent of the chunking: data events are gathered per
// message, so a stream fed one byte at a time and fed whole give the
// same text.

static char logbuf[8192];
static size_t loglen;
static uint8_t msg[70000];
static size_t msglen;

static void put(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
#include <stdarg.h>
static void put(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	loglen += (size_t)vsnprintf(logbuf + loglen, sizeof logbuf - loglen, fmt, ap);
	va_end(ap);
}

static void puthex(const uint8_t *d, uint32_t n)
{
	for (uint32_t i = 0; i < n && i < 16; i++)
		put("%02x", d[i]);
	if (n > 16)
		put("..");
}

static void event(const z_ws_event *ev)
{
	switch (ev->type) {
	case Z_WS_EV_NONE:
		break;
	case Z_WS_EV_DATA:
		memcpy(msg + msglen, ev->data, ev->len);
		msglen += ev->len;
		if (ev->fin) {
			put("D%d:%zu:", ev->opcode, msglen);
			puthex(msg, (uint32_t)msglen);
			put(" ");
			msglen = 0;
		}
		break;
	case Z_WS_EV_PING:
		put("PING:%u:", ev->len); puthex(ev->data, ev->len); put(" ");
		break;
	case Z_WS_EV_PONG:
		put("PONG:%u:", ev->len); puthex(ev->data, ev->len); put(" ");
		break;
	case Z_WS_EV_CLOSE:
		put("CLOSE:%u:%u ", ev->code, ev->len);
		break;
	case Z_WS_EV_ERROR:
		put("ERR:%u ", ev->code);
		break;
	}
}

// Feeds `n` bytes of `in` in pieces of `chunk` bytes (0: all at once).
// The input is copied: the parser unmasks in place.
static const char *feed_chunked(z_ws_parser *p, const uint8_t *in, size_t n, size_t chunk)
{
	static uint8_t buf[70100];

	loglen = 0;
	logbuf[0] = 0;
	msglen = 0;
	if (!chunk)
		chunk = n ? n : 1;
	for (size_t off = 0; off < n; off += chunk) {
		size_t len = n - off < chunk ? n - off : chunk;
		uint8_t *b = buf;

		memcpy(buf, in + off, len);
		while (len) {
			z_ws_event ev;
			size_t used = z_ws_feed(p, b, len, &ev);

			event(&ev);
			if (used == 0 && ev.type == Z_WS_EV_NONE)
				break;		// would loop forever: a parser bug
			b += used;
			len -= used;
		}
	}
	return logbuf;
}

static const char *feed(const uint8_t *in, size_t n, size_t chunk, uint32_t max)
{
	z_ws_parser p;

	z_ws_parser_init(&p, max);
	return feed_chunked(&p, in, n, chunk);
}

static int is(const char *got, const char *want)
{
	if (!strcmp(got, want))
		return 1;
	printf("  got  \"%s\"\n  want \"%s\"\n", got, want);
	return 0;
}

// The same stream whole, byte at a time, and in a few odd sizes
static void same_everywhere(const uint8_t *in, size_t n, uint32_t max, const char *want,
	const char *what)
{
	static const size_t sizes[] = { 0, 1, 2, 3, 5, 7, 13, 64, 1000 };
	char name[96];

	for (size_t i = 0; i < sizeof sizes / sizeof *sizes; i++) {
		snprintf(name, sizeof name, "%s, chunks of %zu", what, sizes[i]);
		CHECK(is(feed(in, n, sizes[i], max), want), name);
	}
}

// -- tests --

static void test_handshake(void)
{
	static const char key[] = "dGhlIHNhbXBsZSBub25jZQ==";
	char acc[Z_WS_ACCEPT_LEN + 1], out[Z_WS_RESPONSE_MAX];
	size_t n;

	CHECK(z_ws_accept(acc, key, 24) == 0 && !strcmp(acc, "s3pPLMBiTxaQ9kYGzzhZRbK+xOo="),
		"RFC 6455 1.3 accept");
	CHECK(z_ws_accept(acc, key, 23) == -1, "short key refused");
	CHECK(z_ws_accept(acc, key, 0) == -1, "empty key refused");

	n = z_ws_response(out, sizeof out, key, 24);
	CHECK(n == strlen(out) && n < Z_WS_RESPONSE_MAX, "response length is its strlen");
	CHECK(!strcmp(out, "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\n"
		"Connection: Upgrade\r\nSec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=\r\n\r\n"),
		"response text");
	CHECK(z_ws_response(out, Z_WS_RESPONSE_MAX - 1, key, 24) == 0, "buffer too small");
	CHECK(z_ws_response(out, sizeof out, key, 25) == 0, "long key refused");
	CHECK(!strstr(out, "extensions"), "no extension is ever accepted");
}

static void test_send(void)
{
	uint8_t h[Z_WS_HEADER_MAX], c[Z_WS_CONTROL_MAX];
	static const uint8_t hello[5] = { 'H', 'e', 'l', 'l', 'o' };

	// RFC 6455 5.7: unmasked "Hello" is 81 05 48 65 6c 6c 6f
	CHECK(z_ws_header(h, Z_WS_TEXT, 5) == 2 && h[0] == 0x81 && h[1] == 5, "text 5");
	CHECK(z_ws_header(h, Z_WS_BINARY, 0) == 2 && h[0] == 0x82 && h[1] == 0, "binary 0");
	CHECK(z_ws_header(h, Z_WS_BINARY, 125) == 2 && h[1] == 125, "125 is the last 7-bit");
	// 126 starts the 16-bit form: 82 7E 0100 for 256 bytes
	CHECK(z_ws_header(h, Z_WS_BINARY, 126) == 4 && h[1] == 126 && h[2] == 0 && h[3] == 126,
		"126 uses 16 bits");
	CHECK(z_ws_header(h, Z_WS_BINARY, 256) == 4 && h[0] == 0x82 && h[1] == 0x7e &&
		h[2] == 1 && h[3] == 0, "RFC 256 bytes");
	CHECK(z_ws_header(h, Z_WS_BINARY, 65535) == 4 && h[2] == 0xff && h[3] == 0xff,
		"65535 is the last 16-bit");
	// 64 KiB: 82 7F 0000000000010000
	CHECK(z_ws_header(h, Z_WS_BINARY, 65536) == 10 && h[1] == 127 &&
		!memcmp(h + 2, "\0\0\0\0\0\1\0\0", 8), "RFC 64 KiB");
	CHECK(z_ws_header(h, Z_WS_BINARY, 0x123456789aULL) == 10 &&
		!memcmp(h + 2, "\0\0\0\x12\x34\x56\x78\x9a", 8), "64-bit length, network order");

	CHECK(z_ws_control(c, Z_WS_PONG, hello, 5) == 7 && c[0] == 0x8a && c[1] == 5 &&
		!memcmp(c + 2, hello, 5), "pong with payload");
	CHECK(z_ws_control(c, Z_WS_PING, hello, 0) == 2 && c[0] == 0x89 && c[1] == 0, "empty ping");
	{
		uint8_t big[200] = { 0 };
		CHECK(z_ws_control(c, Z_WS_PING, big, 200) == 127 && c[1] == 125, "payload cut to 125");
	}
	CHECK(z_ws_close(c, 1002) == 4 && c[0] == 0x88 && c[1] == 2 && c[2] == 0x03 && c[3] == 0xea,
		"close 1002 (the bytes the prototype hard-coded were 03 f1 for 1009)");
	CHECK(z_ws_close(c, 1009) == 4 && c[2] == 0x03 && c[3] == 0xf1, "close 1009");
}

static void test_rfc_frames(void)
{
	// RFC 6455 5.7: a single-frame masked text message "Hello"
	static const uint8_t hello[] = { 0x81, 0x85, 0x37, 0xfa, 0x21, 0x3d, 0x7f, 0x9f, 0x4d, 0x51, 0x58 };
	// a masked ping with "Hello"
	static const uint8_t ping[] = { 0x89, 0x85, 0x37, 0xfa, 0x21, 0x3d, 0x7f, 0x9f, 0x4d, 0x51, 0x58 };
	uint8_t pl[70000], in[70200];
	size_t n;

	same_everywhere(hello, sizeof hello, 125, "D1:5:48656c6c6f ", "RFC masked Hello");
	same_everywhere(ping, sizeof ping, 125, "PING:5:48656c6c6f ", "RFC masked ping");

	// fragmented "Hel" + "lo" (5.7 shows it unmasked; ours is the masked form),
	// with a ping in the middle
	n = cframe(in, 0, Z_WS_TEXT, (const uint8_t *)"Hel", 3, M0);
	n += cframe(in + n, 1, Z_WS_PING, (const uint8_t *)"x", 1, M0);
	n += cframe(in + n, 1, Z_WS_CONT, (const uint8_t *)"lo", 2, M0);
	same_everywhere(in, n, 125, "PING:1:78 D1:5:48656c6c6f ", "fragments with a ping between");

	// three fragments of binary, the middle one empty
	n = cframe(in, 0, Z_WS_BINARY, (const uint8_t *)"ab", 2, M0);
	n += cframe(in + n, 0, Z_WS_CONT, NULL, 0, M0);
	n += cframe(in + n, 1, Z_WS_CONT, (const uint8_t *)"c", 1, M0);
	same_everywhere(in, n, 125, "D2:3:616263 ", "binary in three fragments, one empty");

	// an empty message, a message after it, then close with a reason
	{
		uint8_t cl[] = { 0x03, 0xe8, 'b', 'y', 'e' };
		n = cframe(in, 1, Z_WS_BINARY, NULL, 0, M0);
		n += cframe(in + n, 1, Z_WS_TEXT, (const uint8_t *)"z", 1, M0);
		n += cframe(in + n, 1, Z_WS_CLOSE, cl, sizeof cl, M0);
		same_everywhere(in, n, 125, "D2:0: D1:1:7a CLOSE:1000:3 ", "empty message, then close");
	}
	n = cframe(in, 1, Z_WS_CLOSE, NULL, 0, M0);
	same_everywhere(in, n, 125, "CLOSE:1005:0 ", "close with no status");
	n = cframe(in, 1, Z_WS_PONG, NULL, 0, M0);
	same_everywhere(in, n, 125, "PONG:0: ", "empty pong");

	// lengths at the edges of the three encodings
	{
		static const size_t lens[] = { 125, 126, 127, 65535, 65536, 65537 };
		for (size_t i = 0; i < sizeof lens / sizeof *lens; i++) {
			char want[64], name[32];

			for (size_t k = 0; k < lens[i]; k++)
				pl[k] = (uint8_t)(k * 31 + 7);
			n = cframe(in, 1, Z_WS_BINARY, pl, lens[i], M0);
			snprintf(want, sizeof want, "D2:%zu:%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x.. ",
				lens[i], pl[0], pl[1], pl[2], pl[3], pl[4], pl[5], pl[6], pl[7], pl[8],
				pl[9], pl[10], pl[11], pl[12], pl[13], pl[14], pl[15]);
			snprintf(name, sizeof name, "a frame of %zu bytes", lens[i]);
			CHECK(is(feed(in, n, 0, 70000), want), name);
			CHECK(is(feed(in, n, 1, 70000), want), name);
			CHECK(is(feed(in, n, 4093, 70000), want), name);
		}
	}
	// And that what comes out is the payload, byte for byte
	{
		z_ws_parser p;
		uint8_t buf[70200], got[70000];
		size_t got_n = 0, off = 0;
		z_ws_event ev;

		for (size_t k = 0; k < 65537; k++)
			pl[k] = (uint8_t)(k * 31 + 7);
		n = cframe(in, 1, Z_WS_BINARY, pl, 65537, M0);
		memcpy(buf, in, n);
		z_ws_parser_init(&p, 70000);
		while (off < n) {
			size_t used = z_ws_feed(&p, buf + off, n - off > 999 ? 999 : n - off, &ev);

			if (ev.type == Z_WS_EV_DATA) {
				memcpy(got + got_n, ev.data, ev.len);
				got_n += ev.len;
			}
			off += used;
		}
		CHECK(got_n == 65537 && !memcmp(got, pl, 65537), "65537 bytes arrive intact, unmasked");
	}
}

// The packets the viewer page sends: mouse is [x lo, x hi, y lo, y hi,
// buttons], key is [usage, mods, pressed], each as one masked binary frame.
static void test_viewer_packets(void)
{
	static const uint8_t mouse[5] = { 0x40, 0x01, 0xc8, 0x00, 0x01 };	// x=320 y=200 left
	static const uint8_t key[3] = { 19, 0x04, 1 };				// P, alt, pressed
	static const uint8_t m1[4] = { 0xde, 0xad, 0xbe, 0xef };
	uint8_t in[128];
	size_t n;

	n = cframe(in, 1, Z_WS_BINARY, mouse, 5, M0);
	CHECK(n == 11 && in[0] == 0x82 && in[1] == 0x85, "a mouse packet is 82 85 <mask> <5>");
	same_everywhere(in, n, 125, "D2:5:4001c80001 ", "mouse packet");
	n = cframe(in, 1, Z_WS_BINARY, key, 3, m1);
	CHECK(n == 9 && in[1] == 0x83, "a key packet is 82 83 <mask> <3>");
	same_everywhere(in, n, 125, "D2:3:130401 ", "key packet");

	// a burst of both, back to back, as a drag sends them
	n = 0;
	for (int i = 0; i < 4; i++) {
		n += cframe(in + n, 1, Z_WS_BINARY, mouse, 5, M0);
		n += cframe(in + n, 1, Z_WS_BINARY, key, 3, m1);
	}
	same_everywhere(in, n, 125,
		"D2:5:4001c80001 D2:3:130401 D2:5:4001c80001 D2:3:130401 "
		"D2:5:4001c80001 D2:3:130401 D2:5:4001c80001 D2:3:130401 ", "burst of eight");
}

static void test_errors(void)
{
	uint8_t in[300], pl[300] = { 0 };
	size_t n;

	// unmasked: what a server sends, and not what a client may
	in[0] = 0x82; in[1] = 3; in[2] = 1; in[3] = 2; in[4] = 3;
	same_everywhere(in, 5, 125, "ERR:1002 ", "unmasked frame");

	n = cframe(in, 1, Z_WS_BINARY, pl, 1, M0);
	in[0] |= 0x40;
	same_everywhere(in, n, 125, "ERR:1002 ", "RSV1 (permessage-deflate was not accepted)");
	in[0] = 0x82 | 0x20;
	same_everywhere(in, n, 125, "ERR:1002 ", "RSV3");

	n = cframe(in, 1, 3, pl, 1, M0);
	same_everywhere(in, n, 125, "ERR:1002 ", "reserved data opcode 3");
	n = cframe(in, 1, 0xb, pl, 1, M0);
	same_everywhere(in, n, 125, "ERR:1002 ", "reserved control opcode b");
	n = cframe(in, 0, Z_WS_PING, pl, 1, M0);
	same_everywhere(in, n, 125, "ERR:1002 ", "fragmented ping");
	n = cframe(in, 1, Z_WS_PING, pl, 126, M0);
	same_everywhere(in, n, 125, "ERR:1002 ", "ping of 126 bytes");
	n = cframe(in, 1, Z_WS_CLOSE, pl, 1, M0);
	same_everywhere(in, n, 125, "ERR:1002 ", "close with one byte of status");
	n = cframe(in, 1, Z_WS_CONT, pl, 1, M0);
	same_everywhere(in, n, 125, "ERR:1002 ", "continuation with nothing to continue");

	n = cframe(in, 0, Z_WS_BINARY, pl, 1, M0);
	n += cframe(in + n, 1, Z_WS_TEXT, pl, 1, M0);
	same_everywhere(in, n, 125, "ERR:1002 ", "new message inside a fragmented one");

	// too long: 126 against a limit of 125, and each length form
	n = cframe(in, 1, Z_WS_BINARY, pl, 126, M0);
	same_everywhere(in, n, 125, "ERR:1009 ", "126 against a limit of 125");
	n = cframe(in, 1, Z_WS_BINARY, pl, 125, M0);
	same_everywhere(in, n, 124, "ERR:1009 ", "125 against a limit of 124");
	same_everywhere(in, n, 125, "D2:125:00000000000000000000000000000000.. ", "125 against a limit of 125");
	{
		// 64-bit lengths: 2^32 and 2^63 are 1009 whatever the limit
		static const uint8_t big32[] = { 0x82, 0xff, 0, 0, 0, 1, 0, 0, 0, 0, 1, 2, 3, 4 };
		static const uint8_t big63[] = { 0x82, 0xff, 0x80, 0, 0, 0, 0, 0, 0, 0, 1, 2, 3, 4 };
		static const uint8_t max32[] = { 0x82, 0xff, 0, 0, 0, 0, 0xff, 0xff, 0xff, 0xff, 1, 2, 3, 4 };
		same_everywhere(big32, sizeof big32, 0xffffffffu, "ERR:1009 ", "2^32 bytes");
		same_everywhere(big63, sizeof big63, 0xffffffffu, "ERR:1009 ", "2^63 bytes");
		same_everywhere(max32, sizeof max32, 1000, "ERR:1009 ", "2^32-1 against a limit of 1000");
	}

	// after an error everything is discarded, including a good frame
	n = cframe(in, 1, Z_WS_BINARY, pl, 1, M0);
	in[0] |= 0x40;
	n += cframe(in + n, 1, Z_WS_BINARY, pl, 1, M0);
	same_everywhere(in, n, 125, "ERR:1002 ", "one error, then silence");
}

int main(void)
{
	test_handshake();
	test_send();
	test_rfc_frames();
	test_viewer_packets();
	test_errors();
	printf("%d checks, %d failed\n", run, failed);
	return failed != 0;
}
