/*
 * mausrun3d on a build host: the game itself, driven by a script, its
 * frames drawn by T2's model and timed on the geometry core's ISS.
 * make -C sw/apps/mausrun3d render
 *
 * mausrun3d.c is compiled with -DAR_HOST_TESTS -Dmain=game_main and
 * runs on the arcade runtime's host half (ar_host.c) with chunky game
 * mode on the "board". zgl's chunky driver is stood in for here: T2's
 * stream is collected a frame at a time and
 *
 *   - carried out by T2's model (the geometry firmware, t2fw.c, and the
 *     raster's model) into the chunky picture the shots are taken of;
 *   - run on zeitlos32g's instruction-set simulator (t2g_iss.c) with
 *     the real firmware (sw/t2fw/t2fw.bin): the geometry core's cycles.
 *
 * At the end, what a frame cost: the geometry core's cycles and the
 * raster's and output's estimate (the model's), against 60 fps at 48
 * MHz (800,000 cycles); the triangles sent and drawn; the records.
 *
 *   tests/render_mausrun3d t2 out      (soft: zgl's software strips)
 */

#undef main

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../../common/games/tests/ar_host.h"
#include "../../../common/zgl/zgl.h"
#include "../../../common/zgl/zgl_t2.h"
#include "../../../common/zgl/tests/t2g_iss.h"

int game_main(void);

static int engine;
static uint32_t *words;
static uint32_t nwords;
static bool have_fw;
static t2g_run_t iss;

int zglc_open(uint8_t flags) { return (flags & ZGL_NO_ENGINE) ? 0 : engine; }
volatile uint32_t *zglc_cmd(void) { return NULL; }
void zglc_put(uint32_t w) { if (nwords < (1u << 22)) words[nwords++] = w; }
void zglc_finish(void) { }
/* T2's texture memory: the model's */
uint32_t zglc_tex_room(void) { return engine == 2 ? 1u << T2_TEX_LOG2 : 0; }
void zglc_tex_put(uint32_t word, uint32_t v) { t2_model_tex(word, v); }
void zglc_strip(int32_t y0, int rows, int32_t cx0, int32_t cx1, const uint8_t *col) {
	int r;
	for (r = 0; r < rows; r++)
		memcpy(&ar_host_chunky[y0 + r][cx0], &col[r * 320 + cx0], (size_t)(cx1 - cx0 + 1));
}

/* what the frames cost */
static uint32_t frames, refused, over;
static uint64_t sum_geom, sum_rast, sum_tris, sum_drawn, sum_words, sum_culled, sum_clipped, sum_verts, sum_outside;
static uint32_t max_geom, max_rast, max_tris, max_drawn, max_words, max_recs, overflows;

#define BUDGET 800000u          /* 48 MHz / 60 */

static void t2_account(uint32_t frame) {
	uint32_t at = 0;
	(void)frame;
	while (at < nwords) {
		t2_stats_t st;
		uint32_t used, geom = 0, rast;
		if (t2_model_run(words + at, nwords - at, ar_host_chunky, &st) < 0) {
			refused++;
			break;
		}
		used = t2_model_used();
		if (have_fw) {
			uint64_t c0 = iss.cycles;
			iss.in = words + at;
			iss.in_n = used;
			iss.in_i = 0;
			t2g_run(&iss);
			geom = (uint32_t)(iss.cycles - c0);
		} else
			geom = st.cyc_geom;
		rast = st.cyc_raster + st.cyc_out;
		frames++;
		sum_geom += geom; sum_rast += rast;
		sum_tris += st.tris + st.cpu_tris; sum_drawn += st.drawn;
		sum_words += used; sum_culled += st.culled; sum_clipped += st.clipped; sum_verts += st.verts; sum_outside += st.outside;
		if (geom > max_geom) max_geom = geom;
		if (rast > max_rast) max_rast = rast;
		if (st.tris + st.cpu_tris > max_tris) max_tris = st.tris + st.cpu_tris;
		if (st.drawn > max_drawn) max_drawn = st.drawn;
		if (used > max_words) max_words = used;
		if (st.drawn + st.lines > max_recs) max_recs = st.drawn + st.lines;
		overflows += st.overflows;
		if (st.overflows && getenv("SHOW_OVER")) printf("  dropped: frame %u, %u records dropped, %u tris in, %u lines\n", frame, st.overflows, st.tris, st.lines);
		if ((geom > rast ? geom : rast) > BUDGET) {
			over++;
			if (getenv("SHOW_OVER")) printf("  over: frame %u, %u cycles, %u in, %u verts, %u drawn, %u clipped, %u outside\n", frame, geom, st.tris, st.verts, st.drawn, st.clipped, st.outside);
		}
		at += used;
	}
	nwords = 0;
}

/* -- the game, steered: the mouse put where things are, and checked -- */

bool mr3_test_find(char c, int nth, int32_t *x, int32_t *y, int32_t *z);
bool mr3_test_qbox(int nth, int *tx, int *ty, int *tz);
void mr3_test_place(int32_t x, int32_t y, int32_t z, uint32_t face);
void mr3_test_status(int *h, int *ch, int *gs, int *st, int *pw);
void mr3_test_hearts(int h);
uint8_t mr3_test_block(int x, int y, int z);
bool mr3_test_chasm(int nth, int *tx, int *tz);
static int qx, qy, qz;

#define TQ (16 * 256)
static int bad;
#define CHECK(c, ...) do { if (!(c)) { bad++; printf("FAIL: " __VA_ARGS__); printf("\n"); } \
	else { printf("ok: " __VA_ARGS__); printf("\n"); } } while (0)

static int h0, c0;

/* put the mouse at the nth of a kind, dx dz tiles off, dy tiles up */
static bool go(char c, int nth, int dx, int dy, int dz, uint32_t face) {
	int32_t x, y, z;
	if (!mr3_test_find(c, nth, &x, &y, &z)) { printf("render_mausrun3d: no '%c' %d\n", c, nth); bad++; return false; }
	mr3_test_place(x + dx * TQ, y + dy * TQ, z + dz * TQ, face);
	return true;
}

static void status(int *h, int *ch, int *gs, int *st, int *pw) { mr3_test_status(h, ch, gs, st, pw); }

static void actions(uint32_t f) {
	int h, ch, gs, st, pw;
	status(&h, &ch, &gs, &st, &pw);
	if (getenv("TRACE") && f >= 280 && f <= 350) printf("frame %u: state %d hearts %d\n", f, gs, h);
	switch (f) {
	case 40: go('C', 0, -4, 0, 0, 16384); break;             /* a cat ahead */
	case 50: go('C', 0, 0, 3, 0, 16384); h0 = h; break;       /* onto its head */
	case 64: CHECK(h == h0, "landing on a cat costs nothing (%d hearts)", h); break;
	case 70: go('C', 1, 0, 0, 0, 16384); h0 = h; break;       /* into one */
	case 80: CHECK(h == h0 - 1, "running into a cat costs a heart (%d -> %d)", h0, h); break;
	case 150: {                                               /* under a ? box */
		int tx, ty, tz, y;
		(void)y;
		if (mr3_test_qbox(0, &tx, &ty, &tz)) {
			qx = tx; qy = ty; qz = tz;
			for (y = ty - 1; y >= 0 && !mr3_test_block(tx, y, tz); y--) ;
			printf("render_mausrun3d: ? box at %d %d %d, the floor under it at %d\n", tx, ty, tz, y + 1);
			mr3_test_place(tx * TQ + TQ / 2, (ty - 1) * TQ - 12 * 256, tz * TQ + TQ / 2, 16384);
		} else { printf("render_mausrun3d: no ? box\n"); bad++; }
		pw = 0;
		break;
	}
	case 210: CHECK(mr3_test_block(qx, qy, qz) == 2, "a ? box bumped from below is a box (block %d)", mr3_test_block(qx, qy, qz)); break;
	case 220: c0 = ch; go('c', 0, 0, 0, 0, 16384); break;
	case 226: CHECK(ch == c0 + 1, "a cheese taken (%d -> %d)", c0, ch); break;
	case 230: go('H', 1, 0, 0, 0, 16384); break;
	case 250: go('*', 0, 0, 1, 0, 16384); break;
	case 262: CHECK(st, "the hidden star found"); break;
	case 280: go('G', 0, -4, 0, 0, 16384); break;
	case 330: CHECK(gs == 2, "the cheese wheel: WELL DONE (state %d)", gs); break;
	case 380: CHECK(gs == 0 && h == 3, "SPACE: the level again, the hearts back (state %d, %d hearts)", gs, h); break;
	case 600: CHECK(gs == 1, "the level card gone after three seconds (state %d)", gs); h0 = h; break;
	case 610: go('H', 0, 0, 0, 0, 16384); break;              /* a checkpoint, */
	case 620: {                                               /* then off the edge of the world */
		int tx, tz;
		if (mr3_test_chasm(0, &tx, &tz)) mr3_test_place(tx * TQ + TQ / 2, 6 * TQ, tz * TQ + TQ / 2, 16384);
		else { printf("render_mausrun3d: no chasm\n"); bad++; }
		break;
	}
	case 700: CHECK(h == h0 - 1, "a fall costs a heart and goes back to the hole (%d -> %d)", h0, h); break;
	case 800: mr3_test_hearts(1); go('C', 1, 0, 0, 0, 16384); break;
	case 820: CHECK(gs == 3, "the last heart: GAME OVER (state %d)", gs); break;
	}
}

void ar_host_hook(uint32_t frame) {
	actions(frame);
	t2_account(frame);
}

#define T(f, k)         { (f), ARH_TAP, (k), 0, NULL, 0, 0, 0, 0 }
#define DOWN(f, k)      { (f), ARH_KEY_DOWN, (k), 0, NULL, 0, 0, 0, 0 }
#define UP(f, k)        { (f), ARH_KEY_UP, (k), 0, NULL, 0, 0, 0, 0 }
#define SHOT(f, n)      { (f), ARH_SHOT, 0, 0, (n), 0, 0, 0, 0 }
#define END(f)          { (f), ARH_END, 0, 0, NULL, 0, 0, 0, 0 }

static const ar_host_step_t script[] = {
	SHOT(10, "01_level_card"),
	T(14, ' '),
	DOWN(16, ARH_K_UP), UP(38, ARH_K_UP),
	SHOT(36, "02_running"),
	SHOT(46, "03_a_cat_ahead"),
	SHOT(60, "04_on_its_head"),
	SHOT(78, "05_into_a_cat"),
	T(172, ' '),
	SHOT(190, "06_a_qbox_bumped"),
	DOWN(192, 'q'), UP(206, 'q'),
	SHOT(208, "07_camera_round"),
	SHOT(236, "08_a_mouse_hole"),
	SHOT(256, "09_the_star"),
	DOWN(282, ARH_K_UP), UP(320, ARH_K_UP),
	SHOT(300, "10_the_wheel"),
	SHOT(326, "11_well_done"),
	T(360, ' '),
	SHOT(370, "12_the_level_again"),
	SHOT(640, "13_falling"),
	SHOT(706, "14_back_at_the_hole"),
	SHOT(840, "15_game_over"),
	END(850)
};

int main(int argc, char **argv) {
	const char *dir = argc > 2 ? argv[2] : "out";
	engine = argc > 1 && !strcmp(argv[1], "soft") ? 0 : 2;
	words = malloc((1u << 22) * 4);
	setvbuf(stdout, NULL, _IOLBF, 0);
	have_fw = engine == 2 && t2g_load("../../t2fw/t2fw_tex.bin") == 0;
	if (have_fw) t2g_reset();
	else if (engine == 2) printf("render_mausrun3d: no sw/t2fw/t2fw_tex.bin: the model's estimate for geometry\n");
	ar_host_chunky_avail = true;
	ar_host_run(script, dir);
	game_main();
	printf("render_mausrun3d: %d pictures\n", ar_host_shots());
	if (frames) {
		printf("render_mausrun3d: %u T2 frames; a frame, average and most:\n", frames);
		printf("  geometry core   %8llu %8u cycles%s\n", (unsigned long long)(sum_geom / frames), max_geom,
			have_fw ? " (zeitlos32g's ISS, the real firmware)" : " (the model's estimate)");
		printf("  raster, output  %8llu %8u cycles (the model's estimate)\n", (unsigned long long)(sum_rast / frames), max_rast);
		printf("  triangles in    %8llu %8u\n", (unsigned long long)(sum_tris / frames), max_tris);
		printf("  drawn           %8llu %8u (records at most %u of %d)\n", (unsigned long long)(sum_drawn / frames), max_drawn, max_recs, T2_RECS);
		printf("  stream words    %8llu %8u\n", (unsigned long long)(sum_words / frames), max_words);
		printf("  vertices %llu, culled (outside or facing away) %llu, clipped %llu\n",
			(unsigned long long)(sum_verts / frames), (unsigned long long)(sum_culled / frames),
			(unsigned long long)(sum_clipped / frames));
		printf("  of the culled, wholly outside the view %llu\n", (unsigned long long)(sum_outside / frames));
		printf("  over 60 fps's %u cycles: %u frames; records dropped: %u\n", BUDGET, over, overflows);
	}
	if (have_fw && getenv("T2G_PROF")) {
		extern uint64_t t2g_prof[];
		FILE *f = fopen(getenv("T2G_PROF"), "w");
		unsigned k;
		for (k = 0; k < 3072; k++) if (t2g_prof[k]) fprintf(f, "%08x %llu\n", k * 4, (unsigned long long)t2g_prof[k]);
		fclose(f);
	}
	if (refused) printf("render_mausrun3d: the model REFUSED a stream\n");
	printf("render_mausrun3d: %d checks failed\n", bad);
	return ar_host_shots() == 15 && !refused && !bad ? 0 : 1;
}
