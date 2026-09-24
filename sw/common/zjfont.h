#ifndef ZJFONT_H
#define ZJFONT_H

/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * The Japanese font, held once for every app. See docs/text_encoding.md,
 * "Japanese".
 *
 * -- why a service --
 *
 * The 12x12 font (sw/data/font/jp12.zfn, tools/gen_jfont.py) is 175KB:
 * far more than an app's 16KB heap, and more than it makes sense to load
 * into every process that might draw a kanji. sw/apps/jfont loads it once
 * and stays resident. Every byte of memory has one physical address any
 * process may READ (docs/mpu.md: an app writes only its own memory, reads
 * anything), so apps read the glyphs straight out of jfont's memory.
 *
 * -- how an app finds it, without a message --
 *
 * A lookup happens inside drawing code, in whatever app is drawing, and
 * must not touch that app's mailbox: z_msg_wait() discards what it is
 * not waiting for. So there is no request at all. z_proc_list() gives
 * every process's name and physical block; the client finds "jfont" and
 * scans its block once for this descriptor, which jfont writes directly
 * in front of the font:
 *
 *     magic0, magic1   Z_JFONT_MAGIC0/1 -- written LAST, after the font
 *                      is in place, so a half-loaded font is never seen
 *     check            magic0 ^ magic1 ^ (the descriptor's own physical
 *                      address) -- a copy of the magic words anywhere
 *                      else, or the descriptor of a process that has
 *                      since died and been overwritten, does not check
 *     len              bytes of font that follow
 *
 * The client keeps the address and re-checks the three words before
 * every use -- two loads -- so jfont exiting turns Japanese back into
 * boxes rather than into whatever reuses its memory.
 */

#include <stdint.h>
#include <stddef.h>

#include "zproc.h"		// z_proc_info_t

#define Z_JFONT_PROC      "jfont"
// The name it has in z_proc_list(): the pid registry's, which numbers
// what it is given -- "jfont" registers as "jfont0". jfont refuses to
// run as anything else, so this is the only name a client looks for.
// (Looking for "jfont" itself found nothing, and every kanji was a box.)
#define Z_JFONT_NAME      "jfont0"
#define Z_JFONT_PATH      "/font/jp12.zfn"

#define Z_JFONT_MAGIC0    0x4A464E54u	// "JFNT"
#define Z_JFONT_MAGIC1    0x5A464E31u	// "ZFN1"

typedef struct {
	uint32_t	magic0;
	uint32_t	magic1;
	uint32_t	check;
	uint32_t	len;
	// the font file's bytes follow, 4-byte aligned
} z_jfont_desc_t;

static inline uint32_t z_jfont_check(uint32_t phys) {
	return Z_JFONT_MAGIC0 ^ Z_JFONT_MAGIC1 ^ phys;
}

// True if `d`, at physical address `phys`, is a live descriptor.
static inline int z_jfont_desc_ok(const volatile z_jfont_desc_t *d, uint32_t phys) {
	return d->magic0 == Z_JFONT_MAGIC0 && d->magic1 == Z_JFONT_MAGIC1 &&
		d->check == z_jfont_check(phys);
}

// Finds the descriptor in a process list: the process named
// Z_JFONT_NAME, its block scanned a word at a time for a live
// descriptor. Returns it and its physical address, or NULL. Pure --
// the list comes from z_proc_list() -- so the host tests run it on a
// fake list over a real buffer.
static inline const volatile z_jfont_desc_t *z_jfont_find(
	const z_proc_info_t *procs, uint32_t n, uint32_t *phys) {

	for (uint32_t i = 0; i < n; i++) {
		const char *a = procs[i].name, *b = Z_JFONT_NAME;
		while (*a && *a == *b) { a++; b++; }
		if (*a || *b) continue;
		uint32_t at = (procs[i].base + 3) & ~3u;
		uint32_t end = procs[i].base + procs[i].size;
		for (; at + sizeof(z_jfont_desc_t) <= end; at += 4) {
			const volatile z_jfont_desc_t *d =
				(const volatile z_jfont_desc_t *)(uintptr_t)at;
			if (d->magic0 != Z_JFONT_MAGIC0) continue;
			if (!z_jfont_desc_ok(d, at)) continue;
			*phys = at;
			return d;
		}
	}
	return NULL;

}

// -- the font file (tools/gen_jfont.py) --

// The glyph for `cp` in font file `f` (len bytes), or NULL. Glyphs are
// h rows of 2 bytes, MSB-first, the glyph in the top w bits.
static inline const uint8_t *z_zfn_glyph(const uint8_t *f, uint32_t len,
	uint32_t cp, int *w, int *h) {

	if (!f || len < 12 || f[0] != 'Z' || f[1] != 'F' || f[2] != 'N' || f[3] != '1')
		return NULL;
	if (cp > 0xFFFF) return NULL;

	uint32_t count = (uint32_t)f[8] | ((uint32_t)f[9] << 8) |
		((uint32_t)f[10] << 16) | ((uint32_t)f[11] << 24);
	uint32_t gh = f[5], stride = f[6];
	uint32_t data = 12 + 2 * count;
	data = (data + 3) & ~3u;
	if (data + count * gh * stride > len) return NULL;

	const uint8_t *codes = f + 12;
	int lo = 0, hi = (int)count - 1;
	while (lo <= hi) {
		int mid = (lo + hi) / 2;
		uint32_t c = (uint32_t)codes[2 * mid] | ((uint32_t)codes[2 * mid + 1] << 8);
		if (c == cp) {
			if (w) *w = f[4];
			if (h) *h = (int)gh;
			return f + data + (uint32_t)mid * gh * stride;
		}
		if (c < cp) lo = mid + 1;
		else hi = mid - 1;
	}
	return NULL;

}

#endif
