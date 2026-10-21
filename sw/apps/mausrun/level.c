/*
 * mausrun -- the levels: the parser, and level 1 compiled in. See
 * level.h for the format.
 */

#include <string.h>

#include "level.h"

uint8_t mr_level[MR_LEVEL_ROWS][MR_LEVEL_COLS_MAX];
int mr_level_cols = 128;
int mr_first_solid_row = 3;
mr_level_info_t mr_level_info;

int32_t mr_start_x, mr_start_y;
mr_foe_spawn_t mr_foe_spawns[MR_MAX_FOES];
int mr_foe_count;
int mr_cheese_total;

/* The levels compiled in (levels_builtin.c, gen_levels.py): every level
 * file of the tree, so the game has them all even on a card without
 * /data/mausrun/levels. tests/levelcheck.c checks that they agree. */
const char *mr_level_builtin_text(int n) {
	if (n < 1 || n > mr_builtin_count) return NULL;
	return mr_builtin_levels[n - 1];
}

static void set_err(char *err, int errlen, const char *what, int line) {
	int n = 0, d = 1;
	const char *p;
	if (!err || errlen <= 0) return;
	for (p = what; *p && n < errlen - 1; p++) err[n++] = *p;
	if (line > 0 && n < errlen - 16) {
		for (p = " (line "; *p; p++) err[n++] = *p;
		while (d * 10 <= line) d *= 10;
		for (; d; d /= 10) err[n++] = (char)('0' + (line / d) % 10);
		err[n++] = ')';
	}
	err[n] = 0;
}

/* Does [s, e) start with the word w, then a space or its end? *rest is
 * then what follows the spaces. */
static bool keyword(const char *s, const char *e, const char *w, const char **rest) {
	while (*w) {
		if (s >= e || *s != *w) return false;
		s++;
		w++;
	}
	if (s < e && *s != ' ' && *s != '\t') return false;
	while (s < e && (*s == ' ' || *s == '\t')) s++;
	*rest = s;
	return true;
}

static int small_number(const char *s, const char *e) {
	int v = 0;
	while (s < e && *s >= '0' && *s <= '9' && v < 1000) v = v * 10 + (*s++ - '0');
	return v;
}

int mr_level_parse(const char *text, int len, char *err, int errlen) {

	const char *s = text, *end = text + len;
	int line = 0, row = 0, r, c;
	bool in_map = false, have_start = false;

	memset(mr_level, MR_TILE_EMPTY, sizeof(mr_level));
	memset(&mr_level_info, 0, sizeof(mr_level_info));
	strcpy(mr_level_info.name, "A Level");
	mr_level_info.time = MR_TIME_CYCLE;
	mr_level_info.music = 0xFF;            /* the theme's, below */
	mr_level_cols = 0;
	mr_foe_count = 0;
	mr_cheese_total = 0;
	mr_start_x = 3 * MR_TILE_W;
	mr_start_y = 10 * MR_TILE_H;

	while (s < end && row < MR_LEVEL_ROWS) {
		const char *e = s, *t, *rest;
		while (e < end && *e != '\n') e++;
		line++;
		t = e;
		if (t > s && t[-1] == '\r') t--;      /* a file from Windows */

		if (in_map) {
			int n = (int)(t - s);
			if (n > MR_LEVEL_COLS_MAX) {
				set_err(err, errlen, "a row is longer than 512", line);
				return -1;
			}
			for (c = 0; c < n; c++) {
				uint8_t tile = MR_TILE_EMPTY;
				switch (s[c]) {
				case '.': case ' ': break;
				case '=': tile = MR_TILE_GRASS; break;
				case '#': tile = MR_TILE_GROUND; break;
				case 'B': tile = MR_TILE_BOX; break;
				case 'c': tile = MR_TILE_CHEESE; mr_cheese_total++; break;
				case 'S': tile = MR_TILE_SPRING; break;
				case '-': tile = MR_TILE_ONEWAY; break;
				case 'K': tile = MR_TILE_CRUMBLE; break;
				case '^': tile = MR_TILE_THORNS; break;
				case '~': tile = MR_TILE_WATER; break;
				case '?': tile = MR_TILE_QBOX; break;
				case 'H': tile = MR_TILE_HOLE; break;
				case 'G': tile = MR_TILE_GOAL; mr_level_info.goal = true; break;
				case 'w': tile = MR_TILE_FEATHER; break;
				case 'b': tile = MR_TILE_BUBBLE; break;
				case 'm': tile = MR_TILE_MAGNET; break;
				case 'l': tile = MR_TILE_BELL; break;
				case 'k': tile = MR_TILE_SNEAKER; break;
				case 'h': tile = MR_TILE_HEART; break;
				case '*': tile = MR_TILE_STAR; mr_level_info.star = true; break;
				case 'P': tile = MR_TILE_TRACK; break;
				case 'M':
					mr_start_x = c * MR_TILE_W;
					mr_start_y = row * MR_TILE_H;
					have_start = true;
					break;
				case 'C': case 'Z': case 'F': case 'O':
					if (mr_foe_count < MR_MAX_FOES) {
						mr_foe_spawn_t *f = &mr_foe_spawns[mr_foe_count++];
						f->x = c * MR_TILE_W;
						f->y = row * MR_TILE_H;
						f->kind = s[c] == 'Z' ? MR_FOE_SLEEPER : s[c] == 'F' ? MR_FOE_FROG :
							s[c] == 'O' ? MR_FOE_OWL : MR_FOE_CAT;
					}
					break;
				default:
					set_err(err, errlen, "a map character that means nothing", line);
					return -1;
				}
				mr_level[row][c] = tile;
			}
			if (n > mr_level_cols) mr_level_cols = n;
			row++;
		} else if (t == s || *s == '#') {
			/* blank, or a comment */
		} else if (keyword(s, t, "map", &rest)) {
			in_map = true;
		} else if (keyword(s, t, "name", &rest)) {
			int n = (int)(t - rest);
			if (n > (int)sizeof(mr_level_info.name) - 1)
				n = (int)sizeof(mr_level_info.name) - 1;
			memcpy(mr_level_info.name, rest, (size_t)n);
			mr_level_info.name[n] = 0;
		} else if (keyword(s, t, "time", &rest)) {
			const char *x;
			if (keyword(rest, t, "day", &x)) mr_level_info.time = MR_TIME_DAY;
			else if (keyword(rest, t, "night", &x)) mr_level_info.time = MR_TIME_NIGHT;
			else if (keyword(rest, t, "cycle", &x)) mr_level_info.time = MR_TIME_CYCLE;
			else {
				set_err(err, errlen, "time is day, night or cycle", line);
				return -1;
			}
		} else if (keyword(s, t, "music", &rest)) {
			mr_level_info.music = (uint8_t)small_number(rest, t);
		} else if (keyword(s, t, "weather", &rest)) {
			static const char *const w[5] = { "none", "leaves", "snow", "rain", "fireflies" };
			const char *x;
			int k;
			for (k = 0; k < 5; k++)
				if (keyword(rest, t, w[k], &x)) break;
			if (k == 5) {
				set_err(err, errlen, "weather is none, leaves, snow, rain or fireflies", line);
				return -1;
			}
			mr_level_info.weather = (uint8_t)k;
		} else if (keyword(s, t, "theme", &rest)) {
			static const char *const th[MR_THEMES] = { "meadow", "garden", "kitchen", "roofs" };
			const char *x;
			int k;
			for (k = 0; k < MR_THEMES; k++)
				if (keyword(rest, t, th[k], &x)) break;
			if (k == MR_THEMES) {
				set_err(err, errlen, "theme is meadow, garden, kitchen or roofs", line);
				return -1;
			}
			mr_level_info.theme = (uint8_t)k;
		} else {
			set_err(err, errlen, "a line that is not name, time, music, weather, theme or map", line);
			return -1;
		}
		s = e < end ? e + 1 : e;
	}

	if (!in_map || row < MR_LEVEL_ROWS) {
		set_err(err, errlen, "the map needs 15 rows", line);
		return -1;
	}
	if (mr_level_cols < MR_LEVEL_COLS_MIN) {
		set_err(err, errlen, "the map is too short: 24 columns at least", 0);
		return -1;
	}
	if (mr_level_info.music == 0xFF) mr_level_info.music = mr_level_info.theme;
	if (!have_start) {
		set_err(err, errlen, "no M: where does the mouse start?", 0);
		return -1;
	}

	/* the first row with anything in it: the renderer skips the sky
	 * above it rather than walk ~150 empty cells a frame */
	mr_first_solid_row = MR_LEVEL_ROWS;
	for (r = 0; r < MR_LEVEL_ROWS && mr_first_solid_row == MR_LEVEL_ROWS; r++)
		for (c = 0; c < mr_level_cols; c++)
			if (mr_level[r][c] != MR_TILE_EMPTY) { mr_first_solid_row = r; break; }

	if (err && errlen > 0) err[0] = 0;
	return 0;
}

bool mr_level_builtin_n(int n) {
	const char *t = mr_level_builtin_text(n);
	if (!t) return false;
	return mr_level_parse(t, (int)strlen(t), NULL, 0) == 0;
}

void mr_level_builtin(void) {
	mr_level_builtin_n(1);
}
