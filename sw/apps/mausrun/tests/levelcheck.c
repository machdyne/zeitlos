/*
 * mausrun -- can every level be finished? make -C sw/apps/mausrun test
 *
 *   levelcheck [-v] LEVEL.txt ...
 *
 * Reads each level with the game's own parser (level.c) and searches
 * every way the mouse can move with the game's own physics (phys.c):
 * left, right or neither, jumping or not, a frame at a time, breadth
 * first from the start, over the exact fixed-point state (x, y, the
 * fall speed). Springs, shelves and water are physics and so are in
 * it; thorns end a path, as a pit does; a crumbling biscuit counts as
 * solid (it is, until stood on), and no power-up is used: a level must
 * be finishable without them. A level passes when the search reaches
 * the goal -- the cheese wheel, or without one the last two columns --
 * and fails the build otherwise.
 *
 * A moving platform's track is a shelf along its whole run: the mouse
 * can stand wherever the platform goes by waiting for it there, and
 * riding it goes no further than walking along it would.
 *
 * It also reports the cheese (and the hidden star) the mouse can never
 * touch (an error: a child would hunt for it forever) and how many
 * frames the quickest run takes. Cats are left out: they walk and turn, and the mouse can
 * wait for them; a level a cat makes impossible is a bug the game's
 * own play would show at once.
 *
 * It checks every level compiled in (levels_builtin.c), and that each
 * file of sw/data/mausrun/levels says what its compiled-in copy says.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../level.h"
#include "../phys.h"

typedef struct {
	int32_t x, y, vy;
	uint8_t ground, water;
} state_t;

/* An open-addressed set of visited states and a queue: big, but the
 * build host has the memory. */
#define SET_BITS 24
#define SET_N (1u << SET_BITS)
static uint64_t *set;
static state_t *queue;
static uint32_t *depth;
#define QMAX (SET_N / 2)

static uint64_t key_of(const state_t *s) {
	return ((uint64_t)(uint32_t)(s->x + 4096) << 32) |
		((uint64_t)(uint32_t)((s->y + 8192) & 0xffff) << 16) |
		(uint64_t)(uint16_t)(s->vy + 512) | 1ull << 63;
}

static bool visit(const state_t *s) {
	uint64_t k = key_of(s);
	uint32_t h = (uint32_t)((k * 0x9E3779B97F4A7C15ull) >> (64 - SET_BITS));
	for (;;) {
		if (set[h] == 0) { set[h] = k; return true; }
		if (set[h] == k) return false;
		h = (h + 1) & (SET_N - 1);
	}
}

static int check(const char *name, bool verbose) {

	static uint8_t touched[MR_LEVEL_ROWS][MR_LEVEL_COLS_MAX];
	uint32_t qh = 0, qt = 0, states = 0;
	int32_t goal_px = MR_WORLD_W - 2 * MR_TILE_W;
	int best = -1, unreached = 0, r, c;
	state_t s0;

	memset(set, 0, sizeof(uint64_t) * SET_N);
	memset(touched, 0, sizeof(touched));

	s0.x = TOFP(mr_start_x + MR_PLAYER_XOFF);
	s0.y = TOFP(mr_start_y);
	s0.vy = 0;
	s0.ground = 0;
	s0.water = 0;
	visit(&s0);
	queue[qt] = s0;
	depth[qt++] = 0;

	while (qh < qt) {
		state_t cur = queue[qh];
		uint32_t d = depth[qh++];
		int dir, jump;
		states++;
		for (dir = -1; dir <= 1; dir++)
			for (jump = 0; jump <= 1; jump++) {
				mr_actor_t a;
				mr_step_ev_t ev;
				state_t n;
				if (jump && !cur.ground) continue;   /* the same as no jump */
				memset(&a, 0, sizeof(a));
				a.x = cur.x;
				a.y = cur.y;
				a.vy = cur.vy;
				a.on_ground = cur.ground;
				a.alive = true;
				a.in_water = cur.water;
				/* no power-ups: a level must be finishable without */
				mr_player_step(&a, dir, jump != 0, 0, &ev);
				if (TOPX(a.y) > MR_WORLD_H) continue;   /* in a pit */
				if (mr_thorns_hit(&a)) continue;         /* ouch */
				{
					int32_t cx = TOPX(a.x) + MR_PLAYER_W / 2;
					int32_t cy = TOPX(a.y) + MR_PLAYER_H / 2;
					int tc = (int)(cx / MR_TILE_W), tr = (int)(cy / MR_TILE_H);
					if (tc >= 0 && tc < mr_level_cols && tr >= 0 && tr < MR_LEVEL_ROWS)
						touched[tr][tc] = 1;
				}
				if (mr_level_info.goal ?
				    mr_box_touches(TOPX(a.x), TOPX(a.y), MR_PLAYER_W, MR_PLAYER_H, MR_TILE_GOAL) :
				    TOPX(a.x) >= goal_px) {
					if (best < 0 || (int)d + 1 < best) best = (int)d + 1;
					continue;
				}
				n.x = a.x;
				n.y = a.y;
				n.vy = a.vy;
				n.ground = a.on_ground;
				n.water = a.in_water;
				if (!visit(&n)) continue;
				if (qt >= QMAX) {
					printf("%s: the search ran out of room\n", name);
					return 1;
				}
				queue[qt] = n;
				depth[qt++] = d + 1;
			}
	}

	for (r = 0; r < MR_LEVEL_ROWS; r++)
		for (c = 0; c < mr_level_cols; c++)
			if ((mr_level[r][c] == MR_TILE_CHEESE || mr_level[r][c] == MR_TILE_STAR) &&
			    !touched[r][c]) {
				printf("%s: the %s at column %d, row %d cannot be reached\n",
					name, mr_level[r][c] == MR_TILE_STAR ? "star" : "cheese", c, r);
				unreached++;
			}

	if (best < 0) {
		printf("%s: \"%s\" CANNOT BE FINISHED (%u states searched)\n",
			name, mr_level_info.name, (unsigned)states);
		return 1;
	}
	if (verbose || unreached)
		printf("%s: \"%s\", %d columns, %d cheese, %d foes: finished in %d frames "
			"at best (%.1f s); %u states\n", name, mr_level_info.name,
			mr_level_cols, mr_cheese_total, mr_foe_count, best,
			best / 60.0, (unsigned)states);
	return unreached ? 1 : 0;
}

static char *read_all(const char *path, int *len) {
	FILE *f = fopen(path, "rb");
	char *b;
	long n;
	if (!f) return NULL;
	fseek(f, 0, SEEK_END);
	n = ftell(f);
	fseek(f, 0, SEEK_SET);
	b = malloc((size_t)n + 1);
	if (!b || fread(b, 1, (size_t)n, f) != (size_t)n) { fclose(f); free(b); return NULL; }
	fclose(f);
	b[n] = 0;
	*len = (int)n;
	return b;
}

int main(int argc, char **argv) {

	int i, bad = 0, files = 0;
	bool verbose = false;
	char err[96];

	/* a moving platform: wherever it goes, the mouse can wait for it */
	mr_track_shelf = true;
	set = calloc(SET_N, sizeof(uint64_t));
	queue = malloc(sizeof(state_t) * QMAX);
	depth = malloc(sizeof(uint32_t) * QMAX);
	if (!set || !queue || !depth) { printf("levelcheck: out of memory\n"); return 2; }

	/* every level compiled in */
	for (i = 1; i <= mr_builtin_count; i++) {
		char name[32];
		snprintf(name, sizeof name, "built-in level %d", i);
		mr_level_builtin_n(i);
		bad |= check(name, false);
	}

	for (i = 1; i < argc; i++) {
		int len;
		char *t;
		if (strcmp(argv[i], "-v") == 0) { verbose = true; continue; }
		t = read_all(argv[i], &len);
		files++;
		if (!t) { printf("%s: cannot read it\n", argv[i]); bad = 1; continue; }
		/* a level of the tree must say what its compiled-in copy says */
		{
			const char *b = strrchr(argv[i], '/');
			int n;
			b = b ? b + 1 : argv[i];
			n = (b[0] >= '0' && b[0] <= '9' && b[1] >= '0' && b[1] <= '9' && b[2] == '.')
				? (b[0] - '0') * 10 + (b[1] - '0') : 0;
			if (n >= 1 && strstr(argv[i], "data/mausrun/levels")) {
				static uint8_t a[MR_LEVEL_ROWS][MR_LEVEL_COLS_MAX];
				static mr_level_info_t ai;
				int cols, foes;
				int32_t sx, sy;
				if (!mr_level_builtin_n(n)) {
					printf("%s: not compiled in -- run gen_levels.py\n", argv[i]);
					bad = 1;
				} else {
					memcpy(a, mr_level, sizeof(a));
					ai = mr_level_info;
					cols = mr_level_cols;
					foes = mr_foe_count;
					sx = mr_start_x;
					sy = mr_start_y;
					if (mr_level_parse(t, len, err, sizeof err) == 0 &&
					    (cols != mr_level_cols || memcmp(a, mr_level, sizeof(a)) != 0 ||
					     memcmp(&ai, &mr_level_info, sizeof(ai)) != 0 ||
					     foes != mr_foe_count || sx != mr_start_x || sy != mr_start_y)) {
						printf("%s: differs from the compiled-in level %d -- run gen_levels.py\n",
							argv[i], n);
						bad = 1;
					}
				}
			}
		}
		if (mr_level_parse(t, len, err, sizeof err) != 0) {
			printf("%s: %s\n", argv[i], err);
			bad = 1;
		} else {
			bad |= check(argv[i], verbose);
		}
		free(t);
	}

	printf("levelcheck: %d level file%s and %d built in: %s\n",
		files, files == 1 ? "" : "s", mr_builtin_count,
		bad ? "PROBLEMS" : "every one can be finished");
	return bad;
}
