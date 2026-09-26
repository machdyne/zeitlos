/*
 * Host test for sw/os/authcore.c: PBKDF2-HMAC-SHA256, the stored
 * record, and the backoff schedule.
 *
 *   cc -std=gnu99 -O2 -Wall -o /tmp/ta sw/os/tests/test_auth.c \
 *       sw/os/authcore.c sw/common/zsha256.c && /tmp/ta
 *
 * The PBKDF2 vectors are the SHA-256 ones commonly paired with RFC 6070
 * (which gives only SHA-1): P="password", S="salt", and a long
 * password and salt spanning more than one HMAC block.
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "../authcore.h"

static int fails, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; printf("FAIL %s:%d: ", \
	__FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static void hex(const char *h, uint8_t *out) {
	for (int i = 0; h[2 * i]; i++) {
		unsigned v;
		sscanf(h + 2 * i, "%2x", &v);
		out[i] = (uint8_t)v;
	}
}

static void vec(const char *p, uint32_t plen, const char *s, uint32_t slen,
		uint32_t c, const char *want_hex) {
	uint8_t want[32], got[32];
	hex(want_hex, want);
	auth_pbkdf2((const uint8_t *)p, plen, (const uint8_t *)s, slen, c, got);
	CHECK(!memcmp(got, want, 32), "PBKDF2 P=%.10s c=%u", p, c);
}

int main(void) {
	vec("password", 8, "salt", 4, 1,
		"120fb6cffcf8b32c43e7225256c4f837a86548c92ccc35480805987cb70be17b");
	vec("password", 8, "salt", 4, 2,
		"ae4d0c95af6b46d32d0adff928f06dd02a303f8ef3c251dfd6e2d85a95474c43");
	vec("password", 8, "salt", 4, 4096,
		"c5e478d59288c841aa530db6845c4c8d962893a001ce4e11a4963873aa98134a");
	vec("passwordPASSWORDpassword", 24, "saltSALTsaltSALTsaltSALTsaltSALTsalt", 36, 4096,
		"348c89dbcbd32b2f32d814b8116e84cf2b17347ebc1800181c4e2a1fb8dd53e1");
	vec("pass\0word", 9, "sa\0lt", 5, 4096,
		"89b69d0516f829893c696226650a86878c029ac13ee276509d5ae58b6466a724");
	// a key longer than the HMAC block is hashed first (RFC 2104)
	{
		char longpw[100];
		uint8_t a[32], b[32], k[32];
		z_sha256_ctx c;
		memset(longpw, 'x', sizeof(longpw));
		z_sha256_init(&c); z_sha256_update(&c, longpw, 100); z_sha256_final(&c, k);
		auth_pbkdf2((const uint8_t *)longpw, 100, (const uint8_t *)"s", 1, 3, a);
		auth_pbkdf2(k, 32, (const uint8_t *)"s", 1, 3, b);
		CHECK(!memcmp(a, b, 32), "a 100-byte password is not the same as its hash");
	}

	// the record
	{
		auth_rec_t r, r2;
		uint8_t buf[AUTH_REC_LEN];
		memset(&r, 0, sizeof(r));
		r.flags = AUTH_REC_NET_OK;
		r.iterations = 0x01020304u;
		for (int i = 0; i < AUTH_SALT_LEN; i++) r.salt[i] = (uint8_t)(i + 1);
		for (int i = 0; i < AUTH_DK_LEN; i++) r.dk[i] = (uint8_t)(0xA0 + i);
		auth_rec_pack(&r, buf);
		CHECK(AUTH_REC_LEN == 54, "record is %d bytes", AUTH_REC_LEN);
		CHECK(auth_rec_unpack(buf, sizeof(buf), &r2) == 0 && r2.flags == r.flags &&
			r2.iterations == r.iterations && !memcmp(r2.salt, r.salt, AUTH_SALT_LEN) &&
			!memcmp(r2.dk, r.dk, AUTH_DK_LEN), "record round trip");
		CHECK(auth_rec_unpack(buf, sizeof(buf) - 1, &r2) != 0, "short record accepted");
		buf[0] = 2;
		CHECK(auth_rec_unpack(buf, sizeof(buf), &r2) != 0, "unknown version accepted");
		buf[0] = 1; buf[2] = buf[3] = buf[4] = buf[5] = 0;
		CHECK(auth_rec_unpack(buf, sizeof(buf), &r2) != 0, "zero iterations accepted");
	}

	// comparison
	{
		uint8_t a[32] = { 0 }, b[32] = { 0 };
		CHECK(auth_eq(a, b, 32), "equal");
		b[31] = 1;
		CHECK(!auth_eq(a, b, 32), "differ in the last byte");
		b[31] = 0; b[0] = 0x80;
		CHECK(!auth_eq(a, b, 32), "differ in the first byte");
	}

	// backoff
	{
		uint32_t want[] = { 0, 0, 0, 0, 1000, 2000, 4000, 8000, 16000, 32000, 60000, 60000 };
		for (uint32_t f = 0; f < sizeof(want) / sizeof(want[0]); f++)
			CHECK(auth_backoff_ms(f) == want[f], "backoff(%u) = %u", f, auth_backoff_ms(f));
		CHECK(auth_backoff_ms(0xFFFFFFFFu) == 60000, "backoff saturates");
	}

	printf("%d checks, %d failed\n", checks, fails);
	return fails != 0;
}
