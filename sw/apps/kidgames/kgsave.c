/*
 * kidgames -- persistent score and level. See kgsave.h.
 */

#include <string.h>

#include "kgsave.h"
#include "kgui.h"		/* kg_utoa/kg_append, the printf-free formatting */

#ifndef KG_SAVE_HOSTED
#include "../../common/zfsapp.h"
#endif

/*
 * Static, not stack.
 *
 * An app's stack and heap come out of one 16KB allocation
 * (Z_PROC_STACK_SIZE_DEFAULT, sw/os/kernel.h). This is a kilobyte of
 * text plus 24 records, and kg_save_store() is called from inside a
 * game that is already several frames deep. On the stack it would be
 * most of the remaining room at the worst possible moment; in .bss it
 * is a fixed cost the linker accounts for up front.
 *
 * The same reasoning is why sw/common/zflist.c refuses to malloc and
 * why sw/apps/settings' save buffers are file-static.
 */
#ifndef KG_SAVE_HOSTED
static char filebuf[KG_SAVE_FILE_MAX];
static char ids[KG_SAVE_MAX_GAMES][KG_SAVE_MAX_ID];
static kg_save_t recs[KG_SAVE_MAX_GAMES];
#endif

/* -- parsing ---------------------------------------------------------
 *
 * By hand, because sscanf() links picolibc's scanner the same way
 * printf links its formatter -- and the whole point of kg_utoa() is
 * that neither of them is in this binary. It is also about fifteen
 * lines, which is less than the explanation.
 */

static const char *skip_blank(const char *p)
{
	while (*p == ' ' || *p == '\t') p++;
	return p;
}

/* Reads a non-negative decimal. Sets *ok false if there was no digit
 * at all, which is what makes a malformed line detectable rather than
 * silently reading as zero -- a line that parsed as "score 0 level 0"
 * would reset a kid's progress instead of being skipped. */
static const char *read_int(const char *p, int *out, bool *ok)
{
	int v = 0;
	int digits = 0;

	p = skip_blank(p);

	while (*p >= '0' && *p <= '9') {
		/* Saturate rather than overflow. A hand-edited file with
		 * twenty digits in it is somebody experimenting, not an
		 * attack, and the answer is a big number, not UB. */
		if (v < 1000000) v = v * 10 + (*p - '0');
		p++;
		digits++;
	}

	*out = v;
	*ok = digits > 0;

	return p;
}

int kg_save_parse(const char *text, char out_ids[][KG_SAVE_MAX_ID],
	kg_save_t *out_recs, int max)
{
	int n = 0;
	const char *p = text;

	if (!text) return 0;

	while (*p && n < max) {

		const char *line = skip_blank(p);
		int i = 0;
		int score, level;
		bool ok1, ok2;

		/* The id: everything up to whitespace. */
		while (line[i] && line[i] != ' ' && line[i] != '\t' &&
			line[i] != '\n' && line[i] != '\r' &&
			i < KG_SAVE_MAX_ID - 1) i++;

		if (i > 0 && line[0] != '#') {

			const char *rest = read_int(line + i, &score, &ok1);
			rest = read_int(rest, &level, &ok2);

			if (ok1 && ok2) {
				memcpy(out_ids[n], line, (size_t)i);
				out_ids[n][i] = '\0';
				out_recs[n].best_score = score;
				/* A level of 0 in the file would divide every
				 * difficulty ladder by zero or index a list at -1.
				 * Clamped on the way in, once, rather than at each of
				 * ten games' call sites. */
				out_recs[n].level = level < 1 ? 1 : level;
				n++;
			}

			p = rest;

		} else {
			p = line + i;
		}

		/* To the end of the line, whatever was or was not understood.
		 * A malformed line is skipped, not fatal: the file is meant to
		 * survive being hand-edited badly. */
		while (*p && *p != '\n') p++;
		if (*p == '\n') p++;

	}

	return n;
}

int kg_save_format(char *out, int outlen, char in_ids[][KG_SAVE_MAX_ID],
	const kg_save_t *in_recs, int n)
{
	int i;

	if (outlen <= 0) return 0;

	out[0] = '\0';

	for (i = 0; i < n; i++) {

		/* Length check BEFORE appending, not after. kg_append()
		 * truncates safely, so appending past the end would produce a
		 * half-written last line that the parser would then skip --
		 * silently losing one game's progress on every save from then
		 * on. Better to refuse the whole write. */
		int need = (int)strlen(in_ids[i]) + 24;

		if ((int)strlen(out) + need >= outlen) return 0;

		kg_append(out, outlen, in_ids[i]);
		kg_append(out, outlen, " ");
		kg_append_num(out, outlen, in_recs[i].best_score);
		kg_append(out, outlen, " ");
		kg_append_num(out, outlen, in_recs[i].level);
		kg_append(out, outlen, "\n");

	}

	return (int)strlen(out);
}

/* -- the file --------------------------------------------------------- */

#ifndef KG_SAVE_HOSTED

static int load_all(void)
{
	int len;

	filebuf[0] = '\0';

	/* fs_read_file returns the byte count, or <= 0 for a missing file
	 * or no card. Both are the same thing to us: no saved progress. */
	len = fs_read_file(KG_SAVE_PATH, filebuf, sizeof(filebuf) - 1);
	if (len <= 0) return 0;

	filebuf[len] = '\0';

	return kg_save_parse(filebuf, ids, recs, KG_SAVE_MAX_GAMES);
}

kg_save_t kg_save_load(const char *game_id, bool *found)
{
	kg_save_t empty = { 0, 1 };
	int n, i;

	if (found) *found = false;
	if (!game_id) return empty;

	n = load_all();

	for (i = 0; i < n; i++)
		if (!strcmp(ids[i], game_id)) {
			if (found) *found = true;
			return recs[i];
		}

	return empty;
}

bool kg_save_store(const char *game_id, kg_save_t rec)
{
	int n, i, idx = -1, len;

	if (!game_id) return false;

	n = load_all();

	for (i = 0; i < n; i++)
		if (!strcmp(ids[i], game_id)) { idx = i; break; }

	if (idx < 0) {
		if (n >= KG_SAVE_MAX_GAMES) return false;
		idx = n++;
		{
			int j = 0;
			while (game_id[j] && j < KG_SAVE_MAX_ID - 1) {
				ids[idx][j] = game_id[j];
				j++;
			}
			ids[idx][j] = '\0';
		}
	}

	recs[idx] = rec;

	len = kg_save_format(filebuf, sizeof(filebuf), ids, recs, n);
	if (len <= 0) return false;

	/*
	 * fs_mkdir is attempted and its result ignored: /user exists on a
	 * release card (release/lib/mkfatimg.py's DIRS) but not
	 * necessarily on a card somebody formatted themselves, and
	 * "already exists" and "created" are equally fine. Only the write
	 * that follows decides whether this worked.
	 */
	fs_mkdir("/user");

	return fs_write_file(KG_SAVE_PATH, filebuf, len) == len;
}

#else	/* KG_SAVE_HOSTED -- the tests drive parse/format directly */

kg_save_t kg_save_load(const char *game_id, bool *found)
{
	kg_save_t empty = { 0, 1 };
	(void)game_id;
	if (found) *found = false;
	return empty;
}

bool kg_save_store(const char *game_id, kg_save_t rec)
{
	(void)game_id; (void)rec;
	return false;
}

#endif
