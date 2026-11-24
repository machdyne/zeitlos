/*
 * Host tests for sw/common/zscreen.c's scan, in every screen mode.
 *
 * zscreen.c is included here with its hardware replaced: VRAM is an
 * array, DIRTY a variable that every write below 9600 words marks the
 * way rtl/socctl.v does -- and DIRTY1..3 three more, for the words
 * past it, when the bitstream is to have them (dirty_more) -- and
 * VMODE a number. Every test runs twice, without DIRTY1..3 and with. A model of the viewer
 * page's decoder (esp32/zeitlos-nic/web/index.html: unpack, trailer,
 * SIZES, the stripe at byte 1280*n) takes what net would send and
 * keeps its own framebuffer, and every test ends with that
 * framebuffer equal to VRAM, byte for byte.
 *
 *   cc -std=gnu99 -Wall -Wextra -I sw/common -I sw/apps/net \
 *      -o /tmp/test_zscreen_modes sw/common/tests/test_zscreen_modes.c \
 *      && /tmp/test_zscreen_modes
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#define VRAM_WORDS 32768
static uint32_t vram[VRAM_WORDS + 512];	/* a last stripe can run past */
static uint32_t dirty_regs[4];		/* DIRTY, DIRTY1..3 */
static int dirty_reads[4], dirty_clears[4];
static int dirty_present = 1;
static int dirty_more;			/* DIRTY's bit 31 */
static int fake_mode;
#define dirty_reg dirty_regs[0]

#define ZSCREEN_VRAM           ((const volatile uint32_t *)vram)
#define ZSCREEN_DIRTY_PROBE()  (dirty_present)
#define ZSCREEN_DIRTY_MORE()   (dirty_more)
#define ZSCREEN_DIRTY_REGS(t)  ((void)(t), dirty_regs)
#define ZSCREEN_DIRTY_GET(w)   (dirty_reads[w]++, regs[w] & ZSCREEN_ALL)
#define ZSCREEN_DIRTY_CLEAR(w, m) (dirty_clears[w]++, regs[w] &= ~(m))
#define ZSCREEN_MODE()         (fake_mode)
#include "../zscreen.c"

static int run, failed;

#define CHECK(cond, what) do { run++; if (!(cond)) { failed++; \
	printf("FAIL %s:%d: %s%s\n", __FILE__, __LINE__, what, \
		dirty_more ? " (with DIRTY1..3)" : ""); } } while (0)

/* the desktop of each mode, ZSCREEN_TRAILER_MODE's order */
static const int SIZES[8][2] = {
	{ 640, 480 }, { 800, 600 }, { 1024, 768 }, { 1280, 720 },
	{ 800, 480 }, { 1280, 800 }, { 960, 540 }, { 640, 360 },
};

static int fb_bytes(int m)
{
	return SIZES[m][0] / 8 * SIZES[m][1];
}

/* -- the hardware: a write the way the blitter or the CPU makes one -- */

static void vwrite(int word, uint32_t v)
{
	int st = word / 320;
	vram[word] = v;
	if (dirty_present && (st < 30 || (dirty_more && st < 100)))
		dirty_regs[st / 30] |= 1u << (st % 30);
}

static uint32_t rng = 12345;
static uint32_t rnd(void)
{
	rng = rng * 1103515245u + 12345u;
	return rng >> 8;
}

/* a desktop-ish picture: mostly blank, some text-like noise */
static uint32_t picture(void)
{
	uint32_t r = rnd();
	return (r & 7) ? 0 : rnd() * 2654435761u;
}

static void fill_screen(int m, int mark)
{
	int words = fb_bytes(m) / 4;
	for (int i = 0; i < words; i++) {
		if (mark)
			vwrite(i, picture());
		else
			vram[i] = picture();
	}
}

/* a box of rows y0..y1-1, columns of words x0..x1-1, in mode m */
static void box(int m, int x0, int y0, int x1, int y1, uint32_t v)
{
	int rw = SIZES[m][0] / 32;
	for (int y = y0; y < y1; y++)
		for (int x = x0; x < x1; x++)
			vwrite(y * rw + x, v);
}

/* -- the page -- */

static int page_w = 640, page_h = 480;
static uint8_t page_fb[160 * 800];
static int page_geom_seq = -1;
static int page_switches, page_stale_refused;

static int unpack(const uint8_t *src, int n, uint8_t out[1280], int *used)
{
	int o = 0, i = 0;
	while (i < n && o < 1280) {
		int c = src[i++];
		if (c < 128) {
			for (int k = 0; k <= c && o < 1280; k++)
				out[o++] = src[i++];
		} else if (c > 128) {
			int v = src[i++];
			for (int k = 0; k < 257 - c && o < 1280; k++)
				out[o++] = (uint8_t)v;
		}
	}
	*used = i;
	return o;
}

static int newer(int a, int b)
{
	return (int16_t)(uint16_t)(a - b) > 0;
}

static void page_take(int idx, const uint8_t *src, int len)
{
	uint8_t raw[1280];
	int used;
	if (unpack(src, len, raw, &used) != 1280 || idx >= 30)
		return;
	int n = idx;
	if (len - used == 4 && src[used] == 0x5A) {
		int seq = src[used + 1] | src[used + 2] << 8, f = src[used + 3];
		int m = (f & 2) ? (f >> 4) & 7 : 0;
		if (SIZES[m][0] != page_w || SIZES[m][1] != page_h) {
			int d = (page_geom_seq - seq) & 0xffff;
			if (!(page_geom_seq < 0 || d == 0 || d > 256)) {
				page_stale_refused++;
				return;
			}
			page_w = SIZES[m][0];
			page_h = SIZES[m][1];
			memset(page_fb, 0, sizeof page_fb);
			page_geom_seq = seq;
			page_switches++;
		} else if (page_geom_seq < 0 || newer(seq, page_geom_seq)) {
			page_geom_seq = seq;
		}
		if (f & 2)
			n += 30 * ((f >> 2) & 3);
	}
	int o = n * 1280, lim = page_w / 8 * page_h;
	int cnt = lim - o < 1280 ? lim - o : 1280;
	if (cnt > 0)
		memcpy(page_fb + o, raw, (size_t)cnt);
}

static int page_matches(int m)
{
	return page_w == SIZES[m][0] && page_h == SIZES[m][1] &&
		!memcmp(page_fb, vram, (size_t)fb_bytes(m));
}

/* -- net: screen_poll()'s loop, without the UART -- */

static zscreen_t S;
static uint16_t frame_seq;
static uint32_t now = 1000;
static int sent_total;
/* each stripe sent in 640x480, against the encoder before screen modes */
static int old_format_ok = 1, old_format_n;

static int poll_once(void)
{
	static uint8_t pkt[ZSCREEN_PACK_WORST + ZSCREEN_TRAILER];
	uint32_t changed = zscreen_scan(&S, now, NULL);
	uint32_t out = changed | S.owed_out;
	int sends = 0;

	frame_seq++;
	for (int i = 0; out; i++) {
		if (!(out & (1u << i)))
			continue;
		out &= ~(1u << i);
		int n = zscreen_encode(&S, i, pkt, frame_seq, !out);
		if (S.mode == 1) {
			/* 640x480: idx is the stripe, and the bytes are
			 * PackBits (or literal runs) plus the old trailer */
			static uint8_t ref[ZSCREEN_PACK_WORST];
			int c = zscreen_pack((const uint8_t *)(vram + i * 320), ref);
			if (n != c + 4 || memcmp(pkt, ref, (size_t)c) ||
					pkt[c] != 0x5A || pkt[c + 1] != (frame_seq & 0xff) ||
					pkt[c + 2] != (frame_seq >> 8) ||
					pkt[c + 3] != (out ? 0 : 1) || S.win != 0)
				old_format_ok = 0;
			old_format_n++;
		}
		page_take(i, pkt, n);
		sends++;
	}
	sent_total += sends;
	return sends;
}

/* polls at 20 a second until the page matches, at most `limit` ticks;
 * returns the ticks it took, or -1 */
static int converge(int m, int limit)
{
	uint32_t t0 = now;
	for (;;) {
		poll_once();
		if (page_matches(m))
			return (int)(now - t0);
		if ((int)(now - t0) > limit)
			return -1;
		now += 37;
	}
}

static void reset_all(int m)
{
	memset(&S, 0, sizeof S);
	memset(vram, 0, sizeof vram);
	for (int k = 0; k < 4; k++)	/* as after a reset */
		dirty_regs[k] = k < 3 ? ZSCREEN_ALL : 0x3ffu;
	fake_mode = m;
	page_w = 640;
	page_h = 480;
	memset(page_fb, 0, sizeof page_fb);
	page_geom_seq = -1;
	frame_seq = 0;
}

static void test_each_mode(void)
{
	char what[96];

	for (int m = 0; m < 8; m++) {
		reset_all(m);
		fill_screen(m, 1);
		/* the junk past a mode's framebuffer is not the screen */
		for (int i = fb_bytes(m) / 4; i < fb_bytes(m) / 4 + 320; i++)
			vram[i] = 0xdeadbeefu;
		int t = converge(m, 732);
		snprintf(what, sizeof what, "mode %d (%dx%d): the first frame arrives whole",
			m, SIZES[m][0], SIZES[m][1]);
		CHECK(t >= 0, what);
		snprintf(what, sizeof what, "mode %d: in as many scans as windows (%d ticks)", m, t);
		CHECK(t >= 0 && t <= 37 * (zscreen_windows(&S) - 1), what);
		snprintf(what, sizeof what, "mode %d: %d stripes", m, (int)S.stripes);
		CHECK(S.stripes == (fb_bytes(m) + 1279) / 1280, what);

		/* nothing changes: nothing is sent, however long */
		int quiet = 0;
		for (int k = 0; k < 40; k++, now += 37)
			quiet += poll_once();
		snprintf(what, sizeof what, "mode %d: a still screen sends nothing (%d)", m, quiet);
		CHECK(quiet == 0, what);

		/* small changes anywhere, tracked or not */
		int worst = 0, ok = 1;
		for (int round = 0; round < 30; round++) {
			int w32 = SIZES[m][0] / 32, h = SIZES[m][1];
			int x0 = (int)(rnd() % (uint32_t)w32), y0 = (int)(rnd() % (uint32_t)h);
			int x1 = x0 + 1 + (int)(rnd() % 4), y1 = y0 + 1 + (int)(rnd() % 30);
			if (x1 > w32) x1 = w32;
			if (y1 > h) y1 = h;
			box(m, x0, y0, x1, y1, rnd());
			now += 37;
			t = converge(m, 732);
			if (t < 0)
				ok = 0;
			if (t > worst)
				worst = t;
		}
		snprintf(what, sizeof what, "mode %d: every change arrives", m);
		CHECK(ok, what);
		if (dirty_more) {
			snprintf(what, sizeof what,
				"mode %d: on the next scan of its windows (worst %d ticks)", m, worst);
			CHECK(worst <= 37 * (zscreen_windows(&S) - 1), what);
		} else {
			snprintf(what, sizeof what,
				"mode %d: within the untracked period and a scan a window (worst %d ticks)", m, worst);
			CHECK(worst <= ZSCREEN_UNTRACKED_TICKS + 37 * zscreen_windows(&S), what);
		}
	}
}

/* Something that keeps changing below window 0 is followed scan by
 * scan once it has been seen. */
static void test_hot(void)
{
	reset_all(5);			/* 1280x800: four windows */
	fill_screen(5, 1);
	CHECK(converge(5, 732) >= 0, "1280x800 arrives");
	for (int k = 0; k < 30; k++, now += 37)
		poll_once();
	/* a cursor blinking in the bottom row, window 3 */
	int late = 0, first = -1;
	for (int k = 0; k < 40; k++) {
		box(5, 10, 790, 11, 798, (k & 1) ? ~0u : 0);
		now += 37;
		poll_once();
		poll_once();		/* the window 0 turn, if any */
		if (!page_matches(5)) {
			late++;
			if (first < 0)
				first = k;
			int t = converge(5, 732);
			if (t < 0)
				break;
		}
	}
	if (dirty_more) {
		CHECK(late == 0, "with DIRTY3 it is followed scan by scan from the first change");
	} else {
		CHECK(first >= 0 && first <= 1, "the first change of it waits for the window's turn");
		CHECK(late == 1, "after that it is followed scan by scan");
	}

	/* still for longer than ZSCREEN_HOT_TICKS: it waits again */
	for (int k = 0; k < 50; k++, now += 37)
		poll_once();
	box(5, 10, 790, 11, 798, 0x12345678u);
	now += 37;
	poll_once();
	if (!dirty_more)	/* (with DIRTY3, hot[] is not looked at) */
		CHECK(S.hot[3] == 0, "two seconds still, and it is no longer read every scan");
	CHECK(converge(5, 732) >= 0, "and still arrives");
}

/* Changes of mode, as wm makes them: the page follows each one, and a
 * stripe of the mode before, arriving late, does not switch it back. */
static void test_switch(void)
{
	static const int seq[] = { 0, 5, 3, 7, 1, 0, 6, 2, 4, 0 };
	char what[80];

	reset_all(0);
	fill_screen(0, 1);
	CHECK(converge(0, 732) >= 0, "640x480 arrives");
	for (unsigned k = 1; k < sizeof seq / sizeof seq[0]; k++) {
		int m = seq[k];
		fake_mode = m;
		memset(vram, 0, sizeof vram);	/* wm clears and repaints */
		fill_screen(m, 1);
		now += 37;
		int t = converge(m, 732 * 2);
		snprintf(what, sizeof what, "switch to mode %d: the page follows (%d ticks)", m, t);
		CHECK(t >= 0, what);
	}

	/* a stale stripe from the mode before is refused */
	uint8_t pkt[ZSCREEN_PACK_WORST + ZSCREEN_TRAILER];
	S.flags = ZSCREEN_TRAILER_GEOM | 0 << 2 | 5 << 4;	/* 1280x800, window 0 */
	int n = zscreen_encode(&S, 3, pkt, (uint16_t)(frame_seq - 2), 0);
	int before = page_stale_refused;
	page_take(3, pkt, n);
	CHECK(page_stale_refused == before + 1 && page_w == 640, "an older stripe of another mode is refused");
	n = zscreen_encode(&S, 3, pkt, (uint16_t)(frame_seq - 2000), 0);
	page_take(3, pkt, n);
	CHECK(page_w == 1280, "a much older one is a board that started again, and is taken");
}

static void test_640_unchanged(void)
{
	reset_all(0);
	old_format_ok = 1;
	old_format_n = 0;
	fill_screen(0, 1);
	converge(0, 732);
	for (int r = 0; r < 20; r++) {
		box(0, (int)(rnd() % 20), (int)(rnd() % 470), 20, 480, rnd());
		now += 37;
		converge(0, 732);
	}
	zscreen_owe_all(&S);
	zscreen_full(&S, ZSCREEN_FULL_VERIFY);
	now += 37;
	CHECK(poll_once() == 30, "640x480: a periodic resend is all thirty stripes");
	CHECK(old_format_ok && old_format_n > 30,
		"640x480: every stripe is the bytes the encoder made before screen modes");
}

static void test_no_dirty(void)
{
	reset_all(0);
	dirty_present = 0;
	fill_screen(0, 0);
	CHECK(converge(0, 732) >= 0, "no DIRTY: 640x480 arrives");
	vram[5000] ^= 0xffu;		/* no mark: there is no register */
	now += 37;
	poll_once();
	CHECK(page_matches(0), "no DIRTY: window 0 is read whole every scan, as before");
	reset_all(1);
	dirty_present = 0;
	fill_screen(1, 0);
	CHECK(converge(1, 732) >= 0, "no DIRTY: 800x600 arrives");
	dirty_present = 1;
}

static void test_dirty_edges(void)
{
	reset_all(7);			/* 640x360: 23 stripes */
	fill_screen(7, 1);
	converge(7, 732);
	for (int k = 0; k < 10; k++, now += 37)
		poll_once();
	dirty_reg |= 1u << 25;		/* a write past the framebuffer */
	now += 37;
	CHECK(poll_once() == 0, "a DIRTY bit past a small mode's framebuffer sends nothing");
	CHECK(dirty_reg == 0, "and is cleared, so it does not ask for a scan forever");

	reset_all(0);
	fill_screen(0, 1);
	converge(0, 732);
	vram[4 * 320 + 7] ^= 1;		/* changed behind DIRTY's back */
	zscreen_full(&S, ZSCREEN_FULL_VERIFY);
	uint32_t missed = 0;
	zscreen_scan(&S, now, &missed);
	CHECK(missed == 1u << 4 && S.verify_missed == 1 && S.verify_passes == 1,
		"a verify counts a stripe that changed with no DIRTY bit");

	/* a verify pending when the mode changes is not a verify: every
	 * stripe is new after the change, and none of them is a miss */
	zscreen_full(&S, ZSCREEN_FULL_VERIFY);
	fake_mode = 5;		/* scanned before wm repaints: nothing marked */
	for (int k = 0; k < 4; k++) {
		now += 37;
		zscreen_scan(&S, now, &missed);
	}
	CHECK(S.verify_missed == 1,
		"a change of mode does not count its first read as misses");

	/* the verify asked for after the change, before window 0's first
	 * read (zerdesk's 5 s timer can land there) */
	/* zerdesk: a new viewer resets, the first scan reads window 1, and
	 * the 5 s verify is asked for before window 0 is read again */
	for (int k = 0; k < 4; k++) {
		now += 37;
		zscreen_scan(&S, now, &missed);
	}
	zscreen_full(&S, ZSCREEN_FULL_RESET);
	S.win = 0;
	now += 37;
	zscreen_scan(&S, now, &missed);
	CHECK(S.win == 1, "(the scan after the reset read window 1)");
	zscreen_full(&S, ZSCREEN_FULL_VERIFY);
	for (int k = 0; k < 4; k++) {
		now += 37;
		zscreen_scan(&S, now, &missed);
	}
	CHECK(S.verify_missed == 1, "nor a verify asked for before window 0's first read");
	fake_mode = 0;
	for (int k = 0; k < 3; k++) {
		now += 37;
		zscreen_scan(&S, now, &missed);
	}
	/* a stripe changed behind DIRTY's back still counts */
	vram[2 * 320] ^= 4;
	zscreen_full(&S, ZSCREEN_FULL_VERIFY);
	now += 37;
	zscreen_scan(&S, now, &missed);
	CHECK(missed == 1u << 2 && S.verify_missed == 2, "and a real miss still counts");
}

/* Owed stripes in a later window are read again: the slot has held
 * other windows' stripes since. */
static void test_owed(void)
{
	reset_all(5);
	fill_screen(5, 1);
	converge(5, 732);
	for (int k = 0; k < 30; k++, now += 37)
		poll_once();
	/* the page loses everything (a viewer that just connected) */
	memset(page_fb, 0, sizeof page_fb);
	zscreen_owe_all(&S);
	int sends = 0;
	for (int k = 0; k < 4; k++, now += 37)
		sends += poll_once();
	CHECK(sends == 100, "owed: every stripe of every window, once");
	CHECK(page_matches(5), "owed: and they are the screen");

	zscreen_owe(&S, 2, 1u << 7);	/* the drag cut in screen_poll */
	vram[(67 * 320) + 3] ^= 0x10;	/* stripe 67 = window 2, slot 7, no DIRTY */
	now += 37;
	poll_once();
	poll_once();
	CHECK(page_matches(5), "an owed stripe of a later window is read again, not sent stale");
}

/* With DIRTY1..3, a window past 0 is read as window 0 is: what its
 * register names, and nothing else. */
static void test_more(void)
{
	uint32_t missed = 0;

	dirty_more = 1;
	reset_all(5);			/* 1280x800: four windows */
	fill_screen(5, 1);
	CHECK(converge(5, 732) >= 0, "1280x800 arrives");
	CHECK(S.tracked == 4, "DIRTY's bit 31: four windows have a register");
	for (int k = 0; k < 10; k++, now += 37)
		poll_once();

	/* nothing is read whole any more: a change behind DIRTY2's back is
	 * not seen in four untracked periods... */
	vram[67 * 320 + 3] ^= 0x100;	/* stripe 67: window 2, slot 7 */
	int sends = 0;
	for (int k = 0; k < 4 * ZSCREEN_UNTRACKED_TICKS / 37; k++, now += 37)
		sends += poll_once();
	CHECK(sends == 0 && !page_matches(5),
		"a still screen reads nothing whole: an unmarked change stays unseen");
	/* ...until a verify, which counts it as a miss of window 2 */
	zscreen_owe_all(&S);
	zscreen_full(&S, ZSCREEN_FULL_VERIFY);
	uint32_t passes = S.verify_passes, misses = S.verify_missed;
	int saw = 0;
	for (int k = 0; k < 4; k++, now += 37) {
		zscreen_scan(&S, now, &missed);
		if (S.win == 2)
			saw = missed == 1u << 7;
	}
	CHECK(saw && S.verify_missed == misses + 1,
		"a verify counts the miss, in window 2's slot 7");
	CHECK(S.verify_passes == passes + 4, "one verify pass for each window");
	box(5, 0, 300, 40, 700, 0x3c3c3c3cu);	/* windows 1..3, marked */
	for (int k = 0; k < 8; k++, now += 37)
		poll_once();
	CHECK(S.verify_passes == passes + 4 && page_matches(5),
		"and the scans after it are not verifies");

	/* a key after a pause, in window 2: the next scan */
	reset_all(5);
	fill_screen(5, 1);
	converge(5, 732);
	for (int k = 0; k < 80; k++, now += 37)	/* three seconds still */
		poll_once();
	box(5, 20, 500, 21, 508, 0x0f0f0f0fu);	/* rows 500..507: window 2 */
	now += 37;
	poll_once();
	CHECK(page_matches(5), "a key in window 2 after a pause shows on the next scan");

	/* 800x600: window 1 is 17 stripes; a bit past them sends nothing
	 * and is cleared, and DIRTY2 and DIRTY3 are never read */
	reset_all(1);
	fill_screen(1, 1);
	converge(1, 732);
	for (int k = 0; k < 10; k++, now += 37)
		poll_once();
	dirty_regs[1] |= 1u << 20;
	int r2 = dirty_reads[2], r3 = dirty_reads[3];
	now += 37;
	CHECK(poll_once() == 0 && dirty_regs[1] == 0,
		"a DIRTY1 bit past 800x600's stripes sends nothing and is cleared");
	for (int k = 0; k < 20; k++, now += 37)
		poll_once();
	CHECK(dirty_reads[2] == r2 && dirty_reads[3] == r3 && dirty_regs[2] != 0,
		"a window the mode does not have: its register is left alone");
	dirty_more = 0;
}

/* Without DIRTY's bit 31 (any older bitstream, where words 21..23
 * alias other registers), DIRTY1..3 are never touched. */
static void test_more_absent(void)
{
	dirty_more = 0;
	memset(dirty_reads, 0, sizeof dirty_reads);
	memset(dirty_clears, 0, sizeof dirty_clears);
	reset_all(5);
	fill_screen(5, 1);
	CHECK(converge(5, 732) >= 0, "no bit 31: 1280x800 arrives");
	CHECK(S.tracked == 1, "no bit 31: only window 0 has a register");
	for (int k = 0; k < 40; k++, now += 37)
		poll_once();
	box(5, 0, 790, 40, 800, 0x55555555u);
	now += 37;
	CHECK(converge(5, 732) >= 0, "no bit 31: a change in window 3 arrives, as before");
	zscreen_full(&S, ZSCREEN_FULL_VERIFY);
	for (int k = 0; k < 8; k++, now += 37)
		poll_once();
	CHECK(dirty_reads[0] > 0 && dirty_clears[0] > 0, "(DIRTY is used)");
	CHECK(!dirty_reads[1] && !dirty_reads[2] && !dirty_reads[3] &&
		!dirty_clears[1] && !dirty_clears[2] && !dirty_clears[3],
		"no bit 31: words 21..23 are never read or written");
	reset_all(0);
	dirty_present = 0;
	dirty_more = 1;			/* nonsense without DIRTY: ignored */
	fill_screen(0, 0);
	converge(0, 732);
	CHECK(S.tracked == 0, "no DIRTY at all: no window has a register");
	dirty_present = 1;
	dirty_more = 0;
}

int main(void)
{
	for (dirty_more = 0; dirty_more < 2; dirty_more++) {
		test_each_mode();
		test_hot();
		test_switch();
		test_640_unchanged();
		test_no_dirty();
		test_dirty_edges();
		test_owed();
	}
	test_more();
	test_more_absent();
	printf("%d checks, %d failed\n", run, failed);
	return failed ? 1 : 0;
}
