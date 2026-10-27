/*
 * learn -- what a repaint costs: the app on the build machine, a lesson
 * step with a figure, keys typed one at a time with the page drawn after
 * each, as the board does. Run under valgrind --tool=callgrind to see
 * where the instructions go (make -C sw/apps/learn bench).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../lapp_int.h"
#include "../ldraw.h"
#include "hostplat.h"
#include "zkbd.h"

#define W 640
#define H 480
static uint32_t bits[(W / 32) * H + 2];
static ld_page_t page;
static uint8_t mem[3u << 20];
static uint8_t scratch[96 << 10], cache[128 << 10];
static const char *fontdir = "../../data/font";

static const void *load_font(const char *file, uint32_t *len) {
	char p[512];
	const char *sub = strstr(file, ".zhf") ? "zhf/" : strstr(file, "jp12") ? "" : "zfn/";
	snprintf(p, sizeof p, "%s/%s%s", fontdir, sub, file);
	FILE *f = fopen(p, "rb");
	if (!f) return NULL;
	fseek(f, 0, SEEK_END);
	long n = ftell(f);
	fseek(f, 0, SEEK_SET);
	void *d = malloc((size_t)n);
	if (fread(d, 1, (size_t)n, f) != (size_t)n) { fclose(f); free(d); return NULL; }
	fclose(f);
	*len = (uint32_t)n;
	return d;
}

static void key(uint32_t k) {
	la_ev_t e;
	memset(&e, 0, sizeof e);
	e.type = LA_KEY;
	e.key = k;
	if (!la_event(&e)) return;
	int x, y, w, h;
	if (la_partial(&x, &y, &w, &h)) la_draw_part(&page, x, y, w, h);
	else la_draw(&page);
}

int main(int argc, char **argv) {
	int n = argc > 1 ? atoi(argv[1]) : 20;
	LP = &host_plat;
	snprintf(hp_root, sizeof hp_root, "/tmp/learn_bench_card");
	hp_reset_card();
	if (system("mkdir -p /tmp/learn_bench_card/opt/ark/edu-zeitlos && cp -r ../../data/learn/edu-zeitlos/* /tmp/learn_bench_card/opt/ark/edu-zeitlos/")) return 1;
	ld_fonts_load(load_font);
	ld_init(scratch, sizeof scratch, cache, sizeof cache);
	ld_page(&page, bits, W, H);
	la_init(mem, sizeof mem);
	la_resize(W, argc > 3 && !strcmp(argv[3], "scroll") ? 300 : H);
	while (la_work(50)) hp_ms_now += 50;
	snprintf(hp_prompt_answer, sizeof hp_prompt_answer, "Bench");
	key('n');
	la_open_course("/opt/ark/edu-zeitlos/zeit-learn");
	const char *what = argc > 2 ? argv[2] : "fig";
	if (!strcmp(what, "fig")) { la_open_lesson(3); A.step = 1; lv_enter(); }	// the slider step: a figure and a typed answer
	else { la_open_lesson(1); A.step = 1; lv_enter(); }					// a typed answer, no figure
	la_draw(&page);
	const char *how = argc > 3 ? argv[3] : "type";
	for (int i = 0; i < n; i++) {
		if (!strcmp(how, "type")) key(i & 1 ? 0x7f : '4');		// type and delete
		else if (!strcmp(how, "slide")) key(i & 1 ? Z_KEY_LEFT : Z_KEY_RIGHT);
		else key(i % 4 < 2 ? Z_KEY_DOWN : Z_KEY_UP);				// scroll
	}
	printf("bench: %d repaints\n", n);
	return 0;
}
