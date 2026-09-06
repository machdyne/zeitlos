#ifndef Z_DOCK_LAYOUT_H
#define Z_DOCK_LAYOUT_H

/*
 * Zeitlos -- dock layout arithmetic.
 *
 * Where each dock icon is, which app is under it, and how the icons
 * divide into pages. Pure functions of their arguments: no globals, no
 * drawing, no window manager. wm.c passes its own state in.
 *
 * -- why this is a file and not just code in wm.c --
 *
 * draw_dock(), dock_click() and dock_handle_key() all have to agree
 * exactly on where an icon is and which app it belongs to. While that
 * was one multiplication each, three copies was survivable. With a
 * page offset and a NEXT button that is right-aligned rather than on
 * the stride, it is three chances for a click to launch the icon next
 * to the one you pressed -- and a click landing one slot off is the
 * kind of bug that gets blamed on the mouse for a week.
 *
 * Pulling it out here makes it one copy, and makes it testable without
 * a window manager, a framebuffer or a board: see tests/test_dock.c.
 * That is the real reason for the shape of these functions -- taking
 * `napps` and `page` as parameters rather than reading wm's globals is
 * what lets a test ask about a 40-app dock on a machine that has six.
 *
 * -- the layout --
 *
 *   +--------------------------------------------------+
 *   | [a][b][c][d][e][f][g][h][i][j][k][l][m][n][o][p] [>] |
 *   +--------------------------------------------------+
 *
 * One row, always -- vertical space is the scarcest thing on a 480px
 * screen, so the dock does not get a second row.
 *
 * Up to DOCK_SLOTS_MAX apps, none of this engages: the dock is sized to
 * its contents and grown from the left, exactly as it always was, with
 * no NEXT icon and one page. Paging appears only when the icons
 * genuinely do not all fit.
 *
 * Once it does, the frame spans the full screen width and the NEXT icon
 * is right-aligned inside it. Both of those are deliberate:
 *
 *   - NEXT never moves. It is in the same place on every page, at the
 *     screen edge, so it does not shift under the pointer when a short
 *     final page has fewer icons than the first.
 *
 *   - The empty positions on a short page are visible as empty space
 *     inside a full-width frame. That is the only indication that you
 *     are not on the first page, and it costs no chrome, no label and
 *     no page counter -- the first page is always full, so empty space
 *     can only mean a later one.
 */

#include <stdbool.h>

#ifndef WM_SCREEN_W
#error "define WM_SCREEN_W before including dock_layout.h"
#endif

#define DOCK_ICON_SIZE   32	// fixed icon size
#define DOCK_ICON_GAP     4	// space between adjacent icons
#define DOCK_PADDING      4	// space between icons and the dock's own border
#define DOCK_MARGIN       8	// space between the dock and the screen edges

#define DOCK_STRIDE      (DOCK_ICON_SIZE + DOCK_ICON_GAP)

/*
 * How many icon positions fit across the screen in one row, margin to
 * margin. 17 at the current sizes on a 640px screen.
 *
 * Derived rather than written down, because the number used to live
 * only in a comment and comments do not get recomputed when
 * DOCK_ICON_SIZE or the screen width changes.
 *
 * The + DOCK_ICON_GAP is because n icons have n-1 gaps between them --
 * the last one needs no trailing gap.
 */
#define DOCK_SLOTS_MAX \
	((WM_SCREEN_W - 2 * DOCK_MARGIN - 2 * DOCK_PADDING + DOCK_ICON_GAP) \
		/ DOCK_STRIDE)

/* The rightmost position, reserved for NEXT whenever paging is on. */
#define DOCK_NEXT_SLOT   (DOCK_SLOTS_MAX - 1)

/* Apps per page while paging: every position except NEXT's. */
#define DOCK_PAGE_SIZE   DOCK_NEXT_SLOT

/*
 * Does this many apps need paging at all?
 *
 * Strictly greater, not >=. At exactly DOCK_SLOTS_MAX they all fit,
 * and paging then would spend a slot on a NEXT button leading to an
 * empty page -- worse than useless, since it also drops a visible app
 * to make room for it.
 */
static inline bool z_dock_paged(int napps) {
	return napps > DOCK_SLOTS_MAX;
}

static inline int z_dock_pages(int napps) {
	if (!z_dock_paged(napps)) return 1;
	return (napps + DOCK_PAGE_SIZE - 1) / DOCK_PAGE_SIZE;
}

/* Icons on `page`. The last page is usually short. */
static inline int z_dock_page_apps(int napps, int page) {

	int n;

	if (!z_dock_paged(napps)) return napps;
	if (page < 0) return 0;

	n = napps - page * DOCK_PAGE_SIZE;
	if (n < 0) n = 0;

	return n > DOCK_PAGE_SIZE ? DOCK_PAGE_SIZE : n;

}

/*
 * The app index at `slot` on `page`, or -1 if that position holds no
 * app -- past the end of a short page, or the NEXT position itself.
 *
 * The return value indexes wm's dock_apps[] AND its dock_launching[].
 * Those two are parallel and must stay so, which is why this returns
 * an app index rather than anything page-relative: a page-relative
 * number would have to be converted at every use, and the "launching"
 * highlight would end up on the wrong icon the first time somebody
 * forgot.
 */
static inline int z_dock_app_at(int napps, int page, int slot) {

	if (slot < 0 || slot >= DOCK_SLOTS_MAX) return -1;

	if (!z_dock_paged(napps))
		return slot < napps ? slot : -1;

	if (slot >= z_dock_page_apps(napps, page)) return -1;

	return page * DOCK_PAGE_SIZE + slot;

}

static inline bool z_dock_slot_is_next(int napps, int slot) {
	return z_dock_paged(napps) && slot == DOCK_NEXT_SLOT;
}

/* The dock window's width. */
static inline int z_dock_width(int napps) {

	if (z_dock_paged(napps))
		return WM_SCREEN_W - 2 * DOCK_MARGIN;

	return DOCK_PADDING * 2 + napps * DOCK_ICON_SIZE +
		(napps - 1) * DOCK_ICON_GAP;

}

/* X of `slot` within the dock window. */
static inline int z_dock_slot_x(int napps, int slot) {

	/*
	 * NEXT is right-aligned to the frame rather than placed on the
	 * stride. On a 640px screen that puts it 8px further right than a
	 * stride-placed 17th icon would sit, which is a gap rather than a
	 * rounding error: it reads as "not one of the apps" without
	 * needing a separator drawn, and it puts NEXT hard against the
	 * screen edge, which is the easiest place on the row to hit.
	 */
	if (z_dock_slot_is_next(napps, slot))
		return z_dock_width(napps) - DOCK_PADDING - DOCK_ICON_SIZE;

	return DOCK_PADDING + slot * DOCK_STRIDE;

}

/*
 * The slot at `local_x` (relative to the dock window), or -1.
 *
 * Searched rather than divided by the stride: NEXT is not on the
 * stride, and an empty position on a short page must not answer to a
 * click. Seventeen comparisons on a mouse click is not a cost worth
 * thinking about, and the division would have to be special-cased at
 * both ends anyway.
 */
static inline int z_dock_slot_at_x(int napps, int page, int local_x) {

	int slot;

	for (slot = 0; slot < DOCK_SLOTS_MAX; slot++) {

		int sx;

		if (!z_dock_slot_is_next(napps, slot) &&
			z_dock_app_at(napps, page, slot) < 0) continue;

		sx = z_dock_slot_x(napps, slot);
		if (local_x >= sx && local_x < sx + DOCK_ICON_SIZE) return slot;

	}

	return -1;

}

#endif
