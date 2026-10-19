/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * Host tests for bookmark.c: reading, adding to and removing from the
 * bookmarks file, and the page made from it. docs/web_app.md,
 * "Bookmarks".
 */

#include <stdio.h>
#include <string.h>

#include "../bookmark.h"

static int fails, checks;

static void ck(int cond, const char *what) {
	checks++;
	if (!cond) { fails++; printf("FAIL: %s\n", what); }
}

static char page[8192];
static uint32_t page_len;

static void out(void *user, const char *d, uint32_t n) {
	(void)user;
	if (page_len + n < sizeof(page)) {
		memcpy(page + page_len, d, n);
		page_len += n;
		page[page_len] = 0;
	}
}

static void render(const char *file) {
	page_len = 0;
	page[0] = 0;
	bm_html(file, file ? (uint32_t)strlen(file) : 0, "/data/web/bookmarks.txt",
		out, NULL);
}

int main(void) {

	char line[BM_LINE_MAX], file[2048];
	uint32_t s, e, n;

	// -- formatting a line --

	n = bm_format(line, sizeof(line), "https://example.com/", "Example");
	ck(n == strlen("https://example.com/\tExample\n") &&
		!strcmp(line, "https://example.com/\tExample\n"), "plain line");

	n = bm_format(line, sizeof(line), "http://x/", "");
	ck(!strcmp(line, "http://x/\thttp://x/\n"), "empty title is the url");
	n = bm_format(line, sizeof(line), "http://x/", NULL);
	ck(!strcmp(line, "http://x/\thttp://x/\n"), "NULL title is the url");

	n = bm_format(line, sizeof(line), "http://x/", "a\tb\r\nc");
	ck(!strcmp(line, "http://x/\ta b  c\n"), "tab and line breaks in a title become spaces");

	ck(bm_format(line, sizeof(line), "", "t") == 0, "empty url refused");
	ck(bm_format(line, sizeof(line), "http://x/\ty", "t") == 0, "tab in url refused");
	ck(bm_format(line, sizeof(line), "http://x/\ny", "t") == 0, "newline in url refused");
	ck(bm_format(line, 8, "http://x/", "t") == 0, "too small a buffer refused");

	{
		char longt[400];
		memset(longt, 'T', sizeof(longt) - 1);
		longt[sizeof(longt) - 1] = 0;
		n = bm_format(line, sizeof(line), "u", longt);
		ck(n == 1 + 1 + (BM_TITLE_MAX - 1) + 1, "long title cut");
	}

	// -- finding --

	strcpy(file,
		"# my bookmarks\n"
		"https://a.example/\tA\n"
		"\n"
		"gemini://b.example/\r\n"
		"https://a.example/x\tAX\n"
		"https://c.example/\tC");		// no final newline

	ck(bm_find(file, (uint32_t)strlen(file), "https://a.example/", &s, &e) &&
		s == 15 && e == 15 + 21, "find the first entry, with its newline");
	ck(bm_find(file, (uint32_t)strlen(file), "gemini://b.example/", &s, &e) &&
		!strncmp(file + s, "gemini://", 9) && file[e - 1] == '\n',
		"find a line with no title and a CR");
	ck(bm_find(file, (uint32_t)strlen(file), "https://c.example/", &s, &e) &&
		e == strlen(file), "find the last line, no newline");
	ck(!bm_find(file, (uint32_t)strlen(file), "https://a.example", NULL, NULL),
		"a prefix is not a match");
	ck(!bm_find(file, (uint32_t)strlen(file), "# my bookmarks", NULL, NULL),
		"a comment is not an entry");
	ck(!bm_find(file, (uint32_t)strlen(file), "", NULL, NULL), "empty url");
	ck(!bm_find(NULL, 0, "https://a.example/", NULL, NULL), "no file");

	// -- the page --

	render(file);
	ck(strstr(page, "<title>Bookmarks</title>") != NULL, "page has a title");
	ck(strstr(page, "<a href=\"https://a.example/\">A</a>") != NULL, "first link");
	ck(strstr(page, "<a href=\"gemini://b.example/\">gemini://b.example/</a>") != NULL,
		"untitled entry shows its url, CR dropped");
	ck(strstr(page, "<a href=\"https://c.example/\">C</a>") != NULL, "unterminated last line");
	ck(strstr(page, "my bookmarks") == NULL, "comment not shown");
	ck(strstr(page, "/data/web/bookmarks.txt") != NULL, "says where the file is");
	{
		int links = 0;
		for (const char *p = page; (p = strstr(p, "<li>")); p++) links++;
		ck(links == 4, "four entries");
	}

	render("http://x/?a=1&b=\"2\"\t<Tom & \"Jerry\">\n");
	ck(strstr(page, "<a href=\"http://x/?a=1&amp;b=&quot;2&quot;\">"
		"&lt;Tom &amp; &quot;Jerry&quot;&gt;</a>") != NULL, "escaped");

	render(NULL);
	ck(strstr(page, "No bookmarks yet") != NULL && !strstr(page, "<ul>"),
		"empty list says so");
	render("# only a comment\n\n");
	ck(strstr(page, "No bookmarks yet") != NULL, "comments only is empty");

	// -- adding then removing round-trips --

	{
		char buf[512];
		uint32_t len;

		strcpy(buf, "https://a.example/\tA\n");
		len = (uint32_t)strlen(buf);
		n = bm_format(buf + len, sizeof(buf) - len, "https://b.example/", "B");
		len += n;
		ck(bm_find(buf, len, "https://b.example/", NULL, NULL), "added is found");

		len = bm_remove(buf, len, "https://a.example/");
		ck(!strcmp(buf, "https://b.example/\tB\n") && len == strlen(buf),
			"remove the first");
		len = bm_remove(buf, len, "https://nothere/");
		ck(!strcmp(buf, "https://b.example/\tB\n"), "removing an absent url changes nothing");
		len = bm_remove(buf, len, "https://b.example/");
		ck(len == 0 && buf[0] == 0, "remove the last leaves nothing");

		strcpy(buf, "u\tone\nv\tV\nu\ttwo\n");
		len = bm_remove(buf, (uint32_t)strlen(buf), "u");
		ck(!strcmp(buf, "v\tV\n"), "duplicates all removed");
	}

	printf("%s: %d checks, %d failures\n",
		fails ? "FAIL" : "ok", checks, fails);

	return fails ? 1 : 0;

}
