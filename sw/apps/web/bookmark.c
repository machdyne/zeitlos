/*
 * Zeitlos -- sw/apps/web
 *
 * See bookmark.h.
 */

#include <string.h>

#include "bookmark.h"

// One line of the file: the URL field and the title field, with their
// lengths, and where the next line starts. False at the end.
typedef struct {
	const char	*url;
	uint32_t	ulen;
	const char	*title;
	uint32_t	tlen;
	uint32_t	start, next;
} bm_line_t;

static bool next_line(const char *file, uint32_t len, uint32_t at,
	bm_line_t *ln) {

	uint32_t e, tab;

	if (at >= len) return false;

	for (e = at; e < len && file[e] != '\n'; e++) ;

	ln->start = at;
	ln->next = (e < len) ? e + 1 : e;

	if (e > at && file[e - 1] == '\r') e--;

	for (tab = at; tab < e && file[tab] != '\t'; tab++) ;

	ln->url = file + at;
	ln->ulen = tab - at;
	if (tab < e) {
		ln->title = file + tab + 1;
		ln->tlen = e - tab - 1;
	} else {
		ln->title = ln->url;
		ln->tlen = ln->ulen;
	}
	if (ln->tlen == 0) {
		ln->title = ln->url;
		ln->tlen = ln->ulen;
	}

	return true;

}

static bool is_entry(const bm_line_t *ln) {
	return ln->ulen > 0 && ln->url[0] != '#';
}

static bool same_url(const bm_line_t *ln, const char *url) {
	uint32_t n = (uint32_t)strlen(url);
	return is_entry(ln) && ln->ulen == n && !memcmp(ln->url, url, n);
}

bool bm_find(const char *file, uint32_t len, const char *url,
	uint32_t *start, uint32_t *end) {

	bm_line_t ln;
	uint32_t at = 0;

	if (!file || !url || !url[0]) return false;

	while (next_line(file, len, at, &ln)) {
		if (same_url(&ln, url)) {
			if (start) *start = ln.start;
			if (end) *end = ln.next;
			return true;
		}
		at = ln.next;
	}

	return false;

}

uint32_t bm_format(char *out, uint32_t cap, const char *url,
	const char *title) {

	uint32_t ul, tl, i, n;

	if (!url || !url[0] || !out) return 0;
	if (strpbrk(url, "\t\r\n")) return 0;
	if (!title || !title[0]) title = url;

	ul = (uint32_t)strlen(url);
	tl = (uint32_t)strlen(title);
	if (tl > BM_TITLE_MAX - 1) tl = BM_TITLE_MAX - 1;

	if (ul + 1 + tl + 2 > cap) return 0;

	memcpy(out, url, ul);
	n = ul;
	out[n++] = '\t';
	for (i = 0; i < tl; i++) {
		char c = title[i];
		out[n++] = (c == '\t' || c == '\r' || c == '\n') ? ' ' : c;
	}
	out[n++] = '\n';
	out[n] = 0;
	return n;

}

uint32_t bm_remove(char *file, uint32_t len, const char *url) {

	uint32_t s, e;

	if (!file) return 0;

	while (bm_find(file, len, url, &s, &e)) {
		memmove(file + s, file + e, len - e);
		len -= e - s;
	}

	file[len] = 0;
	return len;

}

// Text into HTML: the four characters that would change its meaning,
// as entities; the rest in runs.
static void out_esc(bm_out_fn out, void *user, const char *s, uint32_t n) {

	uint32_t i, run = 0;

	for (i = 0; i < n; i++) {
		const char *ent = NULL;
		switch (s[i]) {
		case '&': ent = "&amp;"; break;
		case '<': ent = "&lt;"; break;
		case '>': ent = "&gt;"; break;
		case '"': ent = "&quot;"; break;
		default: break;
		}
		if (!ent) continue;
		if (i > run) out(user, s + run, i - run);
		out(user, ent, (uint32_t)strlen(ent));
		run = i + 1;
	}

	if (n > run) out(user, s + run, n - run);

}

static void out_str(bm_out_fn out, void *user, const char *s) {
	out(user, s, (uint32_t)strlen(s));
}

void bm_html(const char *file, uint32_t len, const char *path,
	bm_out_fn out, void *user) {

	bm_line_t ln;
	uint32_t at = 0, count = 0;

	out_str(out, user, "<html><head><title>Bookmarks</title></head><body>\n"
		"<h1>Bookmarks</h1>\n");

	while (file && next_line(file, len, at, &ln)) {
		at = ln.next;
		if (!is_entry(&ln)) continue;
		if (!count) out_str(out, user, "<ul>\n");
		out_str(out, user, "<li><a href=\"");
		out_esc(out, user, ln.url, ln.ulen);
		out_str(out, user, "\">");
		out_esc(out, user, ln.title, ln.tlen);
		out_str(out, user, "</a></li>\n");
		count++;
	}

	if (count) out_str(out, user, "</ul>\n");
	else out_str(out, user, "<p>No bookmarks yet.</p>\n");

	out_str(out, user, "<p>Ctrl+D adds the page you are on, and on a page "
		"already bookmarked removes it. The list is the file ");
	out_esc(out, user, path, (uint32_t)strlen(path));
	out_str(out, user, ", one URL, a tab and a title a line.</p>\n"
		"</body></html>\n");

}
