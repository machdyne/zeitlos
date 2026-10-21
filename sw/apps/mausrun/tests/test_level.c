/*
 * mausrun -- host tests of the level parser and the progress file.
 * make -C sw/apps/mausrun test
 */

#include <stdio.h>
#include <string.h>

#include "../level.h"
#include "../progress.h"

static int fails, checks;

#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; \
	printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); \
	printf("\n"); } } while (0)

static const char *rows15(void) {
	static char b[4096];
	int r;
	b[0] = 0;
	for (r = 0; r < 15; r++) {
		if (r == 10) strcat(b, "..M....c...C....C.........\n");
		else if (r == 11) strcat(b, "==========...=============\n");
		else if (r >= 12) strcat(b, "##########...#############\n");
		else strcat(b, "..........................\n");
	}
	return b;
}

static int parse(const char *head, char *err) {
	static char t[8192];
	strcpy(t, head);
	strcat(t, rows15());
	return mr_level_parse(t, (int)strlen(t), err, 96);
}

static void test_parse(void) {
	char err[96];
	const char *b;

	mr_level_builtin();
	CHECK(mr_level_cols == 128, "level 1 is 128 wide (%d)", mr_level_cols);
	CHECK(strcmp(mr_level_info.name, "The Meadow") == 0, "named (%s)", mr_level_info.name);
	CHECK(mr_level_info.time == MR_TIME_CYCLE, "level 1 has the day cycle");
	CHECK(mr_foe_count == 3, "three cats (%d)", mr_foe_count);
	CHECK(mr_start_x == 3 * MR_TILE_W && mr_start_y == 10 * MR_TILE_H, "the mouse at 3,10");
	CHECK(mr_first_solid_row == 1, "the first row with anything is 1, the star (%d)", mr_first_solid_row);
	CHECK(mr_cheese_total > 20, "cheese counted (%d)", mr_cheese_total);
	CHECK(mr_level[11][0] == MR_TILE_GRASS && mr_level[12][0] == MR_TILE_GROUND, "tiles");
	CHECK(mr_level[11][11] == MR_TILE_EMPTY, "the first pit");
	CHECK(mr_level[10][3] == MR_TILE_EMPTY, "M is sky");

	CHECK(parse("# a comment\n\nname Short Walk\ntime night\nmusic 2\nmap\n", err) == 0,
		"a small level (%s)", err);
	CHECK(mr_level_cols == 26 && strcmp(mr_level_info.name, "Short Walk") == 0, "its size and name");
	CHECK(mr_level_info.time == MR_TIME_NIGHT && mr_level_info.music == 2, "night, tune 2");
	CHECK(mr_foe_count == 2 && mr_cheese_total == 1, "two cats, one cheese");
	CHECK(mr_first_solid_row == 10, "first solid row 10 (%d)", mr_first_solid_row);

	/* round 4: themes, and the tune they bring */
	CHECK(parse("name K\ntheme kitchen\nmap\n", err) == 0, "a kitchen (%s)", err);
	CHECK(mr_level_info.theme == MR_THEME_KITCHEN && mr_level_info.music == MR_THEME_KITCHEN,
		"the kitchen's tune (%d)", mr_level_info.music);
	CHECK(parse("name K\nmusic 1\ntheme roofs\nmap\n", err) == 0 &&
		mr_level_info.theme == MR_THEME_ROOFS && mr_level_info.music == 1, "a music line wins");
	CHECK(parse("name K\nmap\n", err) == 0 && mr_level_info.theme == MR_THEME_MEADOW &&
		mr_level_info.music == 0 && !mr_level_info.star, "the meadow by default, no star");
	CHECK(parse("name K\ntheme moon\nmap\n", err) != 0 && strstr(err, "theme"), "a bad theme (%s)", err);
	{
		static char t[8192];
		int r;
		strcpy(t, "map\n");
		for (r = 0; r < 15; r++)
			strcat(t, r == 10 ? "M.*.PPPP..................\n" : "..........................\n");
		CHECK(mr_level_parse(t, (int)strlen(t), err, 96) == 0 && mr_level_info.star &&
			mr_level[10][2] == MR_TILE_STAR && mr_level[10][4] == MR_TILE_TRACK &&
			mr_level[10][7] == MR_TILE_TRACK && mr_level[10][8] == MR_TILE_EMPTY,
			"a star and a track (%s)", err);
	}

	b = "name X\r\nmap\r\n";
	CHECK(parse(b, err) == 0, "CR LF lines (%s)", err);

	CHECK(parse("name X\ntime dusk\nmap\n", err) != 0 && strstr(err, "time"), "a bad time (%s)", err);
	CHECK(strstr(err, "line 2") != NULL, "the line is given (%s)", err);
	CHECK(parse("name X\nspeed 3\nmap\n", err) != 0, "an unknown line");
	{
		char t[256] = "map\n.....M....\n";
		CHECK(mr_level_parse(t, (int)strlen(t), err, 96) != 0 && strstr(err, "15 rows"), "too few rows (%s)", err);
	}
	{
		static char t[8192];
		int r;
		strcpy(t, "map\n");
		for (r = 0; r < 15; r++) strcat(t, r == 10 ? ".M.........\n" : "...........\n");
		CHECK(mr_level_parse(t, (int)strlen(t), err, 96) != 0 && strstr(err, "too short"), "too narrow (%s)", err);
	}
	{
		static char t[8192];
		int r;
		strcpy(t, "map\n");
		for (r = 0; r < 15; r++) strcat(t, "..........................\n");
		CHECK(mr_level_parse(t, (int)strlen(t), err, 96) != 0 && strstr(err, "no M"), "no start (%s)", err);
	}
	{
		static char t[8192];
		int r;
		strcpy(t, "map\n");
		for (r = 0; r < 15; r++) strcat(t, r == 10 ? "..M.....X.................\n" : "..........................\n");
		CHECK(mr_level_parse(t, (int)strlen(t), err, 96) != 0 && strstr(err, "means nothing"), "a bad map character");
	}
	{
		/* a long level, and one past the limit */
		static char t[20000];
		int r, k;
		for (k = 0; k < 2; k++) {
			int w = k ? 513 : 512;
			strcpy(t, "map\n");
			for (r = 0; r < 15; r++) {
				char *p = t + strlen(t);
				memset(p, r == 11 ? '=' : '.', (size_t)w);
				if (r == 10) p[2] = 'M';
				p[w] = '\n';
				p[w + 1] = 0;
			}
			if (!k) CHECK(mr_level_parse(t, (int)strlen(t), err, 96) == 0 && mr_level_cols == 512, "512 wide (%s)", err);
			else CHECK(mr_level_parse(t, (int)strlen(t), err, 96) != 0, "513 is too wide");
		}
	}
	{
		/* more cats than there is room for: the rest are ignored */
		static char t[8192];
		int r;
		strcpy(t, "map\n");
		for (r = 0; r < 15; r++)
			strcat(t, r == 10 ? "M.CCCCCCCCCCCCCCCCCCCCCCCCCCCCCC\n" : "................................\n");
		CHECK(mr_level_parse(t, (int)strlen(t), err, 96) == 0 && mr_foe_count == MR_MAX_FOES,
			"cats capped at %d (%d)", MR_MAX_FOES, mr_foe_count);
	}
}

static void test_progress(void) {
	mr_progress_t p;
	char out[512];
	int n;

	mr_progress_parse("", &p);
	CHECK(p.skill == -1 && p.unlocked == 1, "nothing saved: no skill, level 1");
	mr_progress_parse("skill Hard\nunlocked 4\nbest 1 28\nbest 3 7\nother thing\n", &p);
	CHECK(p.skill == MR_SKILL_HARD && p.unlocked == 4, "read back (%d %d)", p.skill, p.unlocked);
	CHECK(p.best[1] == 28 && p.best[3] == 7 && p.best[2] == 0, "bests");
	mr_progress_parse("unlocked 99\nunlocked 0\nbest 40 3\nskill wobbly\n", &p);
	CHECK(p.unlocked == 1 && p.skill == -1 && p.best[MR_LEVELS_MAX] == 0, "nonsense ignored");

	memset(&p, 0, sizeof p);
	p.skill = MR_SKILL_EASY;
	p.unlocked = 3;
	p.best[2] = 12;
	n = mr_progress_format(out, sizeof out, "# mine\nskill hard\nunlocked 9\nbest 2 1\nx 1\n", &p);
	CHECK(n > 0 && strcmp(out, "# mine\nx 1\nskill easy\nunlocked 3\nbest 2 12\n") == 0,
		"written back, others kept (%s)", out);
	CHECK(mr_progress_format(out, 10, NULL, &p) < 0, "too small: refused");

	/* round 4: the stars */
	memset(&p, 0, sizeof p);
	p.skill = -1;
	p.unlocked = 2;
	p.stars[1] = MR_STAR_DONE | MR_STAR_HIDDEN;
	p.stars[2] = 7;
	n = mr_progress_format(out, sizeof out, "stars 1 1\n", &p);
	CHECK(n > 0 && strcmp(out, "unlocked 2\nstars 1 5\nstars 2 7\n") == 0, "stars written (%s)", out);
	mr_progress_parse(out, &p);
	CHECK(p.stars[1] == 5 && p.stars[2] == 7 && mr_progress_stars(&p) == 5, "stars read back (%d)",
		mr_progress_stars(&p));
	mr_progress_parse("stars 1 9\nstars 0 3\nstars 2 3\n", &p);
	CHECK(p.stars[1] == 0 && p.stars[2] == 3 && mr_star_count(3) == 2, "stars: nonsense ignored");
	mr_progress_parse(out, &p);
	CHECK(mr_skill_word("normal") == MR_SKILL_NORMAL && mr_skill_word("normally") == -1, "words");
	CHECK(strcmp(mr_skill_name(MR_SKILL_EASY), "EASY") == 0 &&
		strcmp(mr_skill_spoken(MR_SKILL_HARD), "Hard") == 0, "names");
}

int main(void) {
	test_parse();
	test_progress();
	printf("mausrun level and progress: %d checks, %d failed\n", checks, fails);
	return fails ? 1 : 0;
}
