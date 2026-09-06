/*
 * chip8 -- interpreter core. See core.h for the division of labour and
 * for why this file includes nothing from sw/common.
 */

#include <string.h>

#include "core.h"

/* -- fonts ----------------------------------------------------------
 *
 * Loaded into guest RAM at reset rather than being read from a table
 * at FX29 time, because a program is entitled to look at the font
 * bytes directly (several do -- it is the only ROM-independent sprite
 * data a CHIP-8 program has) and to overwrite them. */

static const uint8_t font_small[16 * 5] = {
	0xF0, 0x90, 0x90, 0x90, 0xF0,   /* 0 */
	0x20, 0x60, 0x20, 0x20, 0x70,   /* 1 */
	0xF0, 0x10, 0xF0, 0x80, 0xF0,   /* 2 */
	0xF0, 0x10, 0xF0, 0x10, 0xF0,   /* 3 */
	0x90, 0x90, 0xF0, 0x10, 0x10,   /* 4 */
	0xF0, 0x80, 0xF0, 0x10, 0xF0,   /* 5 */
	0xF0, 0x80, 0xF0, 0x90, 0xF0,   /* 6 */
	0xF0, 0x10, 0x20, 0x40, 0x40,   /* 7 */
	0xF0, 0x90, 0xF0, 0x90, 0xF0,   /* 8 */
	0xF0, 0x90, 0xF0, 0x10, 0xF0,   /* 9 */
	0xF0, 0x90, 0xF0, 0x90, 0x90,   /* A */
	0xE0, 0x90, 0xE0, 0x90, 0xE0,   /* B */
	0xF0, 0x80, 0x80, 0x80, 0xF0,   /* C */
	0xE0, 0x90, 0x90, 0x90, 0xE0,   /* D */
	0xF0, 0x80, 0xF0, 0x80, 0xF0,   /* E */
	0xF0, 0x80, 0xF0, 0x80, 0x80    /* F */
};

/* SUPER-CHIP only ever shipped big glyphs for 0-9. A-F are here
 * because XO-CHIP programs use FX30 on the full hex range and an
 * interpreter that answers is a superset of one that faults. */
static const uint8_t font_big[16 * 10] = {
	0x3C, 0x7E, 0xE7, 0xC3, 0xC3, 0xC3, 0xC3, 0xE7, 0x7E, 0x3C,  /* 0 */
	0x18, 0x38, 0x58, 0x18, 0x18, 0x18, 0x18, 0x18, 0x3C, 0x3C,  /* 1 */
	0x3E, 0x7F, 0xC3, 0x06, 0x0C, 0x18, 0x30, 0x60, 0xFF, 0xFF,  /* 2 */
	0x3C, 0x7E, 0xC3, 0x03, 0x0E, 0x0E, 0x03, 0xC3, 0x7E, 0x3C,  /* 3 */
	0x06, 0x0E, 0x1E, 0x36, 0x66, 0xC6, 0xFF, 0xFF, 0x06, 0x06,  /* 4 */
	0xFF, 0xFF, 0xC0, 0xC0, 0xFC, 0xFE, 0x03, 0xC3, 0x7E, 0x3C,  /* 5 */
	0x3E, 0x7C, 0xC0, 0xC0, 0xFC, 0xFE, 0xC3, 0xC3, 0x7E, 0x3C,  /* 6 */
	0xFF, 0xFF, 0x03, 0x06, 0x0C, 0x18, 0x30, 0x60, 0x60, 0x60,  /* 7 */
	0x3C, 0x7E, 0xC3, 0xC3, 0x7E, 0x7E, 0xC3, 0xC3, 0x7E, 0x3C,  /* 8 */
	0x3C, 0x7E, 0xC3, 0xC3, 0x7F, 0x3F, 0x03, 0x03, 0x3E, 0x7C,  /* 9 */
	0x3C, 0x7E, 0xC3, 0xC3, 0xFF, 0xFF, 0xC3, 0xC3, 0xC3, 0xC3,  /* A */
	0xFC, 0xFE, 0xC3, 0xC3, 0xFE, 0xFE, 0xC3, 0xC3, 0xFE, 0xFC,  /* B */
	0x3C, 0x7E, 0xC3, 0xC0, 0xC0, 0xC0, 0xC0, 0xC3, 0x7E, 0x3C,  /* C */
	0xFC, 0xFE, 0xC7, 0xC3, 0xC3, 0xC3, 0xC3, 0xC7, 0xFE, 0xFC,  /* D */
	0xFF, 0xFF, 0xC0, 0xC0, 0xFF, 0xFF, 0xC0, 0xC0, 0xFF, 0xFF,  /* E */
	0xFF, 0xFF, 0xC0, 0xC0, 0xFF, 0xFF, 0xC0, 0xC0, 0xC0, 0xC0   /* F */
};

/* -- memory --------------------------------------------------------- */

static inline uint8_t rd(const c8_t *c, uint16_t a) {
	return c->ram[a & c->q.addr_mask];
}

static inline void wr(c8_t *c, uint16_t a, uint8_t v) {
	c->ram[a & c->q.addr_mask] = v;
}

/* -- display -------------------------------------------------------- */

static inline int row_words(const c8_t *c) { return c->w >> 5; }

static void mark_all_dirty(c8_t *c) {
	c->dirty = ~(uint64_t)0;
}

static void plane_clear(c8_t *c, int p) {
	/* Clears the whole 128x64 plane, not just the rows the current
	 * resolution uses. A program that clears in lores and then
	 * switches to hires should not find the old hires image waiting
	 * for it in the rows lores never touched. */
	memset(c->px[p], 0, sizeof(c->px[p]));
}

/* XOR a 16-bit MSB-aligned pattern into `row` at pixel column `col`.
 *
 * The 64-bit intermediate is what makes an arbitrary column free: the
 * pattern is placed once in a window spanning two words and then split,
 * rather than being assembled from two separately-shifted halves with
 * the boundary case written out by hand.
 *
 * Returns nonzero if any set bit landed on a bit that was already set,
 * which is the collision every CHIP-8 program's VF is asking about. */
static uint32_t row_xor(uint32_t *row, int nwords, int col, uint16_t bits) {

	uint64_t v;
	uint32_t w0, w1, hit = 0;
	int wi, shift, i0, i1;

	if (bits == 0) return 0;

	wi = col >> 5;
	shift = col & 31;

	/* Leftmost pattern bit (bit 15 of `bits`) must land at bit
	 * (63 - shift) of the window, so the shift is 48 - shift. */
	v = (uint64_t)bits << (48 - shift);

	w0 = (uint32_t)(v >> 32);
	w1 = (uint32_t)v;

	i0 = wi % nwords;
	i1 = (wi + 1) % nwords;

	hit |= row[i0] & w0;
	row[i0] ^= w0;

	/* i1 == i0 only if nwords == 1, which no CHIP-8 resolution
	 * produces (64 and 128 give 2 and 4). Asserted by construction
	 * rather than checked, because a second XOR of the same word
	 * would silently undo the first. */
	hit |= row[i1] & w1;
	row[i1] ^= w1;

	return hit;

}

/* Keep the top `keep` bits of an MSB-aligned 16-bit pattern. */
static inline uint16_t keep_top(uint16_t bits, int keep) {
	if (keep >= 16) return bits;
	if (keep <= 0) return 0;
	return (uint16_t)(bits & (uint16_t)(0xFFFFu << (16 - keep)));
}

static int draw_sprite(c8_t *c, int sx, int sy, int n) {

	int rows    = n ? n : 16;
	int wide    = (n == 0);
	int bpr     = wide ? 2 : 1;
	int len     = wide ? 16 : 8;
	int nwords  = row_words(c);
	int x0      = sx % c->w;
	int y0      = sy % c->h;
	int vf      = 0;
	int pidx    = 0;
	int p, r;

	/* CHIP-8 has no DXY0 at all. SUPER-CHIP added it as 16x16 in
	 * hires; whether it is also 16x16 in lores is the quirk. */
	if (wide && !c->hires && !c->q.wide_sprite_lores) return 0;

	for (p = 0; p < C8_PLANES; p++) {

		if (!(c->plane_mask & (1u << p))) continue;

		for (r = 0; r < rows; r++) {

			uint16_t a = (uint16_t)(c->i + (pidx * rows + r) * bpr);
			uint16_t bits;
			int y = y0 + r;
			int keep;

			if (y >= c->h) {
				if (c->q.clip_sprites) {
					/* SUPER-CHIP counts rows lost off the bottom in
					 * VF along with rows that actually collided.
					 * Nothing is drawn either way. */
					if (c->q.collision_rows) vf++;
					continue;
				}
				y %= c->h;
			}

			bits = wide
				? (uint16_t)((rd(c, a) << 8) | rd(c, (uint16_t)(a + 1)))
				: (uint16_t)(rd(c, a) << 8);

			keep = len;
			if (c->q.clip_sprites && x0 + len > c->w)
				keep = c->w - x0;

			/* `bits` is MSB-aligned in 16 bits whether the sprite is
			 * 8 or 16 wide (the 8-wide case has zeros below), so the
			 * count of bits to keep is the pixel count directly. */
			bits = keep_top(bits, keep);

			if (row_xor(c->px[p][y], nwords, x0, bits)) {
				if (c->q.collision_rows) vf++;
				else vf = 1;
			}

			c->dirty |= (uint64_t)1 << y;

		}

		pidx++;

	}

	/* VF is a byte. A 16-row sprite can in principle collide on every
	 * row plus be clipped, which still fits, but clamp rather than
	 * wrap so a future wider sprite cannot produce a nonsense value. */
	return vf > 255 ? 255 : vf;

}

static void scroll_down(c8_t *c, int n) {

	int nwords = row_words(c);
	int p, y, w;

	if (n <= 0) return;

	for (p = 0; p < C8_PLANES; p++) {
		if (!(c->plane_mask & (1u << p))) continue;
		for (y = c->h - 1; y >= 0; y--)
			for (w = 0; w < nwords; w++)
				c->px[p][y][w] = (y >= n) ? c->px[p][y - n][w] : 0;
	}

	mark_all_dirty(c);

}

static void scroll_up(c8_t *c, int n) {

	int nwords = row_words(c);
	int p, y, w;

	if (n <= 0) return;

	for (p = 0; p < C8_PLANES; p++) {
		if (!(c->plane_mask & (1u << p))) continue;
		for (y = 0; y < c->h; y++)
			for (w = 0; w < nwords; w++)
				c->px[p][y][w] = (y + n < c->h) ? c->px[p][y + n][w] : 0;
	}

	mark_all_dirty(c);

}

static void scroll_right(c8_t *c, int k) {

	int nwords = row_words(c);
	int p, y, w;

	if (k <= 0 || k >= 32) return;

	for (p = 0; p < C8_PLANES; p++) {
		if (!(c->plane_mask & (1u << p))) continue;
		for (y = 0; y < c->h; y++) {
			uint32_t *r = c->px[p][y];
			for (w = nwords - 1; w > 0; w--)
				r[w] = (r[w] >> k) | (r[w - 1] << (32 - k));
			r[0] >>= k;
		}
	}

	mark_all_dirty(c);

}

static void scroll_left(c8_t *c, int k) {

	int nwords = row_words(c);
	int p, y, w;

	if (k <= 0 || k >= 32) return;

	for (p = 0; p < C8_PLANES; p++) {
		if (!(c->plane_mask & (1u << p))) continue;
		for (y = 0; y < c->h; y++) {
			uint32_t *r = c->px[p][y];
			for (w = 0; w < nwords - 1; w++)
				r[w] = (r[w] << k) | (r[w + 1] >> (32 - k));
			r[nwords - 1] <<= k;
		}
	}

	mark_all_dirty(c);

}

/* Scroll distances are quoted in HIRES pixels by SUPER-CHIP, whatever
 * mode is in force -- so in lores they are half a pixel each, and the
 * count gets halved. XO-CHIP quotes them in current-mode pixels
 * instead, which is what Octo programs assume. One place, one rule. */
static inline int scroll_amount(const c8_t *c, int n) {
	if (!c->hires && c->q.scroll_half_lores) return n / 2;
	return n;
}

static void set_mode(c8_t *c, bool hires) {

	c->hires = hires;
	c->w = hires ? 128 : 64;
	c->h = hires ? 64 : 32;

	if (c->q.clear_on_mode) {
		int p;
		for (p = 0; p < C8_PLANES; p++) plane_clear(c, p);
	}

	mark_all_dirty(c);

}

/* -- lifecycle ------------------------------------------------------ */

void c8_init(c8_t *c, const c8_quirks_t *q, uint32_t seed) {

	memset(c, 0, sizeof(*c));

	c->q = *q;

	memcpy(c->ram + C8_FONT_ADDR, font_small, sizeof(font_small));
	memcpy(c->ram + C8_FONT_BIG_ADDR, font_big, sizeof(font_big));

	c->pc = C8_START_ADDR;
	c->plane_mask = 1;
	c->pitch = C8_PITCH_DEFAULT;
	c->wait_key = -1;

	/* xorshift32 is stuck at zero, so a caller that has no entropy
	 * yet (or a test that does not care) still gets a usable and
	 * repeatable sequence instead of CXNN always returning 0. */
	c->rng = seed ? seed : 0x1A2B3C4Du;

	c->hires = false;
	c->w = 64;
	c->h = 32;

	mark_all_dirty(c);

}

bool c8_load(c8_t *c, const uint8_t *rom, uint32_t len) {

	if (len == 0) return false;
	if ((uint32_t)C8_START_ADDR + len > (uint32_t)c->q.addr_mask + 1)
		return false;

	memcpy(c->ram + C8_START_ADDR, rom, len);
	return true;

}

void c8_tick_timers(c8_t *c) {
	if (c->dt) c->dt--;
	if (c->st) c->st--;
}

void c8_frame(c8_t *c) {
	c->wait_frame = false;
}

void c8_key(c8_t *c, int k, bool down) {

	if (k < 0 || k > 15) return;

	if (down) c->keys |= (uint16_t)(1u << k);
	else      c->keys &= (uint16_t)~(1u << k);

	if (!c->waiting) return;

	/* FX0A completes on RELEASE, not on press. That is what the VIP
	 * did, and it is load-bearing: with press semantics, one held key
	 * satisfies every FX0A a program executes while it is down, so a
	 * menu that waits for a key twice takes both answers from a
	 * single press. */
	if (down) {
		if (c->wait_key < 0) c->wait_key = (int8_t)k;
	} else if (c->wait_key == (int8_t)k) {
		c->v[c->wait_reg] = (uint8_t)k;
		c->waiting = false;
		c->wait_key = -1;
	}

}

/* -- execution ------------------------------------------------------ */

/* Advance past the next instruction, which is FOUR bytes if that
 * instruction is XO-CHIP's F000 NNNN long load. Every conditional skip
 * goes through here; getting this wrong makes XO-CHIP programs jump
 * into the middle of an address literal, which presents as a random
 * crash a long way from the skip. */
static void skip_next(c8_t *c) {
	uint16_t nxt = (uint16_t)((rd(c, c->pc) << 8) | rd(c, (uint16_t)(c->pc + 1)));
	c->pc = (uint16_t)(c->pc + (nxt == 0xF000 ? 4 : 2));
}

static inline uint8_t rnd(c8_t *c) {
	c->rng ^= c->rng << 13;
	c->rng ^= c->rng >> 17;
	c->rng ^= c->rng << 5;
	return (uint8_t)(c->rng >> 16);
}

c8_status_t c8_step(c8_t *c) {

	uint16_t op, nnn;
	uint8_t x, y, kk, n;

	if (c->halted)  return C8_HALT;
	if (c->waiting) return C8_WAIT_KEY;
	if (c->wait_frame) return C8_WAIT_FRAME;

	op = (uint16_t)((rd(c, c->pc) << 8) | rd(c, (uint16_t)(c->pc + 1)));
	c->pc = (uint16_t)(c->pc + 2);
	c->steps++;

	nnn = (uint16_t)(op & 0x0FFF);
	x   = (uint8_t)((op >> 8) & 0x0F);
	y   = (uint8_t)((op >> 4) & 0x0F);
	kk  = (uint8_t)(op & 0xFF);
	n   = (uint8_t)(op & 0x0F);

	switch (op >> 12) {

	case 0x0:
		if (op == 0x00E0) {                     /* CLS */
			int p;
			for (p = 0; p < C8_PLANES; p++)
				if (c->plane_mask & (1u << p)) plane_clear(c, p);
			mark_all_dirty(c);
		} else if (op == 0x00EE) {              /* RET */
			if (c->sp == 0) goto bad;
			c->pc = c->stack[--c->sp];
		} else if ((op & 0xFFF0) == 0x00C0) {   /* SCD n */
			scroll_down(c, scroll_amount(c, n));
		} else if ((op & 0xFFF0) == 0x00D0) {   /* SCU n (XO-CHIP) */
			scroll_up(c, scroll_amount(c, n));
		} else if (op == 0x00FB) {              /* SCR */
			scroll_right(c, c->hires ? 4 : (c->q.scroll_half_lores ? 2 : 4));
		} else if (op == 0x00FC) {              /* SCL */
			scroll_left(c, c->hires ? 4 : (c->q.scroll_half_lores ? 2 : 4));
		} else if (op == 0x00FD) {              /* EXIT */
			c->halted = true;
			return C8_HALT;
		} else if (op == 0x00FE) {
			set_mode(c, false);
		} else if (op == 0x00FF) {
			set_mode(c, true);
		} else {
			/* 0NNN is a call into 1802 machine code. There is no
			 * meaningful emulation of it and no ROM worth running
			 * uses it, so it is an error rather than a silent nop --
			 * a silent nop turns "this ROM needs a VIP" into "this
			 * ROM hangs". */
			goto bad;
		}
		break;

	case 0x1: c->pc = nnn; break;

	case 0x2:
		if (c->sp >= C8_STACK_SIZE) goto bad;
		c->stack[c->sp++] = c->pc;
		c->pc = nnn;
		break;

	case 0x3: if (c->v[x] == kk) skip_next(c); break;
	case 0x4: if (c->v[x] != kk) skip_next(c); break;

	case 0x5:
		switch (n) {
		case 0x0:
			if (c->v[x] == c->v[y]) skip_next(c);
			break;
		case 0x2: {                             /* save vX..vY (XO-CHIP) */
			int i, step = (x <= y) ? 1 : -1, count = 0;
			for (i = x; ; i += step) {
				wr(c, (uint16_t)(c->i + count), c->v[i]);
				count++;
				if (i == y) break;
			}
			break;
		}
		case 0x3: {                             /* load vX..vY (XO-CHIP) */
			int i, step = (x <= y) ? 1 : -1, count = 0;
			for (i = x; ; i += step) {
				c->v[i] = rd(c, (uint16_t)(c->i + count));
				count++;
				if (i == y) break;
			}
			break;
		}
		default: goto bad;
		}
		break;

	case 0x6: c->v[x] = kk; break;
	case 0x7: c->v[x] = (uint8_t)(c->v[x] + kk); break;

	case 0x8:
		switch (n) {
		case 0x0: c->v[x] = c->v[y]; break;
		case 0x1:
			c->v[x] |= c->v[y];
			if (c->q.vf_reset) c->v[0xF] = 0;
			break;
		case 0x2:
			c->v[x] &= c->v[y];
			if (c->q.vf_reset) c->v[0xF] = 0;
			break;
		case 0x3:
			c->v[x] ^= c->v[y];
			if (c->q.vf_reset) c->v[0xF] = 0;
			break;
		case 0x4: {
			uint16_t s = (uint16_t)(c->v[x] + c->v[y]);
			c->v[x] = (uint8_t)s;
			/* VF is written LAST, unconditionally. When x is 0xF the
			 * flag is the only result that survives, which is what
			 * real hardware does and what several ROMs rely on. */
			c->v[0xF] = (s > 0xFF) ? 1 : 0;
			break;
		}
		case 0x5: {
			uint8_t f = (c->v[x] >= c->v[y]) ? 1 : 0;
			c->v[x] = (uint8_t)(c->v[x] - c->v[y]);
			c->v[0xF] = f;
			break;
		}
		case 0x6: {
			uint8_t src = c->q.shift_vx ? c->v[x] : c->v[y];
			uint8_t f = (uint8_t)(src & 1);
			c->v[x] = (uint8_t)(src >> 1);
			c->v[0xF] = f;
			break;
		}
		case 0x7: {
			uint8_t f = (c->v[y] >= c->v[x]) ? 1 : 0;
			c->v[x] = (uint8_t)(c->v[y] - c->v[x]);
			c->v[0xF] = f;
			break;
		}
		case 0xE: {
			uint8_t src = c->q.shift_vx ? c->v[x] : c->v[y];
			uint8_t f = (uint8_t)(src >> 7);
			c->v[x] = (uint8_t)(src << 1);
			c->v[0xF] = f;
			break;
		}
		default: goto bad;
		}
		break;

	case 0x9:
		if (n != 0) goto bad;
		if (c->v[x] != c->v[y]) skip_next(c);
		break;

	case 0xA: c->i = nnn; break;

	case 0xB:
		/* BNNN on CHIP-8; the CHIP-48 assembler reinterpreted the
		 * same encoding as BXNN, and ROMs exist for both. */
		c->pc = (uint16_t)(nnn + (c->q.jump_vx ? c->v[x] : c->v[0]));
		break;

	case 0xC: c->v[x] = (uint8_t)(rnd(c) & kk); break;

	case 0xD:
		c->v[0xF] = (uint8_t)draw_sprite(c, c->v[x], c->v[y], n);
		if (c->q.display_wait) {
			c->wait_frame = true;
			return C8_WAIT_FRAME;
		}
		break;

	case 0xE:
		if (kk == 0x9E) {
			if (c->keys & (1u << (c->v[x] & 0x0F))) skip_next(c);
		} else if (kk == 0xA1) {
			if (!(c->keys & (1u << (c->v[x] & 0x0F)))) skip_next(c);
		} else goto bad;
		break;

	case 0xF:
		/* F000 NNNN -- long load, the only four-byte instruction. */
		if (op == 0xF000) {
			c->i = (uint16_t)((rd(c, c->pc) << 8) | rd(c, (uint16_t)(c->pc + 1)));
			c->pc = (uint16_t)(c->pc + 2);
			break;
		}
		if (op == 0xF002) {                     /* audio: pattern := [I] */
			int k;
			for (k = 0; k < C8_PATTERN_BYTES; k++)
				c->pattern[k] = rd(c, (uint16_t)(c->i + k));
			c->audio_gen++;
			break;
		}
		if ((op & 0xF0FF) == 0xF001) {          /* plane N */
			c->plane_mask = (uint8_t)(x & 0x03);
			/* Latched here, and only here: selecting plane 1 is the
			 * one unambiguous statement a program makes that it is a
			 * two-plane program. See two_plane in core.h. */
			if (c->plane_mask & 2) c->two_plane = true;
			break;
		}
		switch (kk) {
		case 0x07: c->v[x] = c->dt; break;
		case 0x0A:
			c->waiting = true;
			c->wait_reg = x;
			c->wait_key = -1;
			return C8_WAIT_KEY;
		case 0x15: c->dt = c->v[x]; break;
		case 0x18: c->st = c->v[x]; break;
		case 0x1E: c->i = (uint16_t)(c->i + c->v[x]); break;
		case 0x29: c->i = (uint16_t)(C8_FONT_ADDR + (c->v[x] & 0x0F) * 5); break;
		case 0x30: c->i = (uint16_t)(C8_FONT_BIG_ADDR + (c->v[x] & 0x0F) * 10); break;
		case 0x33:
			wr(c, c->i,                    (uint8_t)(c->v[x] / 100));
			wr(c, (uint16_t)(c->i + 1),    (uint8_t)((c->v[x] / 10) % 10));
			wr(c, (uint16_t)(c->i + 2),    (uint8_t)(c->v[x] % 10));
			break;
		case 0x3A: c->pitch = c->v[x]; c->audio_gen++; break;
		case 0x55: {
			int k;
			for (k = 0; k <= x; k++) wr(c, (uint16_t)(c->i + k), c->v[k]);
			if (c->q.mem_inc == C8_MEM_INC_X1) c->i = (uint16_t)(c->i + x + 1);
			else if (c->q.mem_inc == C8_MEM_INC_X) c->i = (uint16_t)(c->i + x);
			break;
		}
		case 0x65: {
			int k;
			for (k = 0; k <= x; k++) c->v[k] = rd(c, (uint16_t)(c->i + k));
			if (c->q.mem_inc == C8_MEM_INC_X1) c->i = (uint16_t)(c->i + x + 1);
			else if (c->q.mem_inc == C8_MEM_INC_X) c->i = (uint16_t)(c->i + x);
			break;
		}
		case 0x75: {
			int k, lim = x < c->q.flag_count ? x : c->q.flag_count - 1;
			for (k = 0; k <= lim; k++) c->flags[k] = c->v[k];
			break;
		}
		case 0x85: {
			int k, lim = x < c->q.flag_count ? x : c->q.flag_count - 1;
			for (k = 0; k <= lim; k++) c->v[k] = c->flags[k];
			break;
		}
		default: goto bad;
		}
		break;

	default: goto bad;

	}

	return C8_OK;

bad:
	/* PC is rewound to the offending instruction so a debugger can
	 * show it in place, and so the app can report an address that
	 * matches what a disassembly listing says. */
	c->pc = (uint16_t)(c->pc - 2);
	c->bad_op = op;
	return C8_BAD_OPCODE;

}

c8_status_t c8_run(c8_t *c, int budget, int *executed) {

	int done = 0;
	c8_status_t st = C8_OK;

	while (done < budget) {
		st = c8_step(c);
		if (st != C8_OK) break;
		done++;
	}

	if (executed) *executed = done;
	return st;

}
