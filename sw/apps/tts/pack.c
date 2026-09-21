/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * Reading a speech pack. See pack.h and the speech-data pipeline docs, published with it.
 *
 * The lookup is a binary search over the block index, which lives on
 * the card: each probe seeks and reads one 16-byte record (a block's
 * data offset and the first twelve characters of its first word). The
 * block that must contain the word is then read whole -- a few hundred
 * bytes -- and scanned. The entries in a block are front-coded against
 * each other but the first shares nothing, so a block is readable on
 * its own without the ones before it.
 */

#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "pack.h"
#include "lts.h"
#include "phon.h"

#ifdef PACK_HOST
// Host tests read with stdio; the target uses the app filesystem API.
#include <stdlib.h>
static FILE *fh;
static int io_open(const char *p) { fh = fopen(p, "rb"); return fh ? 1 : -1; }
static void io_close(void) { if (fh) fclose(fh); fh = 0; }
static bool io_read(uint32_t off, void *buf, uint32_t len) {
	if (!fh || fseek(fh, (long)off, SEEK_SET) != 0) return false;
	return fread(buf, 1, len, fh) == len;
}
#else
#include "../../common/zfsapp.h"
static int fh = -1;
static int io_open(const char *p) { fh = fs_open_read(p); return fh; }
static void io_close(void) { if (fh >= 0) fs_close_handle(fh); fh = -1; }
static bool io_read(uint32_t off, void *buf, uint32_t len) {
	if (fh < 0) return false;
	if (!fs_seek(fh, off)) return false;
	int got = fs_read_chunk(fh, buf, (int)len);
	return got == (int)len;
}
#endif

// The phoneme names this build knows, in no particular order -- the
// pack's own PHONES section decides the ids, and open() maps them onto
// these. A pack naming something missing here is refused: better no
// lexicon than one whose bytes mean something else.
static const char *const known[] = {
	"IY","IH","EH","AE","AA","AO","UH","UW","AH","ER","AX","IX",
	"EY","OW","AY","AW","OY","W","Y","R","L","M","N","NG",
	"S","Z","SH","ZH","F","V","TH","DH","HH","P","B","T","D",
	"K","G","CH","JH",
};
#define NKNOWN (int)(sizeof(known) / sizeof(known[0]))

#define PACK_MAGIC	0x4b50535au		// "ZSPK", little-endian
#define IDX_REC		16
#define IDX_KEY		12
#define MAX_BLOCK	2048

static bool ready;
static uint32_t idx_off, idx_len, dat_off, dat_len;
static uint32_t nblocks, nwords;
static const char *names[64];		// pack id -> one of known[]
static int nnames;

static uint32_t rd32(const uint8_t *p) {
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
		((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static bool section(const uint8_t *table, int n, const char *want,
	uint32_t *off, uint32_t *len) {
	for (int i = 0; i < n; i++) {
		const uint8_t *rec = table + i * 16;
		if (strncmp((const char *)rec, want, 8) == 0) {
			*off = rd32(rec + 8);
			*len = rd32(rec + 12);
			return true;
		}
	}
	return false;
}

// -- the trained letter-to-sound model --
//
// Layout, from tools/speech/lib/lts_train.py:
//   u16 node count, u16 class count, u16 26
//   u16 root[26]          index of each letter's root, 0xffff if none
//   u32 class table length
//   class table: per class, u8 phoneme count then that many ids
//   nodes: 6 bytes each --
//     internal: u8 offset (0-8), u8 letter (0='#', 1-26 = a-z),
//               u16 yes, u16 no
//     leaf:     0xff, 0, u16 class id, u16 unused
static uint8_t lts_blob[PACK_LTS_MAX];
static uint32_t lts_len;
static bool lts_ok;
static uint16_t lts_nodes_n, lts_classes_n;
static uint16_t lts_root[26];
static const uint8_t *lts_classes, *lts_nodes;

static uint16_t rd16(const uint8_t *p) {
	return (uint16_t)((uint32_t)p[0] | ((uint32_t)p[1] << 8));
}

static void lts_load(uint32_t off, uint32_t len) {

	lts_ok = false;

	if (len == 0) return;
	if (len > PACK_LTS_MAX) {
		printf("tts: pack's letter rules are %luKB, over the %dKB this build holds\n",
			(unsigned long)(len / 1024), PACK_LTS_MAX / 1024);
		return;
	}
	if (!io_read(off, lts_blob, len)) return;
	lts_len = len;

	if (len < 6 + 52 + 4) return;
	lts_nodes_n = rd16(lts_blob);
	lts_classes_n = rd16(lts_blob + 2);
	if (rd16(lts_blob + 4) != 26) return;

	for (int i = 0; i < 26; i++)
		lts_root[i] = rd16(lts_blob + 6 + i * 2);

	uint32_t tlen = (uint32_t)rd16(lts_blob + 58) | ((uint32_t)rd16(lts_blob + 60) << 16);
	uint32_t base = 6 + 52 + 4;
	if (base + tlen + (uint32_t)lts_nodes_n * 6 > len) return;

	lts_classes = lts_blob + base;
	lts_nodes = lts_classes + tlen;
	lts_ok = lts_nodes_n > 0;

}

bool pack_lts_ready(void) {
	return lts_ok;
}

// The phonemes of class `id`, appended to `out`.
static uint32_t lts_emit(uint16_t id, char *out, uint32_t o, uint32_t size) {

	const uint8_t *p = lts_classes;
	for (uint16_t i = 0; i < id; i++) {
		if (p >= lts_nodes) return o;
		p += 1 + *p;
	}
	if (p >= lts_nodes) return o;

	uint8_t n = *p++;
	for (uint8_t i = 0; i < n; i++) {
		uint8_t phid = p[i];
		if (phid >= (uint8_t)nnames) break;
		const char *nm = names[phid];
		uint32_t l = (uint32_t)strlen(nm);
		if (o + l + 2 >= size) break;
		if (o) out[o++] = ' ';
		memcpy(out + o, nm, l);
		o += l;
		out[o] = 0;
	}
	return o;

}

bool pack_lts(const char *word, char *out, uint32_t size) {

	if (!lts_ok || !word || !out || size < 8) return false;

	// The window the trees ask about: four letters either side.
	char padded[8 + 64];
	uint32_t n = 0;
	memset(padded, '#', 4);
	for (const char *s = word; *s && n < 64; s++) {
		char c = *s;
		if (c >= 'A' && c <= 'Z') c += 32;
		if (c < 'a' || c > 'z') continue;
		padded[4 + n++] = c;
	}
	if (n == 0) return false;
	memset(padded + 4 + n, '#', 4);

	uint32_t o = 0;
	out[0] = 0;

	for (uint32_t i = 0; i < n; i++) {

		char c = padded[4 + i];
		uint16_t idx = lts_root[c - 'a'];
		if (idx == 0xffff) continue;

		for (uint32_t guard = 0; guard < 64; guard++) {
			if (idx >= lts_nodes_n) return false;
			const uint8_t *node = lts_nodes + (uint32_t)idx * 6;
			if (node[0] == 0xff) {
				o = lts_emit(rd16(node + 2), out, o, size);
				break;
			}
			char want = node[1] ? (char)('a' + node[1] - 1) : '#';
			idx = rd16(node + (padded[i + node[0]] == want ? 2 : 4));
		}

	}

	if (!o) return false;

	// The trees learn the phonemes; the stress digits come from the
	// same pass the hand-written rules use (lts.h).
	char letters[65];
	memcpy(letters, padded + 4, n);
	letters[n] = 0;
	for (uint32_t i = 0; i < n; i++)
		if (letters[i] >= 'a' && letters[i] <= 'z') letters[i] -= 32;
	lts_add_stress(out, size, letters, (int)n);

	return out[0] != 0;

}

// -- prosody --
//
// Timing and pitch fitted to a real speaker (tools/speech/lib/
// prosody.py). Version 1:
//   u16 version, u16 field count, i16 fields[] in phon.h's PHON_PRO_*
//   order, u16 phone count, then per phone 2 name bytes and u16 ms.
// Handed to phon.c, which clamps every value to a sane range: a pack is
// data from elsewhere, and a bad number in it must not be able to make
// the voice unusable. A field this build does not know is ignored; one
// the pack does not have keeps its default.
static int prosody_fields;

static void prosody_load(uint32_t off, uint32_t len) {

	uint8_t buf[512];
	prosody_fields = 0;

	phon_prosody_reset();
	if (len < 6 || len > sizeof(buf)) return;
	if (!io_read(off, buf, len)) return;

	if (rd16(buf) != 1) {
		printf("tts: pack's prosody is version %u, keeping the defaults\n",
			(unsigned)rd16(buf));
		return;
	}

	uint32_t nf = rd16(buf + 2);
	uint32_t p = 4;
	for (uint32_t i = 0; i < nf; i++, p += 2) {
		if (p + 2 > len) return;
		if ((int)i < PHON_PRO_N) {
			phon_prosody_set((int)i, (int16_t)rd16(buf + p));
			prosody_fields++;
		}
	}

	if (p + 2 > len) return;
	uint32_t np = rd16(buf + p);
	p += 2;
	for (uint32_t i = 0; i < np; i++, p += 4) {
		if (p + 4 > len) return;
		char nm[3] = { (char)buf[p], (char)buf[p + 1], 0 };
		phon_prosody_dur(nm, rd16(buf + p + 2));
	}

}

// -- formants --
//
// Vowel and sonorant formants measured from a real speaker
// (tools/speech/lib/acoustics.py). Version 1: u16 version, u16 count,
// then per phoneme 2 name bytes and six u16 (F1-F3, end F1-F3; 0 keeps
// the table's). phon.c checks each value against its own table.
static int formant_phones;

static void formants_load(uint32_t off, uint32_t len) {
	uint8_t buf[1024];
	formant_phones = 0;
	if (len < 4 || len > sizeof(buf)) return;
	if (!io_read(off, buf, len)) return;
	if (rd16(buf) != 1) return;
	uint32_t n = rd16(buf + 2), p = 4;
	for (uint32_t i = 0; i < n && p + 14 <= len; i++, p += 14) {
		char nm[3] = { (char)buf[p], (char)buf[p + 1], 0 };
		uint16_t f[6];
		for (int k = 0; k < 6; k++) f[k] = rd16(buf + p + 2 + 2 * k);
		if (phon_prosody_formants(nm, f)) formant_phones++;
	}
}

int pack_formant_phones(void) {
	return formant_phones;
}

int pack_prosody_fields(void) {
	return prosody_fields;
}

bool pack_open(const char *path) {

	uint8_t hdr[16];
	uint8_t table[16 * 16];

	pack_close();

	if (io_open(path) < 0) return false;
	if (!io_read(0, hdr, sizeof(hdr))) { pack_close(); return false; }

	if (rd32(hdr) != PACK_MAGIC) {
		printf("tts: %s is not a speech pack\n", path);
		pack_close();
		return false;
	}

	uint32_t version = (uint32_t)hdr[4] | ((uint32_t)hdr[5] << 8);
	uint32_t nsect = (uint32_t)hdr[6] | ((uint32_t)hdr[7] << 8);
	if (version != 1 || nsect == 0 || nsect > 16) {
		printf("tts: %s is version %lu with %lu sections -- not usable\n",
			path, (unsigned long)version, (unsigned long)nsect);
		pack_close();
		return false;
	}

	if (!io_read(16, table, nsect * 16)) { pack_close(); return false; }

	uint32_t ph_off, ph_len;
	if (!section(table, (int)nsect, "PHONES", &ph_off, &ph_len) ||
	    !section(table, (int)nsect, "LEXIDX", &idx_off, &idx_len) ||
	    !section(table, (int)nsect, "LEXDAT", &dat_off, &dat_len)) {
		printf("tts: %s has no lexicon\n", path);
		pack_close();
		return false;
	}

	// The pack's phoneme names, mapped onto ours.
	static char phbuf[512];
	if (ph_len >= sizeof(phbuf) || !io_read(ph_off, phbuf, ph_len)) {
		pack_close();
		return false;
	}
	phbuf[ph_len] = 0;
	nnames = 0;
	for (char *p = phbuf; *p; ) {
		while (*p == ' ' || *p == '\n') p++;
		if (!*p) break;
		char *s = p;
		while (*p && *p != ' ' && *p != '\n') p++;
		char save = *p;
		*p = 0;
		int k;
		for (k = 0; k < NKNOWN; k++)
			if (strcmp(known[k], s) == 0) break;
		if (k == NKNOWN || nnames >= 64) {
			printf("tts: %s uses phoneme '%s', which this build does not have\n",
				path, s);
			pack_close();
			return false;
		}
		names[nnames++] = known[k];
		*p = save;
	}

	// Prosody, likewise optional: without it phon.c keeps its rules'
	// own numbers.
	uint32_t pr_off, pr_len;
	if (section(table, (int)nsect, "PROSODY", &pr_off, &pr_len))
		prosody_load(pr_off, pr_len);
	else
		phon_prosody_reset();

	// Formants likewise; phon_prosody_reset() above cleared any from
	// an earlier pack.
	uint32_t fm_off, fm_len;
	formant_phones = 0;
	if (section(table, (int)nsect, "FORMANTS", &fm_off, &fm_len))
		formants_load(fm_off, fm_len);

	// The trained letter rules are optional inside an optional pack.
	uint32_t lt_off, lt_len;
	if (section(table, (int)nsect, "LTS", &lt_off, &lt_len))
		lts_load(lt_off, lt_len);

	nblocks = idx_len / IDX_REC;
	nwords = 0;
	ready = nblocks > 0;
	if (!ready) pack_close();
	return ready;

}

void pack_close(void) {
	io_close();
	// Closing the pack takes its voice with it.
	if (prosody_fields) phon_prosody_reset();
	prosody_fields = 0;
	ready = false;
	lts_ok = false;
	lts_len = 0;
	nblocks = 0;
}

bool pack_ready(void) {
	return ready;
}

uint32_t pack_words(void) {
	return nwords;
}

// Reads index record `i`: its block's data offset, and up to twelve
// characters of the block's first word.
static bool index_at(uint32_t i, uint32_t *off, char *key) {
	uint8_t rec[IDX_REC];
	if (!io_read(idx_off + i * IDX_REC, rec, IDX_REC)) return false;
	*off = rd32(rec);
	memcpy(key, rec + 4, IDX_KEY);
	key[IDX_KEY] = 0;
	return true;
}

bool pack_lookup(const char *word, char *out, uint32_t size) {

	if (!ready || !word || !*word || !size) return false;

	// Binary search for the last block whose first word is <= this one.
	uint32_t lo = 0, hi = nblocks - 1, found = 0;
	char key[IDX_KEY + 1];
	uint32_t off;

	while (lo <= hi) {
		uint32_t mid = (lo + hi) / 2;
		if (!index_at(mid, &off, key)) return false;
		if (strncmp(key, word, IDX_KEY) <= 0) {
			found = mid;
			lo = mid + 1;
		} else {
			if (mid == 0) break;
			hi = mid - 1;
		}
	}

	uint32_t start, end;
	if (!index_at(found, &start, key)) return false;
	if (found + 1 < nblocks) {
		char next[IDX_KEY + 1];
		if (!index_at(found + 1, &end, next)) return false;
	} else {
		end = dat_len;
	}

	uint32_t len = end - start;
	if (len > MAX_BLOCK) len = MAX_BLOCK;

	static uint8_t block[MAX_BLOCK];
	if (!io_read(dat_off + start, block, len)) return false;

	// Walk the block's entries, rebuilding each word from the previous
	// one's prefix.
	char cur[64];
	uint32_t p = 0;
	cur[0] = 0;

	while (p + 2 <= len) {

		uint32_t shared = block[p], rest = block[p + 1];
		p += 2;
		if (p + rest + 1 > len) break;
		if (shared > sizeof(cur) - 1) break;
		if (shared + rest > sizeof(cur) - 1) break;

		memcpy(cur + shared, block + p, rest);
		cur[shared + rest] = 0;
		p += rest;

		uint32_t nph = block[p++];
		if (p + nph > len) break;

		int cmp = strcmp(cur, word);
		if (cmp == 0) {
			uint32_t o = 0;
			out[0] = 0;
			for (uint32_t i = 0; i < nph; i++) {
				uint8_t b = block[p + i];
				uint32_t id = b & 0x3f, stress = b >> 6;
				if (id >= (uint32_t)nnames) return false;
				const char *nm = names[id];
				uint32_t l = (uint32_t)strlen(nm);
				bool vowel = strchr("AEIOU", nm[0]) != 0;
				if (o + l + 3 >= size) break;
				if (o) out[o++] = ' ';
				memcpy(out + o, nm, l);
				o += l;
				if (vowel) out[o++] = (char)('0' + stress);
				out[o] = 0;
			}
			return out[0] != 0;
		}
		if (cmp > 0) return false;		// past it: the pack has no such word

		p += nph;

	}

	return false;

}
