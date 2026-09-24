/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * Captions -- see zcaption.h and docs/captions.md.
 */

#include <string.h>

#include "zcaption.h"
#include "zfont.h"
#include "zutf8.h"

#ifndef ZCAPTION_HOST
#include "zeitlos.h"
#include "zwm.h"

// -- client --
//
// A Z_STR payload is BORROWED until the receiver reads it
// (docs/messaging.md), so the text goes into a small ring that is only
// reused four sends later -- the same trade zspeak.c makes.
#define CAP_RING	4
static char cap_ring[CAP_RING][Z_CAPTION_TEXT_MAX * 2];
static int cap_slot;
static uint32_t cap_wm_pid;

static bool cap_send(const char *utf8, uint32_t opts) {
	if (!cap_wm_pid && !z_pid_lookup("wm0", &cap_wm_pid)) {
		cap_wm_pid = 0;
		return false;
	}
	char *buf = cap_ring[cap_slot];
	cap_slot = (cap_slot + 1) % CAP_RING;
	strncpy(buf, utf8 ? utf8 : "", sizeof(cap_ring[0]) - 1);
	buf[sizeof(cap_ring[0]) - 1] = 0;
	z_obj_t o;
	o.type = Z_STR;
	o.val.str = buf;
	if (z_msg_new_send(cap_wm_pid, Z_WM_CAPTION, opts, o) != Z_OK) {
		cap_wm_pid = 0;		// look it up again next time
		return false;
	}
	return true;
}

bool z_caption_show(const char *utf8, uint32_t opts) {
	return cap_send(utf8, opts);
}

bool z_caption_hide(void) {
	return cap_send("", 0);
}
#endif

// -- renderer --

int z_caption_to_l9(const char *utf8, char *out, int cap) {
	int n = 0;
	const char *p = utf8 ? utf8 : "";
	while (*p && n < cap - 1) {
		uint32_t cp = z_utf8_next_z(&p);
		uint8_t b;
		if (cp == '\n') b = '\n';
		else if (cp < 0x20 || cp == 0x7F) b = ' ';
		else {
			b = z_cp_to_l9(cp);
			if (!b) b = Z_GLYPH_MISSING;
		}
		out[n++] = (char)b;
	}
	out[n] = 0;
	return n;
}

// Sets (ink) or clears bits [x, x+n) of one row, n <= 32.
static void row_bits(uint32_t *row, int x, uint32_t bits, int n, bool ink) {
	if (n <= 0) return;
	uint32_t w = x >> 5, sh = x & 31;
	uint32_t lo = bits << sh;
	uint32_t hi = sh ? (n + (int)sh > 32 ? bits >> (32 - sh) : 0) : 0;
	if (ink) { row[w] |= lo; if (hi) row[w + 1] |= hi; }
	else     { row[w] &= ~lo; if (hi) row[w + 1] &= ~hi; }
}

static void row_span(uint32_t *row, int x0, int x1, bool ink) {
	for (int x = x0; x <= x1; ) {
		int n = x1 - x + 1;
		if (n > 32 - (x & 31)) n = 32 - (x & 31);
		uint32_t m = (n == 32) ? 0xFFFFFFFFu : ((1u << n) - 1u);
		row_bits(row, x, m, n, ink);
		x += n;
	}
}

// Greedy word wrap into at most Z_CAPTION_LINES_MAX lines of at most
// `cols` characters. A word longer than a line is broken where it hits
// the edge.
static int wrap(const char *t, int cols, const char **ls, int *ll) {
	int n = 0;
	const char *p = t;
	while (*p && n < Z_CAPTION_LINES_MAX) {
		while (*p == ' ') p++;
		if (!*p) break;
		int len = 0, brk = -1;
		while (p[len] && p[len] != '\n' && len < cols) {
			if (p[len] == ' ') brk = len;
			len++;
		}
		int take = len, next = len;
		if (p[len] && p[len] != '\n') {		// line is full
			if (p[len] == ' ') next = len + 1;
			else if (brk > 0) { take = brk; next = brk + 1; }
		}
		while (take > 0 && p[take - 1] == ' ') take--;
		if (p[next] == '\n') next++;
		ls[n] = p; ll[n] = take; n++;
		p += next;
	}
	return n;
}

bool z_caption_render(const char *l9, uint32_t opts, int band_w,
	uint32_t *bits, int stride_words, int max_h, int *out_w, int *out_h) {

	const z_font_t *f = &z_font_6x12;
	int s = Z_CAPTION_GET_SCALE(opts);
	if (s < 1) s = 2;
	int gw = f->w * s, gh = f->h * s;
	int padx = 4 * s + 2, pady = 2 * s + 2, gap = 2 * s;

	if (band_w > stride_words * 32) band_w = stride_words * 32;
	int cols = (band_w - 2 * padx) / gw;
	if (cols < 1) return false;

	const char *ls[Z_CAPTION_LINES_MAX];
	int ll[Z_CAPTION_LINES_MAX];
	int n = wrap(l9, cols, ls, ll);

	// Drop lines that would not fit the bitmap.
	while (n > 0 && n * gh + (n - 1) * gap + 2 * pady > max_h) n--;
	if (n == 0) return false;

	int longest = 0;
	for (int i = 0; i < n; i++) if (ll[i] > longest) longest = ll[i];
	if (longest == 0) return false;

	int bw = (opts & Z_CAPTION_COMPACT) ? longest * gw + 2 * padx : band_w;
	int bh = n * gh + (n - 1) * gap + 2 * pady;
	bool inv = (opts & Z_CAPTION_INVERSE) != 0;

	// Background, then a 1px frame in the ink colour of the box edge:
	// lit on a dark box so it reads against a dark desktop.
	for (int y = 0; y < bh; y++) {
		uint32_t *row = bits + y * stride_words;
		memset(row, 0, (size_t)((bw + 31) / 32) * 4);
		if (inv) row_span(row, 0, bw - 1, true);
		if (!inv) {
			if (y == 0 || y == bh - 1) row_span(row, 0, bw - 1, true);
			else { row_bits(row, 0, 1, 1, true); row_bits(row, bw - 1, 1, 1, true); }
		}
	}

	// Glyphs, each row widened by s and repeated s times.
	for (int i = 0; i < n; i++) {
		int x0 = (bw - ll[i] * gw) / 2;
		int y0 = pady + i * (gh + gap);
		for (int c = 0; c < ll[i]; c++) {
			int gi = z_font_index(f, (uint8_t)ls[i][c]);
			if (gi < 0) gi = z_font_index(f, Z_GLYPH_MISSING);
			if (gi < 0) continue;
			const uint8_t *g = f->glyphs + gi * f->h;
			int gx = x0 + c * gw;
			for (int r = 0; r < f->h; r++) {
				uint8_t src = g[r];
				if (!src) continue;
				uint32_t wide = 0;
				for (int b = 0; b < f->w; b++)
					if (src & (0x80 >> b))
						wide |= ((1u << s) - 1u) << (b * s);
				for (int k = 0; k < s; k++)
					row_bits(bits + (y0 + r * s + k) * stride_words,
						gx, wide, gw, !inv);
			}
		}
	}

	*out_w = bw;
	*out_h = bh;
	return true;
}
