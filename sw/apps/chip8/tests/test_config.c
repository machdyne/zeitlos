/*
 * chip8 -- config parser and disassembler tests. Host build.
 *
 * The config parser is the kind of code where a bug is silent by
 * construction: a typo'd quirk name, a section that never matches, or
 * an ordering assumption all present as "that ROM still misbehaves"
 * with a config file that looks perfectly correct. So these check the
 * awkward cases rather than the happy path.
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "../config.h"
#include "../disasm.h"

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

#define CHECK_STR(got, want) do { \
	checks++; \
	if (strcmp((got), (want)) != 0) { \
		printf("FAIL %s:%d: \"%s\", wanted \"%s\"\n", \
			__FILE__, __LINE__, (got), (want)); \
		failures++; \
	} \
} while (0)

/* -- config --------------------------------------------------------- */

static const char cfg_text[] =
	"# a rom pack\n"
	"[*]\n"
	"speed 20\n"
	"profile chip8\n"
	"\n"
	"[BLINKY.CH8]\n"
	"name Blinky\n"
	"profile schip\n"
	"speed 30\n"
	"set mem_inc=x1 shift=off\n"
	"pad up=2 down=8 a=5 start=F\n"
	"\n"
	"[OTHER.CH8]  ; trailing comment\n"
	"set clip=off\n";

static void test_config_named_section(void) {

	c8_config_t cfg;
	c8_quirks_t q;

	c8_config_defaults(&cfg);
	CHECK(c8_config_parse(&cfg, cfg_text, "BLINKY.CH8"));

	CHECK_STR(cfg.name, "Blinky");
	CHECK_EQ(cfg.profile, C8_PROFILE_SCHIP);
	CHECK_EQ(cfg.speed, 30);
	CHECK_EQ(cfg.bad_lines, 0);

	CHECK_EQ(cfg.pad[C8_PAD_UP], 2);
	CHECK_EQ(cfg.pad[C8_PAD_DOWN], 8);
	CHECK_EQ(cfg.pad[C8_PAD_A], 5);
	CHECK_EQ(cfg.pad[C8_PAD_START], 0xF);
	/* Buttons the line did not name stay unset, so the app's own
	 * default survives rather than becoming "unmapped". */
	CHECK_EQ(cfg.pad[C8_PAD_B], 0xFF);

	/* The overrides go on top of the SCHIP profile: mem_inc becomes
	 * X1 where SUPER-CHIP 1.1 leaves I alone, and shifting reverts to
	 * the CHIP-8 form. Everything else stays SUPER-CHIP. */
	q = *c8_profile(C8_PROFILE_SCHIP);
	c8_config_apply(&cfg, &q);
	CHECK_EQ(q.mem_inc, C8_MEM_INC_X1);
	CHECK_EQ(q.shift_vx, 0);
	CHECK_EQ(q.jump_vx, 1);          /* untouched */
	CHECK_EQ(q.clip_sprites, 1);     /* untouched */

}

/* The wildcard supplies defaults; the named section overrides them,
 * whichever order they appear in. */
static void test_config_wildcard(void) {

	c8_config_t cfg;

	c8_config_defaults(&cfg);
	CHECK(c8_config_parse(&cfg, cfg_text, "OTHER.CH8"));
	CHECK_EQ(cfg.speed, 20);                     /* from [*] */
	CHECK_EQ(cfg.profile, C8_PROFILE_CHIP8);     /* from [*] */
	CHECK(cfg.set_mask & (1u << C8_Q_CLIP));

	/* A ROM with no section of its own still gets the wildcard. */
	c8_config_defaults(&cfg);
	CHECK(c8_config_parse(&cfg, cfg_text, "NOSUCH.CH8"));
	CHECK_EQ(cfg.speed, 20);
	CHECK_STR(cfg.name, "");

}

/* A named section must win even when it is written ABOVE the wildcard,
 * which is the ordering a hand-edited file drifts into. */
static void test_config_section_order(void) {

	static const char t[] =
		"[GAME.CH8]\n"
		"speed 99\n"
		"[*]\n"
		"speed 20\n";

	c8_config_t cfg;

	c8_config_defaults(&cfg);
	c8_config_parse(&cfg, t, "GAME.CH8");
	CHECK_EQ(cfg.speed, 99);

}

/* `set` before `profile` must not be discarded. A parser that applied
 * each line as it read it would silently lose the override. */
static void test_config_line_order(void) {

	static const char t[] =
		"[G.CH8]\n"
		"set shift=on\n"
		"profile chip8\n";

	c8_config_t cfg;
	c8_quirks_t q;

	c8_config_defaults(&cfg);
	c8_config_parse(&cfg, t, "G.CH8");
	CHECK_EQ(cfg.profile, C8_PROFILE_CHIP8);

	q = *c8_profile(C8_PROFILE_CHIP8);
	c8_config_apply(&cfg, &q);
	CHECK_EQ(q.shift_vx, 1);

}

static void test_config_matching(void) {

	c8_config_t cfg;

	/* Case-insensitive, because FAT gives uppercase names and people
	 * type lowercase. */
	c8_config_defaults(&cfg);
	c8_config_parse(&cfg, cfg_text, "blinky.ch8");
	CHECK_EQ(cfg.speed, 30);

	/* A prefix must NOT match: [BLINKY.CH8] is not [BLINKY.CH8X]. */
	c8_config_defaults(&cfg);
	c8_config_parse(&cfg, cfg_text, "BLINKY.CH");
	CHECK_EQ(cfg.speed, 20);        /* wildcard only */

}

static void test_config_bad_lines(void) {

	static const char t[] =
		"[G.CH8]\n"
		"set shfit=on\n"          /* typo'd field */
		"set clip=maybe\n"        /* not a boolean */
		"profile turbo\n"         /* no such profile */
		"pad up=zz\n"             /* not a hex digit */
		"speed 20\n";

	c8_config_t cfg;

	c8_config_defaults(&cfg);
	c8_config_parse(&cfg, t, "G.CH8");

	/* Counted, not silently ignored: a typo would otherwise look
	 * exactly like a quirk that does not work. */
	CHECK_EQ(cfg.bad_lines, 4);
	CHECK_EQ(cfg.first_bad_line, 2);

	/* ... and the good lines still take effect. */
	CHECK_EQ(cfg.speed, 20);

}

static void test_config_palette(void) {

	static const char t[] =
		"[*]\n"
		"palette solid\n"
		"[ART.XO8]\n"
		"palette index\n"
		"[BAD.XO8]\n"
		"palette rainbow\n";

	c8_config_t cfg;

	c8_config_defaults(&cfg);
	c8_config_parse(&cfg, t, "ART.XO8");
	CHECK_EQ(cfg.palette, 1);          /* index, overriding the wildcard */

	c8_config_defaults(&cfg);
	c8_config_parse(&cfg, t, "OTHER.XO8");
	CHECK_EQ(cfg.palette, 2);          /* solid, from [*] */

	c8_config_defaults(&cfg);
	c8_config_parse(&cfg, t, "BAD.XO8");
	CHECK_EQ(cfg.bad_lines, 1);
	CHECK_EQ(cfg.palette, 2);          /* the wildcard still applies */

}

static void test_config_memory_and_meminc(void) {

	static const char t[] =
		"[G.XO8]\n"
		"set memory=64k mem_inc=none\n";

	c8_config_t cfg;
	c8_quirks_t q;

	c8_config_defaults(&cfg);
	c8_config_parse(&cfg, t, "G.XO8");

	q = *c8_profile(C8_PROFILE_CHIP8);
	c8_config_apply(&cfg, &q);
	CHECK_EQ(q.addr_mask, 0xFFFF);
	CHECK_EQ(q.mem_inc, C8_MEM_INC_NONE);

}

static void test_config_empty(void) {

	c8_config_t cfg;

	c8_config_defaults(&cfg);
	CHECK(!c8_config_parse(&cfg, "", "G.CH8"));
	CHECK(!c8_config_parse(&cfg, "# nothing but a comment\n", "G.CH8"));
	CHECK_EQ(cfg.profile, -1);
	CHECK_EQ(cfg.speed, 0);

}

/* Size decides before the name does. A ROM too large for a 12-bit
 * address space cannot be CHIP-8 or SUPER-CHIP whatever it is called,
 * and mis-naming is common: the file that prompted this was a 65,000
 * byte XO-CHIP ROM called .CH8, which under the extension's word would
 * have been refused by c8_load() as not fitting. */
static void test_profile_hint(void) {

	CHECK_EQ(c8_profile_hint("GAME.CH8", 2048), C8_PROFILE_CHIP8);
	CHECK_EQ(c8_profile_hint("GAME.SC8", 2048), C8_PROFILE_SCHIP);
	CHECK_EQ(c8_profile_hint("GAME.XO8", 2048), C8_PROFILE_XOCHIP);

	/* Case-insensitive: FAT gives uppercase, people type lowercase. */
	CHECK_EQ(c8_profile_hint("game.xo8", 100), C8_PROFILE_XOCHIP);
	CHECK_EQ(c8_profile_hint("Game.Sc8", 100), C8_PROFILE_SCHIP);

	/* Exactly at the 4K ceiling is still allowed to be CHIP-8. */
	CHECK_EQ(c8_profile_hint("GAME.CH8", C8_ROM_MAX_4K), C8_PROFILE_CHIP8);

	/* One byte over, and it cannot be. */
	CHECK_EQ(c8_profile_hint("GAME.CH8", C8_ROM_MAX_4K + 1),
		C8_PROFILE_XOCHIP);
	CHECK_EQ(c8_profile_hint("REDOCT.CH8", 65000), C8_PROFILE_XOCHIP);

	/* Size cannot talk anything DOWN, only up: a small file named
	 * .xo8 is still XO-CHIP. */
	CHECK_EQ(c8_profile_hint("TINY.XO8", 64), C8_PROFILE_XOCHIP);

	/* No name at all still classifies by size. */
	CHECK_EQ(c8_profile_hint(NULL, 65000), C8_PROFILE_XOCHIP);
	CHECK_EQ(c8_profile_hint(NULL, 100), C8_PROFILE_CHIP8);

	/* An extension that is a prefix of a known one must not match. */
	CHECK_EQ(c8_profile_hint("GAME.X", 100), C8_PROFILE_CHIP8);

}

/* -- disassembler --------------------------------------------------- */

static uint8_t ram[65536];

static void put(uint16_t a, uint16_t op) {
	ram[a] = (uint8_t)(op >> 8);
	ram[a + 1] = (uint8_t)op;
}

static void dis(uint16_t a, char *buf, int len, int *outlen) {
	*outlen = c8_disasm(ram, a, 0xFFFF, buf, len);
}

static void test_disasm(void) {

	char buf[C8_DISASM_MAX];
	int len;

	put(0x200, 0x00E0); dis(0x200, buf, sizeof(buf), &len);
	CHECK_STR(buf, "clear"); CHECK_EQ(len, 2);

	put(0x200, 0xA22A); dis(0x200, buf, sizeof(buf), &len);
	CHECK_STR(buf, "i := 0x22A");

	put(0x200, 0x6A0C); dis(0x200, buf, sizeof(buf), &len);
	CHECK_STR(buf, "vA := 0x0C");

	put(0x200, 0xD015); dis(0x200, buf, sizeof(buf), &len);
	CHECK_STR(buf, "sprite v0 v1 5");

	/* Octo's conditionals are INVERTED relative to the opcode: 3XNN
	 * skips when equal, so the body runs when NOT equal. Getting this
	 * backwards produces a listing that reads as the opposite program
	 * and is entirely plausible. */
	put(0x200, 0x3005); dis(0x200, buf, sizeof(buf), &len);
	CHECK_STR(buf, "if v0 != 0x05 then");
	put(0x200, 0x4005); dis(0x200, buf, sizeof(buf), &len);
	CHECK_STR(buf, "if v0 == 0x05 then");
	put(0x200, 0x5010); dis(0x200, buf, sizeof(buf), &len);
	CHECK_STR(buf, "if v0 != v1 then");
	put(0x200, 0x9010); dis(0x200, buf, sizeof(buf), &len);
	CHECK_STR(buf, "if v0 == v1 then");

	/* Same inversion for the key tests. */
	put(0x200, 0xE09E); dis(0x200, buf, sizeof(buf), &len);
	CHECK_STR(buf, "if v0 -key then");
	put(0x200, 0xE0A1); dis(0x200, buf, sizeof(buf), &len);
	CHECK_STR(buf, "if v0 key then");

	put(0x200, 0x00C4); dis(0x200, buf, sizeof(buf), &len);
	CHECK_STR(buf, "scroll-down 4");
	put(0x200, 0x00D4); dis(0x200, buf, sizeof(buf), &len);
	CHECK_STR(buf, "scroll-up 4");

	put(0x200, 0xF201); dis(0x200, buf, sizeof(buf), &len);
	CHECK_STR(buf, "plane 2");
	put(0x200, 0xF002); dis(0x200, buf, sizeof(buf), &len);
	CHECK_STR(buf, "audio");

	put(0x200, 0x5232); dis(0x200, buf, sizeof(buf), &len);
	CHECK_STR(buf, "save v2 - v3");
	put(0x200, 0x5233); dis(0x200, buf, sizeof(buf), &len);
	CHECK_STR(buf, "load v2 - v3");

	put(0x200, 0x8016); dis(0x200, buf, sizeof(buf), &len);
	CHECK_STR(buf, "v0 >>= v1");

}

/* The long load is the only four-byte instruction, and reporting it as
 * two would put every following line out of phase -- which reads as a
 * corrupt ROM rather than as a disassembler bug. */
static void test_disasm_long_load(void) {

	char buf[C8_DISASM_MAX];
	int len;

	put(0x200, 0xF000);
	put(0x202, 0x1234);
	put(0x204, 0x6042);

	dis(0x200, buf, sizeof(buf), &len);
	CHECK_STR(buf, "i := long 0x1234");
	CHECK_EQ(len, 4);

	dis(0x204, buf, sizeof(buf), &len);
	CHECK_STR(buf, "v0 := 0x42");
	CHECK_EQ(len, 2);

}

/* A read at the very top of memory must wrap through the mask rather
 * than run off the array. */
static void test_disasm_wraps(void) {

	char buf[C8_DISASM_MAX];

	ram[0x0FFF] = 0x60;
	ram[0x0000] = 0x42;
	c8_disasm(ram, 0x0FFF, 0x0FFF, buf, sizeof(buf));
	CHECK_STR(buf, "v0 := 0x42");

}

static void test_disasm_unknown(void) {

	char buf[C8_DISASM_MAX];
	int len;

	put(0x200, 0x8FFF); dis(0x200, buf, sizeof(buf), &len);
	CHECK_STR(buf, "; bad 0x8FFF");

	/* 0NNN is a call into 1802 machine code. Marked as such rather
	 * than as an error -- it is a real instruction, just not one
	 * anything here can run. */
	put(0x200, 0x0123); dis(0x200, buf, sizeof(buf), &len);
	CHECK_STR(buf, "; machine 0x123");

}

int main(void) {

	test_config_named_section();
	test_config_wildcard();
	test_config_section_order();
	test_config_line_order();
	test_config_matching();
	test_config_bad_lines();
	test_config_palette();
	test_config_memory_and_meminc();
	test_config_empty();
	test_profile_hint();

	test_disasm();
	test_disasm_long_load();
	test_disasm_wraps();
	test_disasm_unknown();

	printf("%s: %d checks, %d failures\n",
		failures ? "FAILED" : "ok", checks, failures);

	return failures ? 1 : 0;

}
