/*
 * space3d in game mode on a board with chunky (the arcade runtime's
 * AR_MODE_FULL_CHUNKY), on a build host, every screen written out as a
 * picture: make -C sw/apps/space3d render-chunky
 *
 * The same script twice, by zgl's two chunky drivers stood in for here:
 *
 *   soft  ZGL_NO_ENGINE's software strips, written into the picture
 *   t2    T2's stream (docs/zgl_t2.md), carried out a frame at a time by
 *         its C model (the geometry firmware, sw/common/zgl/t2fw.c, and
 *         the raster's model) into the same picture
 *
 * and the two held to each other picture by picture: they should agree
 * but for the few pixels a compiled display list rounds. The script:
 * the window (lines), game mode in colour (chunky, solid), steering and
 * firing, the wireframe in chunky (V) and back, the help (a box: the
 * planes' colours while it is up), mono game mode (lines again), colour
 * again, and the window.
 *
 *   tests/render_chunky soft out_soft
 *   tests/render_chunky t2 out_t2
 */

#undef main

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../../common/games/tests/ar_host.h"
#include "../../../common/zgl/zgl.h"
#include "../../../common/zgl/zgl_t2.h"

int game_main(void);

/* -- zgl's chunky driver (zgl_chunky.c on the board), stood in for -- */

static int engine;                 /* 0 software, 2 T2 */
static uint32_t *words;
static uint32_t nwords, frames_run, refused;
static uint64_t draws, tris, cpu_tris, overflows;
static uint32_t most_records;

int zglc_open(uint8_t flags) { return (flags & ZGL_NO_ENGINE) ? 0 : engine; }
volatile uint32_t *zglc_cmd(void) { return NULL; }
void zglc_put(uint32_t w) {
	if (nwords < (1u << 22)) words[nwords++] = w;
}
void zglc_finish(void) { }
void zglc_strip(int32_t y0, int rows, int32_t cx0, int32_t cx1, const uint8_t *col) {
	int r;
	for (r = 0; r < rows; r++)
		memcpy(&ar_host_chunky[y0 + r][cx0], &col[r * 320 + cx0], (size_t)(cx1 - cx0 + 1));
}

/* after each frame: T2 carries out what came in, a frame at a time */
void ar_host_hook(uint32_t frame) {
	uint32_t at = 0;
	(void)frame;
	while (at < nwords) {
		t2_stats_t st;
		if (t2_model_run(words + at, nwords - at, ar_host_chunky, &st) < 0) {
			refused++;
			break;
		}
		at += t2_model_used();
		frames_run++;
		draws += st.draws; tris += st.tris; cpu_tris += st.cpu_tris;
		overflows += st.overflows;
		if (st.drawn + st.lines > most_records) most_records = st.drawn + st.lines;
	}
	nwords = 0;
}

#define T(f, k)         { (f), ARH_TAP, (k), 0, NULL, 0, 0, 0, 0 }
#define DOWN(f, k)      { (f), ARH_KEY_DOWN, (k), 0, NULL, 0, 0, 0, 0 }
#define UP(f, k)        { (f), ARH_KEY_UP, (k), 0, NULL, 0, 0, 0, 0 }
#define SHOT(f, n)      { (f), ARH_SHOT, 0, 0, (n), 0, 0, 0, 0 }
#define MODE(f, m)      { (f), (m), 0, 0, NULL, 0, 0, 0, 0 }
#define END(f)          { (f), ARH_END, 0, 0, NULL, 0, 0, 0, 0 }

static const ar_host_step_t script[] = {
	SHOT(20, "01_window_lines"),
	MODE(22, ARH_FULL_COLOR),
	SHOT(40, "02_chunky_solid"),
	DOWN(42, ARH_K_LEFT), UP(70, ARH_K_LEFT), T(72, ' '), T(84, ' '),
	SHOT(92, "03_chunky_steered_fired"),
	SHOT(300, "04_chunky_later"),
	SHOT(480, "05_chunky_rock"),
	T(482, 'v'),
	SHOT(500, "06_chunky_wireframe"),
	T(502, 'v'),
	SHOT(520, "07_chunky_solid_again"),
	T(522, ARH_K_F1),
	SHOT(530, "08_help_over_planes"),
	T(532, 0x1b),
	SHOT(550, "09_chunky_after_help"),
	MODE(552, ARH_FULL_MONO),
	SHOT(570, "10_mono_lines"),
	MODE(572, ARH_FULL_COLOR),
	SHOT(590, "11_chunky_again"),
	MODE(592, ARH_WINDOW),
	SHOT(610, "12_window_lines_again"),
	END(614)
};

int main(int argc, char **argv) {
	const char *dir = argc > 2 ? argv[2] : "out_chunky";
	engine = argc > 1 && !strcmp(argv[1], "t2") ? 2 : 0;
	words = malloc((1u << 22) * 4);
	setvbuf(stdout, NULL, _IOLBF, 0);
	ar_host_chunky_avail = true;
	ar_host_run(script, dir);
	game_main();
	printf("render_chunky (%s): %d pictures; %u T2 frames%s\n", engine ? "t2" : "soft",
		ar_host_shots(), frames_run, refused ? ", the model REFUSED a stream" : "");
	if (frames_run)
		printf("render_chunky: T2 frames in all: %llu chunks, %llu triangles from lists, %llu set up by the CPU\n",
			(unsigned long long)draws, (unsigned long long)tris,
			(unsigned long long)cpu_tris);
	if (frames_run)
		printf("render_chunky: the most records in a frame about %u (T2 holds %d); %llu dropped\n",
			most_records, T2_RECS, (unsigned long long)overflows);
	return ar_host_shots() == 12 && !refused && !overflows ? 0 : 1;
}
