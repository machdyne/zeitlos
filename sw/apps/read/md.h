#ifndef MD_H
#define MD_H

/*
 * Zeitlos
 * Copyright (c) 2025 Lone Dynamics Corporation. All rights reserved.
 *
 * A small GitHub-flavoured Markdown parser: one source line in, one
 * described line out. No drawing, no files, no windows -- so it can be
 * compiled for a host and run against real documents (md_test.c),
 * which is the only way to have any confidence in a parser.
 *
 * -- line at a time, and why --
 *
 * sw/apps/read must open files of any size, which rules out holding
 * the document in memory and therefore rules out any parser that
 * wants the whole thing. This one carries a few bytes of state
 * (md_state_t) between lines instead, so a renderer can seek to a
 * remembered position, restore the state that went with it, and start
 * parsing there. That is what makes backward scrolling possible in a
 * file too big to buffer.
 *
 * The cost is that constructs needing lookahead cannot be supported.
 * Setext headings are the exception, handled by the CALLER passing the
 * following line (see md_parse), because they are common enough in
 * real documents to be worth the one-line peek.
 *
 * -- scope --
 *
 * Chosen by counting what actually appears in the corpus this is for
 * (the Timeless Computing book's CHAPTER SOURCES and this project's
 * own docs), not from the specification. In descending order of
 * frequency there: inline code, bullets, headings, indented code,
 * table rows, fenced code, bold, ordered lists, links, block quotes,
 * rules.
 *
 * -- hand-written Markdown, not generated --
 *
 * The input is Markdown as a person wrote it. Pandoc's output was
 * briefly supported and the support was removed: it needed fenced
 * divs (:::), attribute blocks ({#anchor}) and simple tables, none of
 * which appear even once in the real corpus, and each of which is a
 * rule that can silently eat ordinary content -- a line of colons in
 * a document about C++ scope resolution, say.
 *
 * The book's chapters are Markdown to begin with; running them
 * through Pandoc first only produces a downstream form aimed at a
 * print renderer. Read the sources.
 *
 * Two things that CAME from that exercise stayed, because neither is
 * Pandoc-specific: lazy continuation of hard-wrapped lines
 * (md_continues), which any 80-column document needs, and telling a
 * nested list item from an indented code block by context
 * (md_state_t.in_list), which occurs in the chapters independently.
 *
 * Deliberately NOT handled, with reasons:
 *
 *   - Nested list indentation beyond one level. Rare here, and deep
 *     nesting is unreadable at this window size anyway.
 *   - Reference-style links, images, footnotes, HTML blocks. Images
 *     do not appear at all; the rest are rare and have no useful
 *     rendering on a 1bpp text display.
 *   - Emphasis as a VISUAL style. The markers are stripped so the
 *     text reads correctly, but there is no bold or italic face --
 *     one weight per font, and underline already means "link". See
 *     MD_SPAN_CODE for the one inline style that does render.
 */

#include <stdint.h>
#include <stdbool.h>

// Longest source line handled. Real lines run to ~554 characters in
// the corpus (Pandoc output is not wrapped); anything past this is
// truncated rather than wrapping into the next parse, which would
// desynchronise the line numbering the index depends on.
#define MD_LINE_MAX   1024

// Inline spans recorded per line. A line with more styled runs than
// this renders the excess as plain text -- degraded, never wrong.
#define MD_MAX_SPANS  24

// Link targets recorded per line.
#define MD_MAX_LINKS  8
#define MD_LINK_MAX   96

typedef enum {
	MD_BLANK = 0,	// an empty source line -- a paragraph break
	// A line that produces NO output and no spacing either: a fence
	// delimiter, a front-matter line, a table's |---|---| separator.
	// Distinct from MD_BLANK because a renderer turns a blank line
	// into vertical space, and doing that for these leaves a gap
	// where the source had a piece of syntax -- most visibly a hole
	// between a table's header and its first row.
	MD_SKIP,
	MD_PARA,
	MD_HEADING,		// level in md_line_t.level, 1-6
	MD_CODE,		// preformatted: never wrapped, never styled
	MD_QUOTE,
	MD_LIST,		// bullet or ordered; marker already in `marker`
	MD_RULE,
	MD_TABLE,		// a row of a pipe table -- see the note below
} md_kind_t;

typedef enum {
	MD_SPAN_CODE = 1,	// `inline code` -- rendered inverse
	MD_SPAN_LINK,		// [text](target) -- rendered underlined
} md_span_kind_t;

typedef struct {
	uint16_t	start;		// offset into md_line_t.text
	uint16_t	len;
	uint8_t		kind;		// md_span_kind_t
	uint8_t		link;		// index into md_line_t.links, for MD_SPAN_LINK
} md_span_t;

typedef struct {

	md_kind_t	kind;

	// MD_HEADING: 1-6. MD_LIST: nesting depth, 0 for top level.
	uint8_t		level;

	// MD_LIST: the marker to draw, already resolved -- a bullet for
	// unordered, or the literal number text for ordered. The caller
	// draws this in the hanging indent rather than re-deriving it.
	char		marker[8];

	// The line's text with block markers and inline syntax removed,
	// NUL-terminated. This is what gets wrapped and drawn.
	char		text[MD_LINE_MAX];
	uint16_t	len;

	md_span_t	spans[MD_MAX_SPANS];
	uint8_t		nspans;

	char		links[MD_MAX_LINKS][MD_LINK_MAX];
	uint8_t		nlinks;

} md_line_t;

// Parser state carried between lines.
//
// Small and trivially copyable ON PURPOSE: sw/apps/read stores one of
// these alongside every index checkpoint, so it can resume parsing at
// an arbitrary point in a file it has not read into memory. Anything
// bigger or with pointers in it would defeat that.
typedef struct {
	bool	in_fence;		// inside ``` or ~~~
	char	fence_char;
	uint8_t	fence_len;
	bool	in_front;		// inside a --- front matter block
	bool	seen_any;		// any non-blank line yet (front matter must
							// start at the very top of the file)

	// The previous block was a list item.
	//
	// This exists to resolve one genuine ambiguity: four spaces of
	// indentation means an indented code block, but a NESTED list
	// item is also indented, often by exactly four. Deciding by
	// indentation alone turns every nested list in a document into
	// code -- which is what the Timeless Computing book's entire
	// table of contents is made of.
	//
	// So a deeply indented line that starts with a list marker is a
	// list item when we were already in a list, and code otherwise.
	// That keeps a bullet inside a real code block safe, which
	// indentation alone would not.
	bool	in_list;
} md_state_t;

void md_state_init(md_state_t *st);

// Parses one source line.
//
// `next` is the following source line, or NULL if unknown or at end
// of file -- used ONLY to recognise a setext heading (text with
// ===== or ----- underneath it). A caller that cannot cheaply look
// ahead may pass NULL and will simply see those as a paragraph
// followed by a rule.
//
// Returns true if `next` was consumed as a setext underline, telling
// the caller to skip it.
bool md_parse(md_state_t *st, const char *src, const char *next,
	md_line_t *out);

// Would `next` continue the block that `cur` started, rather than
// beginning a new one?
//
// Markdown's "lazy continuation": a paragraph or list item runs on
// across source lines until something ends it. This matters far more
// than it sounds, because generated Markdown is routinely hard
// wrapped -- Pandoc's output splits link syntax across lines:
//
//     -   [Introduction to Timeless
//         Computing](#introduction-to-timeless-computing)
//
// A parser that treats those as two lines shows the reader raw
// brackets and parentheses instead of a link. So the CALLER joins
// continuation lines into one buffer before calling md_parse(),
// using this to decide where a block ends.
//
// Note the indented-continuation case: four spaces after a list item
// is a continuation, not an indented code block. Code cannot
// interrupt a paragraph, which is exactly why this needs to know what
// came before.
bool md_continues(const md_state_t *st, md_kind_t cur, const char *next);

#endif
