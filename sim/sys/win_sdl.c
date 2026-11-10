/*
 * zsys's window (docs/zsys.md, "The window"): the screen, the keyboard
 * and the mouse, through SDL2 -- which is also what Emscripten offers a
 * page, so the browser build is this file too.
 *
 * The screen is redrawn a dirty row at a time (zsys_dirty_take()): the
 * rows VRAM writes touched, and the cursor's, since it is drawn on the
 * picture rather than in VRAM. Keys go to the keyboard on USB port 0 by
 * their scancode, which SDL numbers as USB HID usages; the mouse to
 * port 1, at the position on the emulated screen, so the host's pointer
 * is hidden over the window and the board's cursor stands in for it.
 */
#include <stdio.h>
#include <string.h>
#include <SDL.h>
#include "zsys.h"
#include "win.h"

static SDL_Window *win;
static SDL_Renderer *ren;
static SDL_Texture *tex;
static uint32_t pix[ZSYS_SCREEN_W * ZSYS_SCREEN_H];

// -- sound: the machine's frames, resampled to the device, queued --
static SDL_AudioDeviceID dev;
static int dev_hz;

static void sound_open(void) {
	SDL_AudioSpec want, have;
	SDL_zero(want);
	want.freq = 48000;
	want.format = AUDIO_F32SYS;
	want.channels = 2;
	want.samples = 1024;
	if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) { fprintf(stderr, "zsys: no sound: %s\n", SDL_GetError()); return; }
	dev = SDL_OpenAudioDevice(NULL, 0, &want, &have, SDL_AUDIO_ALLOW_FREQUENCY_CHANGE);
	if (!dev) { fprintf(stderr, "zsys: no sound: %s\n", SDL_GetError()); return; }
	dev_hz = have.freq;
	SDL_PauseAudioDevice(dev, 0);
}

static void sound_feed(zsys_t *s) {
	static float buf[2 * 4096];
	if (!dev) { float t[2 * 256]; while (zsys_audio_take(s, t, 256, 48000)) ; return; }
	// at most a fifth of a second queued: a machine that ran ahead is
	// heard late otherwise, and further behind at every frame
	uint32_t queued = SDL_GetQueuedAudioSize(dev) / 8, n;
	if (queued > (uint32_t)dev_hz / 5) { while (zsys_audio_take(s, buf, 4096, (uint32_t)dev_hz)) ; return; }
	while ((n = zsys_audio_take(s, buf, 4096, (uint32_t)dev_hz)) > 0) SDL_QueueAudio(dev, buf, n * 8);
}

bool win_open(int scale, const char *title, bool sound) {
	if (SDL_Init(SDL_INIT_VIDEO) != 0) {
		fprintf(stderr, "zsys: no window: %s\n", SDL_GetError());
		return false;
	}
	SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "0");	// nearest: pixels stay pixels
	win = SDL_CreateWindow(title, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
		ZSYS_SCREEN_W * scale, ZSYS_SCREEN_H * scale, SDL_WINDOW_RESIZABLE);
	if (win) ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_PRESENTVSYNC);
	if (ren) tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING,
		ZSYS_SCREEN_W, ZSYS_SCREEN_H);
	if (!tex) {
		fprintf(stderr, "zsys: no window: %s\n", SDL_GetError());
		return false;
	}
	SDL_RenderSetLogicalSize(ren, ZSYS_SCREEN_W, ZSYS_SCREEN_H);	// mouse positions in screen pixels
	SDL_ShowCursor(SDL_DISABLE);
	if (sound) sound_open();
	return true;
}

void win_close(void) {
	if (dev) SDL_CloseAudioDevice(dev);
	dev = 0;
	if (tex) SDL_DestroyTexture(tex);
	if (ren) SDL_DestroyRenderer(ren);
	if (win) SDL_DestroyWindow(win);
	SDL_Quit();
	tex = NULL; ren = NULL; win = NULL;
}

static uint8_t buttons;

void win_fullscreen(bool on) {
	SDL_SetWindowFullscreen(win, on ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
	SDL_RenderSetLogicalSize(ren, ZSYS_SCREEN_W, ZSYS_SCREEN_H);	// letterboxed, the mouse mapped through it
}

static void mouse_to(zsys_t *s, int x, int y) { zsys_mouse(s, x, y, buttons); }

bool win_poll(zsys_t *s) {
	SDL_Event e;
	while (SDL_PollEvent(&e)) {
		switch (e.type) {
		case SDL_QUIT:
			return false;
		case SDL_KEYDOWN: case SDL_KEYUP: {
			// Ctrl+Alt+Enter: the window full screen and back -- the host's,
			// not passed on (F11 is Zeitlos's own full screen, for its apps)
			if (e.type == SDL_KEYDOWN && e.key.keysym.scancode == SDL_SCANCODE_RETURN &&
			    (e.key.keysym.mod & KMOD_CTRL) && (e.key.keysym.mod & KMOD_ALT)) {
				if (!e.key.repeat) win_fullscreen(!(SDL_GetWindowFlags(win) & SDL_WINDOW_FULLSCREEN_DESKTOP));
				break;
			}
			if (e.key.repeat) break;		// a USB keyboard does not repeat; wm does
			SDL_Scancode sc = e.key.keysym.scancode;
			if (sc > 0 && sc <= 0xe7) zsys_key(s, (uint8_t)sc, e.type == SDL_KEYDOWN);
			break;
		}
		case SDL_MOUSEMOTION:
			mouse_to(s, e.motion.x, e.motion.y);
			break;
		case SDL_MOUSEBUTTONDOWN: case SDL_MOUSEBUTTONUP: {
			uint8_t bit = e.button.button == SDL_BUTTON_LEFT ? 1 : e.button.button == SDL_BUTTON_RIGHT ? 2 :
				e.button.button == SDL_BUTTON_MIDDLE ? 4 : 0;
			if (e.type == SDL_MOUSEBUTTONDOWN) buttons |= bit; else buttons &= (uint8_t)~bit;
			mouse_to(s, e.button.x, e.button.y);	// a report: the buttons changed
			break;
		}
		case SDL_MOUSEWHEEL: {			// whole notches; a touchpad's fractions add up
			static float acc;
			acc += e.wheel.preciseY * (e.wheel.direction == SDL_MOUSEWHEEL_FLIPPED ? -1.0f : 1.0f);
			int n = (int)acc;
			if (n) { zsys_wheel(s, n); acc -= (float)n; }
			break;
		}
		case SDL_WINDOWEVENT:
			if (e.window.event == SDL_WINDOWEVENT_FOCUS_LOST) {	// nothing stays held
				for (int u = 4; u <= 0xe7; u++) zsys_key(s, (uint8_t)u, false);
				buttons = 0;
			}
			if (e.window.event == SDL_WINDOWEVENT_EXPOSED) s->video_gen++;
			break;
		}
	}
	return true;
}

void win_present(zsys_t *s) {
	uint32_t rows[ZSYS_ROWS / 32];
	sound_feed(s);
	zsys_frame_dirty(s, rows);
	int lo = -1, hi = -1;
	for (int y = 0; y < ZSYS_SCREEN_H; y++) {
		if (!(rows[y >> 5] & (1u << (y & 31)))) continue;
		zsys_render_row(s, y, pix + y * ZSYS_SCREEN_W, true);
		if (lo < 0) lo = y;
		hi = y;
	}
	if (lo >= 0) {
		SDL_Rect r = { 0, lo, ZSYS_SCREEN_W, hi - lo + 1 };
		SDL_UpdateTexture(tex, &r, pix + lo * ZSYS_SCREEN_W, ZSYS_SCREEN_W * 4);
	}
	SDL_RenderClear(ren);
	SDL_RenderCopy(ren, tex, NULL, NULL);
	SDL_RenderPresent(ren);
}
