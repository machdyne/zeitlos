/*
 * Zeitlos
 * Copyright (c) 2025-2026 Lone Dynamics Corporation. All rights reserved.
 *
 * See zscreen.h. Nothing here sends a byte. The caller decides who is
 * watching and what the packet around the payload looks like.
 *
 * ONE FRAME IS ONE INSTANT
 *
 * Reading the framebuffer stripe by stripe, all the way through --
 * hash 16 rows, compress them, clock them out, then the next 16 rows --
 * takes tens of milliseconds, and the screen does not hold still for
 * it. The thirty stripes were thirty different moments. With a window
 * being dragged, a viewer assembled a frame holding the elastic band
 * in several places at once.
 *
 * The cure is a snapshot. One tight pass copies the whole framebuffer
 * into this process's own memory AND hashes it on the way through, and
 * everything after that -- comparison, PackBits, transmission -- reads
 * the copy. Whatever the wm does next cannot reach a scan already in
 * flight. There is no double buffer in the SOC to read from instead;
 * this is 38400 bytes of the caller's own region standing in for one.
 *
 * It is close to free because it replaces reads rather than adding
 * them: the hash pass already read all 9600 words out of VRAM, and
 * every stripe that got sent was then read a SECOND time into a
 * staging buffer for PackBits. Fusing copy and hash trades those
 * second reads for the writes of the copy.
 *
 * A snapshot is only half of it. A viewer paints each stripe as it
 * lands, so a consistent frame still arrives in pieces. So each stripe
 * also carries a frame trailer saying which frame it belongs to and
 * whether it is the last of it, and the viewer page assembles
 * off-screen and shows whole frames.
 *
 * WHAT CHANGED, WITHOUT LOOKING
 *
 * The snapshot read all 9600 words of VRAM on every scan, up to 20
 * times a second, and with nothing on screen moving that was nearly
 * all the work: 14 ms a pass, to learn that nothing had changed.
 *
 * Where the bitstream has rtl/socctl.v's DIRTY register
 * (Z_FEATURE2_VRAM_DIRTY), the hardware keeps one bit per stripe, set
 * by every write the VRAM accepts -- from the CPU, the blitter or the
 * line rasterizer, so nothing that draws can get past it. A scan reads
 * it, clears what it read, and snapshots only those stripes. A stripe
 * whose bit is clear has not been written since it was last copied, so
 * the copy in snap[] IS the screen, and a frame is still one instant:
 * the stripes that are read are read in one tight pass, now a short
 * one, and the rest have not moved. Order matters: clear first, copy
 * after, so a write that races the scan sets its bit again and is
 * picked up next time instead of being lost.
 *
 * Nothing on the wire changes. Same stripes, same PackBits, same
 * trailer.
 *
 * The periodic full resend stays with the caller (it heals a stripe
 * lost in flight), and on this path it doubles as a check on the
 * hardware: it reads every stripe, and any stripe that changed without
 * its bit set is counted. That count should be zero, always. Without
 * the register, every scan reads everything.
 */

#include <stdint.h>
#include <string.h>
#include <stdio.h>

#include "zscreen.h"
#include "zsoc.h"
#include "../apps/net/packbits.h"

#define FB_BASE 0x20000000

_Static_assert(ZSCREEN_ALL == Z_SOCCTL_DIRTY_ALL, "stripe mask");
_Static_assert(ZSCREEN_PACK_WORST == 1709, "packbits worst case");
_Static_assert(ZSCREEN_LITERAL_LEN == 1290, "literal-run length");
_Static_assert(ZSCREEN_LITERAL_LEN <= ZSCREEN_PACK_WORST, "fallback fits");

#ifdef SCREEN_PROFILE
static uint32_t rdcycle(void)
{
	uint32_t v;
	__asm__ volatile ("rdcycle %0" : "=r"(v));
	return v;
}
#endif

/* THE HASH HAS TO SEE A VERTICAL LINE
 *
 * This was `h ^= w[i]; h = rotl(h, 5);` and it could not tell a stripe
 * with a full-height vertical line in it from a blank one. Not a
 * near miss -- the same hash, exactly, for every column.
 *
 * Why: xor and rotate are both linear, so the hash is the xor of every
 * word rotated by its distance from the end, and two words land on the
 * same rotation whenever their indices differ by a multiple of 32. A
 * row is 20 words, so rows r and r+8 alias (20*8 = 160, a multiple of
 * 32), and a 16-row stripe is exactly eight such pairs. A vertical
 * line writes the SAME bit in the SAME word of every row, so each pair
 * cancels and the whole line contributes nothing. Any rotate-and-xor
 * has this hole: with an odd rotate the aliasing distance works out to
 * 8 rows whatever the amount, and an even one is worse.
 *
 * What that cost, on screen: dragging a window over the remote desktop
 * left scraps of the elastic band behind -- its two VERTICAL edges,
 * the only part of the picture made of full-height lines, were the
 * part the scan could not see change. Only the stripes holding the
 * band's horizontal top and bottom were ever resent, and the leftovers
 * survived until the five-second full resend wiped them.
 *
 * `h += w[i]` afterwards is the whole fix. Addition carries across bit
 * positions, so the hash stops being linear over xor and the pairs no
 * longer cancel. One instruction per word, and no multiply -- rv32im
 * has one, but a 32-step sequential multiplier on a board without the
 * DSP option would cost more than the copy this loop exists for.
 *
 * Copy and hash are fused deliberately: a second pass to hash the copy
 * would cost as much as the reads it saves, and the point of the
 * exercise is that the snapshot is not paid for twice.
 *
 * Reads only the stripes in `want` (bit n = stripe n), and returns
 * the ones among them whose contents differ from their last copy. */
static uint32_t snapshot(zscreen_t *s, uint32_t want)
{
	uint32_t changed = 0;
#ifdef SCREEN_PROFILE
	/* rdcycle is wall clock and this loop is longer than a timeslice,
	 * so a single reading can be mostly somebody else's work; the
	 * MINIMUM over many scans is the one that means anything. */
	static uint32_t prof_min = 0xffffffffu, prof_sum, prof_n;
	uint32_t t0 = rdcycle();
#endif

	for (int idx = 0; idx < ZSCREEN_STRIPES; idx++) {
		if (!(want & (1u << idx)))
			continue;
		const volatile uint32_t *v =
			(const volatile uint32_t *)FB_BASE + idx * ZSCREEN_STRIPE_WORDS;
		uint32_t *d = s->snap + idx * ZSCREEN_STRIPE_WORDS;
		uint32_t h = 0x9e3779b9u ^ (uint32_t)idx;
		/* by fours: the loop arithmetic was a fifth of the work
		 * in a body this small */
		for (int i = 0; i < ZSCREEN_STRIPE_WORDS; i += 4) {
			uint32_t a = v[0], b = v[1], c = v[2], e = v[3];
			d[0] = a; d[1] = b; d[2] = c; d[3] = e;
			h ^= a; h = ((h << 5) | (h >> 27)) + a;
			h ^= b; h = ((h << 5) | (h >> 27)) + b;
			h ^= c; h = ((h << 5) | (h >> 27)) + c;
			h ^= e; h = ((h << 5) | (h >> 27)) + e;
			v += 4; d += 4;
		}
		if (!h)
			h = 1;		/* 0 means "never sent" */
		if (h != s->hash[idx]) {
			s->hash[idx] = h;
			changed |= 1u << idx;
		}
	}
#ifdef SCREEN_PROFILE
	uint32_t dt = rdcycle() - t0;
	if (dt < prof_min)
		prof_min = dt;
	prof_sum += dt;
	if (++prof_n == 128) {
		printf("screen: snap %lu cyc min over %lu scans, mean %lu\n",
			(unsigned long)prof_min, (unsigned long)prof_n,
			(unsigned long)(prof_sum / prof_n));
		prof_sum = 0;
		prof_n = 0;
	}
#endif
	return changed;
}

uint32_t zscreen_hash(const uint32_t *words, int idx)
{
	uint32_t h = 0x9e3779b9u ^ (uint32_t)idx;

	for (int i = 0; i < ZSCREEN_STRIPE_WORDS; i++) {
		uint32_t a = words[i];
		h ^= a;
		h = ((h << 5) | (h >> 27)) + a;
	}
	if (!h)
		h = 1;
	return h;
}

void zscreen_init(zscreen_t *s)
{
	memset(s, 0, sizeof(*s));
}

void zscreen_forget(zscreen_t *s)
{
	memset(s->hash, 0, sizeof(s->hash));
}

uint32_t zscreen_want(int dirty_hw, uint32_t marked, int full)
{
	if (!dirty_hw)
		return ZSCREEN_ALL;
	if (full != ZSCREEN_FULL_NONE)
		return ZSCREEN_ALL;
	return marked & ZSCREEN_ALL;
}

uint32_t zscreen_miss(uint32_t changed, uint32_t marked, uint32_t reg_after)
{
	return changed & ~marked & ~reg_after;
}

static int popcount30(uint32_t m)
{
	int n = 0;
	for (; m; m &= m - 1)
		n++;
	return n;
}

uint32_t zscreen_scan(zscreen_t *s, int full, uint32_t *missed)
{
	if (s->dirty_hw == 0)
		s->dirty_hw = z_soc_has_feature2(Z_FEATURE2_VRAM_DIRTY) &&
			z_socctl_present() ? 1 : 2;

	/* Read what was written, clear exactly that, and only then read
	 * the pixels -- see "WHAT CHANGED, WITHOUT LOOKING" above. */
	int has = s->dirty_hw == 1;
	uint32_t marked = 0;
	if (has) {
		marked = reg_socctl_dirty & ZSCREEN_ALL;
		if (marked)
			reg_socctl_dirty = marked;
	}
	int verify = has && full == ZSCREEN_FULL_VERIFY;
	uint32_t want = zscreen_want(has, marked, full);
	uint32_t changed = want ? snapshot(s, want) : 0;
	uint32_t miss = 0;

	if (verify) {
		/* written while the pass ran: its bit is set again, and it
		 * is next scan's business, not a miss */
		miss = zscreen_miss(changed, marked, reg_socctl_dirty);
		s->verify_passes++;
		if (miss)
			s->verify_missed += (uint32_t)popcount30(miss);
	}
	if (missed)
		*missed = miss;
	return changed;
}

/* THE FRAME TRAILER
 *
 * Four bytes after the PackBits payload: a magic byte, the frame this
 * stripe belongs to, and whether it closes that frame.
 *
 *	0x5A | fseq_lo | fseq_hi | flags   (bit 0 = last of frame)
 *
 * Behind the payload rather than in a header because a header does not
 * survive every trip: the ESP32 relays [idx, len, data] to the browser
 * and drops everything else, so anything the page must see has to
 * travel inside the payload. Trailing bytes are the one place where
 * that is invisible to a decoder that does not know about them --
 * PackBits stops the moment it has produced its 1280 bytes -- so an
 * ESP32 running the old firmware and a browser running the old page
 * both keep working, byte for byte, with only the frame assembly
 * missing.
 *
 * A reader tells trailer from payload by arithmetic, not by the magic
 * alone: the trailer is there only if there are exactly four bytes
 * left over after the decode consumed what it needed.
 */
void zscreen_trailer(uint8_t dst[ZSCREEN_TRAILER], uint16_t frame, int last)
{
	dst[0] = ZSCREEN_TRAILER_MAGIC;
	dst[1] = (uint8_t)(frame & 0xff);
	dst[2] = (uint8_t)(frame >> 8);
	dst[3] = last ? ZSCREEN_TRAILER_LAST : 0;
}

/* HOW BIG A STRIPE CAN GET
 *
 * PackBits is not "a shade over raw" at worst. The encoder spends two
 * bytes on a repeat of two and two on a literal of one, so a stripe
 * that alternates them -- A BB A BB ... -- comes out at four bytes for
 * every three: up to ZSCREEN_PACK_WORST for 1280. The dock's bottom
 * stripe encodes to 1373 bytes. A buffer sized under that, with the
 * trailer still to come, writes past its end, and a relay that takes
 * at most 1280 + 64 bytes of payload cuts the datagram short.
 *
 * So the caller sizes its buffer for the real worst case, and a stripe
 * that PackBits would grow past ZSCREEN_LITERAL_LEN is sent as plain
 * literal runs instead: 128 bytes at a time, ten control bytes on
 * 1280, valid PackBits that every decoder already reads, and inside
 * what the relay accepts with the trailer on. The encoder itself is
 * untouched. */
static int literal_runs(const uint8_t *in, int n, uint8_t *out)
{
	int o = 0;
	for (int i = 0; i < n; i += 128) {
		int lit = n - i < 128 ? n - i : 128;
		out[o++] = (uint8_t)(lit - 1);
		memcpy(out + o, in + i, lit);
		o += lit;
	}
	return o;
}

int zscreen_pack(const uint8_t *raw, uint8_t *dst)
{
	int clen = packbits(raw, ZSCREEN_STRIPE_BYTES, dst);
	if (clen > ZSCREEN_LITERAL_LEN)
		clen = literal_runs(raw, ZSCREEN_STRIPE_BYTES, dst);
	return clen;
}

int zscreen_encode(const uint8_t *raw, uint8_t *dst, uint16_t frame, int last)
{
	int clen = zscreen_pack(raw, dst);
	zscreen_trailer(dst + clen, frame, last);
	return clen + ZSCREEN_TRAILER;
}
