/*
 * print -- PWG Raster. See pwg.h.
 */

#include "pwg.h"

#include <string.h>

static void be32(uint8_t *p, uint32_t v) {
	p[0] = (uint8_t)(v >> 24);
	p[1] = (uint8_t)(v >> 16);
	p[2] = (uint8_t)(v >> 8);
	p[3] = (uint8_t)v;
}

static void cstr(uint8_t *p, const char *s, int max) {
	int i;
	for (i = 0; s && s[i] && i < max - 1; i++) p[i] = (uint8_t)s[i];
}

bool pwg_file(out_t *o) {
	return out_put(o, "RaS2", 4);
}

// The header: the CUPS raster v2 page header, with the values PWG 5102.4
// fixes. Offsets are the standard's.
static void header(uint8_t *h, const pwg_page_t *pg, int bpp) {
	uint32_t bpl = (uint32_t)pg->w * (uint32_t)bpp;
	memset(h, 0, PWG_HEADER_BYTES);
	cstr(h + 0, "PwgRaster", 64);					// MediaClass
	cstr(h + 128, pg->media_type, 64);				// MediaType
	be32(h + 276, (uint32_t)pg->dpi);				// HWResolution
	be32(h + 280, (uint32_t)pg->dpi);
	be32(h + 340, 1);								// NumCopies
	be32(h + 352, (uint32_t)((pg->w * 72 + pg->dpi / 2) / pg->dpi));	// PageSize, points
	be32(h + 356, (uint32_t)((pg->h * 72 + pg->dpi / 2) / pg->dpi));
	be32(h + 372, (uint32_t)pg->w);					// cupsWidth
	be32(h + 376, (uint32_t)pg->h);					// cupsHeight
	be32(h + 384, 8);								// cupsBitsPerColor
	be32(h + 388, (uint32_t)bpp * 8);				// cupsBitsPerPixel
	be32(h + 392, bpl);								// cupsBytesPerLine
	be32(h + 396, 0);								// cupsColorOrder: chunky
	be32(h + 400, pg->space == PWG_SRGB8 ? 19 : 18);	// cupsColorSpace: sRGB / sGray
	be32(h + 420, (uint32_t)bpp);					// cupsNumColors
	be32(h + 452, (uint32_t)pg->pages);				// TotalPageCount
	be32(h + 456, 1);								// CrossFeedTransform
	be32(h + 460, 1);								// FeedTransform
	be32(h + 480, 0x00FFFFFF);						// AlternatePrimary
	be32(h + 484, (uint32_t)pg->quality);			// PrintQuality
	cstr(h + 1732, pg->media, 64);					// PageSizeName
}

bool pwg_page(pwg_t *p, out_t *o, const pwg_page_t *pg, uint8_t *prev, uint8_t *enc) {
	uint8_t h[PWG_HEADER_BYTES];
	memset(p, 0, sizeof(*p));
	p->o = o;
	p->w = pg->w;
	p->h = pg->h;
	p->dpi = pg->dpi;
	p->space = pg->space;
	p->bpp = pg->space == PWG_SRGB8 ? 3 : 1;
	p->prev = prev;
	p->enc = enc;
	header(h, pg, p->bpp);
	return out_put(o, h, PWG_HEADER_BYTES);
}

// How many pixels from i on equal gray[i] (at most `max`). Four at a
// time where it can: a white margin is most of what this walks.
static int run_len(const uint8_t *g, int i, int w, int max) {
	uint8_t v = g[i];
	int n = 1, lim = w - i < max ? w - i : max;
	uint32_t v4 = v * 0x01010101u;
	while (n < lim && ((uintptr_t)(g + i + n) & 3)) {
		if (g[i + n] != v) return n;
		n++;
	}
	while (n + 4 <= lim && *(const uint32_t *)(const void *)(g + i + n) == v4) n += 4;
	while (n < lim && g[i + n] == v) n++;
	return n;
}

static inline uint8_t *put_px(uint8_t *d, uint8_t v, int bpp) {
	*d++ = v;
	if (bpp == 3) { *d++ = v; *d++ = v; }
	return d;
}

uint32_t pwg_encode_line(const uint8_t *g, int w, int bpp, uint8_t *dst) {
	uint8_t *d = dst;
	int i = 0;
	while (i < w) {
		int r = run_len(g, i, w, 128);
		if (r >= 2) {
			*d++ = (uint8_t)(r - 1);
			d = put_px(d, g[i], bpp);
			i += r;
			continue;
		}
		// literal: up to the next pair of equal pixels, at most 128
		{
			int s = i, n = 1, k;
			while (s + n < w && n < 128) {
				if (s + n + 1 < w && g[s + n] == g[s + n + 1]) break;
				n++;
			}
			if (n == 1) {
				*d++ = 0;		// one pixel, "repeated once"
				d = put_px(d, g[s], bpp);
			} else {
				*d++ = (uint8_t)(257 - n);
				for (k = 0; k < n; k++) d = put_px(d, g[s + k], bpp);
			}
			i += n;
		}
	}
	return (uint32_t)(d - dst);
}

static bool flush(pwg_t *p) {
	uint8_t rep;
	uint32_t n;
	if (!p->reps) return true;
	rep = (uint8_t)(p->reps - 1);
	n = pwg_encode_line(p->held, p->w, p->bpp, p->enc);
	p->reps = 0;
	return out_put(p->o, &rep, 1) && out_put(p->o, p->enc, n);
}

// Two lines the same? A word at a time, four words a turn: most lines
// of a page are its margin, the same as the line before.
static bool same(const uint8_t *a, const uint8_t *b, int n) {
	if (!(((uintptr_t)a | (uintptr_t)b) & 3)) {
		const uint32_t *x = (const uint32_t *)(const void *)a, *y = (const uint32_t *)(const void *)b;
		int w = n >> 2, i = 0;
		for (; i + 4 <= w; i += 4)
			if ((x[i] ^ y[i]) | (x[i + 1] ^ y[i + 1]) | (x[i + 2] ^ y[i + 2]) | (x[i + 3] ^ y[i + 3]))
				return false;
		for (; i < w; i++) if (x[i] != y[i]) return false;
		a += w * 4;
		b += w * 4;
		n &= 3;
	}
	while (n-- > 0) if (*a++ != *b++) return false;
	return true;
}

bool pwg_line(pwg_t *p, const uint8_t *gray) {
	if (p->lines >= p->h) return true;
	p->lines++;
	if (p->reps && p->reps < 256 && (p->held == gray || same(p->held, gray, p->w))) {
		p->reps++;
		return true;
	}
	if (!flush(p)) return false;
	p->held = gray;
	p->reps = 1;
	return true;
}

void pwg_keep(pwg_t *p) {
	if (p->reps && p->held != p->prev) {
		memcpy(p->prev, p->held, (size_t)p->w);
		p->held = p->prev;
	}
}

bool pwg_end(pwg_t *p) {
	if (p->lines < p->h) {
		// the rest of the page, white
		if (!p->reps || p->held[0] != 255 || run_len(p->held, 0, p->w, p->w) != p->w) {
			if (!flush(p)) return false;
			memset(p->prev, 255, (size_t)p->w);
			p->held = p->prev;
		}
		while (p->lines < p->h) {
			if (p->reps == 256 && !flush(p)) return false;
			p->reps++;
			p->lines++;
		}
	}
	return flush(p);
}
