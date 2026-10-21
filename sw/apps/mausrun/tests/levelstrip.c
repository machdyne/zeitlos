/*
 * mausrun -- every level as one long picture, for looking at a level
 * without playing it. make -C sw/apps/mausrun render
 *
 *   levelstrip OUTDIR LEVEL.txt ...
 *
 * Writes OUTDIR/<name>.ppm: the whole map, 16 pixels a tile, drawn from
 * the game's own tiles and sprites in the four day colours, with the
 * mouse at its start and every foe at its own. ppm2png.py makes PNGs.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../level.h"

/* the four colours: the level's theme's, by day or by night, as the
 * game lights them (mausrun.c, mr_lights[]) */
static const uint16_t lights[MR_THEMES][2][4] = {
	{ { 0x6be, 0x320, 0x4a3, 0xfe8 }, { 0x114, 0x001, 0x132, 0x9ac } },
	{ { 0x7cf, 0x420, 0x3b4, 0xfe8 }, { 0x125, 0x001, 0x3a5, 0xbcd } },
	{ { 0x9bc, 0x431, 0x4a8, 0xffe }, { 0x335, 0x100, 0x386, 0xdcb } },
	{ { 0x8ad, 0x521, 0x5a5, 0xffe }, { 0x113, 0x420, 0x4a6, 0xeed } },
};
static uint8_t pal[4][3];

static void pal_set(void) {
	int k, night = mr_level_info.time == MR_TIME_NIGHT;
	for (k = 0; k < 4; k++) {
		uint16_t c = lights[mr_level_info.theme][night][k];
		pal[k][0] = (uint8_t)((c >> 8 & 15) * 17);
		pal[k][1] = (uint8_t)((c >> 4 & 15) * 17);
		pal[k][2] = (uint8_t)((c & 15) * 17);
	}
}

/* as the game draws it in the theme (mausrun.c, theme_tile()) */
static int theme_tile(int t) {
	if (mr_level_info.theme == MR_THEME_KITCHEN) {
		if (t == MR_TILE_GRASS) return MR_TILE_FLOOR;
		if (t == MR_TILE_THORNS) return MR_TILE_TRAP;
	}
	if (mr_level_info.theme == MR_THEME_ROOFS) {
		if (t == MR_TILE_GRASS) return MR_TILE_ROOF;
		if (t == MR_TILE_BOX) return MR_TILE_CHIMNEY;
	}
	return t;
}

static uint8_t *img;
static int iw, ih;

static void px(int x, int y, int c) {
	if (x < 0 || y < 0 || x >= iw || y >= ih) return;
	memcpy(img + ((size_t)y * iw + x) * 3, pal[c], 3);
}

static void tile(int t, int x0, int y0) {
	const uint32_t *b = mr_tiles[t];
	int x, y;
	for (y = 0; y < MR_TILE_H; y++)
		for (x = 0; x < MR_TILE_W; x++) {
			int on = (b[y] >> x) & 1;
			int c;
			switch (t) {
			case MR_TILE_GRASS:
				if (y < 5) { if (!on) continue; c = 2; }
				else c = on ? 1 : 3;
				break;
			case MR_TILE_GROUND: case MR_TILE_BOX: case MR_TILE_CRUMBLE:
			case MR_TILE_CRACKED: case MR_TILE_USEDBOX: case MR_TILE_FLOOR:
			case MR_TILE_ROOF: case MR_TILE_CHIMNEY:
				c = on ? 1 : 3;
				break;
			case MR_TILE_QBOX:
				c = on ? 2 : 3;
				break;
			case MR_TILE_WATER:
				c = on ? 3 : 2;
				break;
			case MR_TILE_THORNS:
				if (!on) continue;
				c = 2;
				break;
			case MR_TILE_MAGNET: case MR_TILE_SNEAKER: case MR_TILE_HEART:
			case MR_TILE_SPRING: case MR_TILE_ONEWAY: case MR_TILE_HOLE:
			case MR_TILE_TRAP: case MR_TILE_PLATFORM:
				if (!on) continue;
				c = 1;
				break;
			default:            /* cheese, the wheel, the power-ups */
				if (!on) continue;
				c = 3;
				break;
			}
			px(x0 + x, y0 + y, c);
		}
}

static void sprite(const uint16_t *d, const uint16_t *m, int x0, int y0,
	int fg, int outline) {
	int x, y;
	for (y = 0; y < MR_SPR_H; y++)
		for (x = 0; x < MR_SPR_W; x++) {
			if (!((m[y] >> x) & 1)) continue;
			px(x0 + x, y0 + y, ((d[y] >> x) & 1) ? fg : outline);
		}
}

static int write_level(const char *dir, const char *name) {
	char path[512];
	FILE *f;
	int r, c, i;
	iw = mr_level_cols * MR_TILE_W;
	ih = MR_LEVEL_ROWS * MR_TILE_H;
	img = malloc((size_t)iw * ih * 3);
	if (!img) return 1;
	pal_set();
	for (i = 0; i < iw * ih; i++) memcpy(img + i * 3, pal[0], 3);
	for (r = 0; r < MR_LEVEL_ROWS; r++)
		for (c = 0; c < mr_level_cols; c++) {
			int t = mr_level[r][c];
			if (t == MR_TILE_TRACK) {
				/* the track as a dotted line, and the plank at its start */
				int x;
				for (x = 0; x < MR_TILE_W; x += 4) px(c * MR_TILE_W + x, r * MR_TILE_H, 1);
				if (c == 0 || mr_level[r][c - 1] != MR_TILE_TRACK) {
					tile(MR_TILE_PLATFORM, c * MR_TILE_W, r * MR_TILE_H);
					tile(MR_TILE_PLATFORM, (c + 1) * MR_TILE_W, r * MR_TILE_H);
				}
				continue;
			}
			if (t) tile(theme_tile(t), c * MR_TILE_W, r * MR_TILE_H);
		}
	for (i = 0; i < mr_foe_count; i++) {
		const mr_foe_spawn_t *f = &mr_foe_spawns[i];
		switch (f->kind) {
		case MR_FOE_FROG: sprite(mr_spr_frog1, mr_msk_frog1, f->x, f->y, 2, 1); break;
		case MR_FOE_OWL: sprite(mr_spr_owl1, mr_msk_owl1, f->x, f->y, 3, 1); break;
		case MR_FOE_SLEEPER: sprite(mr_spr_cat_sleep, mr_msk_cat_sleep, f->x, f->y, 1, 1); break;
		default: sprite(mr_spr_cat1, mr_msk_cat1, f->x, f->y, 1, 1); break;
		}
	}
	sprite(mr_spr_mouse_stand, mr_msk_mouse_stand, mr_start_x, mr_start_y, 3, 1);
	snprintf(path, sizeof path, "%s/%s.ppm", dir, name);
	f = fopen(path, "wb");
	if (!f) { free(img); return 1; }
	fprintf(f, "P6\n%d %d\n255\n", iw, ih);
	fwrite(img, 1, (size_t)iw * ih * 3, f);
	fclose(f);
	free(img);
	printf("levelstrip: %s, \"%s\", %d x %d\n", path, mr_level_info.name, iw, ih);
	return 0;
}

int main(int argc, char **argv) {
	int i, bad = 0;
	char err[96];
	if (argc < 2) { printf("usage: levelstrip OUTDIR LEVEL.txt ...\n"); return 2; }
	for (i = 2; i < argc; i++) {
		FILE *f = fopen(argv[i], "rb");
		static char t[65536];
		int n;
		const char *base = strrchr(argv[i], '/');
		char name[64];
		if (!f) { printf("%s: cannot read it\n", argv[i]); bad = 1; continue; }
		n = (int)fread(t, 1, sizeof t - 1, f);
		fclose(f);
		t[n] = 0;
		if (mr_level_parse(t, n, err, sizeof err) != 0) {
			printf("%s: %s\n", argv[i], err);
			bad = 1;
			continue;
		}
		base = base ? base + 1 : argv[i];
		snprintf(name, sizeof name, "level_%.*s", (int)strcspn(base, "."), base);
		bad |= write_level(argv[1], name);
	}
	return bad;
}
