/*
 * chip8 -- core unit tests. Host build, no hardware, no framebuffer.
 *
 *   cc -std=gnu99 -Wall -Wextra -o /tmp/chip8_test_core \
 *      sw/apps/chip8/tests/test_core.c \
 *      sw/apps/chip8/core.c sw/apps/chip8/quirks.c
 *
 * or just `make -C sw/apps/chip8 test`.
 *
 * -- what this is for --
 *
 * Nearly every CHIP-8 compatibility problem is a one-line behavioural
 * difference that produces a game which RUNS and looks WRONG. There is
 * no crash to catch and no error to report, so the only way to know is
 * to state the expected behaviour somewhere a machine can check it.
 * That is this file.
 *
 * Tests are written against the quirk fields by name, so a failure
 * names the divergence rather than the symptom.
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "../core.h"

static int failures = 0;
static int checks = 0;

#define CHECK(cond) do { \
	checks++; \
	if (!(cond)) { \
		printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
		failures++; \
	} \
} while (0)

#define CHECK_EQ(got, want) do { \
	long g_ = (long)(got), w_ = (long)(want); \
	checks++; \
	if (g_ != w_) { \
		printf("FAIL %s:%d: %s == %ld, wanted %ld\n", \
			__FILE__, __LINE__, #got, g_, w_); \
		failures++; \
	} \
} while (0)

/* -- helpers -------------------------------------------------------- */

/* Build a machine running a short program at 0x200. The program is
 * given as 16-bit opcodes, which is how CHIP-8 source is written and
 * read; splitting into bytes at the call site is exactly the sort of
 * transcription error a test file should not be able to make. */
static void setup(c8_t *c, c8_profile_t prof, const uint16_t *ops, int n) {
	int k;
	uint8_t rom[512];
	c8_init(c, c8_profile(prof), 1);
	for (k = 0; k < n; k++) {
		rom[k * 2]     = (uint8_t)(ops[k] >> 8);
		rom[k * 2 + 1] = (uint8_t)(ops[k] & 0xFF);
	}
	CHECK(c8_load(c, rom, (uint32_t)n * 2));
}

static void run(c8_t *c, int n) {
	int k;
	for (k = 0; k < n; k++) {
		c8_status_t st = c8_step(c);
		if (st == C8_BAD_OPCODE) {
			printf("FAIL: bad opcode %04X at %04X\n", c->bad_op, c->pc);
			failures++;
			return;
		}
		if (st != C8_OK) return;
	}
}

/* Set one pixel in a plane directly, so a test can build a starting
 * image without going through DXYN. */
static void poke(c8_t *c, int p, int x, int y) {
	c->px[p][y][x >> 5] |= (uint32_t)1 << (31 - (x & 31));
}

/* -- tests ---------------------------------------------------------- */

static void test_reset(void) {

	c8_t c;
	c8_init(&c, c8_profile(C8_PROFILE_CHIP8), 0);

	CHECK_EQ(c.pc, C8_START_ADDR);
	CHECK_EQ(c.sp, 0);
	CHECK_EQ(c.w, 64);
	CHECK_EQ(c.h, 32);
	CHECK_EQ(c.plane_mask, 1);
	CHECK_EQ(c.pitch, C8_PITCH_DEFAULT);
	CHECK(!c.hires);

	/* The font must be in RAM, not in a table consulted at FX29 time:
	 * programs read the glyph bytes directly. Digit 0 is F0 90 90 90
	 * F0 in every implementation there has ever been. */
	CHECK_EQ(c.ram[C8_FONT_ADDR + 0], 0xF0);
	CHECK_EQ(c.ram[C8_FONT_ADDR + 1], 0x90);
	CHECK_EQ(c.ram[C8_FONT_ADDR + 4], 0xF0);

	/* A zero seed must not leave the RNG stuck. */
	CHECK(c.rng != 0);

}

static void test_alu(void) {

	c8_t c;
	static const uint16_t ops[] = {
		0x60FF,   /* v0 := 0xFF */
		0x6102,   /* v1 := 0x02 */
		0x8014,   /* v0 += v1  -> 0x01, VF = 1 */
		0x6205,   /* v2 := 5 */
		0x6307,   /* v3 := 7 */
		0x8235,   /* v2 -= v3  -> 0xFE, VF = 0 */
		0x8327    /* v3 =- v2  ... v3 := v2 - v3 = 0xF7, VF = 1 */
	};

	setup(&c, C8_PROFILE_CHIP8, ops, 7);
	run(&c, 3);
	CHECK_EQ(c.v[0], 0x01);
	CHECK_EQ(c.v[0xF], 1);

	run(&c, 3);
	CHECK_EQ(c.v[2], 0xFE);
	CHECK_EQ(c.v[0xF], 0);

	run(&c, 1);
	CHECK_EQ(c.v[3], (uint8_t)(0xFE - 0x07));
	CHECK_EQ(c.v[0xF], 1);

}

/* 8XY4 with X == F: the sum is written first and then overwritten by
 * the flag, so VF ends up as the carry, not as the sum. Programs do
 * rely on this. */
static void test_alu_vf_destination(void) {

	c8_t c;
	static const uint16_t ops[] = { 0x6FFF, 0x6102, 0x8F14 };

	setup(&c, C8_PROFILE_CHIP8, ops, 3);
	run(&c, 3);
	CHECK_EQ(c.v[0xF], 1);

}

static void test_vf_reset_quirk(void) {

	c8_t c;
	static const uint16_t ops[] = { 0x6FFF, 0x6003, 0x6105, 0x8011 };

	setup(&c, C8_PROFILE_CHIP8, ops, 4);
	run(&c, 4);
	CHECK_EQ(c.v[0], 0x07);
	CHECK_EQ(c.v[0xF], 0);          /* vf_reset on */

	setup(&c, C8_PROFILE_SCHIP, ops, 4);
	run(&c, 4);
	CHECK_EQ(c.v[0], 0x07);
	CHECK_EQ(c.v[0xF], 0xFF);       /* vf_reset off: VF untouched */

}

static void test_shift_quirk(void) {

	c8_t c;
	static const uint16_t ops[] = { 0x6000, 0x610F, 0x8016 };

	/* CHIP-8: shift VY into VX, so v0 becomes 0x0F >> 1. */
	setup(&c, C8_PROFILE_CHIP8, ops, 3);
	run(&c, 3);
	CHECK_EQ(c.v[0], 0x07);
	CHECK_EQ(c.v[0xF], 1);

	/* SUPER-CHIP: shift VX in place, and v0 is 0. */
	setup(&c, C8_PROFILE_SCHIP, ops, 3);
	run(&c, 3);
	CHECK_EQ(c.v[0], 0x00);
	CHECK_EQ(c.v[0xF], 0);

}

static void test_mem_inc_quirk(void) {

	c8_t c;
	static const uint16_t ops[] = { 0xA300, 0x6001, 0x6102, 0x6203, 0xF255 };

	setup(&c, C8_PROFILE_CHIP8, ops, 5);
	run(&c, 5);
	CHECK_EQ(c.i, 0x303);           /* I + X + 1 */
	CHECK_EQ(c.ram[0x300], 1);
	CHECK_EQ(c.ram[0x302], 3);

	setup(&c, C8_PROFILE_SCHIP, ops, 5);
	run(&c, 5);
	CHECK_EQ(c.i, 0x300);           /* unchanged on SUPER-CHIP 1.1 */

	setup(&c, C8_PROFILE_XOCHIP, ops, 5);
	run(&c, 5);
	CHECK_EQ(c.i, 0x303);

}

static void test_jump_quirk(void) {

	c8_t c;
	/* B300 selects V3 under the BXNN reading -- X is the same nibble
	 * that is part of the address under the BNNN reading, which is
	 * exactly why the two are indistinguishable without knowing which
	 * interpreter a ROM was written for. */
	static const uint16_t ops[] = { 0x6310, 0xB300 };

	/* BNNN: NNN + V0, and V0 is 0 here. */
	setup(&c, C8_PROFILE_CHIP8, ops, 2);
	run(&c, 2);
	CHECK_EQ(c.pc, 0x300);

	/* BXNN: 0x300 + V3. */
	setup(&c, C8_PROFILE_SCHIP, ops, 2);
	run(&c, 2);
	CHECK_EQ(c.pc, 0x310);

}

static void test_bcd_and_font(void) {

	c8_t c;
	static const uint16_t ops[] = { 0x60FF, 0xA400, 0xF033, 0xF029, 0x6109, 0xF130 };

	setup(&c, C8_PROFILE_SCHIP, ops, 6);
	run(&c, 3);
	CHECK_EQ(c.ram[0x400], 2);
	CHECK_EQ(c.ram[0x401], 5);
	CHECK_EQ(c.ram[0x402], 5);

	run(&c, 1);
	CHECK_EQ(c.i, C8_FONT_ADDR + 15 * 5);   /* v0 is 0xFF, low nibble F */

	run(&c, 2);
	CHECK_EQ(c.i, C8_FONT_BIG_ADDR + 9 * 10);

}

static void test_call_return(void) {

	c8_t c;
	static const uint16_t ops[] = { 0x2206, 0x1FFF, 0x0000, 0x00EE };

	setup(&c, C8_PROFILE_CHIP8, ops, 4);
	run(&c, 1);
	CHECK_EQ(c.pc, 0x206);
	CHECK_EQ(c.sp, 1);
	run(&c, 1);
	CHECK_EQ(c.pc, 0x202);
	CHECK_EQ(c.sp, 0);

}

/* Returning with an empty stack is an error, not a wild jump into
 * whatever stack[-1] happened to contain. */
static void test_return_underflow(void) {

	c8_t c;
	static const uint16_t ops[] = { 0x00EE };

	setup(&c, C8_PROFILE_CHIP8, ops, 1);
	CHECK_EQ(c8_step(&c), C8_BAD_OPCODE);
	CHECK_EQ(c.pc, 0x200);

}

/* Likewise a call 17 deep, rather than writing past the stack array. */
static void test_call_overflow(void) {

	c8_t c;
	static const uint16_t ops[] = { 0x2200 };   /* calls itself */
	int k;

	setup(&c, C8_PROFILE_CHIP8, ops, 1);
	for (k = 0; k < C8_STACK_SIZE; k++)
		CHECK_EQ(c8_step(&c), C8_OK);
	CHECK_EQ(c8_step(&c), C8_BAD_OPCODE);

}

static void test_draw_and_collision(void) {

	c8_t c;
	/* A one-row sprite of 0xFF at (0,0), drawn twice. */
	static const uint16_t ops[] = {
		0xA300, 0x6000, 0x6100, 0xD011, 0xD011
	};

	setup(&c, C8_PROFILE_XOCHIP, ops, 5);
	c.ram[0x300] = 0xFF;

	run(&c, 4);
	CHECK_EQ(c.px[0][0][0], 0xFF000000u);
	CHECK_EQ(c.v[0xF], 0);
	CHECK(c.dirty & 1);

	/* XOR is its own inverse: the second draw erases and sets VF. */
	run(&c, 1);
	CHECK_EQ(c.px[0][0][0], 0u);
	CHECK_EQ(c.v[0xF], 1);

}

/* A sprite at an arbitrary column must land on that column, including
 * across a 32-bit word boundary. This is the case the 64-bit window in
 * row_xor() exists for. */
static void test_draw_unaligned(void) {

	c8_t c;
	static const uint16_t ops[] = { 0xA300, 0x601C, 0x6100, 0xD011 };

	setup(&c, C8_PROFILE_XOCHIP, ops, 4);
	c.ram[0x300] = 0xFF;
	run(&c, 4);

	/* Columns 28..35: bottom 4 bits of word 0, top 4 of word 1. */
	CHECK_EQ(c.px[0][0][0], 0x0000000Fu);
	CHECK_EQ(c.px[0][0][1], 0xF0000000u);

}

static void test_clip_vs_wrap(void) {

	c8_t c;
	static const uint16_t ops[] = { 0xA300, 0x603C, 0x6100, 0xD011 };

	/* x = 60, 8-wide sprite, lores width 64: four pixels hang off. */

	/* CHIP-8 clips: only columns 60..63 are drawn. */
	setup(&c, C8_PROFILE_CHIP8, ops, 4);
	c.ram[0x300] = 0xFF;
	run(&c, 4);
	CHECK_EQ(c.px[0][0][1], 0x0000000Fu);
	CHECK_EQ(c.px[0][0][0], 0u);

	/* XO-CHIP wraps: the overhang reappears at columns 0..3. */
	setup(&c, C8_PROFILE_XOCHIP, ops, 4);
	c.ram[0x300] = 0xFF;
	run(&c, 4);
	CHECK_EQ(c.px[0][0][1], 0x0000000Fu);
	CHECK_EQ(c.px[0][0][0], 0xF0000000u);

}

static void test_clip_vertical(void) {

	c8_t c;
	/* Four-row sprite starting two rows from the bottom of lores. */
	static const uint16_t ops[] = { 0xA300, 0x6000, 0x611E, 0xD014 };

	setup(&c, C8_PROFILE_CHIP8, ops, 4);
	memset(c.ram + 0x300, 0xFF, 4);
	run(&c, 4);
	CHECK_EQ(c.px[0][30][0], 0xFF000000u);
	CHECK_EQ(c.px[0][31][0], 0xFF000000u);
	CHECK_EQ(c.px[0][0][0], 0u);            /* clipped, not wrapped */

	setup(&c, C8_PROFILE_XOCHIP, ops, 4);
	memset(c.ram + 0x300, 0xFF, 4);
	run(&c, 4);
	CHECK_EQ(c.px[0][0][0], 0xFF000000u);   /* wrapped to the top */
	CHECK_EQ(c.px[0][1][0], 0xFF000000u);

}

static void test_dxy0(void) {

	c8_t c;
	static const uint16_t ops[] = { 0xA300, 0x6000, 0x6100, 0xD010 };

	/* CHIP-8 has no DXY0: nothing is drawn. */
	setup(&c, C8_PROFILE_CHIP8, ops, 4);
	memset(c.ram + 0x300, 0xFF, 32);
	run(&c, 4);
	CHECK_EQ(c.px[0][0][0], 0u);

	/* SUPER-CHIP draws 16x16 even in lores. */
	setup(&c, C8_PROFILE_SCHIP, ops, 4);
	memset(c.ram + 0x300, 0xFF, 32);
	run(&c, 4);
	CHECK_EQ(c.px[0][0][0], 0xFFFF0000u);
	CHECK_EQ(c.px[0][15][0], 0xFFFF0000u);

}

static void test_hires_mode(void) {

	c8_t c;
	static const uint16_t ops[] = { 0x00FF, 0x00FE };

	setup(&c, C8_PROFILE_SCHIP, ops, 2);
	run(&c, 1);
	CHECK(c.hires);
	CHECK_EQ(c.w, 128);
	CHECK_EQ(c.h, 64);
	run(&c, 1);
	CHECK(!c.hires);
	CHECK_EQ(c.w, 64);

	/* XO-CHIP clears the display on a mode switch; SUPER-CHIP does not. */
	setup(&c, C8_PROFILE_SCHIP, ops, 2);
	poke(&c, 0, 1, 1);
	run(&c, 1);
	CHECK(c.px[0][1][0] != 0);

	setup(&c, C8_PROFILE_XOCHIP, ops, 2);
	poke(&c, 0, 1, 1);
	run(&c, 1);
	CHECK_EQ(c.px[0][1][0], 0u);

}

static void test_scroll(void) {

	c8_t c;
	static const uint16_t ops[] = { 0x00C4, 0x00FB, 0x00FC, 0x00D4 };

	/* XO-CHIP scrolls in current-mode pixels: 4 means 4 lores rows. */
	setup(&c, C8_PROFILE_XOCHIP, ops, 4);
	poke(&c, 0, 0, 0);
	run(&c, 1);
	CHECK_EQ(c.px[0][0][0], 0u);
	CHECK_EQ(c.px[0][4][0], 0x80000000u);

	/* SUPER-CHIP scrolls in hires pixels whatever the mode, so 4 is
	 * two lores rows. */
	setup(&c, C8_PROFILE_SCHIP, ops, 4);
	poke(&c, 0, 0, 0);
	run(&c, 1);
	CHECK_EQ(c.px[0][2][0], 0x80000000u);

	/* Right 4, then left 4, returns the image exactly. */
	setup(&c, C8_PROFILE_XOCHIP, ops, 4);
	poke(&c, 0, 10, 3);
	run(&c, 1);                     /* consume the SCD */
	run(&c, 1);
	CHECK_EQ(c.px[0][7][0], (uint32_t)1 << (31 - 14));
	run(&c, 1);
	CHECK_EQ(c.px[0][7][0], (uint32_t)1 << (31 - 10));

	/* Scroll up puts it back where it started. */
	run(&c, 1);
	CHECK_EQ(c.px[0][3][0], (uint32_t)1 << (31 - 10));

}

static void test_planes(void) {

	c8_t c;
	static const uint16_t ops[] = {
		0xF201,   /* plane 2 -- plane 1 only */
		0xA300, 0x6000, 0x6100, 0xD011,
		0xF301,   /* plane 3 -- both */
		0xD012
	};

	setup(&c, C8_PROFILE_XOCHIP, ops, 7);
	c.ram[0x300] = 0xFF;
	c.ram[0x301] = 0x0F;
	c.ram[0x302] = 0xF0;
	c.ram[0x303] = 0x33;

	run(&c, 5);
	CHECK_EQ(c.px[1][0][0], 0xFF000000u);
	CHECK_EQ(c.px[0][0][0], 0u);            /* plane 0 untouched */

	/* With both planes selected, a 2-row sprite consumes FOUR bytes:
	 * rows 0-1 for plane 0, rows 2-3 for plane 1. That is the whole
	 * of XO-CHIP's plane model and the easiest part to get wrong. */
	run(&c, 2);
	CHECK_EQ(c.px[0][0][0], 0xFF000000u);
	CHECK_EQ(c.px[0][1][0], 0x0F000000u);
	CHECK_EQ(c.px[1][0][0], 0xFF000000u ^ 0xF0000000u);
	CHECK_EQ(c.px[1][1][0], 0x33000000u);

}

static void test_cls_selected_planes_only(void) {

	c8_t c;
	static const uint16_t ops[] = { 0xF101, 0x00E0 };

	setup(&c, C8_PROFILE_XOCHIP, ops, 2);
	poke(&c, 0, 5, 5);
	poke(&c, 1, 5, 5);
	run(&c, 2);
	CHECK_EQ(c.px[0][5][0], 0u);
	CHECK(c.px[1][5][0] != 0);

}

static void test_long_load_and_skip(void) {

	c8_t c;
	static const uint16_t ops[] = {
		0xF000, 0x1234,     /* i := 0x1234 */
		0x6005,             /* v0 := 5 */
		0x3005,             /* skip if v0 == 5 -- must skip FOUR bytes */
		0xF000, 0xABCD,     /* the skipped long load */
		0x6142              /* v1 := 0x42 */
	};

	setup(&c, C8_PROFILE_XOCHIP, ops, 7);
	run(&c, 1);
	CHECK_EQ(c.i, 0x1234);
	CHECK_EQ(c.pc, 0x204);

	run(&c, 2);
	CHECK_EQ(c.pc, 0x20C);          /* skipped past the 4-byte literal */
	run(&c, 1);
	CHECK_EQ(c.v[1], 0x42);
	CHECK_EQ(c.i, 0x1234);          /* the skipped load did not happen */

}

static void test_save_load_range(void) {

	c8_t c;
	static const uint16_t ops[] = {
		0x6011, 0x6122, 0x6233, 0xA400, 0x5022,   /* save v0..v2 */
		0x6A00, 0x6B00, 0x6C00, 0xA400, 0x5AC3    /* load vA..vC */
	};

	setup(&c, C8_PROFILE_XOCHIP, ops, 10);
	run(&c, 5);
	CHECK_EQ(c.ram[0x400], 0x11);
	CHECK_EQ(c.ram[0x402], 0x33);

	run(&c, 5);
	CHECK_EQ(c.v[0xA], 0x11);
	CHECK_EQ(c.v[0xC], 0x33);

	/* I is not advanced by 5XY2/5XY3, unlike FX55/FX65. */
	CHECK_EQ(c.i, 0x400);

}

static void test_wait_key(void) {

	c8_t c;
	static const uint16_t ops[] = { 0xF00A, 0x6142 };

	setup(&c, C8_PROFILE_CHIP8, ops, 2);

	CHECK_EQ(c8_step(&c), C8_WAIT_KEY);
	CHECK(c8_blocked(&c));

	/* A press alone must not satisfy it -- the key is delivered on
	 * release, so one held key cannot answer two questions. */
	c8_key(&c, 7, true);
	CHECK_EQ(c8_step(&c), C8_WAIT_KEY);

	c8_key(&c, 7, false);
	CHECK(!c.waiting);
	CHECK_EQ(c.v[0], 7);

	run(&c, 1);
	CHECK_EQ(c.v[1], 0x42);

}

static void test_key_skips(void) {

	c8_t c;
	static const uint16_t ops[] = { 0x6003, 0xE09E, 0x6142, 0x6243 };

	setup(&c, C8_PROFILE_CHIP8, ops, 4);
	c8_key(&c, 3, true);
	run(&c, 2);
	CHECK_EQ(c.pc, 0x206);          /* skipped v1 := 0x42 */

	setup(&c, C8_PROFILE_CHIP8, ops, 4);
	run(&c, 2);
	CHECK_EQ(c.pc, 0x204);          /* not held: no skip */

}

static void test_display_wait(void) {

	c8_t c;
	static const uint16_t ops[] = { 0xA300, 0x6000, 0x6100, 0xD011, 0x6942 };

	setup(&c, C8_PROFILE_CHIP8, ops, 5);
	c.ram[0x300] = 0xFF;

	run(&c, 3);
	CHECK_EQ(c8_step(&c), C8_WAIT_FRAME);
	CHECK(c.wait_frame);

	/* Nothing else runs until the frame ticks over -- which is what
	 * caps VIP-era CHIP-8 at one sprite per frame and is the reason
	 * those games do not flicker. */
	CHECK_EQ(c8_step(&c), C8_WAIT_FRAME);
	CHECK_EQ(c.v[9], 0);

	c8_frame(&c);
	run(&c, 1);
	CHECK_EQ(c.v[9], 0x42);

	/* SUPER-CHIP does not wait. */
	setup(&c, C8_PROFILE_SCHIP, ops, 5);
	c.ram[0x300] = 0xFF;
	run(&c, 5);
	CHECK_EQ(c.v[9], 0x42);

}

static void test_timers(void) {

	c8_t c;
	static const uint16_t ops[] = { 0x6003, 0xF015, 0xF107 };

	setup(&c, C8_PROFILE_CHIP8, ops, 3);
	run(&c, 2);
	CHECK_EQ(c.dt, 3);
	c8_tick_timers(&c);
	c8_tick_timers(&c);
	run(&c, 1);
	CHECK_EQ(c.v[1], 1);

	c8_tick_timers(&c);
	c8_tick_timers(&c);
	CHECK_EQ(c.dt, 0);              /* must not underflow */

}

static void test_audio_state(void) {

	c8_t c;
	static const uint16_t ops[] = { 0xA300, 0xF002, 0x6055, 0xF03A };
	uint32_t gen;

	setup(&c, C8_PROFILE_XOCHIP, ops, 4);
	memset(c.ram + 0x300, 0xA5, C8_PATTERN_BYTES);

	gen = c.audio_gen;
	run(&c, 2);
	CHECK_EQ(c.pattern[0], 0xA5);
	CHECK_EQ(c.pattern[15], 0xA5);
	CHECK(c.audio_gen != gen);

	gen = c.audio_gen;
	run(&c, 2);
	CHECK_EQ(c.pitch, 0x55);
	CHECK(c.audio_gen != gen);

}

static void test_xochip_memory(void) {

	c8_t c;
	static const uint16_t ops[] = { 0xF000, 0x2000, 0x60AB, 0xF055 };

	setup(&c, C8_PROFILE_XOCHIP, ops, 4);
	run(&c, 3);
	CHECK_EQ(c.ram[0x2000], 0xAB);

	/* The same program under CHIP-8 addressing must fold back into
	 * 4KB rather than reaching memory a VIP never had. */
	setup(&c, C8_PROFILE_CHIP8, ops, 4);
	run(&c, 3);
	CHECK_EQ(c.ram[0x000], 0xAB);
	CHECK_EQ(c.ram[0x2000], 0);

}

static void test_bad_opcode_reports_address(void) {

	c8_t c;
	static const uint16_t ops[] = { 0x6001, 0x8FFF };

	setup(&c, C8_PROFILE_CHIP8, ops, 2);
	run(&c, 1);
	CHECK_EQ(c8_step(&c), C8_BAD_OPCODE);
	CHECK_EQ(c.bad_op, 0x8FFF);
	CHECK_EQ(c.pc, 0x202);          /* rewound to the bad instruction */

}

static void test_run_budget(void) {

	c8_t c;
	static const uint16_t ops[] = { 0x6001, 0x6102, 0x6203, 0x6304 };
	int done = 0;

	setup(&c, C8_PROFILE_CHIP8, ops, 4);
	CHECK_EQ(c8_run(&c, 3, &done), C8_OK);
	CHECK_EQ(done, 3);
	CHECK_EQ(c.v[2], 3);

}

int main(void) {

	test_reset();
	test_alu();
	test_alu_vf_destination();
	test_vf_reset_quirk();
	test_shift_quirk();
	test_mem_inc_quirk();
	test_jump_quirk();
	test_bcd_and_font();
	test_call_return();
	test_return_underflow();
	test_call_overflow();
	test_draw_and_collision();
	test_draw_unaligned();
	test_clip_vs_wrap();
	test_clip_vertical();
	test_dxy0();
	test_hires_mode();
	test_scroll();
	test_planes();
	test_cls_selected_planes_only();
	test_long_load_and_skip();
	test_save_load_range();
	test_wait_key();
	test_key_skips();
	test_display_wait();
	test_timers();
	test_audio_state();
	test_xochip_memory();
	test_bad_opcode_reports_address();
	test_run_budget();

	printf("%s: %d checks, %d failures\n",
		failures ? "FAILED" : "ok", checks, failures);

	return failures ? 1 : 0;

}
