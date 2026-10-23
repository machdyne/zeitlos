/*
 * pix -- animations (px_anim.c), on the host: reading anim lines,
 * changing them, following renamed, deleted and copied cells, and the
 * sheet written back with them where they were.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../px_doc.h"
#include "../px_io.h"
#include "../px_anim.h"

static int checks, failures;

#define CHECK(c, ...) do { checks++; if (!(c)) { failures++; \
	printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static const char sheet[] =
	"zsp 1\n"
	"colours 4\n"
	"# a comment, kept\n"
	"anim run 80 m0 m1 m2 m1\n"
	"anim blink 250 eye0 eye1\n"
	"sprite m0 4 2\n"
	"\t0123\n"
	"\t....\n"
	"sprite m1 4 2\n"
	"\t1111\n"
	"\t....\n"
	"sprite m2 4 2\n"
	"\t2222\n"
	"\t....\n"
	"sprite eye0 2 2\n"
	"\t33\n"
	"\t33\n"
	"sprite eye1 2 2\n"
	"\t..\n"
	"\t33\n"
	"sprite lone 3 3\n"
	"\t...\n"
	"\t.1.\n"
	"\t...\n";

static bool load(px_doc_t *d, const char *text) {
	char err[80];
	if (px_read(d, PX_ZSP, (const uint8_t *)text, (uint32_t)strlen(text), err, sizeof(err))) return true;
	printf("read: %s\n", err);
	return false;
}

static char *write_text(const px_doc_t *d) {
	uint32_t len;
	uint8_t *b = px_write(d, false, &len);
	char *s = malloc(len + 1);
	memcpy(s, b, len);
	s[len] = 0;
	free(b);
	return s;
}

int main(void) {
	px_doc_t d;
	px_anim_t a;
	const char *err;
	char w[200], nm[PX_NAME_MAX];
	int k, at, prev, next;
	char *out;

	px_doc_init(&d, PX_ZSP);
	CHECK(load(&d, sheet), "the sheet reads");

	/* -- reading -- */
	CHECK(px_anim_count(&d) == 2, "two animations (%d)", px_anim_count(&d));
	CHECK(px_anim_get(&d, 0, &a) && !strcmp(a.name, "run") && a.ms == 80 && a.n == 4, "run: 80 ms, 4 frames");
	CHECK(a.cell[0] == px_find(&d, "m0") && a.cell[1] == px_find(&d, "m1") &&
		a.cell[2] == px_find(&d, "m2") && a.cell[3] == a.cell[1], "run's cells, m1 twice");
	CHECK(!px_anim_get(&d, 2, &a), "no third");

	/* what F5 plays, and onion skin's neighbours */
	k = px_anim_for(&d, px_find(&d, "m2"), &a, &at);
	CHECK(k == 0 && at == 2, "m2 is run's third frame");
	k = px_anim_for(&d, px_find(&d, "eye1"), &a, &at);
	CHECK(k == 1 && at == 1 && a.ms == 250, "eye1 is blink's second");
	k = px_anim_for(&d, px_find(&d, "lone"), &a, &at);
	CHECK(k == -1 && a.n == 1 && a.ms == PX_ANIM_DEFAULT_MS, "lone: the default, itself alone");
	px_anim_neighbours(&d, px_find(&d, "m0"), &prev, &next);
	CHECK(prev == px_find(&d, "m1") && next == px_find(&d, "m1"), "m0: m1 both ways (the loop)");
	px_anim_neighbours(&d, px_find(&d, "m2"), &prev, &next);
	CHECK(prev == px_find(&d, "m1") && next == px_find(&d, "m1"), "m2: m1 and m1");
	px_anim_neighbours(&d, px_find(&d, "lone"), &prev, &next);
	CHECK(prev == -1 && next == -1, "lone: none");

	/* written back as read */
	out = write_text(&d);
	CHECK(strstr(out, "# a comment, kept\nanim run 80 m0 m1 m2 m1\nanim blink 250 eye0 eye1\n") != NULL,
		"the lines are written back where they were:\n%s", out);
	free(out);

	/* -- changing -- */
	CHECK(px_anim_set(&d, "run 60 m0 m2", &err), "run again: %s", err);
	CHECK(px_anim_count(&d) == 2 && px_anim_get(&d, 0, &a) && a.ms == 60 && a.n == 2, "replaced in place");
	CHECK(px_anim_set(&d, "spin 120 m2 m1 m0", &err), "a new one: %s", err);
	CHECK(px_anim_count(&d) == 3 && px_anim_get(&d, 2, &a) && !strcmp(a.name, "spin"), "added last");
	CHECK(!px_anim_set(&d, "bad 5 m0", &err), "5 ms is too fast");
	CHECK(!px_anim_set(&d, "bad 100 nosuch", &err) && !strcmp(err, "No cell of that name"), "a frame must exist");
	CHECK(!px_anim_set(&d, "bad 100", &err), "no frames");
	CHECK(!px_anim_set(&d, "nosuch", &err), "deleting one that is not there");
	CHECK(px_anim_set(&d, "blink", &err) && px_anim_count(&d) == 2, "just a name deletes");
	CHECK(px_anim_words(&d, 0, w, sizeof(w)) && !strcmp(w, "run 60 m0 m2"), "as words: '%s'", w);
	out = write_text(&d);
	CHECK(strstr(out, "# a comment, kept\nanim run 60 m0 m2\nanim spin 120 m2 m1 m0\n") != NULL, "written:\n%s", out);
	free(out);

	/* -- following the cells -- */
	px_anim_rename(&d, "m2", "jump");
	CHECK(px_anim_words(&d, 1, w, sizeof(w)) && !strcmp(w, "spin 120 jump m1 m0"), "renamed: '%s'", w);
	px_anim_insert_after(&d, "m0", "m0b");
	CHECK(px_anim_words(&d, 0, w, sizeof(w)) && !strcmp(w, "run 60 m0 m0b jump"), "inserted: '%s'", w);
	CHECK(px_anim_words(&d, 1, w, sizeof(w)) && !strcmp(w, "spin 120 jump m1 m0 m0b"), "and in spin: '%s'", w);
	px_anim_forget(&d, "m1");
	CHECK(px_anim_words(&d, 1, w, sizeof(w)) && !strcmp(w, "spin 120 jump m0 m0b"), "forgotten: '%s'", w);
	px_anim_forget(&d, "jump");
	px_anim_forget(&d, "m0");
	px_anim_forget(&d, "m0b");
	CHECK(px_anim_count(&d) == 0, "with no frames left, the lines go");
	out = write_text(&d);
	CHECK(strstr(out, "# a comment, kept\n") && !strstr(out, "anim"), "only the comment:\n%s", out);
	free(out);

	/* -- names for new frames -- */
	px_anim_next_name(&d, "m0", nm);
	CHECK(!strcmp(nm, "m3"), "m0 -> m3, past m1 and m2 (%s)", nm);   /* the cells keep their names here */
	px_anim_next_name(&d, "lone", nm);
	CHECK(!strcmp(nm, "lone2"), "lone -> lone2 (%s)", nm);
	px_anim_next_name(&d, "eye0", nm);
	CHECK(!strcmp(nm, "eye2"), "eye0 -> eye2 (%s)", nm);
	px_anim_next_name(&d, "walk09", nm);
	CHECK(!strcmp(nm, "walk10"), "walk09 -> walk10 (%s)", nm);
	px_anim_next_name(&d, "f007", nm);
	CHECK(!strcmp(nm, "f008"), "f007 -> f008 (%s)", nm);

	/* not a sheet: nothing kept */
	px_doc_free(&d);
	px_doc_init(&d, PX_ZIC);
	CHECK(!px_anim_set(&d, "run 80 a", &err), "an icon set keeps no animations");
	CHECK(px_anim_count(&d) == 0, "and has none");

	/* the default for a sheet without lines: every cell of its size */
	px_doc_free(&d);
	px_doc_init(&d, PX_ZSP);
	px_cell_insert(&d, -1, 8, 8, "a");
	px_cell_insert(&d, -1, 4, 4, "b");
	px_cell_insert(&d, -1, 8, 8, "c");
	px_cell_insert(&d, -1, 8, 8, "d");
	k = px_anim_for(&d, 2, &a, &at);
	CHECK(k == -1 && a.n == 3 && a.cell[0] == 0 && a.cell[1] == 2 && a.cell[2] == 3 && at == 1,
		"the 8x8 cells, c second");
	px_anim_neighbours(&d, 0, &prev, &next);
	CHECK(prev == 3 && next == 2, "a: d before (the loop), c after");
	px_doc_free(&d);

	printf("test_anim: %d checks, %d failed\n", checks, failures);
	return failures != 0;
}
