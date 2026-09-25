/*
 * Host tests for sw/common/zpb.c, the protobuf wire-format codec.
 *
 *   cc -std=gnu99 -Wall -Wextra -I sw/common -o /tmp/test_zpb \
 *      sw/common/tests/test_zpb.c sw/common/zpb.c
 *   /tmp/test_zpb
 *
 * or `make test` in sw/apps/mesh, which runs this with mesh's own.
 *
 * The fixed vectors are the worked examples in the public description
 * of the encoding (protobuf.dev, "Encoding") -- 150 as 08 96 01,
 * "testing" as 12 07 ..., -2 as a ten-byte varint -- so the codec is
 * checked against the spec rather than against itself. Everything else
 * is a round trip, plus malformed input that must be rejected without
 * reading outside the buffer. The malformed cases run over copies at
 * the END of a guarded buffer, so an over-read lands on a canary.
 */

#include <stdio.h>
#include <string.h>
#include <stdint.h>

#include "zpb.h"

static int run, failed;

#define CHECK(cond, what) do { run++; if (!(cond)) { failed++; \
	printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, what); } } while (0)

static void test_spec_vectors(void) {
	zpb_rd_t r;
	zpb_field_t f;

	// field 1 varint 150
	static const uint8_t a[] = { 0x08, 0x96, 0x01 };
	zpb_rd_init(&r, a, sizeof(a));
	CHECK(zpb_next(&r, &f), "150: a field");
	CHECK(f.num == 1 && f.wt == ZPB_VARINT && f.v == 150, "150: value");
	CHECK(!zpb_next(&r, &f) && !r.err, "150: clean end");

	// field 2 string "testing"
	static const uint8_t b[] = { 0x12, 0x07, 't', 'e', 's', 't', 'i', 'n', 'g' };
	zpb_rd_init(&r, b, sizeof(b));
	CHECK(zpb_next(&r, &f), "testing: a field");
	CHECK(f.num == 2 && f.wt == ZPB_LEN && f.len == 7 &&
		memcmp(f.ptr, "testing", 7) == 0, "testing: value");

	// int32 -2: ten bytes
	static const uint8_t c[] = { 0x08, 0xfe, 0xff, 0xff, 0xff, 0xff,
		0xff, 0xff, 0xff, 0xff, 0x01 };
	zpb_rd_init(&r, c, sizeof(c));
	CHECK(zpb_next(&r, &f), "-2: a field");
	CHECK(zpb_i32(&f) == -2, "-2: as int32");
	CHECK(!zpb_next(&r, &f) && !r.err, "-2: consumed all ten bytes");

	// sint32 zigzag: 0 0, -1 1, 1 2, -2 3, 2147483647 4294967294
	zpb_field_t z;
	memset(&z, 0, sizeof(z));
	z.v = 0; CHECK(zpb_sint32(&z) == 0, "zigzag 0");
	z.v = 1; CHECK(zpb_sint32(&z) == -1, "zigzag 1");
	z.v = 2; CHECK(zpb_sint32(&z) == 1, "zigzag 2");
	z.v = 3; CHECK(zpb_sint32(&z) == -2, "zigzag 3");
	z.v = 4294967294u; CHECK(zpb_sint32(&z) == 2147483647, "zigzag max");
	z.v = 4294967295u; CHECK(zpb_sint32(&z) == (int32_t)0x80000000, "zigzag min");

	// Encoder produces exactly the spec's bytes.
	uint8_t out[32];
	zpb_wr_t w;
	zpb_wr_init(&w, out, sizeof(out));
	zpb_put_varint(&w, 1, 150);
	CHECK(!w.err && zpb_wr_len(&w) == 3 && memcmp(out, a, 3) == 0, "enc 150");
	zpb_wr_init(&w, out, sizeof(out));
	zpb_put_str(&w, 2, "testing");
	CHECK(!w.err && zpb_wr_len(&w) == sizeof(b) && memcmp(out, b, sizeof(b)) == 0,
		"enc testing");
	zpb_wr_init(&w, out, sizeof(out));
	zpb_put_int32(&w, 1, -2);
	CHECK(!w.err && zpb_wr_len(&w) == sizeof(c) && memcmp(out, c, sizeof(c)) == 0,
		"enc -2 is ten bytes");
}

static void test_round_trip(void) {
	uint8_t buf[256], subbuf[64];
	zpb_wr_t w, sub;
	zpb_rd_t r, rs;
	zpb_field_t f;
	int seen = 0;

	zpb_wr_init(&sub, subbuf, sizeof(subbuf));
	zpb_put_str(&sub, 3, "LongFast");
	zpb_put_fixed32(&sub, 4, 0xdeadbeef);

	zpb_wr_init(&w, buf, sizeof(buf));
	zpb_put_fixed32(&w, 1, 0xa1b2c3d4);
	zpb_put_varint(&w, 2, 0xffffffffu);
	zpb_put_int32(&w, 12, -95);
	zpb_put_sint32(&w, 9, -1234);
	zpb_put_float(&w, 8, 6.25f);
	zpb_put_varint(&w, 10, 1);
	zpb_put_msg(&w, 4, &sub);
	zpb_put_varint(&w, 300000, 7);		// a big field number
	zpb_put_varint(&w, 5, 0x123456789abcull);
	CHECK(!w.err, "round trip fits");

	zpb_rd_init(&r, buf, zpb_wr_len(&w));
	while (zpb_next(&r, &f)) {
		switch (f.num) {
		case 1: CHECK(f.wt == ZPB_I32 && zpb_u32(&f) == 0xa1b2c3d4, "fixed32"); seen |= 1; break;
		case 2: CHECK(zpb_u32(&f) == 0xffffffffu, "uint32 max"); seen |= 2; break;
		case 12: CHECK(zpb_i32(&f) == -95, "int32 -95"); seen |= 4; break;
		case 9: CHECK(zpb_sint32(&f) == -1234, "sint32 -1234"); seen |= 8; break;
		case 8: CHECK(zpb_float(&f) == 6.25f, "float"); seen |= 16; break;
		case 10: CHECK(zpb_bool(&f), "bool"); seen |= 32; break;
		case 4:
			zpb_sub(&rs, &f);
			CHECK(zpb_next(&rs, &f) && f.num == 3 && f.len == 8 &&
				memcmp(f.ptr, "LongFast", 8) == 0, "nested string");
			CHECK(zpb_next(&rs, &f) && f.num == 4 && zpb_u32(&f) == 0xdeadbeef,
				"nested fixed32");
			CHECK(!zpb_next(&rs, &f) && !rs.err, "nested end");
			seen |= 64;
			break;
		case 300000: CHECK(f.v == 7, "big field number"); seen |= 128; break;
		case 5: CHECK(f.v == 0x123456789abcull, "64-bit varint"); seen |= 256; break;
		default: CHECK(0, "unexpected field");
		}
	}
	CHECK(!r.err, "round trip: no error");
	CHECK(seen == 511, "round trip: every field seen");
}

static void test_unknown_fields_skipped(void) {
	// A newer sender's fields we do not know, of every wire type, then
	// one we do: we must land on it.
	uint8_t buf[64];
	zpb_wr_t w;
	zpb_rd_t r;
	zpb_field_t f;
	uint32_t want = 0;

	zpb_wr_init(&w, buf, sizeof(buf));
	zpb_put_varint(&w, 90, 12345678);
	zpb_put_bytes(&w, 91, "abcdefghij", 10);
	zpb_put_fixed32(&w, 92, 1);
	// fixed64 by hand: tag (93 << 3 | 1), eight bytes
	buf[zpb_wr_len(&w)] = (uint8_t)(0x80 | ((93 << 3 | 1) & 0x7f));
	buf[zpb_wr_len(&w) + 1] = (uint8_t)((93 << 3 | 1) >> 7);
	w.p += 2;
	memset(w.p, 0x55, 8);
	w.p += 8;
	zpb_put_varint(&w, 1, 42);

	zpb_rd_init(&r, buf, zpb_wr_len(&w));
	while (zpb_next(&r, &f))
		if (f.num == 1) want = zpb_u32(&f);
	CHECK(!r.err && want == 42, "unknown fields of every wire type skipped");
}

// The malformed input is copied to the very end of `arena`, followed
// by canary bytes that would parse as a valid field if reached.
static uint8_t arena[256];

static bool parse_all_at_end(const uint8_t *msg, uint32_t len) {
	zpb_rd_t r;
	zpb_field_t f;
	uint8_t *at = arena + sizeof(arena) - 16 - len;
	int guard = 0;
	memset(arena, 0, sizeof(arena));
	memcpy(at, msg, len);
	// canary after the message: "field 15 = 99" repeated
	for (int i = 0; i < 16; i += 2) { at[len + i] = 0x78; at[len + i + 1] = 99; }
	zpb_rd_init(&r, at, len);
	while (zpb_next(&r, &f) && guard++ < 100) {
		if (f.num == 15 && f.v == 99) return false;		// read the canary
		if (f.wt == ZPB_LEN &&
			(f.ptr < at || f.ptr + f.len > at + len)) return false;
	}
	return r.err;
}

static void test_malformed(void) {
	static const uint8_t trunc_varint[] = { 0x08, 0x96 };
	static const uint8_t trunc_tag[] = { 0x80 };
	static const uint8_t len_past_end[] = { 0x12, 0x05, 'a', 'b' };
	static const uint8_t len_huge[] = { 0x12, 0xff, 0xff, 0xff, 0xff, 0x0f };
	static const uint8_t trunc_i32[] = { 0x0d, 1, 2, 3 };
	static const uint8_t trunc_i64[] = { 0x09, 1, 2, 3, 4, 5, 6, 7 };
	static const uint8_t group[] = { 0x0b, 0x0c };
	static const uint8_t wt6[] = { 0x0e, 0x00 };
	static const uint8_t field0[] = { 0x00, 0x01 };
	static const uint8_t varint11[] = { 0x08, 0xff, 0xff, 0xff, 0xff, 0xff,
		0xff, 0xff, 0xff, 0xff, 0xff, 0x01 };
	static const uint8_t tag_too_big[] = { 0xf8, 0xff, 0xff, 0xff, 0xff, 0x01, 0x00 };

	CHECK(parse_all_at_end(trunc_varint, sizeof(trunc_varint)), "truncated varint");
	CHECK(parse_all_at_end(trunc_tag, sizeof(trunc_tag)), "truncated tag");
	CHECK(parse_all_at_end(len_past_end, sizeof(len_past_end)), "LEN past end");
	CHECK(parse_all_at_end(len_huge, sizeof(len_huge)), "LEN of 4 GB");
	CHECK(parse_all_at_end(trunc_i32, sizeof(trunc_i32)), "truncated I32");
	CHECK(parse_all_at_end(trunc_i64, sizeof(trunc_i64)), "truncated I64");
	CHECK(parse_all_at_end(group, sizeof(group)), "group");
	CHECK(parse_all_at_end(wt6, sizeof(wt6)), "wire type 6");
	CHECK(parse_all_at_end(field0, sizeof(field0)), "field number 0");
	CHECK(parse_all_at_end(varint11, sizeof(varint11)), "eleven-byte varint");
	CHECK(parse_all_at_end(tag_too_big, sizeof(tag_too_big)), "tag over 32 bits");

	// After an error the reader stays stopped.
	zpb_rd_t r;
	zpb_field_t f;
	zpb_rd_init(&r, len_past_end, sizeof(len_past_end));
	CHECK(!zpb_next(&r, &f) && r.err, "error reported");
	CHECK(!zpb_next(&r, &f) && r.err, "and sticks");

	// Empty is a valid, empty message.
	zpb_rd_init(&r, arena, 0);
	CHECK(!zpb_next(&r, &f) && !r.err, "empty message");
}

static void test_random_garbage(void) {
	// Nothing may crash or escape the buffer, whatever it is fed. A
	// small LCG, so a failure reproduces.
	uint32_t seed = 12345;
	int bad = 0;
	for (int t = 0; t < 20000; t++) {
		uint8_t msg[48];
		uint32_t len;
		seed = seed * 1103515245u + 12345u;
		len = (seed >> 16) % sizeof(msg);
		for (uint32_t i = 0; i < len; i++) {
			seed = seed * 1103515245u + 12345u;
			msg[i] = (uint8_t)(seed >> 16);
		}
		{
			zpb_rd_t r;
			zpb_field_t f;
			uint8_t *at = arena + sizeof(arena) - 16 - len;
			memcpy(at, msg, len);
			zpb_rd_init(&r, at, len);
			while (zpb_next(&r, &f))
				if (f.wt == ZPB_LEN && (f.ptr < at || f.ptr + f.len > at + len))
					bad++;
			if (r.p != at + len) bad++;
		}
	}
	CHECK(bad == 0, "20000 random messages stay inside their buffer");
}

static void test_writer_overflow(void) {
	uint8_t buf[8];
	zpb_wr_t w;
	zpb_wr_init(&w, buf, 5);
	zpb_put_str(&w, 1, "abc");			// 5 bytes: fits exactly
	CHECK(!w.err && zpb_wr_len(&w) == 5, "exact fit");
	zpb_put_varint(&w, 1, 1);
	CHECK(w.err && zpb_wr_len(&w) == 5, "overflow stops and flags");
	zpb_wr_init(&w, buf, 4);
	zpb_put_str(&w, 1, "abc");
	CHECK(w.err && zpb_wr_len(&w) <= 4, "string that does not fit");

	zpb_wr_t sub, outer;
	uint8_t sb[2], ob[16];
	zpb_wr_init(&sub, sb, sizeof(sb));
	zpb_put_str(&sub, 1, "toolong");
	zpb_wr_init(&outer, ob, sizeof(ob));
	zpb_put_msg(&outer, 2, &sub);
	CHECK(outer.err, "a failed nested message fails its parent");
}

int main(void) {
	test_spec_vectors();
	test_round_trip();
	test_unknown_fields_skipped();
	test_malformed();
	test_random_garbage();
	test_writer_overflow();
	printf("zpb: %d checks, %d failed\n", run, failed);
	return failed ? 1 : 0;
}
