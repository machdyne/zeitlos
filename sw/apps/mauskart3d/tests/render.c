/*
 * mauskart3d on a build host: the game itself, driven by a script, its
 * frames drawn by T2's model and timed on the geometry core's ISS.
 * make -C sw/apps/mauskart3d render
 *
 * mauskart3d.c is compiled with -DAR_HOST_TESTS -Dmain=game_main and
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
 *   tests/render_mauskart3d t2 out      (soft: zgl's software strips)
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
static uint64_t sum_t2;
static uint32_t max_t2;
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
		/* T2_DUMP=n: frame n as a bench vector (rtl/gpu/bench/tb_gpu_t2.v:
		 * s20 the stream, p20 the picture in chunky VRAM's order, t20 the
		 * texture memory, r20 the stream's length) in T2_DUMPDIR */
		if (getenv("T2_DUMP") && (uint32_t)atoi(getenv("T2_DUMP")) == frame && at == 0) {
			const char *d = getenv("T2_DUMPDIR") ? getenv("T2_DUMPDIR") : ".";
			char fn[512];
			static uint32_t vr[19200];
			uint32_t i;
			int yy, xx;
			FILE *f;
			snprintf(fn, sizeof(fn), "%s/s20.hex", d);
			f = fopen(fn, "w");
			for (i = 0; i < used; i++) fprintf(f, "%08x\n", words[i]);
			fclose(f);
			for (yy = 0; yy < 240; yy++) for (xx = 0; xx < 320; xx++) {
				uint32_t sg = (uint32_t)xx / 80, rr = (uint32_t)yy + ((sg & 1) ? 240u : 0u) + ((sg & 2) ? 480u : 0u);
				uint32_t b = rr * 80 + (uint32_t)xx % 80;
				vr[b / 4] = (vr[b / 4] & ~(0xffu << (b % 4 * 8))) | (uint32_t)ar_host_chunky[yy][xx] << (b % 4 * 8);
			}
			snprintf(fn, sizeof(fn), "%s/p20.hex", d);
			f = fopen(fn, "w");
			for (i = 0; i < 19200; i++) fprintf(f, "%08x\n", vr[i]);
			fclose(f);
			snprintf(fn, sizeof(fn), "%s/t20.hex", d);
			f = fopen(fn, "w");
			for (i = 0; i < (1u << T2_TEX_LOG2) / 4u; i++) fprintf(f, "%08x\n", t2_model_tex_word(i));
			fclose(f);
			snprintf(fn, sizeof(fn), "%s/r20.hex", d);
			f = fopen(fn, "w");
			fprintf(f, "%08x\n%08x\n", used, 0u);
			fclose(f);
		}
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
		/* one record buffer: the raster starts when the geometry is done,
		 * so a frame is the two together (docs/zgl_t2.md) */
		{
			/* T2_TWO_LISTS=1 in the environment: two record lists, the
			 * longer of the two (gpu_t2.v TWO_LISTS) */
			uint32_t t2 = getenv("T2_TWO_LISTS") ? (geom > rast ? geom : rast) : geom + rast;
			sum_t2 += t2;
			if (t2 > max_t2) max_t2 = t2;
			rast = t2;          /* for the budget, below */
			geom = 0;
		}
		if (geom + rast > BUDGET) {
			over++;
			if (getenv("SHOW_OVER")) printf("  over: frame %u, %u cycles, %u in, %u verts, %u drawn, %u clipped, %u outside\n", frame, geom, st.tris, st.verts, st.drawn, st.clipped, st.outside);
		}
		at += used;
	}
	nwords = 0;
}

/* -- the game, steered -- */

void mk_test_autopilot(bool on);
void mk_test_status(int *state, int *lap, int *place, int *seg, int32_t *v);
static int bad;
#define CHECK(c, ...) do { if (!(c)) { bad++; printf("FAIL: " __VA_ARGS__); printf("\n"); } \
	else { printf("ok: " __VA_ARGS__); printf("\n"); } } while (0)

static int32_t vbest;
int32_t mk_test_across(void);
int mk_test_stun(void);

static void actions(uint32_t f) {
	int st, lap, place, seg;
	int32_t v;
	mk_test_status(&st, &lap, &place, &seg, &v);
	if (f % 120 == 0) printf("frame %u: state %d lap %d place %d seg %d speed %d\n", f, st, lap, place, seg, (int)v);
	if (f == 30) mk_test_autopilot(true);
	if (f == 100) CHECK(st == 1, "the countdown (state %d)", st);
	if (getenv("TRACE") && f >= 200 && f <= 420 && f % 10 == 0) printf("trace %u: seg %d v %d place %d across %d stun %x\n", f, seg, (int)v, place, (int)mk_test_across(), mk_test_stun());
	if (f >= 250 && f < 400 && v > vbest) vbest = v;
	if (f == 400) CHECK(st == 2 && vbest > 1000, "racing, at speed (state %d, at most %d)", st, (int)vbest);
	if (f == 2400) CHECK(lap >= 2, "a lap done (lap %d)", lap);
	if (f == 6590) CHECK(st == 3, "the race over (state %d, lap %d)", st, lap);
}

void ar_host_hook(uint32_t frame) {
	actions(frame);
	t2_account(frame);
}

#define SHOT(f, n)      { (f), ARH_SHOT, 0, 0, (n), 0, 0, 0, 0 }
#define END(f)          { (f), ARH_END, 0, 0, NULL, 0, 0, 0, 0 }

static const ar_host_step_t script[] = {
	SHOT(20, "01_title"),
	SHOT(60, "02_grid"),
	SHOT(160, "03_countdown"),
	SHOT(230, "04_go"),
	SHOT(300, "05_first_bend"),
	SHOT(420, "06_racing"),
	SHOT(540, "07_racing"),
	SHOT(660, "08_racing"),
	SHOT(800, "09_racing"),
	{ 960, ARH_TAP, 's', 0, NULL, 0, 0, 0, 0 },
	SHOT(1000, "10_no_scenery"),
	{ 1040, ARH_TAP, 's', 0, NULL, 0, 0, 0, 0 },
	SHOT(6600, "11_results"),
	END(6610)
};
/* QUICK=1: Sandy Bay (Right on the title), 700 frames */
static const ar_host_step_t quick[] = {
	{ 10, ARH_TAP, ARH_K_RIGHT, 0, NULL, 0, 0, 0, 0 },
	SHOT(20, "01_title_bay"),
	SHOT(60, "02_grid"),
	SHOT(300, "05_first_bend"),
	SHOT(420, "06_racing"),
	SHOT(540, "07_racing"),
	SHOT(660, "08_racing"),
	END(700)
};

int main(int argc, char **argv) {
	const char *dir = argc > 2 ? argv[2] : "out";
	engine = argc > 1 && !strcmp(argv[1], "soft") ? 0 : 2;
	words = malloc((1u << 22) * 4);
	setvbuf(stdout, NULL, _IOLBF, 0);
	have_fw = engine == 2 && t2g_load("../../t2fw/t2fw_tex.bin") == 0;
	if (have_fw) t2g_reset();
	else if (engine == 2) printf("render_mauskart3d: no sw/t2fw/t2fw_tex.bin: the model's estimate for geometry\n");
	ar_host_chunky_avail = true;
	ar_host_run(getenv("QUICK") ? quick : getenv("SHORT") ? quick + 2 : script, dir);
	game_main();
	printf("render_mauskart3d: %d pictures\n", ar_host_shots());
	if (frames) {
		printf("render_mauskart3d: %u T2 frames; a frame, average and most:\n", frames);
		printf("  geometry core   %8llu %8u cycles%s\n", (unsigned long long)(sum_geom / frames), max_geom,
			have_fw ? " (zeitlos32g's ISS, the real firmware)" : " (the model's estimate)");
		printf("  raster, output  %8llu %8u cycles (the model's estimate)\n", (unsigned long long)(sum_rast / frames), max_rast);
		printf("  T2, a frame     %8llu %8u cycles (%s)\n", (unsigned long long)(sum_t2 / frames), max_t2,
			getenv("T2_TWO_LISTS") ? "two lists: the longer of the two" : "the two, one after the other");
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
	if (refused) printf("render_mauskart3d: the model REFUSED a stream\n");
	printf("render_mauskart3d: %d checks failed\n", bad);
	return ar_host_shots() == 11 && !refused && !bad ? 0 : 1;
}
