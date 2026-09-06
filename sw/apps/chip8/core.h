#ifndef CHIP8_CORE_H
#define CHIP8_CORE_H

/*
 * chip8 -- interpreter core.
 *
 * CHIP-8, SUPER-CHIP 1.1 and XO-CHIP, one interpreter, selected by a
 * quirk profile rather than by three code paths.
 *
 * -- this file depends on nothing --
 *
 * No zeitlos.h, no zgfx.h, no window, no framebuffer, no timers, no
 * filesystem. Plain C99 over two arrays. That is deliberate and it is
 * the single most useful property the emulator has: the whole of the
 * guest machine can be compiled and tested on a host in a second,
 * where a wrong answer is a failing assert on a build machine instead
 * of a game that looks subtly wrong on a TV.
 *
 * The division of labour is therefore:
 *
 *   core.c     the guest machine. No I/O of any kind.
 *   render.c   planes -> framebuffer. Knows about zgfx, not about opcodes.
 *   chip8.c    window, input, sound, pacing. Knows about the OS.
 *
 * Everything below is guest state. Nothing in it is a pointer, so a
 * c8_t can be memcpy'd, snapshotted for save states, or stack
 * allocated in a test.
 *
 * -- the display is 128x64 of BITS, always --
 *
 * Not 64x32 scaled up. Lores and hires are both stored at their own
 * logical size in the same array, with `w`/`h` saying which is in
 * force, because that is what the guest's own coordinate arithmetic
 * uses: a lores DXYN wraps at column 64, and storing lores pre-doubled
 * would put the wrap in the wrong place.
 *
 * Rows are MSB-first: pixel column c is bit (31 - (c & 31)) of
 * px[plane][row][c >> 5]. MSB-first because sprite data is MSB-first,
 * so placing a sprite row is one shift rather than a bit reversal.
 *
 * NOTE this is the opposite of the framebuffer's own convention
 * (zgfx.h: "least significant bit leftmost"). That costs nothing,
 * because the renderer never copies a plane row verbatim -- it is
 * always scaling, so it is always going through a lookup table, and a
 * table can be built either way round for the same price.
 */

#include <stdint.h>
#include <stdbool.h>

/* Guest RAM. 4KB is CHIP-8 and SUPER-CHIP; XO-CHIP extends the
 * address space to 16 bits. One allocation of the larger size, with
 * `addr_mask` deciding what the guest can actually reach -- so a
 * CHIP-8 ROM that runs off the end wraps at 0x1000 exactly as it did
 * on a VIP, rather than reading XO-CHIP memory that a real CHIP-8
 * program could never have seen. */
#define C8_RAM_SIZE   65536
#define C8_RAM_CHIP8  4096

#define C8_START_ADDR 0x200

/* Largest ROM each address space can hold: the space, minus the 512
 * bytes below the load address.
 *
 * 3584 is the theoretical 4K figure. A real COSMAC VIP had less --
 * the interpreter kept its variables, stack and display refresh buffer
 * at 0xEA0-0xFFF, leaving a program 0x200-0xE9F, or 3232 bytes.
 * SUPER-CHIP on the HP48 does not put the display in guest memory and
 * does get the full 3584.
 *
 * The useful consequence is a hard classification rule: a ROM larger
 * than C8_ROM_MAX_4K CANNOT be CHIP-8 or SUPER-CHIP, whatever it is
 * called. See c8_profile_hint() in config.h. */
#define C8_ROM_MAX_4K   3584u
#define C8_ROM_MAX_64K  65024u

/* Small font at 0x000 (16 glyphs x 5 bytes), big font at 0x050
 * (16 glyphs x 10 bytes). SUPER-CHIP only ever defined big glyphs for
 * 0-9; 10-15 are provided anyway because XO-CHIP programs use them and
 * an interpreter that has them is a superset of one that doesn't. */
#define C8_FONT_ADDR      0x000
#define C8_FONT_BIG_ADDR  0x050

#define C8_STACK_SIZE 16
#define C8_PLANES     2
#define C8_ROWS       64
#define C8_ROW_WORDS  4        /* 128 bits */

/* XO-CHIP audio: 16 bytes of pattern, played back as 128 1-bit
 * samples. `pitch` is the FX3A value; playback rate is
 * 4000 * 2^((pitch - 64) / 48) Hz. Kept here as raw guest state --
 * turning it into something the mixer can play is chip8.c's problem. */
#define C8_PATTERN_BYTES 16
#define C8_PITCH_DEFAULT 64

/* SUPER-CHIP RPL user flags. 8 on real SUPER-CHIP hardware, 16 under
 * XO-CHIP. `flag_count` enforces whichever applies. */
#define C8_FLAGS 16

/* -- quirks ---------------------------------------------------------
 *
 * Every field here is a real, documented behavioural divergence
 * between interpreters that real ROMs depend on. This struct is the
 * reason the core is one code path rather than three, and it is also
 * the reason a hardware CHIP-8 CPU would have been a bad trade: in C
 * this is a struct literal, in RTL it is a mode register threaded
 * through the datapath.
 *
 * The names follow the vocabulary of Timendus' chip8-test-suite, so
 * that a failing test names the field it is failing on. */

typedef enum {
	C8_MEM_INC_X1 = 0,   /* FX55/FX65 leave I = I + X + 1 (CHIP-8, XO-CHIP) */
	C8_MEM_INC_X,        /* ... I + X (CHIP-48) */
	C8_MEM_INC_NONE      /* ... I unchanged (SUPER-CHIP 1.1) */
} c8_mem_inc_t;

typedef struct {

	/* 8XY1/8XY2/8XY3 zero VF as a side effect. True on the COSMAC
	 * VIP because the ALU wrote through VF; false everywhere after.
	 * Programs written on a VIP can depend on it either way round,
	 * which is why it cannot simply be picked. */
	bool vf_reset;

	c8_mem_inc_t mem_inc;

	/* DXYN blocks until the next display frame. On a VIP the
	 * interpreter genuinely waited for the CDP1861's vertical
	 * blank, which caps CHIP-8 at one sprite per frame and is what
	 * stops VIP-era games from flickering. */
	bool display_wait;

	/* Sprites clip at the right and bottom edges instead of wrapping
	 * round. The initial position always wraps regardless -- that is
	 * not a quirk, every interpreter does it. */
	bool clip_sprites;

	/* 8XY6/8XYE shift VX in place, rather than shifting VY into VX. */
	bool shift_vx;

	/* BNNN becomes BXNN: jump to XNN + VX rather than NNN + V0. */
	bool jump_vx;

	/* DXY0 draws a 16x16 sprite even in lores. SUPER-CHIP 1.1 does;
	 * CHIP-8 has no DXY0 at all and draws nothing. */
	bool wide_sprite_lores;

	/* VF counts colliding sprite rows (plus rows clipped off the
	 * bottom) instead of being a plain 0/1. SUPER-CHIP hires only. */
	bool collision_rows;

	/* 00CN/00FB/00FC move half as far in lores -- SUPER-CHIP scrolls
	 * in hires pixels whatever the mode, so in lores that is half a
	 * lores pixel and N gets halved. XO-CHIP scrolls in lores pixels
	 * instead, which is the more useful behaviour and the one Octo
	 * programs assume. */
	bool scroll_half_lores;

	/* 00FE/00FF clear the display as a side effect of switching
	 * resolution. */
	bool clear_on_mode;

	/* Guest RAM the program can address: 0x0FFF or 0xFFFF. */
	uint16_t addr_mask;

	/* How many RPL flags FX75/FX85 may touch (8 or 16). */
	uint8_t flag_count;

} c8_quirks_t;

typedef enum {
	C8_PROFILE_CHIP8 = 0,
	C8_PROFILE_SCHIP,
	C8_PROFILE_XOCHIP,
	C8_PROFILE_COUNT
} c8_profile_t;

/* quirks.c */
const c8_quirks_t *c8_profile(c8_profile_t p);
const char        *c8_profile_name(c8_profile_t p);
/* Case-insensitive; returns -1 if unknown. */
int                c8_profile_by_name(const char *s);

/* The same for a token that is not NUL-terminated -- a word inside a
 * config line, which the parser must not modify to terminate. */
int                c8_profile_by_name_n(const char *s, int len);

/* -- machine -------------------------------------------------------- */

typedef enum {
	C8_OK = 0,        /* instruction executed */
	C8_WAIT_KEY,      /* FX0A is pending; nothing will run until a key */
	C8_WAIT_FRAME,    /* display wait: stop until c8_frame() */
	C8_HALT,          /* 00FD exit */
	C8_BAD_OPCODE     /* undefined; PC has NOT advanced past it */
} c8_status_t;

typedef struct {

	uint8_t  ram[C8_RAM_SIZE];

	uint8_t  v[16];
	uint16_t i;
	uint16_t pc;
	uint16_t stack[C8_STACK_SIZE];
	uint8_t  sp;

	uint8_t  dt, st;

	/* Display. `w`/`h` are the LOGICAL guest resolution: 64x32 or
	 * 128x64. Rows beyond h and columns beyond w are not touched and
	 * hold whatever the last hires image left there. */
	uint32_t px[C8_PLANES][C8_ROWS][C8_ROW_WORDS];
	int      w, h;
	bool     hires;

	/* Which planes DXYN, CLS and the scrolls act on. Bit 0 is plane
	 * 0, bit 1 is plane 1. 1 at reset, so a plain CHIP-8 program
	 * that never issues FN01 behaves as a one-plane machine without
	 * knowing planes exist. */
	uint8_t  plane_mask;

	/* Sticky: set the first time a program selects plane 1. It never
	 * clears.
	 *
	 * This exists for the RENDERER, and it is not cosmetic. A
	 * one-plane program's colour 1 is FOREGROUND -- full white. A
	 * two-plane program's colour 1 is the first of three greys, at
	 * roughly a third intensity. Same pixel value, two different
	 * correct answers, and picking the two-plane answer for a CHIP-8
	 * ROM renders the whole game as a dim wash on a display with no
	 * greys to spare.
	 *
	 * Sticky rather than "does plane 1 currently have any bits set",
	 * because the latter makes the palette flicker: an XO-CHIP game
	 * that momentarily clears plane 1 would have every remaining
	 * pixel jump from grey to white for that frame. Sticky is also
	 * one assignment instead of a 1KB scan per frame.
	 *
	 * Deliberately NOT keyed off the profile. Plenty of ROMs run
	 * under the XO-CHIP profile for its memory or its wrapping
	 * without ever using the second plane, and they should look like
	 * what they are. */
	bool     two_plane;

	/* One bit per display row that has changed since the renderer
	 * last cleared it. The renderer redraws only these, which is what
	 * makes a full-plane redraw per frame cheap enough to prefer over
	 * blitting each sprite as it is drawn. */
	uint64_t dirty;

	uint16_t keys;          /* bit k = hex key k is down */

	/* FX0A state. `wait_reg` is the V register the key goes into;
	 * `wait_key` is the key seen going down, or -1 if we are still
	 * waiting for one. The key is delivered on RELEASE, which is what
	 * the VIP did and what stops one press satisfying two
	 * consecutive FX0As. */
	bool     waiting;
	uint8_t  wait_reg;
	int8_t   wait_key;

	/* Set by DXYN under the display_wait quirk, cleared by
	 * c8_frame(). */
	bool     wait_frame;

	bool     halted;

	uint8_t  pattern[C8_PATTERN_BYTES];
	uint8_t  pitch;
	/* Bumped every time the pattern buffer or pitch changes, so the
	 * sound code can notice without diffing 16 bytes every frame. */
	uint32_t audio_gen;

	uint8_t  flags[C8_FLAGS];

	uint32_t rng;           /* xorshift32; see c8_init() */

	c8_quirks_t q;

	/* Diagnostics. `steps` counts executed instructions; `bad_op` is
	 * the opcode that produced the last C8_BAD_OPCODE, so the app can
	 * report it without re-fetching. */
	uint32_t steps;
	uint16_t bad_op;

} c8_t;

/* Reset to power-on state and install the font. `seed` of 0 is
 * replaced with a fixed non-zero constant, so a test that does not
 * care about randomness still gets a deterministic, reproducible
 * sequence rather than a stuck RNG. */
void c8_init(c8_t *c, const c8_quirks_t *q, uint32_t seed);

/* Copy a ROM to C8_START_ADDR. Returns false, having changed nothing,
 * if it does not fit under addr_mask. */
bool c8_load(c8_t *c, const uint8_t *rom, uint32_t len);

/* Execute one instruction. */
c8_status_t c8_step(c8_t *c);

/* Execute up to `budget` instructions, stopping early on anything
 * other than C8_OK. Returns the status that stopped it (C8_OK if the
 * budget ran out) and writes the number executed to *executed if that
 * is not NULL. */
c8_status_t c8_run(c8_t *c, int budget, int *executed);

/* Call at 60Hz. Decrements DT and ST. */
void c8_tick_timers(c8_t *c);

/* Call once per display frame, before running that frame's
 * instructions. Releases a display wait. */
void c8_frame(c8_t *c);

/* Key state. `k` is 0..15. Both edges must be delivered -- a release
 * is what completes an FX0A. */
void c8_key(c8_t *c, int k, bool down);

/* Convenience for a renderer: the 2-bit colour of a logical pixel,
 * plane 0 in bit 0 and plane 1 in bit 1. Out-of-range reads 0. */
static inline int c8_pixel(const c8_t *c, int x, int y) {
	int b;
	if (x < 0 || y < 0 || x >= c->w || y >= c->h) return 0;
	b = 31 - (x & 31);
	return (int)(((c->px[0][y][x >> 5] >> b) & 1u) |
	             (((c->px[1][y][x >> 5] >> b) & 1u) << 1));
}

static inline void c8_clear_dirty(c8_t *c) { c->dirty = 0; }

/* How many distinct colours the display can currently show: 2 or 4.
 * See `two_plane` above for why this is not simply the profile. */
static inline int c8_levels(const c8_t *c) { return c->two_plane ? 4 : 2; }

/* True if the machine is doing nothing until the outside world acts.
 * Used by the app to skip the run budget entirely rather than spinning
 * through it one refused instruction at a time. */
static inline bool c8_blocked(const c8_t *c) {
	return c->halted || c->waiting || c->wait_frame;
}

#endif
