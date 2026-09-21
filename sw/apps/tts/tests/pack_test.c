/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * Host test for sw/apps/tts/pack.c, the speech-pack reader:
 *
 *     cd sw/apps/tts && make test
 *
 * Builds small packs by hand (the same bytes tools/speech writes) and
 * reads them back with the REAL reader, compiled with PACK_HOST so its
 * seeks and reads go through stdio instead of the card.
 *
 * What is worth testing is the refusals as much as the lookups: a
 * truncated file, a version from the future, or a phoneme this build
 * does not have must all leave speech working from the built-in rules
 * rather than half-reading a pack.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "../pack.h"
#include "../phon.h"

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

// -- building a pack, the way tools/speech/lib/pack.py does --

static uint8_t buf[1 << 16];
static uint32_t len;

static void put(const void *p, uint32_t n) { memcpy(buf + len, p, n); len += n; }
static void put32(uint32_t v) { for (int i = 0; i < 4; i++) buf[len++] = (v >> (8 * i)) & 0xff; }
static void put16(uint16_t v) { buf[len++] = v & 0xff; buf[len++] = v >> 8; }

static const char *PHONES =
	"IY IH EH AE AA AO UH UW AH ER AX IX EY OW AY AW OY "
	"W Y R L M N NG S Z SH ZH F V TH DH HH P B T D K G CH JH";

static int phone_id(const char *name) {
	int id = 0;
	const char *p = PHONES;
	while (*p) {
		const char *s = p;
		while (*p && *p != ' ') p++;
		if ((int)strlen(name) == p - s && strncmp(name, s, p - s) == 0) return id;
		id++;
		while (*p == ' ') p++;
	}
	return -1;
}

// words must be sorted; every word gets the same two phonemes plus one
// distinguishing vowel, which is enough to prove the right entry came
// back.
static void build(const char *path, const char *const *words, int n, int block,
	int version, const char *phones, int truncate)
{
	uint8_t idx[4096], dat[16384];
	uint32_t ilen = 0, dlen = 0;
	char prev[64] = "";

	for (int i = 0; i < n; i++) {
		if (i % block == 0) {
			// index record: offset, then 12 bytes of the first word
			idx[ilen++] = dlen & 0xff; idx[ilen++] = (dlen >> 8) & 0xff;
			idx[ilen++] = (dlen >> 16) & 0xff; idx[ilen++] = (dlen >> 24) & 0xff;
			memset(idx + ilen, 0, 12);
			memcpy(idx + ilen, words[i], strlen(words[i]) < 12 ? strlen(words[i]) : 12);
			ilen += 12;
			prev[0] = 0;
		}
		uint32_t shared = 0;
		while (prev[shared] && words[i][shared] && prev[shared] == words[i][shared]) shared++;
		uint32_t rest = (uint32_t)strlen(words[i]) - shared;
		dat[dlen++] = (uint8_t)shared;
		dat[dlen++] = (uint8_t)rest;
		memcpy(dat + dlen, words[i] + shared, rest);
		dlen += rest;
		dat[dlen++] = 3;
		dat[dlen++] = (uint8_t)phone_id("K");
		dat[dlen++] = (uint8_t)((1 << 6) | phone_id(i % 2 ? "IY" : "AA"));
		dat[dlen++] = (uint8_t)phone_id("T");
		strcpy(prev, words[i]);
	}

	len = 0;
	uint32_t nsect = 4;
	uint32_t off = 16 + 16 * nsect;
	uint32_t moff = off, mlen = 6;
	uint32_t poff = moff + mlen, plen = (uint32_t)strlen(phones);
	uint32_t ioff = poff + plen;
	uint32_t doff = ioff + ilen;

	put("ZSPK", 4);
	put16((uint16_t)version);
	put16((uint16_t)nsect);
	put32(doff + dlen);
	put32(0);				// crc: the reader does not check it (see pack.c)

	const char *names[4] = { "MANIFEST", "PHONES", "LEXIDX", "LEXDAT" };
	uint32_t offs[4] = { moff, poff, ioff, doff };
	uint32_t lens[4] = { mlen, plen, ilen, dlen };
	for (int i = 0; i < 4; i++) {
		char nm[8];
		memset(nm, 0, 8);
		memcpy(nm, names[i], strlen(names[i]));
		put(nm, 8);
		put32(offs[i]);
		put32(lens[i]);
	}
	put("hello\n", 6);
	put(phones, plen);
	put(idx, ilen);
	put(dat, dlen);

	FILE *f = fopen(path, "wb");
	fwrite(buf, 1, truncate ? (uint32_t)truncate : len, f);
	fclose(f);
}

static const char *const WORDS[] = {
	"apple", "apples", "banana", "bandit", "bank", "cat", "cats",
	"dog", "dogs", "elephant", "fig", "grape", "hat", "ice",
	"jam", "kite", "lemon", "mango", "nut", "orange", "pear",
	"quince", "rice", "sugar", "tomato", "ugli", "vanilla",
	"walnut", "xigua", "yam", "zucchini",
};
#define NWORDS (int)(sizeof(WORDS) / sizeof(WORDS[0]))

static void test_lookups(void) {
	char out[128];
	build("/tmp/pk_ok.spk", WORDS, NWORDS, 4, 1, PHONES, 0);
	CHECK(pack_open("/tmp/pk_ok.spk"));
	// every word, including the first and last of each block
	for (int i = 0; i < NWORDS; i++) {
		CHECK(pack_lookup(WORDS[i], out, sizeof(out)));
		CHECK(strcmp(out, i % 2 ? "K IY1 T" : "K AA1 T") == 0);
	}
	// absent words, at the start, the middle and past the end
	CHECK(!pack_lookup("aardvark", out, sizeof(out)));
	CHECK(!pack_lookup("mangosteen", out, sizeof(out)));
	CHECK(!pack_lookup("zzzz", out, sizeof(out)));
	CHECK(!pack_lookup("", out, sizeof(out)));
	// a word sharing a long prefix with a real one -- the front coding
	// is where an off-by-one would show
	CHECK(!pack_lookup("apple-pie", out, sizeof(out)));
	CHECK(pack_lookup("apples", out, sizeof(out)));
	pack_close();
	CHECK(!pack_ready());
	CHECK(!pack_lookup("cat", out, sizeof(out)));
}

static void test_refusals(void) {
	char out[128];

	CHECK(!pack_open("/tmp/pk_nothere.spk"));

	build("/tmp/pk_v9.spk", WORDS, NWORDS, 4, 9, PHONES, 0);
	CHECK(!pack_open("/tmp/pk_v9.spk"));			// future version

	build("/tmp/pk_ph.spk", WORDS, NWORDS, 4, 1,
		"IY IH EH AE AA AO UH UW AH ER AX IX EY OW AY AW OY "
		"W Y R L M N NG S Z SH ZH F V TH DH HH P B T D K G CH JH XQ", 0);
	CHECK(!pack_open("/tmp/pk_ph.spk"));			// unknown phoneme

	build("/tmp/pk_cut.spk", WORDS, NWORDS, 4, 1, PHONES, 40);
	if (pack_open("/tmp/pk_cut.spk")) {			// truncated: must not crash
		CHECK(!pack_lookup("cat", out, sizeof(out)));
		pack_close();
	}

	FILE *f = fopen("/tmp/pk_junk.spk", "wb");
	fwrite("not a pack at all, really not", 1, 29, f);
	fclose(f);
	CHECK(!pack_open("/tmp/pk_junk.spk"));
}

// One block per entry, and one block holding everything: both ends of
// the block-size choice have to work.
static void test_block_sizes(void) {
	char out[128];
	for (int b = 1; b <= 64; b *= 2) {
		build("/tmp/pk_b.spk", WORDS, NWORDS, b, 1, PHONES, 0);
		CHECK(pack_open("/tmp/pk_b.spk"));
		CHECK(pack_lookup("apple", out, sizeof(out)));
		CHECK(pack_lookup("nut", out, sizeof(out)));
		CHECK(pack_lookup("zucchini", out, sizeof(out)));
		CHECK(!pack_lookup("zzz", out, sizeof(out)));
		pack_close();
	}
}

// A pack's prosody is data from elsewhere: every value is clamped, and
// closing the pack puts the defaults back.
static void test_prosody_limits(void) {
	phon_prosody_reset();
	CHECK(phon_prosody_get(PHON_PRO_UNSTRESSED) == 55);
	CHECK(phon_prosody_get(PHON_PRO_PAUSE_STOP) == 300);

	phon_prosody_set(PHON_PRO_UNSTRESSED, 5);		// far too short
	CHECK(phon_prosody_get(PHON_PRO_UNSTRESSED) == 30);
	phon_prosody_set(PHON_PRO_PAUSE_STOP, 30000);		// thirty seconds
	CHECK(phon_prosody_get(PHON_PRO_PAUSE_STOP) == 900);
	phon_prosody_set(PHON_PRO_COMMA_RISE, -50);
	CHECK(phon_prosody_get(PHON_PRO_COMMA_RISE) == -10);
	phon_prosody_set(99, 1);				// no such field: ignored
	phon_prosody_set(-1, 1);

	CHECK(phon_prosody_dur("AA", 200));
	CHECK(!phon_prosody_dur("AA", 5000));			// not believable
	CHECK(!phon_prosody_dur("QQ", 100));			// no such phoneme

	phon_prosody_reset();
	CHECK(phon_prosody_get(PHON_PRO_UNSTRESSED) == 55);
}

int main(void) {
	test_lookups();
	test_refusals();
	test_block_sizes();
	test_prosody_limits();
	if (fails) { printf("pack_test: %d FAILED\n", fails); return 1; }
	printf("pack_test: all passed\n");
	return 0;
}
