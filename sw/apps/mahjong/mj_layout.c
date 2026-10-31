/*
 * Zeitlos mahjong -- layouts. See mj_layout.h for the format and the
 * free rule.
 *
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 */

#include <string.h>

#include "mj_layout.h"

/* One layer's text, held until the next `layer` line, because a tile's
 * block spans two lines and is only known complete on the second. A
 * static: 6KB is too much for a 16KB stack to lend. */
static char grid[MJ_MAX_H][MJ_MAX_W];
static int  grid_rows;

static void say(char *err, int errlen, const char *what, int line)
{
	int i = 0, k;
	char num[12];

	if (!err || errlen <= 0) return;
	while (*what && i < errlen - 1) err[i++] = *what++;
	if (line > 0) {
		const char *p = " (line ";
		while (*p && i < errlen - 1) err[i++] = *p++;
		k = 0;
		do { num[k++] = (char)('0' + line % 10); line /= 10; } while (line && k < 11);
		while (k && i < errlen - 1) err[i++] = num[--k];
		if (i < errlen - 1) err[i++] = ')';
	}
	err[i] = 0;
}

/* Claims every "[]"/"[]" block in grid[] for layer z, scanning row by
 * row. The first unclaimed '[' met in that order is always the top
 * left of a tile: anything above it or left of it in the same block
 * would have been met first. */
static bool take_layer(mj_layout_t *L, int z, int line0, char *err, int errlen)
{
	int x, y;

	for (y = 0; y < grid_rows; y++) {
		for (x = 0; x < MJ_MAX_W; x++) {
			char c = grid[y][x];
			if (c == 0 || c == '.' || c == ' ') continue;
			if (c != '[') {
				say(err, errlen, c == ']' ? "a ']' without its '['" :
				    "not '[', ']' or '.'", line0 + y);
				return false;
			}
			if (x + 1 >= MJ_MAX_W || grid[y][x + 1] != ']') {
				say(err, errlen, "a '[' without its ']'", line0 + y);
				return false;
			}
			if (y + 1 >= grid_rows || grid[y + 1][x] != '[' ||
			    grid[y + 1][x + 1] != ']') {
				say(err, errlen, "a tile's bottom half is missing", line0 + y);
				return false;
			}
			if (L->n >= MJ_MAX) {
				say(err, errlen, "too many tiles", line0 + y);
				return false;
			}
			L->pos[L->n].x = (uint8_t)x;
			L->pos[L->n].y = (uint8_t)y;
			L->pos[L->n].z = (uint8_t)z;
			L->n++;
			grid[y][x] = grid[y][x + 1] = '.';
			grid[y + 1][x] = grid[y + 1][x + 1] = '.';
		}
	}
	return true;
}

bool mj_layout_overlap(const mj_layout_t *L, int a, int b)
{
	int dx = (int)L->pos[a].x - (int)L->pos[b].x;
	int dy = (int)L->pos[a].y - (int)L->pos[b].y;
	return dx > -2 && dx < 2 && dy > -2 && dy < 2;
}

static bool neighbours(mj_layout_t *L, char *err, int errlen)
{
	int a, b;

	for (a = 0; a < L->n; a++) {
		L->nabove[a] = L->nleft[a] = L->nright[a] = 0;
	}
	for (a = 0; a < L->n; a++) {
		const mj_pos_t *p = &L->pos[a];
		for (b = 0; b < L->n; b++) {
			const mj_pos_t *q = &L->pos[b];
			int dx, dy;
			if (a == b) continue;
			dx = (int)q->x - (int)p->x;
			dy = (int)q->y - (int)p->y;
			if (q->z > p->z) {
				if (dx > -2 && dx < 2 && dy > -2 && dy < 2) {
					if (L->nabove[a] >= MJ_ABOVE) {
						say(err, errlen, "more than 8 tiles stacked on one", 0);
						return false;
					}
					L->above[a][L->nabove[a]++] = (uint16_t)b;
				}
			} else if (q->z == p->z && dy > -2 && dy < 2) {
				/* Same layer and overlapping rows: two blocks that are
				 * not overlapping each other are at least a tile apart,
				 * so dx is -2, 2, or farther. */
				if (dx == -2) {
					if (L->nleft[a] >= MJ_SIDE) goto crowded;
					L->left[a][L->nleft[a]++] = (uint16_t)b;
				} else if (dx == 2) {
					if (L->nright[a] >= MJ_SIDE) goto crowded;
					L->right[a][L->nright[a]++] = (uint16_t)b;
				}
			}
		}
	}
	return true;
crowded:
	/* Cannot happen with blocks that do not overlap -- two tiles beside
	 * one, at most, on each side -- so this is a parser bug, not a
	 * layout's. Said anyway rather than writing past the array. */
	say(err, errlen, "internal: three tiles beside one", 0);
	return false;
}

static bool is_word(const char *s, int len, const char *w)
{
	int n = (int)strlen(w);
	if (len < n || memcmp(s, w, (size_t)n) != 0) return false;
	return len == n || s[n] == ' ' || s[n] == '\t';
}

bool mj_layout_parse(const char *text, mj_layout_t *L, char *err, int errlen)
{
	const char *s = text;
	int line = 0, z = -1, line0 = 0;
	int a, minx, miny, maxx, maxy, maxz;

	memset(L, 0, sizeof(*L));
	memcpy(L->name, "Untitled", 9);
	grid_rows = 0;
	if (err && errlen > 0) err[0] = 0;

	while (*s) {
		const char *e = s;
		int len;
		while (*e && *e != '\n') e++;
		len = (int)(e - s);
		if (len && s[len - 1] == '\r') len--;
		line++;

		if (len && s[0] == '#') {
			/* a comment, anywhere; not a row */
		} else if (is_word(s, len, "name")) {
			int i = 4, k = 0;
			while (i < len && (s[i] == ' ' || s[i] == '\t')) i++;
			while (i < len && k < MJ_NAME - 1) L->name[k++] = s[i++];
			while (k && L->name[k - 1] == ' ') k--;
			L->name[k] = 0;
			if (!k) memcpy(L->name, "Untitled", 9);
		} else if (is_word(s, len, "layer")) {
			if (z >= 0 && !take_layer(L, z, line0, err, errlen)) return false;
			z++;
			if (z >= MJ_MAX_Z) {
				say(err, errlen, "too many layers", line);
				return false;
			}
			memset(grid, 0, sizeof(grid));
			grid_rows = 0;
			line0 = line + 1;
		} else if (z < 0) {
			int i;
			for (i = 0; i < len; i++)
				if (s[i] != ' ' && s[i] != '\t') break;
			if (i < len) {
				say(err, errlen, "tiles before the first 'layer'", line);
				return false;
			}
		} else {
			/* A row of this layer. Blank lines are rows too: every
			 * layer counts from the same top line. */
			if (grid_rows >= MJ_MAX_H) {
				say(err, errlen, "the layout is too tall", line);
				return false;
			}
			if (len > MJ_MAX_W) {
				int i;
				for (i = MJ_MAX_W; i < len; i++)
					if (s[i] != '.' && s[i] != ' ' && s[i] != '\t') {
						say(err, errlen, "the layout is too wide", line);
						return false;
					}
				len = MJ_MAX_W;
			}
			memcpy(grid[grid_rows], s, (size_t)len);
			grid_rows++;
		}
		s = *e ? e + 1 : e;
	}
	if (z < 0) {
		say(err, errlen, "no 'layer' in it", 0);
		return false;
	}
	if (!take_layer(L, z, line0, err, errlen)) return false;

	if (L->n < 2) {
		say(err, errlen, "fewer than two tiles", 0);
		return false;
	}
	if (L->n & 1) {
		say(err, errlen, "an odd number of tiles", 0);
		return false;
	}

	/* Move it to (0, 0) and measure it. */
	minx = miny = 255;
	maxx = maxy = maxz = 0;
	for (a = 0; a < L->n; a++) {
		if (L->pos[a].x < minx) minx = L->pos[a].x;
		if (L->pos[a].y < miny) miny = L->pos[a].y;
	}
	for (a = 0; a < L->n; a++) {
		L->pos[a].x = (uint8_t)(L->pos[a].x - minx);
		L->pos[a].y = (uint8_t)(L->pos[a].y - miny);
		if (L->pos[a].x > maxx) maxx = L->pos[a].x;
		if (L->pos[a].y > maxy) maxy = L->pos[a].y;
		if (L->pos[a].z > maxz) maxz = L->pos[a].z;
	}
	L->w = maxx + 2;
	L->h = maxy + 2;
	L->d = maxz + 1;

	return neighbours(L, err, errlen);
}
