/*
 * view -- its screen face on a build host: a picture (tests/gen.py)
 * through vscreen.c on the arcade runtime's software framebuffer, the
 * window in black and white, its colour window, F11 full screen in 256
 * colours, mono full screen, back to the window, and F10 back to the
 * desktop face. make -C sw/apps/view render (and AR_HOST_PLANES=6).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../../common/games/tests/ar_host.h"
#include "../../../common/games/zarcade.h"
#include "../vscreen.h"

/* zimg.c's file source is not used here: the picture is in memory */
int fs_open_read(const char *p) { (void)p; return -1; }
int fs_read_chunk(int h, void *b, int n) { (void)h; (void)b; (void)n; return -1; }
int fs_seek(int h, uint32_t o) { (void)h; (void)o; return 0; }
int fs_close_handle(int h) { (void)h; return 1; }

static uint8_t *data;
static long len, pos;

static int mem_read(void *ctx, uint8_t *buf, int n) {
	(void)ctx;
	if (n > len - pos) n = (int)(len - pos);
	memcpy(buf, data + pos, (size_t)n);
	pos += n;
	return n;
}

static bool mem_rewind(void *ctx) { (void)ctx; pos = 0; return true; }

#define T(f, k)         { (f), ARH_TAP, (k), 0, NULL, 0, 0, 0, 0 }
#define SHOT(f, n)      { (f), ARH_SHOT, 0, 0, (n), 0, 0, 0, 0 }
#define MODE(f, m)      { (f), (m), 0, 0, NULL, 0, 0, 0, 0 }
#define WCOLOR(f, on)   { (f), ARH_WCOLOR, (on), 0, NULL, 0, 0, 0, 0 }
#define END(f)          { (f), ARH_END, 0, 0, NULL, 0, 0, 0, 0 }

static const ar_host_step_t script[] = {
	SHOT(4, "v1_window_mono"),
	WCOLOR(6, 1),
	SHOT(10, "v2_window_colour"),
	MODE(12, ARH_FULL_COLOR),
	SHOT(16, "v3_full_colour"),
	MODE(18, ARH_FULL_MONO),
	SHOT(22, "v4_full_mono"),
	MODE(24, ARH_WINDOW),
	SHOT(28, "v5_window_again"),
	T(30, ARH_K_F10),               /* back to the desktop face */
	END(60)
};

int main(int argc, char **argv) {
	const char *dir = argc > 2 ? argv[2] : "out";
	vs_src_t s;
	FILE *f;
	int r;

	f = fopen(argc > 1 ? argv[1] : "out/pic.png", "rb");
	if (!f) { perror("picture"); return 1; }
	fseek(f, 0, SEEK_END);
	len = ftell(f);
	fseek(f, 0, SEEK_SET);
	data = malloc((size_t)len);
	if (fread(data, 1, (size_t)len, f) != (size_t)len) return 1;
	fclose(f);

	s.read = mem_read;
	s.rewind = mem_rewind;
	s.ctx = NULL;
	if (!vs_load(&s)) { puts("render_view: the picture did not load"); return 1; }

	setvbuf(stdout, NULL, _IOLBF, 0);
	ar_window_colour(true);
	ar_host_run(script, dir);
	r = vs_run(NULL, false, "pic.png");
	printf("render_view: %d pictures, vs_run %d (%s)\n", ar_host_shots(), r,
		r == VS_DESKTOP ? "back to the desktop" : "?");
	return r != VS_DESKTOP || ar_host_shots() != 5;
}
