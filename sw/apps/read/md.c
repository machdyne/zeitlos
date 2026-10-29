/*
 * Zeitlos
 * Copyright (c) 2025 Lone Dynamics Corporation. All rights reserved.
 *
 * Markdown parsing. See md.h for scope and why this is line-at-a-time.
 */

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#include "md.h"

void md_state_init(md_state_t *st) {
	st->in_fence = false;
	st->fence_char = 0;
	st->fence_len = 0;
	st->in_front = false;
	st->seen_any = false;
	st->in_list = false;
	st->front_lines = 0;
	st->skip_run = 0;
	st->tbl_cols = 0;
	st->tbl_head = false;
	for (int i = 0; i < MD_TBL_COLS; i++) st->tbl_w[i] = st->tbl_min[i] = 0;
}


// -- small helpers, no libc --

static bool is_space(char c) { return c == ' ' || c == '\t'; }
static bool is_digit(char c) { return c >= '0' && c <= '9'; }

// Word character, for deciding whether an underscore is a delimiter
// or just part of an identifier.
static bool is_word(char c) {
	return is_digit(c) || (c >= 'a' && c <= 'z') ||
		(c >= 'A' && c <= 'Z') || c == '_';
}

static int skip_spaces(const char *s, int i) {
	while (s[i] && is_space(s[i])) i++;
	return i;
}

// A line of nothing but `c` (at least `min` of them) and spaces.
static bool all_of(const char *s, char c, int min) {

	int n = 0;

	for (int i = 0; s[i]; i++) {
		if (s[i] == c) n++;
		else if (!is_space(s[i])) return false;
	}

	return n >= min;

}

// Does a fence open at s[i]? Three or more backticks or tildes; the
// count goes in *n.
//
// A BACKTICK fence whose info string contains a backtick is not a
// fence (CommonMark, 4.5) -- it is inline code at the start of a
// paragraph. docs/learn_format.md opens a paragraph with exactly that,
// ```` ```figure ```` naming the fence it describes, and reading it as
// a fence put the whole rest of that document inside a code block.
static bool fence_open(const char *s, int i, int *n) {

	char c = s[i];

	if (c != '`' && c != '~') return false;

	int k = 0;
	while (s[i + k] == c) k++;
	if (k < 3) return false;

	if (c == '`')
		for (int j = i + k; s[j]; j++)
			if (s[j] == '`') return false;

	if (n) *n = k;
	return true;

}

// Does this look like the first line of YAML front matter -- `key:`,
// or the closing --- of an empty block? Decides whether a --- on the
// first line opens front matter or is a rule; see md_parse().
static bool looks_like_key(const char *s) {

	if (!s) return false;
	if (all_of(s, '-', 3)) return true;

	int i = 0;
	while (is_word(s[i]) || s[i] == '-') i++;

	return i > 0 && s[i] == ':';

}

// Does this line, ignoring indentation, start with a list marker?
static bool looks_like_list(const char *s) {

	int i = skip_spaces(s, 0);

	if ((s[i] == '-' || s[i] == '*' || s[i] == '+') && is_space(s[i + 1]))
		return true;

	if (is_digit(s[i])) {
		int n = i;
		while (is_digit(s[n])) n++;
		if ((s[n] == '.' || s[n] == ')') && is_space(s[n + 1])) return true;
	}

	return false;

}

// -- output builders --

static void put(md_line_t *o, char c) {
	if (o->len < MD_LINE_MAX - 1) o->text[o->len++] = c;
}

static void put_str(md_line_t *o, const char *s, int n) {
	for (int i = 0; i < n && s[i]; i++) put(o, s[i]);
}

static void span_open(md_line_t *o, uint8_t kind, uint8_t link) {

	if (o->nspans >= MD_MAX_SPANS) return;

	o->spans[o->nspans].start = o->len;
	o->spans[o->nspans].len = 0;
	o->spans[o->nspans].kind = kind;
	o->spans[o->nspans].link = link;

}

static void span_close(md_line_t *o) {

	if (o->nspans >= MD_MAX_SPANS) return;

	o->spans[o->nspans].len = (uint16_t)(o->len - o->spans[o->nspans].start);

	// A zero-width span is not worth a slot, and would draw nothing
	// anyway.
	if (o->spans[o->nspans].len) o->nspans++;

}

// -- inline pass --
//
// Runs over the block's text content and strips inline syntax,
// recording spans for the two things that actually render
// differently. Everything else (emphasis) has its markers removed and
// its text kept -- see md.h on why there is no bold face to switch to.

static void inline_pass(md_line_t *o, const char *s) {

	int i = 0;

	while (s[i]) {

		char c = s[i];

		// `code` -- the most common inline construct in the corpus by
		// an order of magnitude, and the one with a natural 1bpp
		// rendering (inverse video).
		//
		// A run of N backticks is closed by the next run of exactly N
		// (CommonMark), which is how code containing a backtick is
		// written: `` a`b ``. Matching single backticks instead turned
		// ```` ```figure ```` into an empty span and stray text.
		if (c == '`') {

			int run = 0;
			while (s[i + run] == '`') run++;

			int j = i + run;
			int close = -1;

			while (s[j]) {
				if (s[j] != '`') { j++; continue; }
				int r = 0;
				while (s[j + r] == '`') r++;
				if (r == run) { close = j; break; }
				j += r;
			}

			if (close >= 0) {
				int a = i + run, b = close;
				// One space each side is padding, so the code can
				// begin or end with a backtick.
				if (b - a >= 2 && s[a] == ' ' && s[b - 1] == ' ') { a++; b--; }
				span_open(o, MD_SPAN_CODE, 0);
				put_str(o, s + a, b - a);
				span_close(o);
				i = close + run;
				continue;
			}

			// Unterminated -- literal backticks, not a span.
			put_str(o, s + i, run);
			i += run;
			continue;

		}

		// [text](target), and ![alt](src) which is treated the same
		// minus the bang -- there is nothing to show for an image, so
		// its alt text is all that is left.
		if (c == '!' && s[i + 1] == '[') { i++; continue; }

		if (c == '[') {

			int close = i + 1;
			int depth = 1;

			while (s[close] && depth) {
				if (s[close] == '[') depth++;
				else if (s[close] == ']') depth--;
				if (depth) close++;
			}

			if (s[close] == ']' && s[close + 1] == '(') {

				int tstart = close + 2;
				int tend = tstart;
				int pd = 1;

				while (s[tend] && pd) {
					if (s[tend] == '(') pd++;
					else if (s[tend] == ')') pd--;
					if (pd) tend++;
				}

				if (s[tend] == ')') {

					uint8_t li = o->nlinks;

					if (li < MD_MAX_LINKS) {

						int n = 0;

						// A target may carry a title after a space
						// ("url \"Title\"") -- keep only the url.
						for (int k = tstart; k < tend && n < MD_LINK_MAX - 1; k++) {
							if (s[k] == ' ') break;
							o->links[li][n++] = s[k];
						}

						o->links[li][n] = 0;
						o->nlinks++;

						span_open(o, MD_SPAN_LINK, li);
						put_str(o, s + i + 1, close - i - 1);
						span_close(o);

					} else {

						// Out of link slots: keep the text, lose the
						// target. Degraded, not wrong.
						put_str(o, s + i + 1, close - i - 1);

					}

					i = tend + 1;
					continue;

				}

			}

			put(o, c);
			i++;
			continue;

		}

		// Emphasis: markers dropped, text kept. See md.h.
		if (c == '*' || c == '_') {

			bool run2 = (s[i + 1] == c);
			char after = run2 ? s[i + 2] : s[i + 1];
			char before = (i > 0) ? s[i - 1] : ' ';

			// An UNDERSCORE only delimits at a word boundary.
			// Inside a word it is an ordinary character, which is
			// what keeps snake_case_names intact -- the corpus is
			// full of identifiers and exactly one real _emphasis_.
			// Asterisks have no such rule and may delimit anywhere.
			bool word_before = is_word(before);
			bool word_after = is_word(after);

			if (c == '_' && word_before && word_after) {
				put(o, c);
				i++;
				continue;
			}

			// Opening: followed by content rather than space.
			if (after && !is_space(after)) {
				i += run2 ? 2 : 1;
				continue;
			}

			// Closing: preceded by content.
			if (!is_space(before)) {
				i += run2 ? 2 : 1;
				continue;
			}

			put(o, c);
			i++;
			continue;

		}

		// Bare HTML tags. Rare, and there is nothing to render.
		if (c == '<') {

			int j = i + 1;

			// Only treat it as a tag if it closes on the same line
			// and looks like one -- otherwise "a < b" loses text.
			if (s[j] == '/' || (s[j] >= 'a' && s[j] <= 'z') ||
				(s[j] >= 'A' && s[j] <= 'Z')) {
				while (s[j] && s[j] != '>') j++;
				if (s[j] == '>') { i = j + 1; continue; }
			}

			put(o, c);
			i++;
			continue;

		}

		// Backslash escape: the next character is literal.
		if (c == '\\' && s[i + 1]) {
			put(o, s[i + 1]);
			i += 2;
			continue;
		}

		put(o, c);
		i++;

	}

	o->text[o->len] = 0;

}

// -- tables --

// A |---|:--:| separator row: nothing but pipes, dashes, colons and
// spaces, with at least one dash.
static bool is_sep_row(const char *s) {

	bool dash = false;

	for (int k = 0; s[k]; k++) {
		char ch = s[k];
		if (ch == '-') dash = true;
		else if (ch != '|' && ch != ':' && !is_space(ch)) return false;
	}

	return dash;

}

// Out of a table. Everything cleared, not just tbl_cols: the state is
// compared whole (index checkpoints, render_test's seekcache), and a
// table's widths left behind would make two ways to the same line
// look like two different states.
static void table_end(md_state_t *st) {
	st->tbl_cols = 0;
	st->tbl_head = false;
	for (int i = 0; i < MD_TBL_COLS; i++) st->tbl_w[i] = st->tbl_min[i] = 0;
}

// Cell `i` of a split row, in characters.
static int cell_w(const md_line_t *o, int i) {
	int n = o->cell[i + 1] - o->cell[i];
	return n > 255 ? 255 : n;
}

// Its longest word, at most MD_TBL_WORD.
static int cell_word(const md_line_t *o, int i) {
	int most = 0, run = 0;
	for (int k = o->cell[i]; k < o->cell[i + 1]; k++) {
		if (o->text[k] == ' ') run = 0;
		else if (++run > most) most = run;
	}
	return most > MD_TBL_WORD ? MD_TBL_WORD : most;
}

// Both, for each cell of a split row.
static void cell_sizes(const md_line_t *o, uint8_t *w, uint8_t *mw) {
	for (int i = 0; i < o->ncells; i++) {
		w[i] = (uint8_t)cell_w(o, i);
		mw[i] = (uint8_t)cell_word(o, i);
	}
}

// Splits the row at s (its leading '|') into o's cells.
//
// A cell ends at the next '|' that is not escaped; `\|` is a literal
// pipe, in code too (GitHub's rule -- docs/learn_format.md writes
// tables of shell pipes that way). The trailing pipe is optional.
// Each cell is trimmed and goes through inline_pass() like any text.
static void table_row(md_line_t *o, const char *s) {

	static char cbuf[MD_LINE_MAX];

	int i = 1;

	while (o->ncells < MD_TBL_COLS) {

		int k = skip_spaces(s, i);
		if (!s[k]) break;		// the trailing pipe, or nothing after it

		bool last = (o->ncells == MD_TBL_COLS - 1);
		int n = 0;

		i = k;
		while (s[i] && (last || s[i] != '|') && n < MD_LINE_MAX - 1) {
			if (s[i] == '\\' && s[i + 1] == '|') { cbuf[n++] = '|'; i += 2; }
			else cbuf[n++] = s[i++];
		}

		while (n && is_space(cbuf[n - 1])) n--;
		if (last && n && cbuf[n - 1] == '|') {
			n--;
			while (n && is_space(cbuf[n - 1])) n--;
		}
		cbuf[n] = 0;

		o->cell[o->ncells] = o->len;
		inline_pass(o, cbuf);
		o->ncells++;
		o->cell[o->ncells] = o->len;

		if (s[i] == '|') i++;
		else break;

	}

	if (!o->ncells) {		// a lone '|': one empty cell
		o->cell[0] = o->cell[1] = o->len;
		o->ncells = 1;
	}

	o->text[o->len] = 0;

}

int md_table_measure(const char *src, uint8_t *w, uint8_t *mw) {

	static md_line_t row;

	int fi = skip_spaces(src, 0);
	if (src[fi] != '|') return 0;
	if (is_sep_row(src + fi)) return -1;

	row.len = 0;
	row.nspans = 0;
	row.nlinks = 0;
	row.ncells = 0;
	table_row(&row, src + fi);

	cell_sizes(&row, w, mw);

	return row.ncells;

}

void md_table_widen(md_state_t *st, md_line_t *row, const uint8_t *w,
	const uint8_t *mw, int n) {

	if (n > MD_TBL_COLS) n = MD_TBL_COLS;

	if (st && st->tbl_cols) {
		if (n > st->tbl_cols) st->tbl_cols = (uint8_t)n;
		for (int i = 0; i < n; i++) {
			if (w[i] > st->tbl_w[i]) st->tbl_w[i] = w[i];
			if (mw[i] > st->tbl_min[i]) st->tbl_min[i] = mw[i];
		}
	}

	if (row && row->kind == MD_TABLE) {
		if (n > row->tcols) row->tcols = (uint8_t)n;
		for (int i = 0; i < n; i++) {
			if (w[i] > row->colw[i]) row->colw[i] = w[i];
			if (mw[i] > row->colmin[i]) row->colmin[i] = mw[i];
		}
	}

}

// -- table layout --

#define TBL_MAX_LINES  32	// display lines in one cell, at most

void md_table_layout(md_line_t *row, int chars) {

	static md_line_t src;
	static uint8_t sidx[MD_LINE_MAX];	// span covering each char, or 0xFF
	static uint16_t ls[MD_TBL_COLS][TBL_MAX_LINES], le[MD_TBL_COLS][TBL_MAX_LINES];
	int nl[MD_TBL_COLS];
	int fw[MD_TBL_COLS];

	int n = row->tcols > row->ncells ? row->tcols : row->ncells;
	if (n < 1) n = 1;
	if (n > MD_TBL_COLS) n = MD_TBL_COLS;

	// -- column widths
	int mn[MD_TBL_COLS];
	int total = n + 1, tmin = n + 1;
	for (int i = 0; i < n; i++) {
		fw[i] = row->colw[i];
		mn[i] = row->colmin[i];
		if (i < row->ncells && row->cell[i + 1] - row->cell[i] > fw[i])
			fw[i] = row->cell[i + 1] - row->cell[i];
		if (fw[i] < 1) fw[i] = 1;
		if (mn[i] < 1) mn[i] = 1;
		if (mn[i] > fw[i]) mn[i] = fw[i];
		total += fw[i];
		tmin += mn[i];
	}

	if (total > chars && tmin <= chars) {

		// Each its longest word, and the room left shared out by how
		// much more each wants. Rounded down; the remainder a
		// character at a time to the columns still furthest short.
		int room = chars - tmin, want = total - tmin;
		int given = 0;
		for (int i = 0; i < n; i++) {
			int extra = (fw[i] - mn[i]) * room / want;
			fw[i] = mn[i] + extra;
			given += extra;
		}
		for (; given < room; given++) {
			int b = -1;
			for (int i = 0; i < n; i++)
				if (fw[i] < row->colw[i] && (b < 0 || row->colw[i] - fw[i] > row->colw[b] - fw[b]))
					b = i;
			if (b < 0) break;
			fw[b]++;
		}
		total = chars;

	} else if (total > chars) {

		for (int i = 0; i < n; i++) fw[i] = mn[i];
		total = tmin;

		while (total > chars) {
			int w = 0;
			for (int i = 1; i < n; i++) if (fw[i] > fw[w]) w = i;
			// Not below a few characters: past that a table is clipped
			// at the right rather than shredded a letter a line.
			if (fw[w] <= 4) break;
			// Down to the next widest at once; a character at a time
			// between columns tied for widest, so they share the cut.
			int next = 4;
			for (int i = 0; i < n; i++)
				if (i != w && fw[i] > next) next = fw[i];
			int cut = fw[w] - next;
			if (cut > total - chars) cut = total - chars;
			if (cut < 1) cut = 1;
			fw[w] -= cut;
			total -= cut;
		}

	}

	// -- each cell wrapped into its column
	src = *row;

	for (int i = 0; i < src.len; i++) sidx[i] = 0xFF;
	for (int k = 0; k < src.nspans; k++)
		for (int i = src.spans[k].start;
			i < src.spans[k].start + src.spans[k].len && i < src.len; i++)
			sidx[i] = (uint8_t)k;

	int lines = 1;

	for (int c = 0; c < n; c++) {

		nl[c] = 0;
		if (c >= src.ncells) continue;

		int i = src.cell[c], end = src.cell[c + 1];

		while (i < end && nl[c] < TBL_MAX_LINES) {

			int stop = i + fw[c];

			if (stop >= end) stop = end;
			else {
				// the last space that ends a word in range
				int brk = -1;
				for (int k = stop; k > i; k--)
					if (src.text[k] == ' ') { brk = k; break; }
				if (brk > i) stop = brk;
			}

			ls[c][nl[c]] = (uint16_t)i;
			le[c][nl[c]] = (uint16_t)stop;
			nl[c]++;

			i = stop;
			while (i < end && src.text[i] == ' ') i++;

		}

		if (nl[c] > lines) lines = nl[c];

	}

	// -- the lines, with the spans carried across
	int len = total;
	if (lines * len > MD_LINE_MAX - 1) lines = (MD_LINE_MAX - 1) / len;
	if (lines < 1) lines = 1;

	row->len = 0;
	row->nspans = 0;
	int open = -1;		// row->spans index being extended, or -1
	uint8_t open_src = 0xFF;

	for (int l = 0; l < lines; l++) {
		for (int c = 0; c <= n; c++) {

			// the border cell
			if (row->len < MD_LINE_MAX - 1) row->text[row->len++] = ' ';
			open = -1;
			if (c == n) break;

			int a = 0, b = 0;
			if (l < nl[c]) { a = ls[c][l]; b = le[c][l]; }

			for (int k = 0; k < fw[c] && row->len < MD_LINE_MAX - 1; k++) {

				int si = a + k;
				char ch = (si < b) ? src.text[si] : ' ';
				uint8_t sp = (si < b) ? sidx[si] : 0xFF;

				if (sp == 0xFF) open = -1;
				else if (open >= 0 && sp == open_src) row->spans[open].len++;
				else if (row->nspans < MD_MAX_SPANS) {
					open = row->nspans++;
					open_src = sp;
					row->spans[open].start = row->len;
					row->spans[open].len = 1;
					row->spans[open].kind = src.spans[sp].kind;
					row->spans[open].link = src.spans[sp].link;
				} else open = -1;

				row->text[row->len++] = ch;

			}

		}
	}

	row->text[row->len] = 0;

	// borders: where they fall in a line
	row->ncells = (uint8_t)n;
	row->cell[0] = 0;
	for (int c = 0; c < n; c++)
		row->cell[c + 1] = (uint16_t)(row->cell[c] + fw[c] + 1);
	row->tflags |= MD_TF_LAID;

}

// -- block pass --

// A line that draws nothing (md.h, MD_SKIP) -- but every MD_SKIP_RUN-th
// in a row is a blank line instead.
//
// The reader fills a screen by reading blocks until something takes
// space, and a block that takes none costs a read and a parse and
// moves nothing. A long run of them -- 5,000 table separator rows was
// the fuzz case -- made every key walk the whole run, however far it
// went. With this, a screen is filled within MD_SKIP_RUN lines of
// anything; a real document never has that many in a row outside
// front matter, which does not count (see md_parse()).
static bool skip(md_state_t *st, md_line_t *out) {

	if (++st->skip_run >= MD_SKIP_RUN) {
		st->skip_run = 0;
		out->kind = MD_BLANK;
	} else {
		out->kind = MD_SKIP;
	}

	return false;

}

static bool parse_line(md_state_t *st, const char *src, const char *next,
	md_line_t *out);

bool md_parse(md_state_t *st, const char *src, const char *next,
	md_line_t *out) {

	bool ate = parse_line(st, src, next, out);

	// Anything that draws ends a run of lines that do not (skip()).
	if (out->kind != MD_SKIP) st->skip_run = 0;

	// And anything but a row ends a table -- one whose last row was
	// not seen as last (a caller that passed no `next`).
	if (out->kind != MD_TABLE && out->kind != MD_SKIP) table_end(st);

	return ate;

}

static bool parse_line(md_state_t *st, const char *src, const char *next,
	md_line_t *out) {

	out->kind = MD_BLANK;
	out->level = 0;
	out->marker[0] = 0;
	out->len = 0;
	out->text[0] = 0;
	out->nspans = 0;
	out->nlinks = 0;
	out->ncells = 0;
	out->tcols = 0;
	out->tflags = 0;

	if (!src) return false;

	// -- fenced code --
	//
	// Checked first and unconditionally: inside a fence, nothing else
	// is markup. That is the whole point of a fence, and getting it
	// wrong turns every '#' in a shell script into a heading.
	int fi = skip_spaces(src, 0);

	if (st->in_fence) {

		if ((src[fi] == '`' || src[fi] == '~') && src[fi] == st->fence_char) {
			int n = 0;
			while (src[fi + n] == st->fence_char) n++;
			if (n >= st->fence_len) {
				st->in_fence = false;
				return skip(st, out);
			}
		}

		out->kind = MD_CODE;
		put_str(out, src, MD_LINE_MAX - 1);
		out->text[out->len] = 0;
		return false;

	}

	{
		int n;
		if (fence_open(src, fi, &n)) {
			st->in_fence = true;
			st->fence_char = src[fi];
			st->fence_len = (uint8_t)(n > 255 ? 255 : n);
			st->seen_any = true;
			return skip(st, out);	// the fence line itself draws nothing
		}
	}

	// -- front matter --
	//
	// Only at the very top of the file. A --- anywhere else is a rule
	// or a setext underline, and treating it as front matter would
	// silently swallow the rest of the document.
	//
	// And only when it looks like front matter: the --- must be
	// followed by a `key:` line, and the block ends at its closing ---
	// or after MD_FRONT_MAX lines, whichever comes first. A document
	// that simply opens with a rule and never closes it -- found by
	// fuzzing -- was otherwise one long front matter block: a blank
	// page, and every key read to the end of the file looking for
	// something to draw.
	if (st->in_front) {
		if (all_of(src, '-', 3) || ++st->front_lines >= MD_FRONT_MAX)
			st->in_front = false;
		out->kind = MD_SKIP;
		return false;
	}

	if (!st->seen_any && all_of(src, '-', 3) && looks_like_key(next)) {
		st->in_front = true;
		st->front_lines = 0;
		st->seen_any = true;
		out->kind = MD_SKIP;
		return false;
	}

	// blank
	if (!src[fi]) {
		out->kind = MD_BLANK;
		return false;
	}

	st->seen_any = true;

	// -- indented code --
	//
	// Four spaces, but NOT when it is a continuation of a list item,
	// which is indented the same way. The corpus has 638 indented
	// code lines and plenty of indented list text; treating the
	// latter as code is very visible.
	// A nested list item is indented too -- see md_state_t.in_list
	// for why this cannot be decided by indentation alone.
	if (fi >= 4 && !(st->in_list && looks_like_list(src))) {
		out->kind = MD_CODE;
		put_str(out, src + 4, MD_LINE_MAX - 1);
		out->text[out->len] = 0;
		return false;
	}

	// -- ATX heading --
	if (src[fi] == '#') {

		int n = 0;
		while (src[fi + n] == '#') n++;

		if (n <= 6 && (is_space(src[fi + n]) || !src[fi + n])) {

			int t = skip_spaces(src, fi + n);

			// Trailing #### on a heading is closing syntax, not text.
			int end = 0;
			while (src[t + end]) end++;
			while (end > 0 && (src[t + end - 1] == '#' ||
				is_space(src[t + end - 1]))) end--;

			out->kind = MD_HEADING;
			out->level = (uint8_t)n;
			st->in_list = false;

			char tmp[MD_LINE_MAX];
			int c = 0;
			for (int k = 0; k < end && c < MD_LINE_MAX - 1; k++)
				tmp[c++] = src[t + k];
			tmp[c] = 0;

			inline_pass(out, tmp);
			return false;

		}

	}

	// -- horizontal rule --
	//
	// Before the list check: "- - -" and "***" would otherwise parse
	// as bullets.
	if (all_of(src, '-', 3) || all_of(src, '*', 3) || all_of(src, '_', 3)) {

		// A --- directly under text is a setext underline, handled
		// below by the paragraph branch, not a rule.
		out->kind = MD_RULE;
		st->in_list = false;
		return false;

	}

	// -- block quote --
	if (src[fi] == '>') {

		int t = fi + 1;
		if (is_space(src[t])) t++;

		out->kind = MD_QUOTE;
		st->in_list = false;
		inline_pass(out, src + t);
		return false;

	}

	// -- table row --
	//
	// Split into cells (table_row()); laid out in columns by the
	// reader, which knows the view width. The |---|---| separator
	// row draws nothing: the reader rules the header off instead.
	if (src[fi] == '|') {

		if (is_sep_row(src + fi)) {
			if (st->tbl_cols) st->tbl_head = true;
			return skip(st, out);
		}

		out->kind = MD_TABLE;
		st->in_list = false;
		table_row(out, src + fi);

		uint8_t w[MD_TBL_COLS], mw[MD_TBL_COLS];
		cell_sizes(out, w, mw);

		if (!st->tbl_cols) {
			// A new table: columns sized from this row for now.
			out->tflags |= MD_TF_FIRST;
			st->tbl_head = false;
			table_end(st);
			st->tbl_cols = out->ncells;
		} else {
			if (st->tbl_head) out->tflags |= MD_TF_BODY1;
			st->tbl_head = false;
		}

		// A row wider than the ones before widens the table.
		md_table_widen(st, NULL, w, mw, out->ncells);

		int ni = next ? skip_spaces(next, 0) : 0;
		bool next_row = next && next[ni] == '|';

		if ((out->tflags & MD_TF_FIRST) && next_row && is_sep_row(next + ni))
			out->tflags |= MD_TF_HEAD;

		out->tcols = st->tbl_cols;
		for (int i = 0; i < MD_TBL_COLS; i++) {
			out->colw[i] = st->tbl_w[i];
			out->colmin[i] = st->tbl_min[i];
		}

		if (!next_row) {
			out->tflags |= MD_TF_LAST;
			table_end(st);
		}

		return false;

	}

	// -- lists --
	if ((src[fi] == '-' || src[fi] == '*' || src[fi] == '+') &&
		is_space(src[fi + 1])) {

		out->kind = MD_LIST;
		out->level = (uint8_t)(fi / 2);		// two spaces per level
		if (out->level > 3) out->level = 3;

		out->marker[0] = '-';
		out->marker[1] = 0;

		st->in_list = true;

		inline_pass(out, src + skip_spaces(src, fi + 1));
		return false;

	}

	if (is_digit(src[fi])) {

		int n = fi;
		while (is_digit(src[n])) n++;

		if ((src[n] == '.' || src[n] == ')') && is_space(src[n + 1])) {

			out->kind = MD_LIST;
			out->level = (uint8_t)(fi / 2);
			if (out->level > 3) out->level = 3;

			int m = 0;
			for (int k = fi; k <= n && m < (int)sizeof(out->marker) - 1; k++)
				out->marker[m++] = src[k];
			out->marker[m] = 0;

			st->in_list = true;

			inline_pass(out, src + skip_spaces(src, n + 1));
			return false;

		}

	}

	// -- paragraph, possibly with a setext underline --
	//
	// An UNINDENTED paragraph ends any list context; an indented one
	// is continuation text belonging to the item above it.
	if (fi == 0) st->in_list = false;

	out->kind = MD_PARA;
	inline_pass(out, src + fi);

	if (next) {

		int ni = skip_spaces(next, 0);

		if (next[ni] && (all_of(next, '=', 1) || all_of(next, '-', 1))) {
			out->kind = MD_HEADING;
			out->level = (next[ni] == '=') ? 1 : 2;
			return true;		// caller skips the underline
		}

	}

	return false;

}

bool md_continues(const md_state_t *st, md_kind_t cur, const char *next) {

	// Inside a fence every line stands alone -- joining them would
	// destroy the layout that is the entire point of a code block.
	if (st->in_fence) return false;

	if (cur != MD_PARA && cur != MD_LIST && cur != MD_QUOTE) return false;

	if (!next) return false;

	int i = skip_spaces(next, 0);

	if (!next[i]) return false;			// blank ends the block

	// Anything that starts a block of its own ends this one.
	if (next[i] == '#') return false;
	if (next[i] == '>') return false;
	if (next[i] == '|') return false;

	if (fence_open(next, i, NULL)) return false;

	if (all_of(next, '-', 3) || all_of(next, '*', 3) || all_of(next, '_', 3))
		return false;

	// A new list item ends the previous one. Note this is checked
	// AFTER the rule test above, so "- - -" is a rule rather than an
	// item.
	if ((next[i] == '-' || next[i] == '*' || next[i] == '+') &&
		is_space(next[i + 1]))
		return false;

	if (is_digit(next[i])) {
		int n = i;
		while (is_digit(next[n])) n++;
		if ((next[n] == '.' || next[n] == ')') && is_space(next[n + 1]))
			return false;
	}

	return true;

}
