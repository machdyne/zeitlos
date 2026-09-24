/*
 * Host tests for sw/common/zargs.h (splitting with quotes) and
 * sw/common/zglob.h (wildcard matching).
 *
 *   cc -std=gnu99 -Wall -I sw/common -o /tmp/test_args sw/common/tests/test_args.c
 *   /tmp/test_args
 *
 * See docs/posix.md, "Quoting" and "Wildcards".
 */

#include <stdio.h>
#include <string.h>

#include "zargs.h"
#include "zglob.h"

static int run, failed;

#define CHECK(cond, what) do { run++; if (!(cond)) { failed++; \
	printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, what); } } while (0)

static char buf[512];
static char *av[16];
static uint8_t fl[16];

static int split(const char *line) {
	return z_args_split(line, buf, sizeof(buf), av, fl, 16);
}

static void test_split(void) {

	int n = split("cat notes.txt");
	CHECK(n == 2 && !strcmp(av[0], "cat") && !strcmp(av[1], "notes.txt"), "plain");
	CHECK(fl[0] == 0 && fl[1] == 0, "plain flags");

	n = split("  cat   a  ");
	CHECK(n == 2 && !strcmp(av[1], "a"), "extra spaces");

	n = split("cat 'My Notes.txt'");
	CHECK(n == 2 && !strcmp(av[1], "My Notes.txt") && (fl[1] & Z_ARG_QUOTED), "single quotes");

	n = split("cat \"My Notes.txt\" x");
	CHECK(n == 3 && !strcmp(av[1], "My Notes.txt") && !strcmp(av[2], "x"), "double quotes");

	n = split("cat My\\ Notes.txt");
	CHECK(n == 2 && !strcmp(av[1], "My Notes.txt"), "backslash space");

	n = split("echo \"say \\\"hi\\\" \\\\ ok\"");
	CHECK(n == 2 && !strcmp(av[1], "say \"hi\" \\ ok"), "escapes inside double quotes");

	n = split("echo 'it''s'");
	CHECK(n == 2 && !strcmp(av[1], "its"), "adjacent quoted parts join");

	n = split("echo \"it's\"");
	CHECK(n == 2 && !strcmp(av[1], "it's"), "' inside double quotes");

	n = split("echo 'unclosed here");
	CHECK(n == 2 && !strcmp(av[1], "unclosed here"), "unclosed quote runs to the end");

	n = split("echo '' x");
	CHECK(n == 3 && !strcmp(av[1], "") && (fl[1] & Z_ARG_QUOTED), "empty quoted argument");

	n = split("cat \"Gr\xC3\xBC\xC3\x9F" "e.txt\"");
	CHECK(n == 2 && !strcmp(av[1], "Gr\xC3\xBC\xC3\x9F" "e.txt"), "UTF-8 inside quotes");

	// Wildcards.
	n = split("ls *.txt");
	CHECK((fl[1] & Z_ARG_WILD) && !strcmp(av[1], "*.txt"), "unquoted * is wild");
	n = split("ls '*.txt'");
	CHECK(!(fl[1] & Z_ARG_WILD) && !strcmp(av[1], "*.txt"), "quoted * is not, and has no marks");
	n = split("ls \\*.txt");
	CHECK(!(fl[1] & Z_ARG_WILD) && !strcmp(av[1], "*.txt"), "escaped * is not");
	n = split("ls \"a*\"*");
	CHECK((fl[1] & Z_ARG_WILD) && av[1][0] == 'a' && av[1][1] == Z_ARG_LIT &&
		av[1][2] == '*' && av[1][3] == '*', "mixed: quoted * marked, unquoted * bare");
	z_args_unmark(av[1]);
	CHECK(!strcmp(av[1], "a**"), "unmark");

	// A quoted operator is an argument, and says so.
	n = split("echo '>' x");
	CHECK(n == 3 && !strcmp(av[1], ">") && (fl[1] & Z_ARG_QUOTED), "quoted >");

	// Max.
	n = z_args_split("a b c d", buf, sizeof(buf), av, fl, 2);
	CHECK(n == 2, "max arguments");

}

static void test_quote(void) {

	char out[128];
	const char *cases[] = { "plain", "My Notes.txt", "it's", "say \"hi\"",
		"back\\slash", "", "*.txt", "Gr\xC3\xBC\xC3\x9F" "e" };
	for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
		CHECK(z_args_quote(cases[i], out, sizeof(out)) >= 0, "quote fits");
		int n = split(out);
		CHECK(n == 1 && !strcmp(av[0], cases[i]), "quote round trip");
	}
	CHECK(z_args_quote("plain", out, sizeof(out)) == 5 && !strcmp(out, "plain"), "no quotes when not needed");

	char *args[] = { "-o", "my prog", "a.c" };
	CHECK(z_args_join(3, args, out, sizeof(out)), "join");
	int n = split(out);
	CHECK(n == 3 && !strcmp(av[1], "my prog"), "join round trip");
	CHECK(!z_args_join(3, args, out, 6), "join reports not fitting");

}

static void test_skip(void) {
	const char *line = "echo 'a|b' | wc";
	const char *p = line;
	int bars = 0;
	while (*p) {
		if (*p == '\'' || *p == '"' || *p == '\\') { p = z_args_skip(p); continue; }
		if (*p == '|') bars++;
		p++;
	}
	CHECK(bars == 1, "a quoted | is skipped");
}

static void test_glob(void) {

	CHECK(z_glob_match("*.txt", "notes.txt"), "*.txt");
	CHECK(z_glob_match("*.txt", "NOTES.TXT"), "case-insensitive");
	CHECK(!z_glob_match("*.txt", "notes.txt.gz"), "anchored at the end");
	CHECK(z_glob_match("*", "anything"), "*");
	CHECK(z_glob_match("*", ""), "* matches empty");
	CHECK(z_glob_match("a*b*c", "aXXbYYc"), "two stars");
	CHECK(!z_glob_match("a*b*c", "aXXbYY"), "two stars, no c");
	CHECK(z_glob_match("?.c", "a.c") && !z_glob_match("?.c", "ab.c"), "?");
	CHECK(z_glob_match("Gr??e.txt", "Gr\xC3\xBC\xC3\x9F" "e.txt"), "? is one character, not a byte");
	CHECK(z_glob_match("?.txt", "\xE6\x97\xA5.txt"), "? matches a kanji");
	CHECK(z_glob_match("[abc].c", "b.c") && !z_glob_match("[abc].c", "d.c"), "class");
	CHECK(z_glob_match("[a-c]x", "bx") && !z_glob_match("[a-c]x", "dx"), "range");
	CHECK(z_glob_match("[!a-c]x", "dx") && !z_glob_match("[!a-c]x", "bx"), "negated");
	CHECK(z_glob_match("[A-C]x", "bx"), "range, case-insensitive");
	CHECK(z_glob_match("a[", "a["), "unclosed [ is literal");
	CHECK(z_glob_match("My Notes*", "My Notes.txt"), "space in a pattern");
	char lit[] = { 'a', Z_ARG_LIT, '*', 0 };
	CHECK(z_glob_match(lit, "a*") && !z_glob_match(lit, "ab"), "marked * is literal");
	CHECK(z_glob_has_wild("*.c") && !z_glob_has_wild(lit), "has_wild");

}

int main(void) {
	test_split();
	test_quote();
	test_skip();
	test_glob();
	printf("%d checks, %d failed\n", run, failed);
	return failed ? 1 : 0;
}
