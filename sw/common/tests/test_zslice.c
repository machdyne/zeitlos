/*
 * Host test for sw/common/zslice.c: a long call run a slice at a time
 * (z_slice_run()), with zsvg as the long call -- the use view and web
 * make of it.
 *
 *   cc -std=gnu99 -Wall -I sw/common -o /tmp/t sw/common/tests/test_zslice.c \
 *      sw/common/zslice.c sw/common/zco.c sw/common/zsvg.c && /tmp/t
 *
 *   - a drawing rendered in slices is the drawing rendered in one call,
 *     bit for bit, and it really was cut into many slices
 *   - stopped from the pump, it stops at once and says so
 *   - z_slice is put back as it was
 */

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

#include "zslice.h"
#include "zsvg.h"

// The clock: each look at it is 3 ticks later, so a slice (Z_TICK_HZ /
// 100, about 7 ticks) is a few checkpoints long.
static uint32_t now;
uint32_t z_uptime_ticks(void) { return now += 3; }

static int fails, checks;
static void ck(int c, const char *w) { checks++; if (!c) { fails++; printf("FAIL: %s\n", w); } }

#define W 320
#define H 240
#define WPL (W / 32)
static uint32_t once[H][WPL], sliced[H][WPL];
static z_svg_edge_t edges[4096];
static char doc[64 * 1024];
static uint8_t stack[64 * 1024];

static void render(uint32_t *bits) {
	z_svg_t sv;
	memset(&sv, 0, sizeof sv);
	sv.doc = doc;
	sv.len = (uint32_t)strlen(doc);
	sv.w = W;
	sv.h = H;
	sv.edges = edges;
	sv.max_edges = 4096;
	z_svg_render_bitmap(&sv, bits, WPL);
}

static void fn(void *arg) { render(arg); }

static int pumps, stop_after;
static bool pump(void *ctx) { (void)ctx; pumps++; return stop_after <= 0 || pumps < stop_after; }

static void mine(void) {}

int main(void) {

	// a busy drawing: rings of circles and a big polygon
	int n = snprintf(doc, sizeof doc, "<svg viewBox=\"0 0 320 240\" xmlns=\"http://www.w3.org/2000/svg\">");
	for (int i = 0; i < 120; i++)
		n += snprintf(doc + n, sizeof doc - (size_t)n,
			"<circle cx=\"%d\" cy=\"%d\" r=\"%d\" fill=\"%s\"/>",
			(i * 37) % 320, (i * 53) % 240, 5 + i % 40, i % 2 ? "black" : "white");
	n += snprintf(doc + n, sizeof doc - (size_t)n,
		"<polygon points=\"10,10 310,30 200,230 20,200 160,120\" fill=\"black\" fill-rule=\"evenodd\"/></svg>");

	render(&once[0][0]);

	z_slice = mine;
	pumps = 0;
	stop_after = 0;
	ck(z_slice_run(fn, &sliced[0][0], stack, sizeof stack, pump, NULL), "it finishes");
	ck(pumps > 10, "in many slices");
	ck(!memcmp(once, sliced, sizeof once), "the same drawing, bit for bit");
	ck(z_slice == mine, "z_slice put back");

	memset(sliced, 0, sizeof sliced);
	pumps = 0;
	stop_after = 3;
	ck(!z_slice_run(fn, &sliced[0][0], stack, sizeof stack, pump, NULL), "stopped, it says so");
	ck(pumps == 3, "at once");
	ck(z_slice == mine, "z_slice put back after a stop");

	printf("test_zslice: %d checks, %d failed\n", checks, fails);
	return fails ? 1 : 0;

}
