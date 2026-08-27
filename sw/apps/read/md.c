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
		if (c == '`') {

			int j = i + 1;
			while (s[j] && s[j] != '`') j++;

			if (s[j] == '`') {
				span_open(o, MD_SPAN_CODE, 0);
				put_str(o, s + i + 1, j - i - 1);
				span_close(o);
				i = j + 1;
				continue;
			}

			// Unterminated -- a literal backtick, not a run.
			put(o, c);
			i++;
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

// -- block pass --

bool md_parse(md_state_t *st, const char *src, const char *next,
	md_line_t *out) {

	out->kind = MD_BLANK;
	out->level = 0;
	out->marker[0] = 0;
	out->len = 0;
	out->text[0] = 0;
	out->nspans = 0;
	out->nlinks = 0;

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
				out->kind = MD_SKIP;
				return false;
			}
		}

		out->kind = MD_CODE;
		put_str(out, src, MD_LINE_MAX - 1);
		out->text[out->len] = 0;
		return false;

	}

	if (src[fi] == '`' || src[fi] == '~') {

		int n = 0;
		while (src[fi + n] == src[fi]) n++;

		if (n >= 3) {
			st->in_fence = true;
			st->fence_char = src[fi];
			st->fence_len = (uint8_t)n;
			st->seen_any = true;
			out->kind = MD_SKIP;		// the fence line itself draws nothing
			return false;
		}

	}

	// -- front matter --
	//
	// Only at the very top of the file. A --- anywhere else is a rule
	// or a setext underline, and treating it as front matter would
	// silently swallow the rest of the document.
	if (st->in_front) {
		if (all_of(src, '-', 3)) st->in_front = false;
		out->kind = MD_SKIP;
		return false;
	}

	if (!st->seen_any && all_of(src, '-', 3)) {
		st->in_front = true;
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
	// Rendered VERBATIM rather than laid out into columns. The font is
	// fixed width, so a markdown table's own alignment already lines
	// up on screen -- and real column layout would need the whole
	// table measured before drawing any of it, which a reader that
	// streams a file cannot do.
	//
	// The |---|---| separator row is dropped: it carries no
	// information a reader needs once the header is already above it.
	if (src[fi] == '|') {

		bool sep = true;

		for (int k = fi; src[k]; k++) {
			char ch = src[k];
			if (ch != '|' && ch != '-' && ch != ':' && !is_space(ch)) {
				sep = false;
				break;
			}
		}

		if (sep) { out->kind = MD_SKIP; return false; }

		out->kind = MD_TABLE;
		st->in_list = false;
		put_str(out, src + fi, MD_LINE_MAX - 1);
		out->text[out->len] = 0;
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

	if (next[i] == '`' || next[i] == '~') {
		int n = 0;
		while (next[i + n] == next[i]) n++;
		if (n >= 3) return false;
	}

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
