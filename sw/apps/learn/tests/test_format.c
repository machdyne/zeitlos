/* learn -- the course and lesson format (host). make -C sw/apps/learn test */
#include <stdio.h>
#include <string.h>
#include "../lformat.h"
#include "hostfs.h"

static uint8_t mem[1 << 20];
static int fails;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

static lf_course_t course;
static lf_lesson_t les;

static bool load_lesson(const char *path) {
	static char buf[LF_MAX_LESSON + 1];
	long n = host_fs.read(NULL, path, buf, sizeof buf - 1);
	if (n < 0) return false;
	lx_arena_t a;
	char err[160];
	lx_arena_init(&a, mem, sizeof mem);
	bool ok = lf_lesson_parse(&a, buf, n, &les, err, sizeof err);
	if (!ok) printf("  (%s: %s)\n", path, err);
	return ok;
}

static void parse_str(const char *s, bool want, const char *want_err) {
	lx_arena_t a;
	char err[160];
	lx_arena_init(&a, mem, sizeof mem);
	bool ok = lf_lesson_parse(&a, s, (long)strlen(s), &les, err, sizeof err);
	CHECK(ok == want, "parse %s: got %d (%s)", s, ok, err);
	if (want_err) CHECK(strstr(err, want_err) != NULL, "error \"%s\" should mention \"%s\"", err, want_err);
}

int main(int argc, char **argv) {
	const char *dir = argc > 1 ? argv[1] : "tests/courses";
	char p[512], err[160];

	snprintf(p, sizeof p, "%s/t-parts", dir);
	CHECK(lf_course_load(&host_fs, p, &course, err, sizeof err), "load t-parts: %s", err);
	CHECK(!strcmp(course.m.id, "t-parts"), "id %s", course.m.id);
	CHECK(course.m.version == 2, "version");
	CHECK(course.m.nauthors == 2 && !strcmp(course.m.authors[1], "Claude (Anthropic model)"), "authors");
	CHECK(course.m.nreviewers == 1, "reviewers");
	CHECK(course.m.nneeds == 1 && !strcmp(course.m.needs[0], "t-units"), "needs");
	CHECK(!strcmp(course.m.classes, "A"), "class %s", course.m.classes);
	CHECK(course.nparts == 2, "parts %d", course.nparts);
	CHECK(!strcmp(course.parts[0].title, "Numbers"), "part title %s", course.parts[0].title);
	CHECK(!strcmp(course.parts[1].title, "Shapes"), "part title from name %s", course.parts[1].title);
	CHECK(course.nunits == 3, "units %d", course.nunits);
	CHECK(!strcmp(course.units[0].title, "Counting to ten"), "unit title %s", course.units[0].title);
	CHECK(!strcmp(course.units[1].id, "adding"), "unit id %s", course.units[1].id);
	CHECK(course.nlessons == 4, "lessons %d", course.nlessons);
	CHECK(course.m.nwarn == 0, "warnings %d: %s", course.m.nwarn, course.m.nwarn ? course.m.warn[0] : "");
	char id[160];
	lf_lesson_id(&course, 1, id, sizeof id);
	CHECK(!strcmp(id, "t-parts/numbers/counting/six-to-ten"), "lesson id %s", id);
	lf_lesson_path(&course, 3, id, sizeof id);
	CHECK(strstr(id, "02-shapes/01-flat/01-circles.md") != NULL, "lesson path %s", id);

	// lesson 1: a title, a meta block, two steps, a figure, a check
	lf_lesson_path(&course, 0, p, sizeof p);
	CHECK(load_lesson(p), "lesson 1 parses");
	CHECK(les.title && !strcmp(les.title, "One to five"), "title");
	CHECK(les.meta && lx_str_of(les.meta, "objective"), "meta");
	CHECK(les.nsteps == 2, "steps %d", les.nsteps);
	int fig = 0, data = 0, para = 0;
	for (int i = 0; i < les.nblocks; i++) {
		if (les.blocks[i].kind == LB_FIGURE) fig++;
		if (les.blocks[i].kind == LB_DATA) data++;
		if (les.blocks[i].kind == LB_PARA) para++;
	}
	CHECK(fig == 1 && data == 2 && para == 2, "blocks fig %d data %d para %d", fig, data, para);
	lf_block_t *chk = &les.blocks[les.steps[1].first];
	CHECK(chk->kind == LB_DATA && !strcmp(chk->name, "check"), "step 2 is the check");
	lx_node_t *ch = lx_nth(chk->forms, 0);
	CHECK(lx_is_form(ch, "choice") && lx_count(ch, "wrong") == 2 && lx_str_of(ch, "right"), "choice form");

	// lesson 2: a numbered list with a continuation line
	lf_lesson_path(&course, 1, p, sizeof p);
	CHECK(load_lesson(p), "lesson 2 parses");
	int items = 0;
	for (int i = 0; i < les.nblocks; i++) if (les.blocks[i].kind == LB_ITEM) {
		items++;
		if (items == 2) CHECK(strstr(les.blocks[i].text, "Point as you go") != NULL, "continued item: %s", les.blocks[i].text);
		CHECK(les.blocks[i].num == items, "item number %d", les.blocks[i].num);
	}
	CHECK(items == 3, "items %d", items);

	// lesson 3: inline math, display math, a table, a quote
	lf_lesson_path(&course, 2, p, sizeof p);
	CHECK(load_lesson(p), "lesson 3 parses");
	int kinds[16] = { 0 };
	for (int i = 0; i < les.nblocks; i++) kinds[les.blocks[i].kind]++;
	CHECK(kinds[LB_MATH] == 1 && kinds[LB_TABLE] == 1 && kinds[LB_QUOTE] == 1, "math %d table %d quote %d",
		kinds[LB_MATH], kinds[LB_TABLE], kinds[LB_QUOTE]);
	{
		lx_arena_t a;
		lx_arena_init(&a, mem + (1 << 19), 1 << 19);
		lf_span_t sp[16];
		const char *t = les.blocks[0].text;
		int n = lf_inline(&a, t, (int)strlen(t), sp, 16);
		int bold = 0, math = 0;
		for (int i = 0; i < n; i++) { if (sp[i].kind == LS_TEXT && sp[i].bold) bold++; if (sp[i].kind == LS_MATH) math++; }
		CHECK(bold == 1 && math == 1, "inline: bold %d math %d (n %d)", bold, math, n);
	}

	// lesson 4: an image, a link, escapes, a note and a code block
	lf_lesson_path(&course, 3, p, sizeof p);
	CHECK(load_lesson(p), "lesson 4 parses");
	memset(kinds, 0, sizeof kinds);
	for (int i = 0; i < les.nblocks; i++) kinds[les.blocks[i].kind]++;
	CHECK(kinds[LB_IMAGE] == 1 && kinds[LB_CODE] == 1 && kinds[LB_DATA] == 1, "image %d code %d data %d",
		kinds[LB_IMAGE], kinds[LB_CODE], kinds[LB_DATA]);
	{
		lx_arena_t a;
		lx_arena_init(&a, mem + (1 << 19), 1 << 19);
		lf_span_t sp[16];
		const char *t = les.blocks[0].text;
		int n = lf_inline(&a, t, (int)strlen(t), sp, 16);
		int links = 0, ital = 0;
		for (int i = 0; i < n; i++) { if (sp[i].kind == LS_LINK) links++; if (sp[i].italic) ital++; }
		CHECK(links == 2 && ital == 0, "links %d italic %d", links, ital);
	}

	// a course of units only
	snprintf(p, sizeof p, "%s/t-units", dir);
	CHECK(lf_course_load(&host_fs, p, &course, err, sizeof err), "load t-units: %s", err);
	CHECK(course.nparts == 0 && course.nunits == 1 && course.nlessons == 1, "units only");
	lf_lesson_id(&course, 0, id, sizeof id);
	CHECK(!strcmp(id, "t-units/first/only"), "id %s", id);
	CHECK(!strcmp(course.m.classes, "B"), "class B");

	// broken lessons
	parse_str("# T\n\n```check\n(choice (right \"a\")\n```\n", false, "not closed");
	parse_str("# T\n\n```check\n(choice)\n", false, "not closed");
	parse_str("# T\n# U\n", false, "one # title");
	parse_str("# T\n\nText.\n\n---\n\n---\n\nMore.\n", true, NULL);
	CHECK(les.nsteps == 2, "empty steps collapse: %d", les.nsteps);
	parse_str("# T\n\nA *b* and **c** and `d` and $1/2$.\n", true, NULL);

	printf("%s: %d failed\n", fails ? "test_format FAILED" : "test_format ok", fails);
	return fails != 0;
}
