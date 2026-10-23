/*
 * Zeitlos -- host tests of the arcade runtime (zarcade.c) and its sound
 * engine (zsfx.c), on the software framebuffer of ar_host.c.
 *
 *   make -C sw/common/games test-arcade
 *
 * The test that matters most is the first: the runtime promises that a
 * game which only marks what changed and resubmits its overlays ends up
 * with EXACTLY the picture a full repaint would give -- no ghost of a
 * sprite that moved, no hole where one went away. So it runs random
 * frames of moving overlays and dirty rectangles in every mode (with
 * and without another window over ours), and after each frame repaints
 * everything from scratch and compares the two framebuffers.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../zarcade_int.h"
#include "../zsfx.h"
#include "ar_host.h"

static int fails, checks;

#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; \
	printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); \
	printf("\n"); } } while (0)

/* -- a little model to paint: a few rectangles on a patterned floor -- */

static uint8_t model[AR_H / 8][AR_W / 8];     /* colour per 8x8 cell */

static void paint(void *user, const ar_rect_t *r) {
	int cx, cy;
	(void)user;
	ar_fill2(r->x, r->y, r->w, r->h, 0, 1, &ar_ink_dots);
	for (cy = r->y / 8; cy < (r->y + r->h + 7) / 8 && cy < AR_H / 8; cy++)
		for (cx = r->x / 8; cx < (r->x + r->w + 7) / 8 && cx < AR_W / 8; cx++)
			if (model[cy][cx])
				ar_fill(cx * 8 + 1, cy * 8 + 1, 6, 6, model[cy][cx]);
	/* text across tile boundaries, as a status line would */
	ar_text(3, 2, "SCORE 0123", 15, &z_font_8x8, 1);
}

static ar_desc_t desc = {
	"test", NULL, NULL, NULL,
	{ 0x000, 0x111, 0x222, 0x333, 0x444, 0x555, 0x666, 0x777,
	  0x888, 0x999, 0xaaa, 0xbbb, 0xccc, 0xddd, 0xeee, 0xfff },
	{ { { 0 } } },
	paint, NULL, false, NULL, false
};

static ar_sprite_t spr[3];

static const char *const ball[6] = {
	".5555.", "5ffff5", "5f99f5", "5f99f5", "5ffff5", ".5555."
};
static const char *const ship[4] = {
	"...a...", "..aaa..", ".aa3aa.", "aa...aa"
};

typedef struct { int x, y, vx, vy, kind, c; } mover_t;

static void one_mode(const char *name, int mode, bool covered) {

	static uint8_t inc[AR_HOST_FB_BYTES], full[AR_HOST_FB_BYTES];
	mover_t m[24];
	int f, i, worst = 0;

	if (mode == 0 || mode == 3) {
		ar_set_fullscreen(false);
		/* 3: the window in colour (docs/color.md, "The colour window") */
		ar__wcolor(mode == 3, 4, 480);
	} else {
		ar__s.want_color = (mode == 2);
		ar_set_fullscreen(true);
		ar_set_color(mode == 2);
	}
	ar_host_cover(0, 0, 0, 0);
	if (covered) ar_host_cover(150, 80, 120, 200);

	for (i = 0; i < 24; i++) {
		m[i].x = (int)ar_rand_below(300);
		m[i].y = (int)ar_rand_below(220);
		m[i].vx = (int)ar_rand_below(7) - 3;
		m[i].vy = (int)ar_rand_below(7) - 3;
		m[i].kind = (int)ar_rand_below(4);
		m[i].c = 2 + (int)ar_rand_below(13);
	}

	ar__invalidate();
	ar_host_render_only();

	for (f = 0; f < 300; f++) {
		/* the model changes now and then */
		if (ar_rand_below(4) == 0) {
			int cx = (int)ar_rand_below(AR_W / 8), cy = (int)ar_rand_below(AR_H / 8);
			model[cy][cx] = (uint8_t)ar_rand_below(16);
			ar_dirty(cx * 8, cy * 8, 8, 8);
		}
		/* overlays move; some stand still; one comes and goes */
		for (i = 0; i < 24; i++) {
			mover_t *o = &m[i];
			if (i % 5 != 0) { o->x += o->vx; o->y += o->vy; }
			if (o->x < -10 || o->x > 325) o->vx = -o->vx;
			if (o->y < -10 || o->y > 245) o->vy = -o->vy;
			if (i == 7 && (f / 13) % 2) continue;
			switch (o->kind) {
			case 0: ar_ov_fill(o->x, o->y, 3, 3, o->c); break;
			case 1: ar_ov_sprite(&spr[0], o->x, o->y); break;
			case 2: ar_ov_sprite(&spr[1], o->x, o->y); break;
			default: ar_ov_fill(o->x, o->y, 9, 2, o->c); break;
			}
		}
		if (f == 150) ar_parts_burst(160, 120, 40, 3, 3, 400, 30, 6);
		ar_parts_frame(1);

		ar_host_render_only();
		ar_host_fb_copy(inc);

		/* the same picture from nothing: everything dirty, every
		 * overlay drawn anew (and the same list resubmitted) */
		ar__invalidate();
		{
			/* resubmit exactly what this frame had */
			extern void ar__test_resubmit(void);
			ar__test_resubmit();
		}
		ar_host_render_only();
		ar_host_fb_copy(full);

		{
			int d = ar_host_fb_diff(inc, full);
			if (d > worst) worst = d;
		}
	}
	CHECK(worst == 0, "%s: incremental frames match a full repaint "
		"(worst %d pixels differ)", name, worst);
}

static void test_sprites(void) {
	ar_sprite_t s;
	int x, y, k;
	CHECK(ar_sprite_make(&s, ball, 6, 6, NULL), "sprite built");
	CHECK(s.stride == 4, "stride rounded to a word (%d)", s.stride);
	for (y = 0; y < 6; y++)
		for (x = 0; x < 6; x++) {
			int m = (s.mask[y] >> x) & 1;
			int want = ball[y][x] != '.';
			int c = 0;
			for (k = 0; k < 4; k++)
				if (s.plane[k] && ((s.plane[k][y] >> x) & 1)) c |= 1 << k;
			CHECK(m == want, "mask at %d,%d", x, y);
			if (want) {
				int v = ball[y][x] >= 'a' ? ball[y][x] - 'a' + 10 : ball[y][x] - '0';
				CHECK(c == v, "colour %d at %d,%d is %d", v, x, y, c);
			}
			for (k = 0; k < 4; k++)
				if (s.plane[k]) CHECK(!(((s.plane[k][y] >> x) & 1) && !m),
					"plane %d bit outside the mask", k);
			CHECK(!(((s.mono[y] >> x) & 1) && !m), "mono bit outside the mask");
		}
	{
		uint8_t rc[16];
		for (k = 0; k < 16; k++) rc[k] = (uint8_t)k;
		rc[5] = 2;
		CHECK(ar_sprite_make(&s, ball, 6, 6, rc), "recoloured");
		CHECK(s.plane[2] == NULL || !(s.plane[2][0] & 0x1e),
			"5 became 2: no plane-2 bits in the top row");
	}
}

static void test_scores(void) {
	ar_table_t t;
	char out[1024];
	const char *file = "1 500 AAA\n0 100 BOB\n0 300 CAT\n1 200 DOG\n0 200 EEL\n";
	int n;
	n = ar_scores_parse(file, 0, &t);
	CHECK(n == 3, "three for mode 0 (%d)", n);
	CHECK(t.e[0].score == 300 && !strcmp(t.e[0].who, "CAT"), "sorted, best first");
	CHECK(t.e[2].score == 100, "worst last");
	CHECK(ar_scores_rank(&t, 250) == 1, "250 goes second");
	CHECK(ar_scores_rank(&t, 50) == 3, "50 still makes a short table");
	CHECK(ar_scores_rank(&t, 0) == -1, "nothing for 0");
	CHECK(ar_scores_insert(&t, 250, "ZED") == 1, "inserted at 1");
	n = ar_scores_format(out, sizeof out, file, 0, &t);
	CHECK(n > 0, "formats");
	CHECK(strstr(out, "1 500 AAA") && strstr(out, "1 200 DOG"), "other modes kept");
	CHECK(strstr(out, "0 250 ZED"), "new entry written");
	{
		ar_table_t u;
		ar_scores_parse(out, 0, &u);
		CHECK(u.n == 4 && u.e[1].score == 250, "round trip");
	}
	{
		int i;
		for (i = 0; i < 15; i++) ar_scores_insert(&t, 1000 + (uint32_t)i, "X");
		CHECK(t.n == AR_SCORES && t.e[0].score == 1014, "full table keeps the best ten");
		CHECK(ar_scores_rank(&t, 5) == -1, "a full table refuses a small one");
		CHECK(!strcmp(t.e[0].who, "X--"), "short initials padded (%s)", t.e[0].who);
	}
	CHECK(ar_scores_parse("garbage\n\n0\n0 x\n", 0, &t) == 0, "junk is ignored");
}

void ar__skill_load(void);

static void test_skill(void) {
	char out[256];
	int n;
	CHECK(ar_skill_word("easy") == AR_SKILL_EASY, "easy");
	CHECK(ar_skill_word(" Normal") == AR_SKILL_NORMAL, "Normal, leading space");
	CHECK(ar_skill_word("HARD\n") == AR_SKILL_HARD, "HARD, then a newline");
	CHECK(ar_skill_word("hardly") == -1, "not a prefix match");
	CHECK(ar_skill_parse("") == -1, "no settings, no skill");
	CHECK(ar_skill_parse("x 1\nskill hard\ny 2\n") == AR_SKILL_HARD, "found among others");
	CHECK(ar_skill_parse("skill easy\nskill normal") == AR_SKILL_NORMAL, "the last one wins");
	CHECK(ar_skill_parse("skill wobbly\n") == -1, "an unknown word is none");
	n = ar_skill_format(out, sizeof out, "x 1\nskill hard\ny 2\n", AR_SKILL_EASY);
	CHECK(n > 0 && strcmp(out, "x 1\ny 2\nskill easy\n") == 0, "others kept, skill replaced (%s)", out);
	n = ar_skill_format(out, sizeof out, NULL, AR_SKILL_NORMAL);
	CHECK(n > 0 && strcmp(out, "skill normal\n") == 0, "from nothing");
	CHECK(ar_skill_format(out, 8, "", AR_SKILL_NORMAL) < 0, "too small: refused");

	/* load: settings first, then the configuration, then easy */
	ar_host_set_settings("");
	ar_host_cfg_skill = -1;
	ar__skill_load();
	CHECK(ar_skill() == AR_SKILL_EASY, "the default is easy");
	ar_host_cfg_skill = AR_SKILL_HARD;
	ar__skill_load();
	CHECK(ar_skill() == AR_SKILL_HARD, "apps.arcade.skill when nothing is saved");
	ar_host_set_settings("skill normal\n");
	ar__skill_load();
	CHECK(ar_skill() == AR_SKILL_NORMAL, "a saved choice beats the configuration");
	ar_skill_set(AR_SKILL_EASY);
	CHECK(strcmp(ar_host_settings(), "skill easy\n") == 0, "set writes it back");
	CHECK(ar_skill_table(3) == 19, "easy tables are mode + 16");
	ar_skill_set(AR_SKILL_NORMAL);
	CHECK(ar_skill_table(3) == 3, "normal keeps the old tables");
	ar_skill_set(AR_SKILL_HARD);
	CHECK(ar_skill_table(3) == 35, "hard + 32");
	ar_skill_set(7);
	CHECK(ar_skill() == AR_SKILL_HARD, "out of range: ignored");
	CHECK(strcmp(ar_skill_name(AR_SKILL_EASY), "EASY") == 0, "names");
	n = ar_host_says;
	ar_say("Easy");
	CHECK(ar_host_says == n + 1 && strcmp(ar_host_said, "Easy") == 0, "ar_say speaks");
	ar_say("");
	CHECK(ar_host_says == n + 1, "nothing to say: silent");
	ar_host_set_settings("");
	ar_host_cfg_skill = -1;
	ar__skill_load();
}

static void test_helpers(void) {
	ar_ink_t k;
	char b[16];
	ar_ink_level(&k, 0);
	CHECK(k.row[0] == 0 && k.row[7] == 0, "level 0 is black");
	ar_ink_level(&k, 16);
	CHECK(k.row[0] == 0xff && k.row[5] == 0xff, "level 16 is white");
	ar_ink_level(&k, 8);
	{
		int i, n = 0;
		for (i = 0; i < 8; i++) { uint8_t v = k.row[i]; while (v) { n += v & 1; v >>= 1; } }
		CHECK(n == 32, "level 8 is half (%d)", n);
	}
	CHECK(ar_rgb_mix(0x000, 0xfff, 256) == 0xfff, "mix to the end");
	CHECK(ar_rgb_mix(0x0f0, 0xf00, 0) == 0x0f0, "mix at the start");
	CHECK(ar_rgb_mix(0x000, 0xeee, 128) == 0x777, "mix halfway (%03x)", ar_rgb_mix(0x000, 0xeee, 128));
	ar_utoa(b, 42, 6, '0');
	CHECK(!strcmp(b, "000042"), "utoa pads (%s)", b);
	ar_utoa(b, 0, 0, ' ');
	CHECK(!strcmp(b, "0"), "utoa zero");
	ar_utoa(b, 4294967295u, 0, ' ');
	CHECK(!strcmp(b, "4294967295"), "utoa max");
	{
		int i, hist[5] = { 0 };
		for (i = 0; i < 5000; i++) hist[ar_rand_below(5)]++;
		for (i = 0; i < 5; i++) CHECK(hist[i] > 850 && hist[i] < 1150, "rand_below even (%d)", hist[i]);
	}
	{
		ar_repeat_t r = { 0, false };
		int i, fired = 0;
		for (i = 0; i < 40; i++) fired += ar_repeat(&r, true, 10, 5);
		CHECK(fired == 1 + 6, "repeat: the press, then every 5 after 10 (%d)", fired);
		CHECK(!ar_repeat(&r, false, 10, 5) && ar_repeat(&r, true, 10, 5), "release resets");
	}
}

static void test_copy(void) {
	static uint8_t a[AR_HOST_FB_BYTES], b[AR_HOST_FB_BYTES];
	int mode;
	for (mode = 0; mode < 3; mode++) {
		if (mode == 0) ar_set_fullscreen(false);
		else { ar_set_fullscreen(true); ar_set_color(mode == 2); }
		ar_host_cover(0, 0, 0, 0);
		/* a column of cells, copied down by 8 (overlapping) must equal
		 * the model moved down by 8 and repainted */
		memset(model, 0, sizeof(model));
		{
			int y;
			for (y = 2; y < 20; y++) model[y][5] = (uint8_t)(1 + y % 15);
		}
		ar__invalidate();
		ar_host_render_only();
		CHECK(ar_copy(40, 16, 40, 24, 8, 8 * 18), "mode %d: copy allowed", mode);
		{
			int y;
			for (y = 19; y >= 2; y--) model[y + 1][5] = model[y][5];
		}
		ar_host_fb_copy(a);
		ar_dirty(0, 0, 40, 8);  /* the text row is unaffected; nothing to mark */
		ar__invalidate();
		ar_host_render_only();
		ar_host_fb_copy(b);
		{
			/* compare only the column */
			int x, y, d = 0;
			for (y = 3 * 8; y < 21 * 8; y++)
				for (x = 40; x < 48; x++) {
					int k;
					for (k = 0; k < (mode == 2 ? 4 : 1); k++) {
						int px = ar__s.ox + x + ((k & 1) ? 320 : 0);
						int py = ar__s.oy + y + ((k & 2) ? 240 : 0);
						d += a[py * 640 + px] != b[py * 640 + px];
					}
				}
			CHECK(d == 0, "mode %d: overlapping copy down is exact (%d pixels)", mode, d);
		}
	}
	ar_set_fullscreen(false);
	ar_host_cover(100, 100, 50, 50);
	CHECK(!ar_copy(0, 0, 0, 8, 8, 8), "a covered window refuses a copy");
	ar_host_cover(0, 0, 0, 0);
}

static void test_input(void) {
	ar_event_t e;
	while (ar__dequeue(&e)) ;
	ar__key(0x110002u, true, 0);         /* left arrow */
	ar__key('A', true, 0);               /* shift+a: player 1 left */
	CHECK(ar_held(0) == AR_BTN_LEFT, "arrows are player 0");
	CHECK(ar_held(1) == AR_BTN_LEFT, "A is player 1's left, any case");
	CHECK(ar_pressed(0) & AR_BTN_LEFT, "and it was a press this frame");
	ar__key('a', false, 0);              /* released without shift */
	CHECK(ar_held(1) == 0, "released whatever the case");
	{
		int keys = 0, buttons = 0;
		while (ar__dequeue(&e)) {
			if (e.type == AR_EV_KEY) keys++;
			if (e.type == AR_EV_BUTTON) buttons++;
		}
		CHECK(keys == 3 && buttons == 3, "key and button events (%d, %d)", keys, buttons);
	}
	ar__frame_input();
	CHECK(ar_pressed(0) == 0, "a new frame forgets the press");
	CHECK(ar_held(0) == AR_BTN_LEFT, "but not the hold");
	ar__key(0x110002u, false, 0);
}

static void test_sfx(void) {
	static const zsfx_seg_t seg[] = {
		{ ZSFX_SQUARE, 3, 440, 880, 200, 100 },
		{ ZSFX_NOISE, 2, 1000, 1000, 100, 0 },
	};
	static const zsfx_fx_t fx = { 1, 2, seg };
	static const char *const rows[16] = {
		"C-4 E-4 A-2 K--", "--- --- --- H--", "=== --- --- S--", "", "", "", "", "",
		"", "", "", "", "", "", "", "A-4"
	};
	static const zsfx_song_t song = {
		480, 4, { { ZSFX_SQUARE, 20, 240, 0 }, { ZSFX_TRI, 20, 240, 0 },
		{ ZSFX_TRI, 20, 240, 0 }, { ZSFX_DRUMS, 255, 0, 0 } }, rows, 1, "0"
	};
	uint32_t notes;
	int i;

	CHECK(zsfx_parse_cell("A-4") == 44000, "A-4 is 440Hz");
	CHECK(zsfx_parse_cell("A-5") == 88000, "an octave up doubles");
	CHECK(zsfx_parse_cell("C#4") == 27718, "sharps");
	CHECK(zsfx_parse_cell("---") == ZSFX_CELL_HOLD, "hold");
	CHECK(zsfx_parse_cell("===") == ZSFX_CELL_OFF, "off");
	CHECK(zsfx_parse_cell("S--") == ZSFX_CELL_DRUM + 1, "snare");
	CHECK(zsfx_parse_cell("B#4") == zsfx_parse_cell("C-5"), "B# is C");
	CHECK(zsfx_order_len("01 2a") == 4 && zsfx_order_at("01 2a", 3) == 10, "orders");

	CHECK(zsfx_init(), "init on the host");
	notes = zsfxh_notes;
	zsfx_play(&fx, 0);
	CHECK(zsfxh_notes == notes + 1, "an effect starts a note");
	for (i = 0; i < 10; i++) zsfx_tick(1);
	CHECK(zsfxh_notes == notes + 2, "and its second segment, a new wave, another");
	/* four effects at once: channels 4..7 */
	notes = zsfxh_notes;
	for (i = 0; i < 6; i++) zsfx_play(&fx, 0);
	CHECK(zsfxh_notes == notes + 6, "more effects than channels steal the oldest");

	zsfx_music_enable(true);
	notes = zsfxh_notes;
	zsfx_music_play(&song);
	zsfx_tick(1);
	CHECK(zsfxh_notes >= notes + 4, "row 0: three notes and a kick (%u)", zsfxh_notes - notes);
	for (i = 0; i < 200; i++) zsfx_tick(1);
	CHECK(zsfxh_peak[0] <= 20, "music gains are the instrument's (%d)", zsfxh_peak[0]);
	zsfx_music_enable(false);
	notes = zsfxh_notes;
	for (i = 0; i < 100; i++) zsfx_tick(1);
	CHECK(zsfxh_notes == notes, "music off: no notes");
	zsfx_suspend(true);
	zsfx_play(&fx, 0);
	CHECK(zsfxh_notes == notes, "suspended: no effects");
	zsfx_suspend(false);

	/* layers: row 0 has three notes and a kick */
	zsfx_music_enable(true);
	zsfx_music_layers(0x5, 0x0);
	notes = zsfxh_notes;
	zsfx_music_play(&song);
	zsfx_tick(1);
	CHECK(zsfxh_notes == notes + 2, "layers 0x5, no drums: two notes (%u)", zsfxh_notes - notes);
	zsfx_music_layers(0x0f, 0x0f);
	notes = zsfxh_notes;
	zsfx_music_play(&song);
	zsfx_tick(1);
	CHECK(zsfxh_notes >= notes + 4, "everything again (%u)", zsfxh_notes - notes);
	zsfx_music_intensity(0);
	notes = zsfxh_notes;
	zsfx_music_play(&song);
	zsfx_tick(1);
	CHECK(zsfxh_notes == notes + 2, "intensity 0: tune and bass (%u)", zsfxh_notes - notes);
	zsfx_music_intensity(4);
	notes = zsfxh_notes;
	zsfx_music_play(&song);
	zsfx_tick(1);
	CHECK(zsfxh_notes >= notes + 4, "intensity 4 has the kick (%u)", zsfxh_notes - notes);
	zsfx_music_intensity(ZSFX_INTENSITY_FULL);
	zsfx_music_tempo(100);
	zsfx_suspend(false);
	zsfx_shutdown();
}

/* 64 and 256 colours: an index's bits land in planes 4..7, in the
 * second sheet, and a 6-plane board shows 64 of 256. */
static int index_at(const uint8_t *fbuf, int x, int y, int planes) {
	int k, idx = 0;
	for (k = 0; k < planes; k++) {
		int dx, dy;
		ar__plane_off(k, &dx, &dy);
		idx |= fbuf[(y + dy) * 640 + x + dx] << k;
	}
	return idx;
}

static void test_colours(void) {
	static uint8_t f[AR_HOST_FB_BYTES];
	static uint8_t to16[256];
	int i;

	ar_set_fullscreen(true);
	ar_set_color(true);
	CHECK(ar_set_colours(256) == 256, "256 colours on an 8-plane host");
	ar_host_render_only();
	ar_fill(8, 8, 4, 4, 0xa5);
	ar_fill(20, 8, 4, 4, 0xff);
	ar_host_fb_copy(f);
	CHECK(index_at(f, 9, 9, 8) == 0xa5, "index 0xa5 in eight planes (%#x)", index_at(f, 9, 9, 8));
	CHECK(index_at(f, 21, 9, 8) == 0xff, "and 255");
	ar_pal(200, 0x123);
	CHECK(ar_pal_get(200) == 0x123 && ar_pal_get(216) == desc.palette[216 & 15],
		"256 palette entries, the rest from the first 16");

	setenv("AR_HOST_PLANES", "6", 1);
	CHECK(ar_set_colours(256) == 64, "a 6-plane board shows 64");
	ar_host_render_only();
	ar_fill(8, 8, 4, 4, 0x2a);
	ar_host_fb_copy(f);
	CHECK(index_at(f, 9, 9, 6) == 0x2a, "index 0x2a in six planes (%#x)", index_at(f, 9, 9, 6));
	unsetenv("AR_HOST_PLANES");
	one_mode("full screen, 256 colours", 2, false);
	CHECK(ar_set_colours(16) == 16, "back to 16");

	/* monochrome: index 40's ink through the map */
	for (i = 0; i < 256; i++) to16[i] = (uint8_t)(i & 15);
	to16[40] = 15;
	desc.ink[15] = ar_ink_white;                    /* 40 & 15 = 8 stays black */
	ar_mono_map(to16);
	ar_set_color(false);
	ar_host_render_only();
	ar_fill(8, 8, 8, 8, 40);
	ar_host_fb_copy(f);
	CHECK(f[8 * 640 + 8] == 1, "index 40 drawn in ink 15, white");
	ar_mono_map(NULL);
	ar_host_render_only();
	ar_fill(8, 8, 8, 8, 40);
	ar_host_fb_copy(f);
	CHECK(f[8 * 640 + 8] == 0, "and without the map in ink 8, black");
	desc.ink[15] = desc.ink[0];
	ar_set_fullscreen(false);
}

/* Planes as layers: ar_plane() is one pass where the blitter can
 * (never blank in between); a line drawn a band of rows at a time
 * (ar_plane_line_rows()) is the same pixels as the whole line. */
static void test_planes(void) {
	static uint8_t a[AR_HOST_FB_BYTES], b[AR_HOST_FB_BYTES];
	static uint32_t bits[AR_H * (AR_W / 32)];
	int i, y;
	uint64_t ops;
	ar_set_fullscreen(true);
	ar_set_color(true);
	CHECK(ar_set_colours(256) == 256, "256 colours");
	ar_host_render_only();
	for (i = 0; i < (int)(sizeof(bits) / 4); i++) bits[i] = 0x9a3c5af0u * (uint32_t)(i + 1);
	ar_plane_fill(3, 0, 0, AR_W, AR_H, 1);
	ops = arh_ops;
	ar_plane(3, bits, AR_W / 8, 5, 3, 17, 11, 101, 77);
	CHECK(arh_ops - ops == 1, "a plane, opaque, in one pass (%d)", (int)(arh_ops - ops));
	ar_host_fb_copy(a);
	ar_plane_fill(3, 0, 0, AR_W, AR_H, 1);
	ar_plane_fill(3, 17, 11, 101, 77, 0);
	ar_plane_or(3, bits, AR_W / 8, 5, 3, 17, 11, 101, 77);
	ar_host_fb_copy(b);
	CHECK(!ar_host_fb_diff(a, b), "and the same as cleared, then ORed");

	for (i = 0; i < 40; i++) {
		int x0 = (int)(ar_rand() % 400) - 40, y0 = (int)(ar_rand() % 320) - 40;
		int x1 = (int)(ar_rand() % 400) - 40, y1 = (int)(ar_rand() % 320) - 40;
		ar_plane_fill(5, 0, 0, AR_W, AR_H, 0);
		ar_plane_line(5, x0, y0, x1, y1, 1);
		ar_host_fb_copy(a);
		ar_plane_fill(5, 0, 0, AR_W, AR_H, 0);
		for (y = 0; y < AR_H; y += 30) ar_plane_line_rows(5, x0, y0, x1, y1, 1, y, y + 29);
		ar_host_fb_copy(b);
		CHECK(!ar_host_fb_diff(a, b), "a line a band at a time (%d,%d)-(%d,%d)", x0, y0, x1, y1);
	}
	ar_set_colours(16);
	ar_set_fullscreen(false);
}

/* -- lines in black and white: the window, covered or not, against
 * mono full screen -- the same pixels, less what is covered. XOR, so
 * the line's pixels are the ones that changed. */
static void lit_map(uint8_t *m) {
	int x, y;
	for (y = 0; y < AR_H; y++)
		for (x = 0; x < AR_W; x++) m[y * AR_W + x] = (uint8_t)ar_host_lit(x, y, 1, 1);
}

static void test_mono_lines(void) {
	static uint8_t a0[AR_W * AR_H], a1[AR_W * AR_H], b0[AR_W * AR_H], b1[AR_W * AR_H];
	int i, j, bad;
	for (i = 0; i < 12; i++) {
		int x0 = (int)(ar_rand() % 400) - 40, y0 = (int)(ar_rand() % 320) - 40;
		int x1 = (int)(ar_rand() % 400) - 40, y1 = (int)(ar_rand() % 320) - 40;
		ar__s.want_color = false;
		ar_set_fullscreen(true);
		ar_host_render_only();
		lit_map(a0);
		ar_plane_line(0, x0, y0, x1, y1, 2);
		lit_map(a1);
		ar_set_fullscreen(false);
		ar_host_cover(i & 1 ? 100 : 0, 50, i & 1 ? 120 : 0, 100);
		ar_host_render_only();
		lit_map(b0);
		ar_plane_line(0, x0, y0, x1, y1, 2);
		lit_map(b1);
		bad = 0;
		for (j = 0; j < AR_W * AR_H; j++) {
			int x = j % AR_W, y = j / AR_W;
			bool hid = (i & 1) && x >= 100 && x < 220 && y >= 50 && y < 150;
			bool want = !hid && a0[j] != a1[j];
			bad += want != (b0[j] != b1[j]);
		}
		CHECK(bad == 0, "a line in the window%s, black and white: %d pixels wrong",
			i & 1 ? ", covered" : "", bad);
		ar_host_cover(0, 0, 0, 0);
	}
	ar_plane_line(1, 0, 0, 100, 100, 1);    /* no plane 1 in black and white */
}

/* -- colour in the window (docs/color.md, "The colour window") --
 *
 * The same picture drawn full screen in colour and in a colour window
 * must be the same indices pixel for pixel: full screen read through
 * the quadrants, the window through its own pixels (bit 0) and the
 * folded planes in the second sheet. The host's window sits at
 * (40, 60), so the fold wraps inside it both ways -- columns at
 * logical x 280 (screen 320 + 320 = 640), rows at logical y 180
 * (screen 240 + 240 = 480) -- and fills, sprites and text are cut
 * there. No patterns: they are anchored to the screen, and the window
 * is not where full screen is. */
static uint8_t wc_cell[AR_H / 8][AR_W / 8];

static void paint_wc(void *user, const ar_rect_t *r) {
	int cx, cy;
	(void)user;
	for (cy = r->y / 8; cy < (r->y + r->h + 7) / 8 && cy < AR_H / 8; cy++)
		for (cx = r->x / 8; cx < (r->x + r->w + 7) / 8 && cx < AR_W / 8; cx++)
			ar_fill(cx * 8, cy * 8, 8, 8, wc_cell[cy][cx]);
	ar_fill(270, 170, 23, 19, 13);             /* across both folds */
	ar_sprite(&spr[0], 276, 176);
	ar_sprite(&spr[1], 100, 177);
	ar_text(240, 100, "ACROSS THE FOLD", 9, &z_font_8x8, 1);
}

/* The same with the plane-at-a-time calls on top, across the folds:
 * a plane opaque, a plane filled, ORed and cleared through a mask, and
 * lines in each plane (some a band at a time). */
static uint32_t wc_bits[AR_H * (AR_W / 32)];

static void paint_wcp(void *user, const ar_rect_t *r) {
	int k;
	paint_wc(user, r);
	ar_plane(1, wc_bits, AR_W / 8, 3, 5, 230, 150, 81, 63);
	ar_plane_fill(2, 260, 20, 40, 200, 1);
	ar_plane_fill(0, 10, 172, 300, 12, 0);
	ar_plane_or(3, wc_bits, AR_W / 8, 0, 0, 200, 160, 110, 70);
	ar_plane_andn(2, wc_bits, AR_W / 8, 7, 1, 250, 100, 60, 120);
	for (k = 0; k < 4; k++) {
		ar_plane_line(k, 5 + k * 9, 230, 315, 10 + k * 13, 2);
		ar_plane_line(k, 279 - k, 0, 281 + k, 239, 1);
		ar_plane_line_rows(k, 0, 179 + k, 319, 181 - k, 1, 175, 185);
		ar_plane_line_rows(k, 0, 179 + k, 319, 181 - k, 1, 186, 239);
	}
}

static void decode(const uint8_t *fb, bool window, uint8_t *out) {
	int x, y, k;
	for (y = 0; y < AR_H; y++)
		for (x = 0; x < AR_W; x++) {
			int idx = 0;
			if (!window) {
				for (k = 0; k < 4; k++) {
					int dx, dy;
					ar__plane_off(k, &dx, &dy);
					idx |= fb[(y + dy) * 640 + x + dx] << k;
				}
			} else {
				int px = 40 + x, py = 60 + y;
				idx = fb[py * 640 + px];
				for (k = 1; k < ar__s.wl; k++) {
					int j = k - 1;
					int fx = (px + ((j & 1) ? 320 : 0)) % 640;
					int fy = 480 + ((py + ((j & 2) ? 240 : 0)) % 480) % ar__s.rows1;
					idx |= fb[fy * 640 + fx] << k;
				}
			}
			out[y * AR_W + x] = (uint8_t)idx;
		}
}

static void test_window_colour(void) {
	static uint8_t fb[AR_HOST_FB_BYTES], a[AR_W * AR_H], b[AR_W * AR_H];
	ar_desc_t d = desc;
	const ar_desc_t *was = ar__s.d;
	int i, diff = 0, x, y;

	for (y = 0; y < AR_H / 8; y++)
		for (x = 0; x < AR_W / 8; x++)
			wc_cell[y][x] = (uint8_t)ar_rand_below(16);
	d.paint = paint_wc;
	ar__s.d = &d;

	ar__s.want_color = true;
	ar_set_fullscreen(true);
	ar_set_color(true);
	ar__invalidate();
	ar_host_render_only();
	ar_host_fb_copy(fb);
	decode(fb, false, a);

	ar_set_fullscreen(false);
	ar__wcolor(true, 4, 480);
	CHECK(ar_color() && ar_mode() == AR_MODE_WINDOW && ar_colours() == 16,
		"window colour: ar_color() %d, mode %d, %d colours",
		ar_color(), ar_mode(), ar_colours());
	ar_host_render_only();
	ar_host_fb_copy(fb);
	decode(fb, true, b);
	for (i = 0; i < AR_W * AR_H; i++) diff += a[i] != b[i];
	CHECK(diff == 0, "window colour: %d pixels differ from full screen", diff);

	/* six planes: two window planes, 8 colours: the low three bits */
	ar__wcolor(true, 2, 240);
	CHECK(ar_colours() == 8, "six planes: %d colours", ar_colours());
	ar_host_render_only();
	ar_host_fb_copy(fb);
	decode(fb, true, b);
	diff = 0;
	for (i = 0; i < AR_W * AR_H; i++) diff += (a[i] & 7) != b[i];
	CHECK(diff == 0, "window colour, six planes: %d pixels differ", diff);

	/* the plane-at-a-time calls */
	for (i = 0; i < (int)(sizeof(wc_bits) / 4); i++)
		wc_bits[i] = 0x6b3c5a91u * (uint32_t)(i + 7);
	d.paint = paint_wcp;
	ar__wcolor(false, 0, 0);
	ar_set_fullscreen(true);
	ar__invalidate();
	ar_host_render_only();
	ar_host_fb_copy(fb);
	decode(fb, false, a);
	ar_set_fullscreen(false);
	ar__wcolor(true, 4, 480);
	ar_host_render_only();
	ar_host_fb_copy(fb);
	decode(fb, true, b);
	diff = 0;
	for (i = 0; i < AR_W * AR_H; i++) diff += a[i] != b[i];
	CHECK(diff == 0, "window colour, planes and lines: %d pixels differ", diff);
	ar__wcolor(true, 2, 240);
	ar_host_render_only();
	ar_host_fb_copy(fb);
	decode(fb, true, b);
	diff = 0;
	for (i = 0; i < AR_W * AR_H; i++) diff += (a[i] & 7) != b[i];
	CHECK(diff == 0, "six planes, planes and lines: %d pixels differ", diff);
	ar_plane_fill(3, 0, 0, 10, 10, 1);       /* no plane 3 there: nothing */
	{
		static uint8_t fb2[AR_HOST_FB_BYTES];
		ar_host_fb_copy(fb2);
		CHECK(!memcmp(fb2, fb, sizeof(fb)), "six planes: plane 3 draws nothing");
	}

	ar__wcolor(false, 0, 0);
	CHECK(!ar_color(), "window colour off");
	ar__s.d = was;
	ar__invalidate();
}

int main(void) {

	ar_init(&desc);
	CHECK(ar_sprite_make(&spr[0], ball, 6, 6, NULL), "ball");
	CHECK(ar_sprite_make(&spr[1], ship, 7, 4, NULL), "ship");

	one_mode("window", 0, false);
	one_mode("window, covered", 0, true);
	one_mode("full screen mono", 1, false);
	one_mode("full screen colour", 2, false);
	one_mode("window colour", 3, false);
	ar__wcolor(false, 0, 0);

	test_sprites();
	test_colours();
	test_planes();
	test_scores();
	test_skill();
	test_helpers();
	test_copy();
	test_mono_lines();
	test_window_colour();
	test_input();
	test_sfx();

	printf("zarcade: %d checks, %d failed\n", checks, fails);
	return fails ? 1 : 0;
}
