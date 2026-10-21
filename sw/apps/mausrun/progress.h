#ifndef MAUSRUN_PROGRESS_H
#define MAUSRUN_PROGRESS_H

/*
 * mausrun -- what is remembered between games, in
 * /data/mausrun/progress.txt:
 *
 *     skill easy               easy, normal or hard
 *     unlocked 3               the furthest level reached
 *     best 1 28                level, most cheese collected
 *     best 2 17
 *     stars 1 7                level, which stars: 1 finished, 2 every
 *                              cheese, 4 the hidden star (round 4)
 *
 * Unknown lines are kept as they were when the file is written back,
 * so a later version's additions survive an older game. The skill is
 * the arcade games' (docs/arcade.md, "Skill"): without a saved one it
 * comes from apps.arcade.skill, and then easy.
 */

#include <stdint.h>
#include <stdbool.h>

enum { MR_SKILL_EASY = 0, MR_SKILL_NORMAL, MR_SKILL_HARD, MR_SKILLS };

#define MR_LEVELS_MAX 32

typedef struct {
	int skill;                  /* -1: none saved */
	int unlocked;               /* 1.. */
	uint16_t best[MR_LEVELS_MAX + 1];   /* cheese, by level number */
	uint8_t stars[MR_LEVELS_MAX + 1];   /* MR_STAR_* bits, by level */
} mr_progress_t;

/* A level's three stars. */
#define MR_STAR_DONE   1        /* finished it */
#define MR_STAR_CHEESE 2        /* every cheese in it */
#define MR_STAR_HIDDEN 4        /* found its hidden star */

/* stars in a mask (0..3), and in every level */
int mr_star_count(unsigned mask);
int mr_progress_stars(const mr_progress_t *p);

void mr_progress_parse(const char *text, mr_progress_t *p);
/* `old` is the file as it was (or NULL); its other lines are kept.
 * Returns the length, or -1 if it does not fit. */
int mr_progress_format(char *out, int cap, const char *old,
	const mr_progress_t *p);

/* "easy" -> MR_SKILL_EASY, ignoring case; -1 for anything else */
int mr_skill_word(const char *w);
const char *mr_skill_name(int s);        /* "EASY" */
const char *mr_skill_spoken(int s);      /* "Easy" */

#endif
