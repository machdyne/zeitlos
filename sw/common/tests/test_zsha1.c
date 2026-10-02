/*
 * Host tests for sw/common/zsha1.h: SHA-1 (RFC 3174) and base64 with
 * padding (RFC 4648), and the WebSocket accept value of RFC 6455
 * section 1.3 that the two exist for.
 *
 *   cc -std=gnu99 -Wall -I sw/common -o /tmp/test_zsha1 \
 *      sw/common/tests/test_zsha1.c sw/common/zsha1.c && /tmp/test_zsha1
 */

#include <stdio.h>
#include <string.h>

#include "zsha1.h"

static int run, failed;

#define CHECK(cond, what) do { run++; if (!(cond)) { failed++; \
	printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, what); } } while (0)

static void hex(const uint8_t *d, char *o)
{
	for (int i = 0; i < Z_SHA1_DIGEST; i++)
		sprintf(o + 2 * i, "%02x", d[i]);
}

static int digest_is(const void *msg, uint32_t len, const char *want)
{
	uint8_t d[Z_SHA1_DIGEST];
	char got[41];

	z_sha1(d, msg, len);
	hex(d, got);
	if (strcmp(got, want)) {
		printf("  got  %s\n  want %s\n", got, want);
		return 0;
	}
	return 1;
}

static void test_sha1(void)
{
	// RFC 3174 section 7.3 and FIPS 180 examples
	CHECK(digest_is("abc", 3, "a9993e364706816aba3e25717850c26c9cd0d89d"), "abc");
	CHECK(digest_is("", 0, "da39a3ee5e6b4b0d3255bfef95601890afd80709"), "empty");
	CHECK(digest_is("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq", 56,
		"84983e441c3bd26ebaae4aa1f95129e5e54670f1"), "56 bytes, two-block padding");
	CHECK(digest_is("0123456701234567012345670123456701234567012345670123456701234567", 64,
		"e0c094e867ef46c350ef54a7f59dd60bed92ae83"), "64 bytes, one block exactly");

	// Padding edges: 55 fits the length in the same block, 56 does not.
	char a[130];
	memset(a, 'a', sizeof a);
	CHECK(digest_is(a, 55, "c1c8bbdc22796e28c0e15163d20899b65621d65a"), "55 'a'");
	CHECK(digest_is(a, 56, "c2db330f6083854c99d4b5bfb6e8f29f201be699"), "56 'a'");
	CHECK(digest_is(a, 64, "0098ba824b5c16427bd7a1122a5a442a25ec644d"), "64 'a'");
	CHECK(digest_is(a, 119, "ee971065aaa017e0632a8ca6c77bb3bf8b1dfc56"), "119 'a'");

	// RFC 3174 TEST4: the 64-byte pattern repeated 10 times
	{
		static const char t4[] =
			"0123456701234567012345670123456701234567012345670123456701234567";
		z_sha1_ctx c;
		uint8_t d[Z_SHA1_DIGEST];
		char got[41];

		z_sha1_init(&c);
		for (int i = 0; i < 10; i++)
			z_sha1_update(&c, t4, 64);
		z_sha1_final(&c, d);
		hex(d, got);
		CHECK(!strcmp(got, "dea356a2cddd90c7a7ecedc5ebb563934f460452"), "TEST4 x10");
	}

	// One million 'a', in odd chunk sizes
	{
		z_sha1_ctx c;
		uint8_t d[Z_SHA1_DIGEST];
		char got[41], chunk[997];
		long left = 1000000;

		memset(chunk, 'a', sizeof chunk);
		z_sha1_init(&c);
		while (left) {
			long n = left < (long)sizeof chunk ? left : (long)sizeof chunk;
			z_sha1_update(&c, chunk, (uint32_t)n);
			left -= n;
		}
		z_sha1_final(&c, d);
		hex(d, got);
		CHECK(!strcmp(got, "34aa973cd4c4daa4f61eeb2bdbad27316534016f"), "million 'a'");
	}

	// Any split of the input gives the one-shot digest
	{
		char msg[200];
		uint8_t want[Z_SHA1_DIGEST], d[Z_SHA1_DIGEST];
		int bad = 0;

		for (int i = 0; i < (int)sizeof msg; i++)
			msg[i] = (char)(i * 7 + 3);
		z_sha1(want, msg, sizeof msg);
		for (int s = 0; s <= (int)sizeof msg; s++) {
			z_sha1_ctx c;
			z_sha1_init(&c);
			z_sha1_update(&c, msg, (uint32_t)s);
			z_sha1_update(&c, msg + s, (uint32_t)(sizeof msg - s));
			z_sha1_final(&c, d);
			bad += memcmp(d, want, sizeof d) != 0;
		}
		CHECK(!bad, "every two-way split of 200 bytes");
	}
}

static int b64_is(const char *in, const char *want)
{
	char out[64];
	size_t n = z_base64_encode(out, in, strlen(in));

	return n == strlen(want) && !strcmp(out, want) &&
		Z_BASE64_SIZE(strlen(in)) == strlen(want) + 1;
}

static void test_base64(void)
{
	// RFC 4648 section 10: 0, 1 and 2 bytes of padding and the lengths between
	CHECK(b64_is("", ""), "empty");
	CHECK(b64_is("f", "Zg=="), "f: two '='");
	CHECK(b64_is("fo", "Zm8="), "fo: one '='");
	CHECK(b64_is("foo", "Zm9v"), "foo: none");
	CHECK(b64_is("foob", "Zm9vYg=="), "foob");
	CHECK(b64_is("fooba", "Zm9vYmE="), "fooba");
	CHECK(b64_is("foobar", "Zm9vYmFy"), "foobar");

	// All 64 symbols, and bytes with the high bit set
	{
		static const uint8_t all[] = { 0x00, 0x10, 0x83, 0x10, 0x51, 0x87, 0x20, 0x92, 0x8b,
			0x30, 0xd3, 0x8f, 0x41, 0x14, 0x93, 0x51, 0x55, 0x97, 0x61, 0x96, 0x9b, 0x71,
			0xd7, 0x9f, 0x82, 0x18, 0xa3, 0x92, 0x59, 0xa7, 0xa2, 0x9a, 0xab, 0xb2, 0xdb,
			0xaf, 0xc3, 0x1c, 0xb3, 0xd3, 0x5d, 0xb7, 0xe3, 0x9e, 0xbb, 0xf3, 0xdf, 0xbf };
		char out[Z_BASE64_SIZE(sizeof all)];

		z_base64_encode(out, all, sizeof all);
		CHECK(!strcmp(out, "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/"),
			"alphabet");
		z_base64_encode(out, "\xff\xff\xff\xfe", 4);
		CHECK(!strcmp(out, "/////g=="), "0xff bytes");
	}
}

// The example of RFC 6455 section 1.3, built from the pieces
static void test_ws_accept(void)
{
	static const char key[] = "dGhlIHNhbXBsZSBub25jZQ==";
	static const char guid[] = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
	z_sha1_ctx c;
	uint8_t d[Z_SHA1_DIGEST];
	char out[Z_BASE64_SIZE(Z_SHA1_DIGEST)];

	z_sha1_init(&c);
	z_sha1_update(&c, key, (uint32_t)strlen(key));
	z_sha1_update(&c, guid, (uint32_t)strlen(guid));
	z_sha1_final(&c, d);
	CHECK(z_base64_encode(out, d, sizeof d) == 28, "accept is 28 characters");
	CHECK(!strcmp(out, "s3pPLMBiTxaQ9kYGzzhZRbK+xOo="), "RFC 6455 1.3 accept");
}

int main(void)
{
	test_sha1();
	test_base64();
	test_ws_accept();
	printf("%d checks, %d failed\n", run, failed);
	return failed != 0;
}
