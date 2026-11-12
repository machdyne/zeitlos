/*
 * Host test: the list box's two kinds of selection (sw/common/zwidget.c,
 * z_listbox_*): single -- the selected row is the choice (settings'
 * languages and desktop colours) -- and multiple, with a check box a
 * row (z_listbox_set_checks()), whose state the app keeps.
 *
 *   make -C sw/common/tests listbox
 *
 * Needs -no-pie and vm.mmap_min_addr=0 (zrender.h); exits 77 without.
 */
#include "zrender.h"
#include "ztramp.h"

#include <stdio.h>
#include <string.h>

#include "zwin.h"
#include "zwidget.h"
#include "zfont.h"
#include "zkbd.h"
#include "zwm.h"

static int fails, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; \
	printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static z_win_t win;

static const char *const names[] = { "Apple", "Banana", "Cherry", "Damson", "Elder" };
static bool ticked[5];

static const char *label(void *u, int i) { (void)u; return names[i]; }
static bool checked(void *u, int i) { (void)u; return ticked[i]; }

// Ink in a row's check box cell -- the "x" -- at screen pixels.
static int ink_in_box(const z_listbox_t *lb, int row) {
	z_clip_t c;
	z_win_content_rect(&win, &c);
	int x0 = c.x0 + lb->x + 1 + 2 + z_font_5x8.w;		// the cell after "["
	int y0 = c.y0 + lb->y + 1 + row * (z_font_5x8.h + 2) + 1;
	int n = 0;
	for (int y = y0; y < y0 + z_font_5x8.h; y++)
		for (int x = x0; x < x0 + z_font_5x8.w; x++) n += z_render_get(x, y);
	return n;
}

int main(void) {

	if (!z_render_open(&win, 320, 240) || !z_tramp_install()) {
		printf("test_listbox: skipped (needs -no-pie and vm.mmap_min_addr=0)\n");
		return 77;
	}

	z_listbox_t lb;
	z_listbox_init(&lb, &win);
	z_listbox_set_geom(&lb, 10, 10, 200, 5 * (z_font_5x8.h + 2) + 2);
	z_listbox_set_items(&lb, 5, label, NULL);
	z_listbox_select(&lb, 0);

	// single: Space does nothing to a row; the selection is the choice
	CHECK(z_listbox_key(&lb, ' ') == Z_LIST_NONE, "single: Space is not a toggle");
	CHECK(z_listbox_key(&lb, Z_KEY_DOWN) == Z_LIST_SELECTED && z_listbox_selected(&lb) == 1,
		"single: Down selects the next row");
	CHECK(z_listbox_key(&lb, '\r') == Z_LIST_ACTIVATED, "single: Enter activates");

	// multiple: Space toggles the selected row, and the app keeps it
	z_listbox_set_checks(&lb, checked);
	z_render_clear();
	z_listbox_draw(&lb, true);
	CHECK(ink_in_box(&lb, 2) == 0, "multiple: an unchecked box is empty");
	CHECK(z_listbox_key(&lb, ' ') == Z_LIST_TOGGLED, "multiple: Space toggles");
	ticked[z_listbox_selected(&lb)] = !ticked[z_listbox_selected(&lb)];
	z_listbox_refresh(&lb);
	z_listbox_draw(&lb, false);
	CHECK(z_listbox_key(&lb, Z_KEY_DOWN) == Z_LIST_SELECTED && !ticked[2],
		"multiple: moving checks nothing");
	// row 1 no longer selected, so not inverted: its box has the x
	CHECK(ticked[1] && ink_in_box(&lb, 1) > 0 && ink_in_box(&lb, 3) == 0,
		"multiple: the box shows the check, redrawn in place");
	CHECK(z_listbox_key(&lb, '\r') == Z_LIST_ACTIVATED, "multiple: Enter still activates");

	// a letter still finds a row ("d": Damson), and Space right after it
	// is part of the name being typed, not a toggle
	CHECK(z_listbox_key(&lb, 'd') == Z_LIST_SELECTED && z_listbox_selected(&lb) == 3,
		"multiple: a letter finds a row");
	CHECK(z_listbox_key(&lb, ' ') != Z_LIST_TOGGLED, "multiple: Space while typing a name is not a toggle");

	// a click on the box toggles; on the name, selects
	z_clip_t c;
	z_win_content_rect(&win, &c);
	int ry = lb.y + 1 + 4 * (z_font_5x8.h + 2) + 3;
	z_listbox_mouse(&lb, lb.x + 6, ry, 0);
	CHECK(z_listbox_mouse(&lb, lb.x + 6, ry, Z_MOUSE_BTN_LEFT) == Z_LIST_TOGGLED &&
		z_listbox_selected(&lb) == 4, "multiple: a click on the box toggles that row");
	z_listbox_mouse(&lb, lb.x + 60, ry - (z_font_5x8.h + 2), 0);
	int r = z_listbox_mouse(&lb, lb.x + 60, ry - (z_font_5x8.h + 2), Z_MOUSE_BTN_LEFT);
	CHECK(r == Z_LIST_SELECTED && z_listbox_selected(&lb) == 3, "multiple: a click on the name selects it");

	// and back to single: no boxes
	z_listbox_set_checks(&lb, NULL);
	z_render_clear();
	z_listbox_draw(&lb, true);
	CHECK(z_listbox_key(&lb, ' ') == Z_LIST_NONE, "single again: Space is not a toggle");

	printf("test_listbox: %d checks, %d failed\n", checks, fails);
	return fails ? 1 : 0;

}
