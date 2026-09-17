#ifndef KGSAVE_H
#define KGSAVE_H

/*
 * kidgames -- persistent score and level, one line per game.
 *
 *   /user/kidgames.sav
 *
 *   spelling 120 3
 *   counting 40 2
 *
 * The original's format exactly: `<id> <best_score> <level>`, plain
 * text, one line per game, malformed lines skipped. Worth keeping
 * because it is hand-editable and corruption-tolerant, and because a
 * parent who wants to reset a kid's level can do it in `text` without
 * being told how.
 *
 * -- THE PATH --
 *
 * FatFs here is built FF_USE_LFN 0, so 8.3 names are not advice: a
 * longer name cannot be written to the card at all
 * (release/lib/mkfatimg.py). "kidgames.sav" is exactly 8.3, and
 * /user is a directory the release image already creates.
 *
 * $HOME and its /root fallback have no equivalent and no purpose --
 * this is a single-user machine, which is the same assumption the
 * original made for a different reason.
 *
 * -- FAILURE IS NORMAL --
 *
 * A Zeitlos machine can boot to a desktop with no sdcard at all
 * (docs/flash_apps.md), and then there is nowhere to write. Every
 * function here degrades silently to "no saved progress": the games
 * keep an in-memory score for the session and say nothing. A kid does
 * not need to be told about a filesystem, and a modal error on startup
 * would be the first thing they ever saw from this app.
 *
 * -- WHEN IT WRITES --
 *
 * At natural breakpoints only: leaving a game, and levelling up. Not
 * per answer. The original's reason was embedded flash wear; here it
 * is that an SD write goes over bit-banged SPI and takes long enough
 * to be felt, and feeling it after every letter would make the whole
 * app seem slow.
 */

#include "kg.h"

#define KG_SAVE_PATH     "/user/kidgames.sav"
#define KG_SAVE_MAX_ID   16
#define KG_SAVE_MAX_GAMES 24

/* Bounded by what KG_SAVE_MAX_GAMES lines of the format above can
 * occupy, with room to spare. Everything here is static -- an app's
 * stack and heap come out of one 16KB allocation
 * (Z_PROC_STACK_SIZE_DEFAULT), and a 1KB buffer on the stack
 * underneath a game that is already several calls deep is how that
 * runs out. */
#define KG_SAVE_FILE_MAX 1024

typedef struct {
	int best_score;
	int level;
} kg_save_t;

/*
 * The record for `game_id`. A game with none yet comes back as score
 * 0, level 1 -- never level 0, which several games would divide the
 * difficulty ladder by.
 *
 * `found` may be NULL. It distinguishes "never played" from "played
 * and scored nothing", which the level-up screen wants and nothing
 * else does.
 */
kg_save_t kg_save_load(const char *game_id, bool *found);

/* Store or update, rewriting the file. Returns false on any failure --
 * no card, read-only, table full. Callers ignore the result and keep
 * their in-memory score; it is returned for the tests. */
bool kg_save_store(const char *game_id, kg_save_t rec);

/*
 * The parser and the serialiser, exposed for the host tests.
 *
 * These are the part worth testing and the part with no I/O in them,
 * so the tests drive them directly on a buffer rather than needing a
 * filesystem. Same split as sw/apps/poker's engine.
 *
 * kg_save_parse() fills `ids` and `recs` from `text`, returning how
 * many lines it understood. kg_save_format() writes the other way and
 * returns the length, or 0 if it would not fit.
 */
int kg_save_parse(const char *text, char ids[][KG_SAVE_MAX_ID],
	kg_save_t *recs, int max);
int kg_save_format(char *out, int outlen, char ids[][KG_SAVE_MAX_ID],
	const kg_save_t *recs, int n);

#endif
