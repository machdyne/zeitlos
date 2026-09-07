/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * See layout.h.
 */

#include <string.h>

#include "layout.h"

// Indentation, in CHARACTERS of the body font rather than pixels, so
// that it stays proportional if the font is ever changed.
#define IND_LIST_PER_LEVEL   2
#define IND_LIST_HANG        3		// room for "12." at the widest
#define IND_QUOTE_PER_LEVEL  2
#define IND_PRE              2

// A quote's vertical bar sits this many pixels left of its text.
#define QUOTE_BAR_GAP        3

// EVERY heading gets the larger font, not just h1 and h2.
//
// The first version gave 6x12 to h1/h2 and left h3 and below at body
// size with no rule, on the reasoning that there is no third size to
// give them. Rendered (tests/render.c) that turned out to mean an
// <h3> was pixel-for-pixel indistinguishable from the paragraph above
// it -- and <h3> is the level a Wikipedia article's subsections
// actually use, so the effect was a page with no visible structure
// below the first two headings.
//
// So the two signals available are spent separately: the larger font
// says "this is a heading", the rule says "this is a top-level one".
static const z_font_t *block_font(const html_line_t *l,
	const layout_cfg_t *cfg) {
	if (l->kind == HTML_HEADING) return cfg->head;
	return cfg->body;
}

// Left inset of the block's text, in pixels, relative to cfg->x.
static int block_indent(const html_line_t *l, const layout_cfg_t *cfg) {

	int cw = cfg->body->w;

	switch (l->kind) {
	case HTML_LIST:
		return (l->level * IND_LIST_PER_LEVEL + IND_LIST_HANG) * cw;
	case HTML_QUOTE:
		return (l->level ? l->level : 1) * IND_QUOTE_PER_LEVEL * cw;
	case HTML_PRE:
		return IND_PRE * cw;
	default:
		return 0;
	}

}

static int text_width(const html_line_t *l, const layout_cfg_t *cfg) {
	int w = cfg->width - block_indent(l, cfg);
	// Never return zero or negative: a window narrow enough to make
	// this happen would otherwise divide by zero in the wrapper, and
	// one character per line is a survivable rendering while a hang is
	// not.
	return w < block_font(l, cfg)->w ? block_font(l, cfg)->w : w;
}

// -- wrapping ------------------------------------------------------
//
// Greedy word wrap. Breaks at spaces; a single word longer than the
// line is broken at the last character that fits, because the
// alternative is a line that runs off the right edge and takes its
// text with it.
//
// Recomputed on every call rather than cached -- see layout.h.

typedef struct {
	uint16_t	start[LAYOUT_MAX_LINES];
	uint16_t	end[LAYOUT_MAX_LINES];		// exclusive, excludes the
											// break's own space
	int			n;
} wrap_t;

static void wrap_block(const html_line_t *l, const layout_cfg_t *cfg,
	wrap_t *w) {

	int cols;
	uint16_t i = 0;

	w->n = 0;

	// Preformatted text is not wrapped: that is what makes it
	// preformatted. It is clipped at the window edge instead, and the
	// caller offers horizontal scrolling for the rest.
	if (l->kind == HTML_PRE) {
		w->start[0] = 0;
		w->end[0] = l->len;
		w->n = 1;
		return;
	}

	cols = text_width(l, cfg) / block_font(l, cfg)->w;
	if (cols < 1) cols = 1;

	if (l->len == 0) {
		w->start[0] = 0;
		w->end[0] = 0;
		w->n = 1;
		return;
	}

	while (i < l->len && w->n < LAYOUT_MAX_LINES) {

		uint16_t line_start = i;
		uint16_t limit = (uint16_t)(i + cols);
		uint16_t brk;

		if (limit >= l->len) {
			w->start[w->n] = line_start;
			w->end[w->n] = l->len;
			w->n++;
			break;
		}

		// Look back from the limit for a space. Scanning back rather
		// than forward is what makes this greedy rather than
		// first-fit, and it is also what makes it O(line) instead of
		// O(block) per line.
		brk = 0;
		for (uint16_t k = limit; k > line_start; k--) {
			if (l->text[k] == ' ') { brk = k; break; }
		}

		if (brk) {
			w->start[w->n] = line_start;
			w->end[w->n] = brk;
			w->n++;
			i = (uint16_t)(brk + 1);
			// A run of spaces at a break -- possible in <pre>-adjacent
			// text that escaped collapsing -- would otherwise start
			// the next line with them.
			while (i < l->len && l->text[i] == ' ') i++;
		} else {
			// One long word: hard break.
			w->start[w->n] = line_start;
			w->end[w->n] = limit;
			w->n++;
			i = limit;
		}

	}

	if (w->n == 0) { w->start[0] = 0; w->end[0] = 0; w->n = 1; }

}

// -- public geometry -----------------------------------------------

int layout_line_height(const html_line_t *l, const layout_cfg_t *cfg) {
	// One pixel of leading below every line. At 5x8 that is a 12.5%
	// increase in line spacing for a 1-in-9 cost in lines per screen,
	// and it is the difference between a wall of text and something
	// readable at this size.
	return block_font(l, cfg)->h + 1;
}

int layout_gap_before(const html_line_t *l, const layout_cfg_t *cfg) {

	if (l->tight) return 0;

	switch (l->kind) {
	case HTML_HEADING:
		// More space above a heading than below it, so a heading reads
		// as belonging to what follows rather than to what precedes.
		return cfg->body->h;
	case HTML_LIST:
	case HTML_TABLE:
	case HTML_PRE:
		// Consecutive list items and table rows are one structure, not
		// a series of paragraphs. The caller suppresses this gap
		// between same-kind neighbours; see web.c.
		return cfg->body->h / 2;
	case HTML_BLANK:
		return 0;
	default:
		return cfg->body->h / 2;
	}

}

// A pixel-at-a-time blit, for boards whose blitter has no
// memory-source mode and as the reference the hardware path is
// checked against.
//
// Slow and obviously correct: source pixel (x, sy+j) to screen pixel
// (dx+x, dy+j), with the source in framebuffer packing -- bit
// (x & 31) of word (x >> 5), least significant bit LEFTMOST, which is
// the opposite of font and icon data.
static void blit_rows(const uint32_t *src, int wpl, int sy,
	int dx, int dy, int w, int h, const z_clip_t *clip) {

	int i, j;

	for (j = 0; j < h; j++) {
		const uint32_t *row = src + (long)(sy + j) * wpl;
		for (i = 0; i < w; i++) {
			int bit = (int)((row[i >> 5] >> (i & 31)) & 1);
			if (bit) z_fb_set_pixel(dx + i, dy + j, 1, clip);
		}
	}

}

// -- image placeholders --------------------------------------------
//
// An image is drawn as an empty box with its caption inside, at the
// size the markup declared when it declared one.
//
// The size is honoured because it is most of the point: a page of
// thumbnails and a page of banners then look different, and a reader
// can tell a decorative flourish from the diagram the text refers to.
// Most of the modern web sizes images in CSS instead, so the default
// below is the common case.
//
// Nothing is fetched. Decoding needs inflate and a PNG reader, which
// is Phase 5 -- and on a 1bpp screen a photograph is mostly noise
// anyway, so line art is the sensible eventual scope.

#define IMG_DEFAULT_W 160		// CSS pixels, when the markup is silent
#define IMG_DEFAULT_H 90

// The box, in display pixels, clamped to the content width.
//
// Declared sizes are in CSS pixels and this screen is 640 wide, so a
// full-width banner would otherwise run off the edge. Scaling keeps
// the aspect ratio, because a squashed box misrepresents what is
// missing.
static void image_box(const html_line_t *l, const layout_cfg_t *cfg,
	int *bw, int *bh) {

	int w, h;

	// A loaded image is drawn at the size it decoded to, so the box
	// is exactly the picture and there is no leftover.
	if (cfg->img_src && l->nlinks && cfg->img_dw > 0 &&
		!strcmp(l->links[0], cfg->img_src)) {
		*bw = cfg->img_dw;
		*bh = cfg->img_dh;
		return;
	}

	w = l->img_w ? l->img_w : IMG_DEFAULT_W;
	h = l->img_h ? l->img_h : IMG_DEFAULT_H;

	if (w > cfg->width) {
		// Integer scale, height first so the rounding goes into the
		// dimension nobody is comparing against a margin.
		h = (int)(((long)h * cfg->width) / w);
		w = cfg->width;
	}

	// Floors: a 1x1 tracking pixel that survived the empty-alt check
	// should still be visible as something, and a box shorter than a
	// line of text cannot hold its caption.
	if (w < 3 * cfg->body->w) w = 3 * cfg->body->w;
	if (h < 2 * cfg->body->h) h = 2 * cfg->body->h;

	// And a ceiling, so one absurd height attribute cannot push the
	// rest of the page off the bottom.
	//
	// 24 lines, not 12: at an 8-pixel font 12 lines is 96 pixels, and
	// a perfectly ordinary 120-pixel diagram was being squashed to
	// four fifths of its declared height. The cap is for the
	// pathological case -- height="4000" -- not for real images.
	if (h > 24 * cfg->body->h) h = 24 * cfg->body->h;

	*bw = w;
	*bh = h;

}

void layout_image_box(const html_line_t *l, const layout_cfg_t *cfg,
	int *w, int *h) {
	image_box(l, cfg, w, h);
}

static int image_lines(const html_line_t *l, const layout_cfg_t *cfg) {
	int bw, bh;
	image_box(l, cfg, &bw, &bh);
	(void)bw;
	// Rounded up, so the box always has room for its own border.
	return (bh + cfg->body->h - 1) / cfg->body->h;
}

int layout_count(const html_line_t *l, const layout_cfg_t *cfg) {

	wrap_t w;

	if (l->kind == HTML_IMAGE) return image_lines(l, cfg);
	if (l->kind == HTML_RULE || l->kind == HTML_BLANK) return 1;

	wrap_block(l, cfg, &w);
	return w.n;

}

bool layout_line_range(const html_line_t *l, const layout_cfg_t *cfg,
	int n, uint16_t *start, uint16_t *end) {

	wrap_t w;

	// An image has no text rows to select: the caption is drawn as
	// part of the box, not wrapped.
	if (l->kind == HTML_IMAGE) return false;
	if (l->kind == HTML_RULE || l->kind == HTML_BLANK) return false;

	wrap_block(l, cfg, &w);
	if (n < 0 || n >= w.n) return false;

	*start = w.start[n];
	*end = w.end[n];
	return true;

}

// -- style lookup ---------------------------------------------------

// Which span, if any, covers character `i`. Linear over the block's
// spans; there are at most HTML_MAX_SPANS of them and the scan runs
// once per character, which at 120 characters a line is nothing next
// to the per-character glyph blit it accompanies.
static const html_span_t *span_at(const html_line_t *l, uint16_t i) {
	for (int k = 0; k < l->nspans; k++)
		if (i >= l->spans[k].start &&
			i < (uint16_t)(l->spans[k].start + l->spans[k].len))
			return &l->spans[k];
	return NULL;
}

// -- drawing --------------------------------------------------------

int layout_draw(const html_line_t *l, const layout_cfg_t *cfg,
	int y, int from, int max, const z_clip_t *clip,
	uint16_t block, layout_hit_t *hits, int *nhits, int maxhits) {

	const z_font_t *f = block_font(l, cfg);
	int indent = block_indent(l, cfg);
	int lh = layout_line_height(l, cfg);
	int drawn = 0;
	wrap_t w;

	if (max <= 0) return 0;

	if (l->kind == HTML_BLANK) return (from == 0) ? 1 : 0;

	if (l->kind == HTML_IMAGE) {

		int bw, bh, rows, cap_w, cx, cy;
		const char *cap = l->text;

		image_box(l, cfg, &bw, &bh);
		rows = image_lines(l, cfg);

		// Scrolled partly off the top, the box is drawn CLIPPED
		// rather than skipped.
		//
		// It used to return without drawing anything, so scrolling
		// into an image showed blank lines until the whole thing fit
		// -- which reads as a broken page, not as a picture arriving.
		{
			int skip = from * cfg->body->h;

			if (skip >= bh) return rows - from > max ? max : rows - from;

			// Everything below draws from the box's true top, with
			// the clip rectangle the caller already supplied keeping
			// it inside the view.
			y -= skip;
		}

		// A decoded image, when the browser has one for this block.
		//
		// Drawn INSTEAD of the box, at the size the decoder produced
		// -- which is at most the box, because that is the size it
		// was asked for. Anything left over stays blank rather than
		// being stretched: a dithered bitmap does not survive
		// resampling, and a border of background is honest about what
		// was decoded.
		if (cfg->img) {

			layout_img_t o;

			cfg->img(block, &o);

			// Destination is SCREEN coordinates, which is what
			// cfg->x already is and what every other hardware call
			// in this file takes.
			//
			// The blit is not clipped: a box scrolled partly off the
			// top is not drawn at all (see `from` above), and the
			// window's own content rectangle bounds the rest. If the
			// hardware path is unavailable the call reports so, and
			// the placeholder box below is drawn instead -- an empty
			// frame is a better answer than a half-blitted picture.
			// src_y skips the rows scrolled off the top, so a
			// partly visible picture shows its lower part rather
			// than nothing.
			if (o.bits) {

				int skip = from * cfg->body->h;
				int sy = skip, dy = y + skip;
				int hh = o.h - skip;
				int ww = o.w;

				// z_fb_hw_blit_mem() clips to the SCREEN and nothing
				// else (zgfx.h), so the rectangle has to be narrowed
				// here -- otherwise an image scrolled up paints over
				// the toolbar and out of the window.
				if (clip) {
					if (dy < clip->y0) {
						int cut = clip->y0 - dy;
						sy += cut; dy += cut; hh -= cut;
					}
					if (dy + hh - 1 > clip->y1) hh = clip->y1 - dy + 1;
					if (cfg->x + ww - 1 > clip->x1)
						ww = clip->x1 - cfg->x + 1;
				}

				// A SOFTWARE path as well as the hardware one.
				//
				// sw/apps/view checks z_fb_hw_blit_mem_available()
				// once and keeps blit_sw() for bitstreams whose
				// blitter predates memory-source mode. This had no
				// equivalent, so on such a board the call returns
				// false and the picture silently becomes an empty
				// box -- and if the hardware path is subtly wrong,
				// there is nothing to compare against.
				//
				// WEB_IMG_SW=1 forces the software path, which is the
				// way to tell a decode fault from a blit fault
				// without a logic analyser.
				// STRIDE IS IN BYTES, not words.
				//
				// Passing words scrambles every row by a factor of
				// four, which does not look like a stride bug -- it
				// looks like the dither is broken, which is how it
				// was reported. sw/apps/view passes DOC_WPL * 4 for
				// the same reason.
				if (hh > 0 && ww > 0) {

					bool done = false;

#if !WEB_IMG_SW
					done = z_fb_hw_blit_mem(o.bits, o.wpl * 4,
						0, sy, cfg->x, dy, ww, hh);
#endif

					if (!done) {
						blit_rows(o.bits, o.wpl, sy, cfg->x, dy,
							ww, hh, clip);
						done = true;
					}

					if (done)
						return rows - from > max ? max : rows - from;

				}

			}

			if (o.caption) cap = o.caption;

		}

		// An OUTLINE, not a filled box: four lines. A filled
		// rectangle would be a black slab on a 1bpp screen and would
		// hide its own caption.
		{
			int x0 = cfg->x, x1 = cfg->x + bw - 1;
			int y0 = y, y1 = y + bh - 1;

			z_fb_hw_line(x0, y0, x1, y0, 1, clip);
			z_fb_hw_line(x0, y1, x1, y1, 1, clip);
			z_fb_hw_line(x0, y0, x0, y1, 1, clip);
			z_fb_hw_line(x1, y0, x1, y1, 1, clip);
		}

		// Caption centred, TRUNCATED to the box rather than wrapped:
		// this is a label on a placeholder, not content, and a
		// caption that reflowed the page would defeat the point of
		// drawing the declared size.
		{
			int fit = (bw - 2 * cfg->body->w) / cfg->body->w;
			int n = (int)strlen(cap);
			int i;

			if (fit < 1) fit = 1;
			if (n > fit) n = fit;

			cap_w = n * cfg->body->w;
			cx = cfg->x + (bw - cap_w) / 2;
			cy = y + (bh - cfg->body->h) / 2;

			for (i = 0; i < n; i++)
				z_fb_draw_char(cx + i * cfg->body->w, cy,
					(unsigned char)cap[i], 1, cfg->body, clip);
		}

		// A hit rectangle over the whole box, so the box can be
		// clicked exactly like a link. The browser decides what that
		// means -- for an image it loads it in place rather than
		// navigating.
		if (l->nlinks && hits && nhits && *nhits < maxhits) {
			layout_hit_t *hh = &hits[(*nhits)++];
			hh->x = (int16_t)cfg->x;
			hh->y = (int16_t)y;
			hh->w = (int16_t)bw;
			hh->h = (int16_t)bh;
			hh->link = 0;
			hh->block = block;
		}

		return rows > max ? max : rows;

	}

	if (l->kind == HTML_RULE) {
		if (from != 0) return 0;
		// Centred vertically in its line so a rule between two
		// paragraphs is not stuck to either.
		z_fb_hw_line(cfg->x, y + lh / 2, cfg->x + cfg->width - 1,
			y + lh / 2, 1, clip);
		return 1;
	}

	wrap_block(l, cfg, &w);

	for (int n = from; n < w.n && drawn < max; n++, drawn++) {

		int row = y + drawn * lh;
		int cx = cfg->x + indent;
		int link_run_x = 0;
		int link_run_w = 0;
		int link_run_idx = -1;

		// The bullet or number goes in the hanging indent, on the
		// FIRST display line of the block only -- a wrapped list item
		// lines up under its own text, not under its marker.
		if (l->kind == HTML_LIST && n == 0 && l->marker[0]) {
			int mx = cfg->x + l->level * IND_LIST_PER_LEVEL * f->w;
			z_fb_draw_text(mx, row, l->marker, 1, cfg->body, clip);
		}

		// A blockquote's vertical bar, drawn on every line of the
		// block so a quote reads as one object.
		if (l->kind == HTML_QUOTE) {
			int bx = cfg->x + indent - QUOTE_BAR_GAP;
			if (bx >= cfg->x)
				z_fb_hw_line(bx, row, bx, row + f->h - 1, 1, clip);
		}

		for (uint16_t i = w.start[n]; i < w.end[n]; i++) {

			const html_span_t *sp = span_at(l, i);
			char ch = l->text[i];
			bool is_link = sp && sp->kind == HTML_SPAN_LINK && cfg->links_live;
			bool is_code = sp && sp->kind == HTML_SPAN_CODE;

			if (is_code) {
				// Inverse: one pixel of padding is not available at
				// this size, so the glyph cell itself carries it.
				z_fb_draw_char2(cx, row, ch, 0, 1, f, clip);
			} else {
				z_fb_draw_char(cx, row, ch, 1, f, clip);
			}

			if (is_link) {
				// Underline the character cell. Drawn per character
				// rather than per run so that a run interrupted by a
				// non-link character does not get a continuous rule.
				z_fb_hw_line(cx, row + f->h, cx + f->w - 1, row + f->h,
					1, clip);
				if (link_run_idx == (int)sp->link) {
					link_run_w += f->w;
				} else {
					// A new link starts here; bank the previous run.
					if (link_run_idx >= 0 && hits && nhits &&
						*nhits < maxhits) {
						layout_hit_t *h = &hits[(*nhits)++];
						h->x = (int16_t)link_run_x;
						h->y = (int16_t)row;
						h->w = (int16_t)link_run_w;
						h->h = (int16_t)(f->h + 1);
						h->link = (uint8_t)link_run_idx;
						h->block = block;
					}
					link_run_idx = (int)sp->link;
					link_run_x = cx;
					link_run_w = f->w;
				}
			} else if (link_run_idx >= 0) {
				if (hits && nhits && *nhits < maxhits) {
					layout_hit_t *h = &hits[(*nhits)++];
					h->x = (int16_t)link_run_x;
					h->y = (int16_t)row;
					h->w = (int16_t)link_run_w;
					h->h = (int16_t)(f->h + 1);
					h->link = (uint8_t)link_run_idx;
					h->block = block;
				}
				link_run_idx = -1;
			}

			cx += f->w;

			// Preformatted text is clipped at the window edge rather
			// than wrapped, so stop before drawing past it.
			if (cx >= cfg->x + cfg->width) break;

		}

		if (link_run_idx >= 0 && hits && nhits && *nhits < maxhits) {
			layout_hit_t *h = &hits[(*nhits)++];
			h->x = (int16_t)link_run_x;
			h->y = (int16_t)row;
			h->w = (int16_t)link_run_w;
			h->h = (int16_t)(f->h + 1);
			h->link = (uint8_t)link_run_idx;
			h->block = block;
		}

		// A truncated block says so, on its last line, rather than
		// simply stopping -- a reader cannot otherwise tell a
		// truncation from a short paragraph.
		if (l->truncated && n == w.n - 1 &&
			cx + 4 * f->w < cfg->x + cfg->width)
			z_fb_draw_text(cx, row, "[...]", 1, f, clip);

	}

	// h1 and h2 get a rule under the whole block. Below h2 there is no
	// third size and no rule -- see layout.h.
	if (l->kind == HTML_HEADING && l->level <= 2 &&
		from + drawn >= w.n && drawn > 0) {
		int row = y + (drawn - 1) * lh + f->h;
		z_fb_hw_line(cfg->x, row, cfg->x + cfg->width - 1, row, 1, clip);
	}

	return drawn;

}
