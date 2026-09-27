/*
 * Zeitlos -- sw/apps/web
 *
 * See bodyview.h.
 */

#include <string.h>

#include "bodyview.h"
#include "../../common/zgfx.h"

// Blocks of these kinds run on without a gap between them when they
// follow one of their own kind: a list is not a stack of paragraphs.
static bool runs_on(int kind) {
	return kind == HTML_LIST || kind == HTML_TABLE || kind == HTML_PRE;
}

// Draws from line `sub` of block `blk`, at content-relative `y`, until
// the view is full or the page ends, appending to the model and to the
// link rectangles. `prev_kind` is the kind of the block drawn just
// above (-1 for none); `at_top` says this is the top of the view, where
// no gap is left above the first block. A continuation in the middle of
// a block leaves no gap either.
//
// This is the loop web.c's draw_body() always had, plus the bookkeeping:
// every display line drawn goes into vblock/vsub/vy, and where it stops
// goes into nblock/nsub/ny.
static void draw_run(bodyview_t *v, uint32_t blk, uint32_t sub, int y,
	int prev_kind, bool at_top) {

	static html_line_t blocks[PAGE_FETCH_MAX];
	int bottom = v->view_y + v->view_h;
	uint32_t n, i;

	n = page_fetch(v->pg, blk, PAGE_FETCH_MAX, blocks);

	for (i = 0; i < n; i++) {

		const html_line_t *l = &blocks[i];
		int lh = layout_line_height(l, v->cfg);
		int from = (i == 0) ? (int)sub : 0;
		bool cont = (i == 0) && (at_top || sub > 0);
		int gap = cont ? 0 : layout_gap_before(l, v->cfg);
		int avail, drew;

		if ((int)l->kind == prev_kind && runs_on(prev_kind)) gap = 0;

		avail = (y < bottom) ? (bottom - (y + gap)) / lh : 0;
		if (avail <= 0) {
			// This line does not fit: it is the first not on the glass.
			v->more = true;
			v->nblock = blk + i;
			v->nsub = (uint16_t)from;
			v->ny = (int16_t)y;
			v->nkind = (int8_t)prev_kind;
			return;
		}

		y += gap;
		drew = layout_draw(l, v->cfg, v->crect->y0 + y, from, avail,
			v->crect, (uint16_t)(blk + i), v->hits, v->nhits, v->max_hits);

		for (int j = 0; j < drew && v->nvis < BV_VIS_MAX; j++) {
			v->vblock[v->nvis] = blk + i;
			v->vsub[v->nvis] = (uint16_t)(from + j);
			v->vy[v->nvis] = (int16_t)(y + j * lh);
			v->nvis++;
		}

		y += drew * lh;
		prev_kind = (int)l->kind;

		// Stopped inside this block: the view is full, and the next line
		// not on the glass is the next line of this same block.
		if (drew == avail && from + drew < layout_count(l, v->cfg)) {
			v->more = true;
			v->nblock = blk + i;
			v->nsub = (uint16_t)(from + drew);
			v->ny = (int16_t)y;
			v->nkind = (int8_t)prev_kind;
			return;
		}

	}

	// Out of blocks: the end of the page, or of one fetch's worth.
	v->more = (blk + n < page_blocks(v->pg));
	v->nblock = blk + n;
	v->nsub = 0;
	v->ny = (int16_t)y;
	v->nkind = (int8_t)prev_kind;

}

void bv_draw(bodyview_t *v, page_pos_t top) {

	z_win_fill_rect(v->win, 0, v->view_y, z_win_content_w(v->win), v->view_h, 0);

	*v->nhits = 0;
	v->nvis = 0;
	v->more = false;
	v->valid = true;
	v->drawn_view_h = v->view_h;
	v->drawn_x0 = (int16_t)v->crect->x0;
	v->drawn_y0 = (int16_t)v->crect->y0;

	if (page_blocks(v->pg) == 0) { v->ny = (int16_t)v->view_y; return; }

	draw_run(v, top.block, top.sub, v->view_y, -1, true);

}

bool bv_scroll_to(bodyview_t *v, page_pos_t top) {

	int k, shift, x, y, w, h, bottom, from_y;

	if (!v->valid || v->nvis == 0) return false;
	if (z_win_frozen(v->win)) return false;
	if (v->drawn_view_h != v->view_h || v->drawn_x0 != v->crect->x0 ||
		v->drawn_y0 != v->crect->y0)
		return false;

	// The new top line, on the glass now.
	for (k = 0; k < v->nvis; k++)
		if (v->vblock[k] == top.block && v->vsub[k] == top.sub) break;
	if (k == 0 || k == v->nvis) return false;

	shift = v->vy[k] - v->view_y;
	if (shift <= 0 || shift >= v->view_h) return false;

	x = v->crect->x0;
	y = v->crect->y0 + v->view_y;
	w = v->body_w;
	h = v->view_h;

	// Asked first: on a window not wholly visible the hardware scroll
	// does nothing, and the model must not move while the pixels stay.
	if (!z_fb_hw_scroll_allowed(x, y, w, h)) return false;

	z_fb_hw_scroll(x, y, w, h, -shift);

	// -- the model follows the pixels --
	memmove(v->vblock, v->vblock + k, (size_t)(v->nvis - k) * sizeof(v->vblock[0]));
	memmove(v->vsub, v->vsub + k, (size_t)(v->nvis - k) * sizeof(v->vsub[0]));
	memmove(v->vy, v->vy + k, (size_t)(v->nvis - k) * sizeof(v->vy[0]));
	v->nvis -= k;
	for (int i = 0; i < v->nvis; i++) v->vy[i] = (int16_t)(v->vy[i] - shift);

	// Link rectangles: moved with their text, and gone with it if it
	// went off the top.
	{
		int keep = 0, top_y = v->crect->y0 + v->view_y;
		for (int i = 0; i < *v->nhits; i++) {
			layout_hit_t hit = v->hits[i];
			hit.y = (int16_t)(hit.y - shift);
			if (hit.y < top_y) continue;
			v->hits[keep++] = hit;
		}
		*v->nhits = keep;
	}

	// -- what is new --
	//
	// Below where drawing stopped the glass was blank; that, and the
	// strip the blit leaves behind at the bottom (it copies, it does not
	// clear), is cleared, and drawing carries on from the first line
	// that was not on the glass.
	bottom = v->view_y + v->view_h;
	from_y = v->ny - shift;
	if (from_y < v->view_y) from_y = v->view_y;
	if (from_y < bottom)
		z_win_fill_rect(v->win, 0, from_y, v->body_w, bottom - from_y, 0);

	if (v->more)
		draw_run(v, v->nblock, v->nsub, v->ny - shift, v->nkind, false);
	else
		v->ny = (int16_t)(v->ny - shift);

	return true;

}
