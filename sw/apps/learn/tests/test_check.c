/* learn -- the checker: a clean course, and one with a planted error for every rule. */
#include <stdio.h>
#include <string.h>
#include "../lcheck.h"
#include "hostfs.h"

static char log_[64][300];
static int nlog;
static int fails;
static void report(lc_ctx_t *c, int level, const char *file, int line, const char *msg) {
	(void)c;
	if (nlog < 64) snprintf(log_[nlog++], sizeof log_[0], "%s %d %s: %s", level == 0 ? "E" : level == 1 ? "W" : "N",
		line, strrchr(file, '/') ? strrchr(file, '/') + 1 : file, msg);
}
static bool has(const char *kind, const char *s) {
	for (int i = 0; i < nlog; i++) if (log_[i][0] == kind[0] && strstr(log_[i], s)) return true;
	return false;
}
#define WANT(k, s) do { if (!has(k, s)) { printf("FAIL: expected %s \"%s\"\n", k, s); fails++; } } while (0)

static unsigned char mem[1 << 20];

int main(int argc, char **argv) {
	const char *dir = argc > 1 ? argv[1] : "tests/courses";
	char p[512];
	lc_ctx_t c;
	memset(&c, 0, sizeof c);
	c.fs = &host_fs; c.report = report; c.mem = mem; c.memsize = sizeof mem;

	snprintf(p, sizeof p, "%s/t-good", dir);
	lc_check_course(&c, p);
	if (c.errors || c.warnings) {
		printf("FAIL: t-good should be clean:\n");
		for (int i = 0; i < nlog; i++) printf("  %s\n", log_[i]);
		fails++;
	}
	if (c.checks != 2 || c.lessons != 1 || c.steps != 3 || c.figures != 1) {
		printf("FAIL: t-good counts: %d checks %d lessons %d steps %d figures\n", c.checks, c.lessons, c.steps, c.figures);
		fails++;
	}
	c.release = true;
	nlog = 0; c.errors = c.warnings = 0;
	lc_check_course(&c, p);
	if (c.errors) { printf("FAIL: t-good is releasable\n"); fails++; }

	nlog = 0; c.errors = c.warnings = 0;
	snprintf(p, sizeof p, "%s/t-bad", dir);
	lc_check_course(&c, p);
	WANT("E", "is not the folder's name");
	WANT("E", "level \"13\"");
	WANT("E", "no version");
	WANT("E", "no author");
	WANT("E", "needs \"Not An Id\"");
	WANT("E", "write name; qualification; date");
	WANT("E", "release: class B needs a qualified reviewer");
	WANT("E", "needs (objective");
	WANT("W", "(minutes 90)");
	WANT("W", "Greek letter");
	WANT("E", "formula: something is missing");
	WANT("E", "formula: a ( is not closed");
	WANT("E", "goes to no lesson");
	WANT("E", "a link starts lesson:");
	WANT("E", "picture img/none.png is not in the course");
	WANT("E", "has no line in media.cfg");
	WANT("E", "add (several)");
	WANT("E", "the answer \"a\" is there twice");
	WANT("E", "\"c\" has no feedback");
	WANT("E", "the answer 1.5 is not what");
	WANT("E", "the claim \"2/5 > 2/3\" is false");
	WANT("E", "the claim \"1 + 1 = 2\" is true");
	WANT("E", "(essay) is not a kind of check");
	WANT("E", "(text) needs at least one (accept");
	WANT("E", "cannot have 0 below");
	WANT("E", "\"rm\" is not an app");
	WANT("E", "figure: 1 ( not closed");
	if (fails) for (int i = 0; i < nlog; i++) printf("  %s\n", log_[i]);

	{
		int w;
		double g1 = lc_reading_grade("The cat sat. The dog ran. We had fun.", &w);
		double g2 = lc_reading_grade("Photosynthesis transforms electromagnetic radiation into chemical potential energy within chloroplasts, facilitating carbohydrate biosynthesis.", &w);
		if (!(g1 < 2 && g2 > 12)) { printf("FAIL: reading grades %.1f %.1f\n", g1, g2); fails++; }
	}
	if (!lc_drawable(0xE4, false) || !lc_drawable(0x20AC, false) || lc_drawable(0x3C0, false) ||
		!lc_drawable(0x304B, true) || lc_drawable(0x304B, false) || !lc_drawable(0x201C, false)) {
		printf("FAIL: drawable\n"); fails++;
	}
	printf("%s: %d failed\n", fails ? "test_check FAILED" : "test_check ok", fails);
	return fails != 0;
}
