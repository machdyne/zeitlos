/*
 * Host test for wm's dock layout.
 *
 *   cc -std=gnu99 -Wall -Wextra -o /tmp/t sw/apps/wm/tests/test_dock.c && /tmp/t
 *
 * Unlike tests/test_region.c, nothing here is copied out of wm.c --
 * dock_layout.h is included directly, so there is exactly one copy of
 * this arithmetic and this tests the one that runs.
 *
 * What makes it worth testing: draw_dock(), dock_click() and
 * dock_handle_key() all ask these functions where an icon is and which
 * app is under it. If they disagree by one position, a click launches
 * the app NEXT to the one that was pressed -- which looks like a mouse
 * problem, not a layout problem, and gets debugged in the wrong place
 * for a week.
 */

#include <stdio.h>
#include <stdbool.h>

#define WM_SCREEN_W 640
#define WM_SCREEN_H 480

#include "../dock_layout.h"

static int failures = 0;
static int checks = 0;

#define CHECK(cond) do { \
	checks++; \
	if (!(cond)) { \
		printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
		failures++; \
	} \
} while (0)

#define CHECK_EQ(got, want) do { \
	long g_ = (long)(got), w_ = (long)(want); \
	checks++; \
	if (g_ != w_) { \
		printf("FAIL %s:%d: %s == %ld, wanted %ld\n", \
			__FILE__, __LINE__, #got, g_, w_); \
		failures++; \
	} \
} while (0)

/* The numbers this whole design is built on. If the screen or the icon
 * size ever changes, this is the test that says so out loud rather
 * than letting a comment go stale. */
static void test_capacity(void) {

	CHECK_EQ(DOCK_STRIDE, 36);
	CHECK_EQ(DOCK_SLOTS_MAX, 17);
	CHECK_EQ(DOCK_PAGE_SIZE, 16);
	CHECK_EQ(DOCK_NEXT_SLOT, 16);

	/* A full row must actually fit on the screen, with the margin on
	 * both sides. This is the check the old hand-computed comment
	 * could not do. */
	CHECK(DOCK_MARGIN + z_dock_width(DOCK_SLOTS_MAX) <=
		WM_SCREEN_W - DOCK_MARGIN);

	/* ... and one more must not, or DOCK_SLOTS_MAX is too small. */
	CHECK(DOCK_MARGIN + DOCK_PADDING * 2 +
		(DOCK_SLOTS_MAX + 1) * DOCK_ICON_SIZE +
		DOCK_SLOTS_MAX * DOCK_ICON_GAP > WM_SCREEN_W - DOCK_MARGIN);

}

/* Below the limit, none of the paging machinery engages and the dock
 * is exactly what it was before any of this existed. */
static void test_unpaged(void) {

	int n;

	for (n = 1; n <= DOCK_SLOTS_MAX; n++) {
		CHECK(!z_dock_paged(n));
		CHECK_EQ(z_dock_pages(n), 1);
		CHECK_EQ(z_dock_page_apps(n, 0), n);
		/* the old formula, spelled out */
		CHECK_EQ(z_dock_width(n),
			DOCK_PADDING * 2 + n * DOCK_ICON_SIZE + (n - 1) * DOCK_ICON_GAP);
	}

	/* Every app is on page 0 and on the stride. */
	CHECK_EQ(z_dock_app_at(10, 0, 0), 0);
	CHECK_EQ(z_dock_app_at(10, 0, 9), 9);
	CHECK_EQ(z_dock_app_at(10, 0, 10), -1);      /* past the end */
	CHECK_EQ(z_dock_slot_x(10, 0), DOCK_PADDING);
	CHECK_EQ(z_dock_slot_x(10, 3), DOCK_PADDING + 3 * DOCK_STRIDE);

	/* No NEXT position exists at all. Slot 16 is just an empty
	 * position, not a button -- otherwise a 17-app dock would show a
	 * NEXT leading to an empty page. */
	CHECK(!z_dock_slot_is_next(17, DOCK_NEXT_SLOT));
	CHECK_EQ(z_dock_app_at(17, 0, DOCK_NEXT_SLOT), 16);

}

/* The boundary: 17 fits, 18 pages. */
static void test_threshold(void) {

	CHECK(!z_dock_paged(17));
	CHECK(z_dock_paged(18));

	CHECK_EQ(z_dock_pages(17), 1);
	CHECK_EQ(z_dock_pages(18), 2);
	CHECK_EQ(z_dock_pages(32), 2);
	CHECK_EQ(z_dock_pages(33), 3);
	CHECK_EQ(z_dock_pages(48), 3);
	CHECK_EQ(z_dock_pages(49), 4);

	/* Crossing it costs one visible app -- 17 shown, then 16 plus a
	 * NEXT. That is the price of the button and it should be
	 * deliberate, not a surprise. */
	CHECK_EQ(z_dock_page_apps(17, 0), 17);
	CHECK_EQ(z_dock_page_apps(18, 0), 16);
	CHECK_EQ(z_dock_page_apps(18, 1), 2);

}

static void test_paged_layout(void) {

	int w = z_dock_width(20);

	/* Full width, margin to margin. */
	CHECK_EQ(w, WM_SCREEN_W - 2 * DOCK_MARGIN);

	/* Page 0 is full, page 1 has the remainder. */
	CHECK_EQ(z_dock_page_apps(20, 0), 16);
	CHECK_EQ(z_dock_page_apps(20, 1), 4);

	CHECK_EQ(z_dock_app_at(20, 0, 0), 0);
	CHECK_EQ(z_dock_app_at(20, 0, 15), 15);
	CHECK_EQ(z_dock_app_at(20, 1, 0), 16);
	CHECK_EQ(z_dock_app_at(20, 1, 3), 19);

	/* Short page: positions past the end hold nothing. */
	CHECK_EQ(z_dock_app_at(20, 1, 4), -1);
	CHECK_EQ(z_dock_app_at(20, 1, 15), -1);

	/* The NEXT position is never an app, on any page. */
	CHECK(z_dock_slot_is_next(20, DOCK_NEXT_SLOT));
	CHECK_EQ(z_dock_app_at(20, 0, DOCK_NEXT_SLOT), -1);
	CHECK_EQ(z_dock_app_at(20, 1, DOCK_NEXT_SLOT), -1);

	/* And it is in the SAME PLACE on every page, which is the whole
	 * reason it is right-aligned rather than trailing the last icon.
	 * A NEXT that followed the last icon would sit at slot 4 on
	 * page 1 above -- somewhere completely different. */
	CHECK_EQ(z_dock_slot_x(20, DOCK_NEXT_SLOT), w - DOCK_PADDING - DOCK_ICON_SIZE);
	CHECK_EQ(z_dock_slot_x(20, DOCK_NEXT_SLOT), z_dock_slot_x(40, DOCK_NEXT_SLOT));

	/* It clears the last app position rather than overlapping it. */
	CHECK(z_dock_slot_x(20, DOCK_NEXT_SLOT) >=
		z_dock_slot_x(20, DOCK_PAGE_SIZE - 1) + DOCK_ICON_SIZE);

	/* And stays on screen. */
	CHECK(DOCK_MARGIN + z_dock_slot_x(20, DOCK_NEXT_SLOT) + DOCK_ICON_SIZE
		<= WM_SCREEN_W);

}

/* Hit testing is where an off-by-one becomes "the dock launched the
 * wrong app", so it gets checked at every edge. */
static void test_hit(void) {

	int x0 = z_dock_slot_x(20, 5);

	CHECK_EQ(z_dock_slot_at_x(20, 0, x0), 5);
	CHECK_EQ(z_dock_slot_at_x(20, 0, x0 + DOCK_ICON_SIZE - 1), 5);

	/* The gap between icons belongs to neither. The stride is 36 and
	 * an icon is 32, so the four pixels before a slot begins are dead
	 * -- including the one immediately before it. */
	CHECK_EQ(z_dock_slot_at_x(20, 0, x0 - 1), -1);
	CHECK_EQ(z_dock_slot_at_x(20, 0, x0 - DOCK_ICON_GAP), -1);
	CHECK_EQ(z_dock_slot_at_x(20, 0, x0 - DOCK_ICON_GAP - 1), 4);
	CHECK_EQ(z_dock_slot_at_x(20, 0, x0 + DOCK_ICON_SIZE), -1);

	/* Left padding is not slot 0. */
	CHECK_EQ(z_dock_slot_at_x(20, 0, 0), -1);
	CHECK_EQ(z_dock_slot_at_x(20, 0, DOCK_PADDING), 0);

	/* NEXT is hittable on both pages. */
	CHECK_EQ(z_dock_slot_at_x(20, 0, z_dock_slot_x(20, DOCK_NEXT_SLOT)),
		DOCK_NEXT_SLOT);
	CHECK_EQ(z_dock_slot_at_x(20, 1, z_dock_slot_x(20, DOCK_NEXT_SLOT)),
		DOCK_NEXT_SLOT);

	/* The empty positions on a short page must NOT answer a click.
	 * Page 1 of 20 apps holds four; position 8 is empty, and a click
	 * there has to do nothing rather than launch app 24. */
	CHECK_EQ(z_dock_slot_at_x(20, 1, z_dock_slot_x(20, 8)), -1);
	CHECK_EQ(z_dock_slot_at_x(20, 1, z_dock_slot_x(20, 3)), 3);

	/* The gap between NEXT and the last app position is dead space,
	 * not a rounding error that lands on one of them. */
	CHECK_EQ(z_dock_slot_at_x(20, 0,
		z_dock_slot_x(20, DOCK_NEXT_SLOT) - 1), -1);

}

/* Every app must be reachable on exactly one page at exactly one
 * position. This is the property that actually matters -- an app that
 * appears twice, or not at all, is a dock that lies. */
static void test_every_app_reachable_once(void) {

	int n, seen[64], app, page, slot;

	for (n = 1; n <= 60; n++) {

		for (app = 0; app < n; app++) seen[app] = 0;

		for (page = 0; page < z_dock_pages(n); page++)
			for (slot = 0; slot < DOCK_SLOTS_MAX; slot++) {
				app = z_dock_app_at(n, page, slot);
				if (app < 0) continue;
				CHECK(app < n);
				if (app < n) seen[app]++;
			}

		for (app = 0; app < n; app++)
			if (seen[app] != 1) {
				printf("FAIL: %d apps: app %d appears %d times\n",
					n, app, seen[app]);
				failures++;
			}
		checks++;

	}

}

/* Views, along either axis: down a 480px screen for a dock on a side,
 * with and without a group's BACK. */
static void test_views(void) {

	int slots, lead, n, page, slot, full;

	CHECK_EQ(DOCK_SLOTS_TALL, 12);
	CHECK(DOCK_MARGIN + z_dock_len_for(DOCK_SLOTS_TALL, 464, DOCK_SLOTS_TALL) <= 480 - DOCK_MARGIN);
	CHECK(DOCK_MARGIN + DOCK_PADDING * 2 + (DOCK_SLOTS_TALL + 1) * DOCK_STRIDE - DOCK_ICON_GAP >
		480 - DOCK_MARGIN);
	CHECK_EQ(z_dock_full(480), 464);
	CHECK_EQ(z_dock_len_for(12, 464, 12), 436);
	CHECK_EQ(z_dock_len_for(12, 464, 13), 464);
	CHECK_EQ(z_dock_len_for(12, 464, 0), z_dock_len_for(12, 464, 1));
	CHECK_EQ(z_dock_width_for(17), z_dock_width(17));

	/* across, lead 0: the same as the dock before groups */
	for (n = 1; n <= 60; n++)
		for (page = 0; page < z_dock_pages(n); page++)
			for (slot = 0; slot < DOCK_SLOTS_MAX; slot++)
				CHECK_EQ(z_dock_vapp_at(DOCK_SLOTS_MAX, n, 0, page, slot), z_dock_app_at(n, page, slot));

	/* every app once, on either axis, with or without BACK */
	for (slots = 12; slots <= 17; slots += 5)
	for (lead = 0; lead <= 1; lead++)
	for (n = 0; n <= 60; n++) {
		int seen[64] = { 0 }, ok = 1, app;
		for (page = 0; page < z_dock_vpages(slots, n, lead); page++)
			for (slot = 0; slot < slots; slot++) {
				app = z_dock_vapp_at(slots, n, lead, page, slot);
				if (app >= 0 && app < n) seen[app]++;
				else if (app >= n) ok = 0;
			}
		for (app = 0; app < n; app++) if (seen[app] != 1) ok = 0;
		CHECK(ok);
	}

	/* down a side: NEXT hard against the bottom end, BACK at the top,
	 * and a position answers only where something is */
	full = z_dock_full(480);
	CHECK(z_dock_vpaged(12, 12, 1));
	CHECK(!z_dock_vpaged(12, 11, 1));
	CHECK_EQ(z_dock_vpage_size(12, 30, 1), 10);
	CHECK(z_dock_vslot_is_next(12, 30, 1, 11));
	CHECK(z_dock_vslot_is_back(1, 0));
	CHECK_EQ(z_dock_vslot_pos(full, 12, 30, 1, 11), full - DOCK_PADDING - DOCK_ICON_SIZE);
	CHECK_EQ(z_dock_vslot_pos(full, 12, 30, 1, 3), DOCK_PADDING + 3 * DOCK_STRIDE);
	CHECK_EQ(z_dock_vslot_at(full, 12, 30, 1, 0, DOCK_PADDING), 0);
	CHECK_EQ(z_dock_vslot_at(full, 12, 30, 1, 0, DOCK_PADDING - 1), -1);
	CHECK_EQ(z_dock_vslot_at(full, 12, 30, 1, 0, full - DOCK_PADDING - 1), 11);
	CHECK_EQ(z_dock_vslot_at(full, 12, 30, 1, 2, DOCK_PADDING + 9 * DOCK_STRIDE), 9);   /* 30 = 10+10+10 */
	CHECK_EQ(z_dock_vslot_at(full, 12, 25, 1, 2, DOCK_PADDING + 9 * DOCK_STRIDE), -1);  /* page 3 holds 5 */
	CHECK_EQ(z_dock_vslot_at(full, 12, 25, 1, 2, DOCK_PADDING + 5 * DOCK_STRIDE), 5);

}

int main(void) {

	test_capacity();
	test_unpaged();
	test_threshold();
	test_paged_layout();
	test_hit();
	test_every_app_reachable_once();
	test_views();

	printf("%s: %d checks, %d failures\n",
		failures ? "FAILED" : "ok", checks, failures);

	return failures ? 1 : 0;

}
