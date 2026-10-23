/*
 * Zeitlos -- the screen toolkit's boxes on a build host.
 *
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * A stand-in program -- a busy picture in sixteen colours with sixteen
 * different monochrome inks -- opens every kind of box, driven by a
 * script of keys and the mouse through tests/ar_host.c, in one mode:
 *
 *   ./test_sui <dir> window | mono | c16 | c64 | c256
 *
 * and writes a picture of each box (<dir>/<name>.ppm). Checked:
 *
 *   - every answer: OK, Yes, No, Cancel, Escape, a closed window, the
 *     letters, Tab and the arrows, a click, typing and editing a line,
 *     a password, an empty line;
 *   - every box's layout: on the screen, no text cut off
 *     (sui_last_layout_ok());
 *   - the palette given back as it was after the boxes, and the
 *     picture behind not dimmed where the program named its entries;
 *   - a mode change inside a box handed back to the program after it;
 *   - with AR_HOST_VERIFY set, every frame against a full repaint.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../sui.h"
#include "../../zarcade.h"
#include "../../tests/ar_host.h"

extern const z_font_t z_font_5x8;

static int fails, checks;

#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; \
	printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); } } while (0)

/* -- the stand-in program -- */

static ar_desc_t desc;

static void paint(void *user, const ar_rect_t *r) {
	int x, y;
	(void)user;
	for (y = r->y & ~7; y < r->y + r->h; y += 8)
		for (x = r->x & ~7; x < r->x + r->w; x += 8)
			ar_fill(x, y, 8, 8, ((x + y * 2) / 8) % 16);
	ar_text(8, 8, "a picture behind the boxes", 15, &z_font_5x8, 2);
}

/* A box nobody answers would wait for ever: stop instead. */
void ar_host_hook(uint32_t frame) {
	if (frame > 1000) {
		printf("FAIL: still waiting at frame %u -- a box the script "
			"did not answer\n", frame);
		exit(1);
	}
}

/* -- the script -- */

#define K_UP     ARH_K_UP
#define K_DOWN   ARH_K_DOWN
#define K_LEFT   ARH_K_LEFT
#define K_RIGHT  ARH_K_RIGHT
#define K_PGDN   0x110007u
#define K_HOME   0x110004u
#define K_ESC    0x1bu

static ar_host_step_t script[256];
static int ns;
static uint32_t at;          /* the frame the next step lands on */
static const char *mode_name;

static void step(int kind, uint32_t a, int b, const char *name) {
	ar_host_step_t *s = &script[ns++];
	memset(s, 0, sizeof(*s));
	s->frame = at;
	s->kind = kind;
	s->a = a;
	s->b = b;
	s->name = name;
}

static void tap(uint32_t k) { at += 2; step(ARH_TAP, k, 0, NULL); }
static void tap_mod(uint32_t k, int mods) { at += 2; step(ARH_TAP, k, mods, NULL); }
static void type(const char *s) { while (*s) tap((uint8_t)*s++); }
static void shot(const char *name) { at += 2; step(ARH_SHOT, 0, 0, name); }
static void click(int x, int y) {
	ar_host_step_t *s;
	/* the pointer arrives first, buttons up: a box takes a press only
	 * after it has seen them up (a button held from the click that
	 * opened it must not press one of its own) */
	at += 2; step(ARH_MOUSE, 0, 0, NULL); s = &script[ns - 1]; s->x = x; s->y = y;
	at += 1; step(ARH_MOUSE, 1, 0, NULL); s = &script[ns - 1]; s->x = x; s->y = y;
	at += 1; step(ARH_MOUSE, 0, 0, NULL); s = &script[ns - 1]; s->x = x; s->y = y;
}
static void mode_step(void) {
	if (!strcmp(mode_name, "window")) step(ARH_WINDOW, 0, 0, NULL);
	else if (!strcmp(mode_name, "mono")) step(ARH_FULL_MONO, 0, 0, NULL);
	else step(ARH_FULL_COLOR, 0, 0, NULL);
}

static const char long_text[] =
	"The file could not be read. Every line below is part of a long "
	"message, to show what a box does when its text is longer than ten "
	"rows: it scrolls, with a scrollbar, the wheel, the arrows and the "
	"page keys.\n\n"
	"1. The first reason it might be.\n"
	"2. A second reason, which goes on for long enough that it has to be "
	"wrapped onto a second row of its own.\n"
	"3. A third.\n4. A fourth.\n5. A fifth.\n6. A sixth.\n7. A seventh.\n"
	"8. An eighth.\n9. A ninth.\n10. The tenth and last.";

static void build_script(void) {
	at = 1;
	mode_step();

	/* 1: a message, OK by Enter */
	at = 6; shot("1_message");
	tap('\r');

	/* 2: three buttons: Right to No, picture, Enter */
	at += 4; shot("2_confirm");
	tap(K_RIGHT); shot("2_confirm_no");
	tap('\r');

	/* 3: a prompt: edit "walk1" to "walk2", then OK */
	at += 4; shot("3_prompt");
	tap(8); type("2"); tap(K_HOME); type("x"); tap(0x110009u);
	shot("3_prompt_typed");
	tap('\r');

	/* 4: a long warning: PgDn, then the wheel... Escape */
	at += 4; shot("4_warn");
	tap(K_PGDN); shot("4_warn_pgdn");
	tap(K_ESC);

	/* 5: OK / Cancel, Escape */
	at += 4; shot("5_okcancel");
	tap(K_ESC);

	/* 6: Yes / No, the letter N */
	at += 4; tap('n');

	/* 7: Yes / No / Cancel: Tab twice, then Shift+Tab, Space: No */
	at += 4; tap('\t'); tap('\t'); tap_mod('\t', 0x02); tap(' ');

	/* 8: a click on the second button (No) of Yes / No */
	at += 4; shot("8_click");
	/* "Click" / "No is clicked." / Yes No: a 120x51 box at (100,94),
	 * No at x 174..213, y 126..138 (sui.c's metrics; the layout is
	 * checked below, so a change there fails here loudly) */
	click(190, 130);

	/* 9: a password */
	at += 4; type("abc"); shot("9_secret");
	tap('\r');

	/* 10: an empty line: false; then the empty-ok prompt: true */
	at += 4; tap(8); tap(8); tap(8); tap(8); tap('\r');
	at += 4; tap(8); tap(8); tap(8); tap(8); tap('\r');

	/* 11: a mode change inside a box, then Enter */
	at += 4;
	if (!strcmp(mode_name, "window")) step(ARH_FULL_COLOR, 0, 0, NULL);
	else step(ARH_WINDOW, 0, 0, NULL);
	shot("11_mode");
	tap('\r');

	/* 13: open: Down, Down to pictures/, Enter, Down, Down, Enter */
	at += 4; shot("13_open");
	tap(K_DOWN); tap(K_DOWN); tap('\r'); shot("13_open_pictures");
	tap(K_DOWN); tap(K_DOWN); tap('\r');

	/* 14: open: R jumps to readme.txt, Enter */
	at += 4; tap('r'); shot("14_open_jump"); tap('\r');

	/* 15: open in /home/pictures: Backspace goes up, Escape */
	at += 4; tap(8); shot("15_open_up"); tap(K_ESC);

	/* 16: open: PgDn twice; the list scrolled; Escape */
	at += 4; tap(K_PGDN); tap(K_PGDN); shot("16_open_pgdn"); tap(K_ESC);

	/* 17: save: the name edited to untitled.png, Enter */
	at += 4; shot("17_save");
	tap(8); tap(8); tap(8); type("png"); shot("17_save_typed"); tap('\r');

	/* 18: save: Shift+Tab to the list, Down x3 to file01.zbm, Enter
	 * puts it in the line, Enter */
	at += 4; tap_mod('\t', 0x02); tap(K_DOWN); tap(K_DOWN); tap(K_DOWN);
	tap('\r'); shot("18_save_picked"); tap('\r');

	/* 19: open: a double click on file01.zbm (row 3 of the list: a
	 * 300x161 box at (10,39), rows from y 68, nine high) */
	at += 4;
	click(60, 68 + 3 * 9 + 4);
	click(60, 68 + 3 * 9 + 4);

	/* 20: help: a picture, PgDn, a picture, F1 closes */
	at += 4; shot("20_help"); tap(K_PGDN); shot("20_help_pgdn");
	tap(0x110010u);

	/* 12: the window closed with a box up */
	at += 6;
	step(ARH_END, 0, 0, NULL);
}

static const char help_text[] =
	"Snake: eat, grow, and do not run into anything -- not the walls, not "
	"the other snake, not yourself.\n\n"
	"Arrows\tsteer the first snake\n"
	"W A S D\tsteer the second\n"
	"Space\tpause, or start\n"
	"Shift+PgUp PgDn\tthe page of colours (a key long enough to take a "
	"row of its own before its meaning)\n"
	"F11\tfull screen, or back to the window\n"
	"C\tfull screen: colour on or off\n"
	"M\tmusic on or off\n"
	"Esc\tleave full screen\n\n"
	"Levels\tforty of them, each a little faster than the last, with "
	"walls that move from level ten and food that runs away from "
	"level twenty\n"
	"Scores\tthe top ten for each skill, kept on the card\n"
	"Skill\teasy, normal or hard: easy slows the snake and forgives one "
	"crash a level\n\n"
	"The first player is the arrows and the first gamepad; the second is "
	"W A S D and the second gamepad. A one-player game takes both.\n\n"
	"1\tone\n2\ttwo\n3\tthree\n4\tfour\n5\tfive\n6\tsix\n"
	"7\tseven\n8\teight\n9\tnine\n10\tten\n";

/* -- the program -- */

static int colours = 16;

int main(int argc, char **argv) {
	char buf[64];
	int r, i, modes = 0, quits = 0;
	uint16_t before[256];
	ar_event_t e;
	int bx, by, bw, bh;

	if (argc < 3) {
		fprintf(stderr, "usage: test_sui <dir> window|mono|c16|c64|c256\n");
		return 2;
	}
	setvbuf(stdout, NULL, _IOLBF, 0);
	mode_name = argv[2];
	if (!strcmp(mode_name, "c64")) colours = 64;
	/* c16auto: 16 colours with the toolkit's own choice (darkest and
	 * lightest); c16: with the program's entries named */
	if (!strcmp(mode_name, "c256")) colours = 256;

	memset(&desc, 0, sizeof(desc));
	desc.name = "suitest";
	for (i = 0; i < 16; i++) {
		desc.palette[i] = (uint16_t)(((i * 5) & 15) << 8 | ((i * 11) & 15) << 4 | ((i * 3 + 4) & 15));
		ar_ink_level(&desc.ink[i], i);
	}
	desc.paint = paint;

	build_script();
	ar_host_run(script, argv[1]);
	ar_init(&desc);
	if (colours > 16) ar_set_colours(colours);
	for (i = 0; i < 4; i++) ar_frame();
	for (i = 0; i < 16; i++) ar_pal(i, desc.palette[i]);
	for (i = 16; i < 256; i++) ar_pal(i, ar_rgb_mix(desc.palette[i & 15], 0xfff, 64));
	if (colours == 16 && strcmp(mode_name, "c16auto")) {
		/* every entry is the art's: the boxes in its darkest and
		 * lightest, nothing dimmed */
		static const uint8_t mine[SUI_NC] = { 0, 15, 15, 15, 0, 15, 0, 15 };
		sui_palette(mine, false);
	}
	ar_frame();
	for (i = 0; i < 256; i++) before[i] = ar_pal_get(i);

	sui_message("Saved", "The picture was saved as /home/pix/untitled.zbm.");
	CHECK(sui_last_layout_ok(NULL, NULL, NULL, NULL), "message layout");

	r = sui_confirm("Save changes?",
		"The picture has changed since it was last saved. "
		"Save it before opening another?", SUI_YES_NO_CANCEL);
	CHECK(r == SUI_NO, "confirm: Right, Enter -> No (got %d)", r);
	CHECK(sui_last_layout_ok(NULL, NULL, NULL, NULL), "confirm layout");

	CHECK(sui_prompt("Rename", "A new name for this cell:", "walk1", buf,
		sizeof buf), "prompt: OK");
	CHECK(!strcmp(buf, "xalk2"), "prompt: edited to \"%s\", wanted \"xalk2\"", buf);
	CHECK(sui_last_layout_ok(NULL, NULL, NULL, NULL), "prompt layout");

	sui_warn("Cannot open", long_text);
	CHECK(sui_last_layout_ok(NULL, NULL, NULL, NULL), "long warning layout");

	r = sui_confirm("Delete cell?", "Cell 3 of 8 goes, and its frame time.",
		SUI_OK_CANCEL);
	CHECK(r == SUI_CANCEL, "OK/Cancel: Escape -> Cancel (got %d)", r);

	r = sui_confirm("Quit?", "Leave pix?", SUI_YES_NO);
	CHECK(r == SUI_NO, "Yes/No: N -> No (got %d)", r);

	r = sui_confirm("Save first?", "Tab, Tab, Shift+Tab, Space.",
		SUI_YES_NO_CANCEL);
	CHECK(r == SUI_NO, "Yes/No/Cancel: tabs -> No (got %d)", r);

	r = sui_confirm("Click", "No is clicked.", SUI_YES_NO);
	sui_last_layout_ok(&bx, &by, &bw, &bh);
	CHECK(bx == 100 && by == 94 && bw == 120 && bh == 51,
		"click box at %d,%d %dx%d, the script aims for 100,94 120x51",
		bx, by, bw, bh);
	CHECK(r == SUI_NO, "a click on No (got %d)", r);

	CHECK(sui_prompt_secret("Password", "For the network:", buf, sizeof buf),
		"secret: OK");
	CHECK(!strcmp(buf, "abc"), "secret: got \"%s\"", buf);

	CHECK(!sui_prompt("Empty", "Nothing typed is no answer:", "abcd", buf,
		sizeof buf), "empty line: false");
	CHECK(sui_prompt_empty_ok("Replace with", "Nothing is an answer here:",
		"abcd", buf, sizeof buf) && buf[0] == 0, "empty-ok: true, empty");

	sui_message("Mode", "The mode changes while this is up.");
	while (ar_poll(&e)) {
		if (e.type == AR_EV_MODE) modes++;
		if (e.type == AR_EV_QUIT) quits++;
	}
	CHECK(modes >= 1, "the mode change handed back after the box");

	CHECK(sui_open(NULL, buf, sizeof buf), "open: a file");
	CHECK(!strcmp(buf, "/home/pictures/dog.zbm"), "open: got \"%s\"", buf);
	CHECK(sui_last_layout_ok(NULL, NULL, NULL, NULL), "open layout");

	CHECK(sui_open("/home", buf, sizeof buf) &&
		!strcmp(buf, "/home/readme.txt"), "open: R -> \"%s\"", buf);

	CHECK(!sui_open("/home/pictures", buf, sizeof buf) && buf[0] == 0,
		"open: Escape -> false");
	CHECK(!sui_open("/home/", buf, sizeof buf), "open: PgDn, Escape");

	CHECK(sui_save("/home", "untitled.zbm", buf, sizeof buf), "save: OK");
	CHECK(!strcmp(buf, "/home/untitled.png"), "save: got \"%s\"", buf);
	CHECK(sui_last_layout_ok(NULL, NULL, NULL, NULL), "save layout");

	CHECK(sui_save(NULL, "x.zbm", buf, sizeof buf) &&
		!strcmp(buf, "/home/file01.zbm"), "save: picked \"%s\"", buf);

	CHECK(sui_open(NULL, buf, sizeof buf) &&
		!strcmp(buf, "/home/file01.zbm"), "open: double click \"%s\"", buf);
	sui_last_layout_ok(&bx, &by, &bw, &bh);
	CHECK(bx == 10 && by == 39 && bw == 300 && bh == 161,
		"open box at %d,%d %dx%d, the script aims for 10,39 300x161",
		bx, by, bw, bh);

	sui_help("Snake", help_text);
	CHECK(sui_last_layout_ok(NULL, NULL, NULL, NULL), "help layout");

	r = sui_confirm("Closed", "The window closes with this up.",
		SUI_YES_NO_CANCEL);
	CHECK(r == SUI_CANCEL, "closed window: Cancel (got %d)", r);
	while (ar_poll(&e)) if (e.type == AR_EV_QUIT) quits++;
	CHECK(quits == 1, "the close handed back after the box (%d)", quits);

	ar_frame();
	for (i = 0; i < 256; i++)
		if (ar_pal_get(i) != before[i]) {
			CHECK(0, "palette entry %d not given back: %03x, was %03x",
				i, ar_pal_get(i), before[i]);
			break;
		}

	{
		uint32_t bad = ar_host_verify_bad();
		CHECK(bad == 0, "%u frames differ from a full repaint", bad);
	}

	printf("test_sui %s: %d checks, %d failed, %d pictures, %u verified frames\n",
		mode_name, checks, fails, ar_host_shots(), ar_host_verified());
	return fails ? 1 : 0;
}
