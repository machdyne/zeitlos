/*
 * zeitlos-sim: ./zeitlos-sim app.bin
 *
 * Runs an unmodified Zeitlos app binary (as produced by
 * `objcopy -O binary` from the app's own Makefile) against a software
 * model of the SOC's CPU, framebuffer, blitter and line rasterizer,
 * and displays the result in an SDL2 window.
 */

#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <SDL2/SDL.h>
#include "machine.h"

#define WINDOW_SCALE 2

static volatile sig_atomic_t g_quit = 0;
static void on_sigint(int sig) { (void)sig; g_quit = 1; }

static void update_cursor(machine_t *m, int x, int y, uint32_t buttons) {
	if (x < 0) x = 0;
	if (x > 1023) x = 1023;
	if (y < 0) y = 0;
	if (y > 1023) y = 1023;
	m->usb_cursor = ((uint32_t)x & 0x3ff) | (((uint32_t)y & 0x3ff) << 10) |
	                ((buttons & 0xf) << 20);
}

int main(int argc, char **argv) {
	if (argc < 2) {
		fprintf(stderr, "usage: %s <app.bin> [instructions_per_frame]\n", argv[0]);
		fprintf(stderr, "  app.bin: a raw Zeitlos app image (objcopy -O binary output)\n");
		return 1;
	}
	uint64_t insns_per_frame = argc > 2 ? strtoull(argv[2], NULL, 0) : 400000;

	signal(SIGINT, on_sigint);

	machine_t m;
	if (machine_init(&m, 0) != 0) {
		fprintf(stderr, "zeitlos-sim: failed to initialize machine\n");
		return 1;
	}
	if (machine_load_bin(&m, argv[1]) != 0) {
		machine_destroy(&m);
		return 1;
	}

	if (SDL_Init(SDL_INIT_VIDEO) != 0) {
		fprintf(stderr, "zeitlos-sim: SDL_Init failed: %s\n", SDL_GetError());
		machine_destroy(&m);
		return 1;
	}

	SDL_Window *win = SDL_CreateWindow("zeitlos-sim",
		SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
		ZS_SCREEN_W * WINDOW_SCALE, ZS_SCREEN_H * WINDOW_SCALE, 0);
	SDL_Renderer *ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_PRESENTVSYNC);
	SDL_Texture *tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888,
		SDL_TEXTUREACCESS_STREAMING, ZS_SCREEN_W, ZS_SCREEN_H);

	uint32_t *pixels = malloc((size_t)ZS_SCREEN_W * ZS_SCREEN_H * 4);

	fprintf(stderr, "zeitlos-sim: running %s (Ctrl+C or close window to quit)\n", argv[1]);

	while (!g_quit && m.running && !m.exit_requested) {

		SDL_Event ev;
		while (SDL_PollEvent(&ev)) {
			if (ev.type == SDL_QUIT) g_quit = 1;
			if (ev.type == SDL_KEYDOWN && ev.key.keysym.sym == SDLK_ESCAPE) g_quit = 1;
			if (ev.type == SDL_MOUSEMOTION) {
				int mx = ev.motion.x / WINDOW_SCALE, my = ev.motion.y / WINDOW_SCALE;
				uint32_t buttons = SDL_GetMouseState(NULL, NULL);
				uint32_t b = ((buttons & SDL_BUTTON(SDL_BUTTON_LEFT)) ? 1 : 0) |
				             ((buttons & SDL_BUTTON(SDL_BUTTON_RIGHT)) ? 2 : 0);
				update_cursor(&m, mx, my, b);
			}
		}

		machine_run(&m, insns_per_frame);

		for (int y = 0; y < ZS_SCREEN_H; y++)
			for (int x = 0; x < ZS_SCREEN_W; x++)
				pixels[y * ZS_SCREEN_W + x] = machine_get_pixel(&m, x, y) ? 0xFFFFFFFFu : 0xFF000000u;

		SDL_UpdateTexture(tex, NULL, pixels, ZS_SCREEN_W * 4);
		SDL_RenderClear(ren);
		SDL_RenderCopy(ren, tex, NULL, NULL);
		SDL_RenderPresent(ren);
	}

	if (m.exit_requested)
		fprintf(stderr, "zeitlos-sim: app exited\n");
	if (!m.running)
		fprintf(stderr, "zeitlos-sim: app halted (illegal instruction or trap)\n");

	free(pixels);
	SDL_DestroyTexture(tex);
	SDL_DestroyRenderer(ren);
	SDL_DestroyWindow(win);
	SDL_Quit();
	machine_destroy(&m);
	return 0;
}
